#include "HudWindow.h"
#include "AppAssets.h"
#include "Layout.h"
#include "WindowsIntegration.h"

#include <QApplication>
#include <QCloseEvent>
#include <QContextMenuEvent>
#include <QCursor>
#include <QJsonArray>
#include <QLinearGradient>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>
#include <QWheelEvent>
#include <QWindow>
#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace {
constexpr int SurfaceInset = 14;
constexpr int ArtworkSize = 96;
constexpr int ArtworkBlurRadius = 10;
constexpr int ArtworkBlurPasses = 3;
constexpr double ArtworkMaxLuminance = 0.035;
const QStringList Transport{"previous", "play", "next"};

QImage blurredArtwork(const QPixmap& cover) {
    if (cover.isNull()) return {};
    QImage image(ArtworkSize, ArtworkSize, QImage::Format_RGB32);
    image.fill(Qt::black);
    {
        QPainter painter(&image);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.drawPixmap(image.rect(), cover);
    }
    constexpr int samples = ArtworkBlurRadius * 2 + 1;
    for (int pass = 0; pass < ArtworkBlurPasses * 2; ++pass) {
        QImage blurred(image.size(), QImage::Format_RGB32);
        const bool horizontal = pass % 2 == 0;
        for (int line = 0; line < ArtworkSize; ++line) {
            const auto pixel = [&](int index) {
                index = std::clamp(index, 0, ArtworkSize - 1);
                const int x = horizontal ? index : line;
                const int y = horizontal ? line : index;
                return reinterpret_cast<const QRgb*>(image.constScanLine(y))[x];
            };
            int red = 0, green = 0, blue = 0;
            const auto add = [&](QRgb color, int direction) {
                red += qRed(color) * direction;
                green += qGreen(color) * direction;
                blue += qBlue(color) * direction;
            };
            for (int index = -ArtworkBlurRadius; index <= ArtworkBlurRadius; ++index) add(pixel(index), 1);
            for (int index = 0; index < ArtworkSize; ++index) {
                const int x = horizontal ? index : line;
                const int y = horizontal ? line : index;
                reinterpret_cast<QRgb*>(blurred.scanLine(y))[x] = qRgb(red / samples, green / samples, blue / samples);
                add(pixel(index - ArtworkBlurRadius), -1);
                add(pixel(index + ArtworkBlurRadius + 1), 1);
            }
        }
        image = std::move(blurred);
    }
    static const auto linear = [] {
        std::array<double, 256> values{};
        for (int index = 0; index < 256; ++index) {
            const double channel = index / 255.0;
            values[index] = channel <= 0.04045 ? channel / 12.92 : std::pow((channel + 0.055) / 1.055, 2.4);
        }
        return values;
    }();
    const auto channel = [](double value) {
        return qRound(255 * (value <= 0.0031308 ? value * 12.92 : 1.055 * std::pow(value, 1 / 2.4) - 0.055));
    };
    // Limit lightness in linear RGB so white covers stay readable without bleaching colored covers.
    for (int y = 0; y < image.height(); ++y) {
        auto* row = reinterpret_cast<QRgb*>(image.scanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            const double red = linear[qRed(row[x])], green = linear[qGreen(row[x])], blue = linear[qBlue(row[x])];
            const double luminance = 0.2126 * red + 0.7152 * green + 0.0722 * blue;
            if (luminance <= ArtworkMaxLuminance) continue;
            const double exposure = ArtworkMaxLuminance / luminance;
            row[x] = qRgb(channel(red * exposure), channel(green * exposure), channel(blue * exposure));
        }
    }
    return image;
}

QImage blendArtwork(const QImage& previous, const QImage& next, double progress) {
    if (progress >= 1 || previous.cacheKey() == next.cacheKey()) return next;
    if (progress <= 0) return previous;
    QImage blended(ArtworkSize, ArtworkSize, QImage::Format_ARGB32_Premultiplied);
    blended.fill(Qt::transparent);
    QPainter painter(&blended);
    painter.setOpacity(1 - progress);
    painter.drawImage(0, 0, previous);
    painter.setCompositionMode(QPainter::CompositionMode_Plus);
    painter.setOpacity(progress);
    painter.drawImage(0, 0, next);
    return blended;
}
QColor ink(const QString& value, double alpha = 1) {
    QColor c(value);
    c.setAlphaF(std::clamp(alpha, 0.0, 1.0));
    return c;
}
void icon(QPainter& p, const QString& name, const QRectF& rect, const QColor& color,
          double size, bool playing = false) {
    p.save();
    p.translate(rect.center());
    p.scale(size / 24, size / 24);
    p.setPen(Qt::NoPen);
    p.setBrush(color);
    QPainterPath path;
    if (name == "play" && playing) {
        p.drawRoundedRect(QRectF(-7, -8, 5, 16), 1.5, 1.5);
        p.drawRoundedRect(QRectF(2, -8, 5, 16), 1.5, 1.5);
    } else if (Transport.contains(name)) {
        if (name == "previous") p.scale(-1, 1);
        path.moveTo(-6, -8); path.lineTo(8, 0); path.lineTo(-6, 8); path.closeSubpath();
        p.drawPath(path);
        if (name != "play") p.drawRoundedRect(QRectF(8, -8, 3, 16), 1, 1);
    } else if (name == "volume") {
        path.moveTo(-9, -3); path.lineTo(-5, -3); path.lineTo(1, -8);
        path.lineTo(1, 8); path.lineTo(-5, 3); path.lineTo(-9, 3); path.closeSubpath();
        p.drawPath(path);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(color, 1.8, Qt::SolidLine, Qt::RoundCap));
        p.drawArc(QRectF(-3, -7, 13, 14), -65 * 16, 130 * 16);
    } else {
        p.setPen(QPen(color, 2.4, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(5, -9), QPointF(5, 5));
        p.drawLine(QPointF(5, -9), QPointF(-5, -6));
        p.drawLine(QPointF(-5, -6), QPointF(-5, 8));
        p.setPen(Qt::NoPen);
        p.drawEllipse(QRectF(-11, 4, 7, 5));
        p.drawEllipse(QRectF(-1, 1, 7, 5));
    }
    p.restore();
}
}

HudWindow::HudWindow(const QJsonObject& config)
    : QWidget(nullptr, Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool | Qt::WindowDoesNotAcceptFocus),
      config_(config) {
    AppAssets::settingsFontFamily();
    setWindowTitle("SCARP ISLAND");
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setMouseTracking(true);
    setAccessibleName(QStringLiteral("SCARP ISLAND - музыкальный оверлей"));
    frameTimer_.setTimerType(Qt::PreciseTimer);
    connect(&frameTimer_, &QChronoTimer::timeout, this, qOverload<>(&QWidget::update));
    hideTimer_.setSingleShot(true);
    connect(&hideTimer_, &QTimer::timeout, this, [this] {
        if (editing_ || menuOpen_ || dragWindow_ || !dragElement_.isEmpty()) return;
        if (underMouse() || pointerInDockArea()) { restartHideTimer(); return; }
        hoverFromDock_ = false;
        if (config_["idle_collapse"].toBool(true)) setCollapsed(true);
        else conceal();
    });
    connect(qApp, &QGuiApplication::screenAdded, this, [this](QScreen* s) { watchScreen(s); placeOnScreen(); });
    connect(qApp, &QGuiApplication::screenRemoved, this, [this] { placeOnScreen(); });
    for (auto* screen : QGuiApplication::screens()) watchScreen(screen);
    applyConfig(config);
}

HudWindow::~HudWindow() {
    frameTimer_.stop();
    hideTimer_.stop();
    animations_.stopAll();
    if (QGuiApplication::platformName() == "windows") WindowsIntegration::releaseOverlayBackdrop(winId());
}

void HudWindow::watchScreen(QScreen* screen) {
    connect(screen, &QScreen::availableGeometryChanged, this, [this] { placeOnScreen(); });
    connect(screen, &QScreen::geometryChanged, this, [this] { placeOnScreen(); });
    connect(screen, &QScreen::logicalDotsPerInchChanged, this, [this] { placeOnScreen(); });
    connect(screen, &QScreen::refreshRateChanged, this, [this] { syncRefreshRate(); });
}

bool HudWindow::event(QEvent* event) {
    if (event->type() == QEvent::WindowBlocked || event->type() == QEvent::WindowDeactivate) cancelSeek();
    const bool handled = QWidget::event(event);
    if (event->type() == QEvent::Move || event->type() == QEvent::DevicePixelRatioChange) syncRefreshRate();
    return handled;
}

void HudWindow::syncRefreshRate() {
    const bool docked = collapsed_ || dockProgress_ > 0;
    const auto* target = !docked && isVisible() && windowHandle() ? windowHandle()->screen() : targetScreen();
    animations_.setRefreshRate(target ? target->refreshRate() : 0);
    syncFrameTimer();
}

QMap<QString, QRectF> HudWindow::elementRects() const { return Layout::elements(config_); }

QScreen* HudWindow::targetScreen() const {
    auto* target = QGuiApplication::primaryScreen();
    for (auto* screen : QGuiApplication::screens())
        if (screen->name() == config_["monitor"].toString()) target = screen;
    return target;
}

void HudWindow::placeOnScreen() {
    auto* target = targetScreen();
    if (!target) return;
    const double scale = config_["scale"].toDouble(1);
    const int inset = qCeil(SurfaceInset * scale);
    const QSize expandedSize(qRound(config_["width"].toDouble(560) * scale) + inset * 2,
                             qRound(config_["height"].toDouble(132) * scale) + inset * 2);
    const auto saved = config_["monitor_positions"].toObject()[target->name()].toArray();
    QPoint point;
    if (saved.size() == 2) point = QPoint(saved[0].toInt(), saved[1].toInt());
    expandedGeometry_ = QRect(Layout::screenPosition(target->availableGeometry(), expandedSize,
        config_["anchor"].toString(), config_["offset_y"].toInt(12), saved.size() == 2 ? &point : nullptr), expandedSize);
    const QSize compactSize(std::min(target->geometry().width(),
                                     qRound(config_["compact_width"].toDouble(156) * scale) + inset * 2),
                            qRound(config_["compact_height"].toDouble(32) * scale) + inset * 2);
    compactGeometry_ = Layout::dockGeometry(target->geometry(), compactSize, inset,
        qRound(config_["compact_visible_height"].toDouble(8) * scale));
    stopAnimation("dock");
    applyDockGeometry(collapsed_ ? 1 : 0);
    syncRefreshRate();
}

void HudWindow::applyConfig(const QJsonObject& config) {
    cancelSeek();
    config_ = config;
    QFont font(config_["font_family"].toString("Inter"));
    font.setPixelSize(config_["font_size"].toInt(14));
    font.setWeight(static_cast<QFont::Weight>(std::clamp(config_["font_weight"].toInt(600), 100, 900)));
    setFont(font);
    if (!config_["idle_collapse"].toBool(true) || editing_) collapsed_ = false;
    placeOnScreen();
    if (isVisible()) applyNative();
    syncFrameTimer(); restartHideTimer(); update();
}

void HudWindow::applyDockGeometry(double progress) {
    dockProgress_ = std::clamp(progress, 0.0, 1.0);
    const auto interpolate = [this](int from, int to) { return qRound(from + (to - from) * dockProgress_); };
    const QSize size(interpolate(expandedGeometry_.width(), compactGeometry_.width()),
                     interpolate(expandedGeometry_.height(), compactGeometry_.height()));
    setGeometry(QRect(QPoint(interpolate(expandedGeometry_.x(), compactGeometry_.x()),
                            interpolate(expandedGeometry_.y(), compactGeometry_.y())), size));
    updateNativeRegion();
    syncFrameTimer();
    update();
}

void HudWindow::setCollapsed(bool collapsed) {
    if (collapsed && (editing_ || menuOpen_ || manualHidden_ || !isVisible()
        || !config_["idle_collapse"].toBool(true))) return;
    if (collapsed_ == collapsed && dockProgress_ == (collapsed ? 1.0 : 0.0)) return;
    cancelSeek();
    collapsed_ = collapsed;
    if (collapsed) hoverFromDock_ = false;
    hover_.clear();
    setToolTip({});
    hideTimer_.stop();
    animate("dock", dockProgress_, collapsed ? 1 : 0, [this](double value) { applyDockGeometry(value); },
        [this] { restartHideTimer(); });
    if (isVisible()) applyNative();
}

bool HudWindow::pointerInDockArea() const {
    if (!hoverFromDock_ || !config_["idle_collapse"].toBool(true)) return false;
    const auto* target = targetScreen();
    if (!target) return false;
    const int inset = qCeil(SurfaceInset * config_["scale"].toDouble(1));
    const QRect card = expandedGeometry_.adjusted(inset, inset, -inset, -inset);
    const QRect handle = compactGeometry_.adjusted(inset, inset, -inset, -inset).intersected(target->geometry());
    const QPoint cursor = QCursor::pos();
    if (card.contains(cursor) || handle.contains(cursor)) return true;
    // Keep the path from the edge handle to the opening card reachable during its transition.
    QPainterPath corridor;
    corridor.moveTo(handle.bottomLeft()); corridor.lineTo(handle.bottomRight());
    corridor.lineTo(card.topRight()); corridor.lineTo(card.topLeft()); corridor.closeSubpath();
    return corridor.contains(cursor);
}

void HudWindow::applyNative() {
    if (QGuiApplication::platformName() != "windows") return;
    const double scale = config_["scale"].toDouble(1);
    const int inset = qCeil(SurfaceInset * scale);
    const QRectF card = QRectF(rect()).adjusted(inset, inset, -inset, -inset);
    const double radius = (config_["radius"].toDouble(30) * (1 - dockProgress_)
                           + config_["compact_radius"].toDouble(16) * dockProgress_) * scale;
    const auto* target = targetScreen();
    const QRectF bounds = target && dockProgress_ > 0 ? QRectF(target->geometry()).translated(-pos()) : QRectF{};
    WindowsIntegration::applyOverlayBackdrop(winId(), config_["blur"].toBool(true), card, radius, devicePixelRatioF(),
        config_[collapsed_ ? "compact_background" : "background"].toString("#10121B"),
        config_[collapsed_ ? "compact_opacity" : "opacity"].toDouble(.94), bounds);
    WindowsIntegration::setClickThrough(winId(), config_["click_through"].toBool() && !editing_ && !collapsed_);
    WindowsIntegration::ensureTopmost(winId());
    WindowsIntegration::setOverlayOpacity(winId(), windowOpacity());
}

void HudWindow::setHudOpacity(double opacity) {
    setWindowOpacity(opacity);
    if (QGuiApplication::platformName() == "windows") WindowsIntegration::setOverlayOpacity(winId(), opacity);
}

void HudWindow::updateNativeRegion() {
    if (!isVisible() || QGuiApplication::platformName() != "windows") return;
    const double scale = config_["scale"].toDouble(1);
    const int inset = qCeil(SurfaceInset * scale);
    const QRectF card = QRectF(rect()).adjusted(inset, inset, -inset, -inset);
    const double radius = (config_["radius"].toDouble(30) * (1 - dockProgress_)
                           + config_["compact_radius"].toDouble(16) * dockProgress_) * scale;
    const auto* target = targetScreen();
    const QRectF bounds = target && dockProgress_ > 0 ? QRectF(target->geometry()).translated(-pos()) : QRectF{};
    WindowsIntegration::updateOverlayRegion(winId(), card, radius, devicePixelRatioF(), bounds);
}
void HudWindow::showEvent(QShowEvent* e) {
    QWidget::showEvent(e);
    if (windowHandle())
        connect(windowHandle(), &QWindow::screenChanged, this, &HudWindow::syncRefreshRate, Qt::UniqueConnection);
    syncRefreshRate(); applyNative(); syncFrameTimer();
}
void HudWindow::hideEvent(QHideEvent* e) {
    cancelSeek();
    frameTimer_.stop(); hideTimer_.stop();
    animations_.stopAll();
    coverAlpha_ = titleAlpha_ = playAlpha_ = hoverAlpha_ = 1;
    displayedArtwork_ = artwork_;
    dragElement_.clear(); dragOrigin_.reset(); dragWindow_.reset();
    hover_.clear(); hoverFromDock_ = false;
    QWidget::hideEvent(e);
}
void HudWindow::closeEvent(QCloseEvent* e) { e->ignore(); conceal(true); }
void HudWindow::enterEvent(QEnterEvent* e) {
    if (!manualHidden_) {
        if (collapsed_ || dockProgress_ > 0) hoverFromDock_ = true;
        if (collapsed_ || dockProgress_ > 0 || fadingOut_) reveal();
        hideTimer_.stop();
    }
    QWidget::enterEvent(e);
}
void HudWindow::leaveEvent(QEvent* e) { hover_.clear(); update(); restartHideTimer(); QWidget::leaveEvent(e); }

void HudWindow::stopAnimation(const QString& name) {
    animations_.stop(name);
}

void HudWindow::animate(const QString& name, double from, double to,
                        std::function<void(double)> callback, std::function<void()> finished) {
    stopAnimation(name);
    const bool transition = name == "dock" || name == "appear" || name == "disappear";
    if (!config_["animations"].toObject()[name].toBool(true)
        || (!transition && (!isVisible() || dockProgress_ == 1))) {
        callback(to); if (finished) finished(); return;
    }
    syncRefreshRate();
    animations_.start(name, from, to, std::chrono::milliseconds(config_["animation_duration"].toInt(260)),
                      std::move(callback), std::move(finished));
}

void HudWindow::reveal(bool manual) {
    if (manual) manualHidden_ = false;
    if (manualHidden_) return;
    const bool visible = isVisible();
    fadingOut_ = false;
    stopAnimation("disappear");
    setCollapsed(false);
    if (!visible) setHudOpacity(config_["animations"].toObject()["appear"].toBool(true) ? 0 : 1);
    show();
    applyNative();
    if (!visible || windowOpacity() < 1)
        animate("appear", visible ? windowOpacity() : 0, 1, [this](double a) { setHudOpacity(a); },
            [this] { restartHideTimer(); });
    restartHideTimer();
}
void HudWindow::conceal(bool manual) {
    cancelSeek();
    if (manual) manualHidden_ = true;
    hideTimer_.stop(); stopAnimation("appear"); stopAnimation("dock");
    if (isVisible()) {
        fadingOut_ = true;
        animate("disappear", windowOpacity(), 0, [this](double a) { setHudOpacity(a); }, [this] { hide(); });
    }
}
void HudWindow::toggle() {
    if (isVisible() && !fadingOut_ && !collapsed_) conceal(true);
    else reveal(true);
}
void HudWindow::restartHideTimer() {
    hideTimer_.stop();
    for (const auto& name : {QStringLiteral("dock"), QStringLiteral("appear")}) {
        if (animations_.isRunning(name)) return;
    }
    const bool docking = config_["idle_collapse"].toBool(true);
    const int delay = docking ? config_["idle_collapse_seconds"].toInt(3) : config_["auto_hide_seconds"].toInt();
    if (delay > 0 && !editing_ && !menuOpen_ && !collapsed_ && !fadingOut_ && dockProgress_ == 0 && isVisible())
        hideTimer_.start(delay * 1000);
}
void HudWindow::syncFrameTimer() {
    if (isVisible() && snapshot_.playing && dockProgress_ < 1) {
        const auto interval = config_["animations"].toObject()["progress"].toBool(true)
            ? animations_.frameInterval() : std::chrono::seconds(1);
        if (frameTimer_.interval() != interval) frameTimer_.setInterval(interval);
        if (!frameTimer_.isActive()) frameTimer_.start();
    } else frameTimer_.stop();
}
void HudWindow::setEditing(bool enabled) {
    if (editing_ != enabled) cancelSeek();
    if (editing_ != enabled) { dragElement_.clear(); dragOrigin_.reset(); dragWindow_.reset(); }
    editing_ = enabled;
    if (enabled) reveal(true);
    applyNative(); restartHideTimer(); update();
}
void HudWindow::setVolume(double value) { volume_ = std::clamp(value, 0.0, 1.0); update(); }
void HudWindow::setVolumeAvailable(bool available) {
    if (volumeAvailable_ == available) return;
    volumeAvailable_ = available;
    if (!available && !editing_ && dragElement_ == "volume") dragElement_.clear();
    if (hover_ == "volume") {
        hover_.clear();
        setToolTip({});
        setCursor(Qt::ArrowCursor);
    }
    update();
}

void HudWindow::setSnapshot(const MediaSnapshot& snapshot) {
    const bool changed = snapshot.sourceId != snapshot_.sourceId || snapshot.title != snapshot_.title
        || snapshot.artist != snapshot_.artist || snapshot.album != snapshot_.album;
    if (changed || !snapshot.active || !snapshot.canSeek || !std::isfinite(snapshot.duration)
        || snapshot.duration <= 0 || snapshot.duration != snapshot_.duration) cancelSeek();
    if (snapshot.cover != snapshot_.cover) {
        oldCover_ = cover_; cover_ = QPixmap();
        if (!snapshot.cover.isEmpty()) cover_.loadFromData(snapshot.cover);
        oldArtwork_ = displayedArtwork_;
        artwork_ = blurredArtwork(cover_);
        animate("cover", 0, 1, [this](double a) {
            coverAlpha_ = a;
            displayedArtwork_ = blendArtwork(oldArtwork_, artwork_, a);
            update();
        });
    }
    if (changed) {
        animate("title", .15, 1, [this](double a) { titleAlpha_ = a; update(); });
        if (snapshot.active && !isVisible()) reveal();
    }
    if (snapshot.playing != snapshot_.playing)
        animate("play", .25, 1, [this](double a) { playAlpha_ = a; update(); });
    snapshot_ = snapshot;
    syncFrameTimer(); update();
}

void HudWindow::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const double scale = config_["scale"].toDouble(1);
    const int inset = qCeil(SurfaceInset * scale);
    p.translate(inset, inset);
    p.scale(scale, scale);
    const QRectF card(0, 0, (width() - inset * 2) / scale, (height() - inset * 2) / scale);
    const auto mix = [this](double from, double to) { return from + (to - from) * dockProgress_; };
    const auto blendColor = [mix](const QColor& from, const QColor& to) {
        return QColor::fromRgbF(mix(from.redF(), to.redF()), mix(from.greenF(), to.greenF()),
                               mix(from.blueF(), to.blueF()));
    };
    const double radius = std::min({mix(config_["radius"].toDouble(30), config_["compact_radius"].toDouble(16)),
                                     card.width() / 2, card.height() / 2});
    p.setPen(Qt::NoPen);
    QLinearGradient gradient(card.topLeft(), card.bottomRight());
    const double opacity = mix(config_["opacity"].toDouble(.94), config_["compact_opacity"].toDouble(.94));
    const QColor compactColor(config_["compact_background"].toString("#10121B"));
    QColor first = blendColor(QColor(config_["background"].toString()), compactColor);
    QColor last = blendColor(QColor(config_[config_["gradient_enabled"].toBool(true) ? "gradient_color" : "background"].toString()), compactColor);
    first.setAlphaF(opacity); last.setAlphaF(opacity);
    gradient.setColorAt(0, first);
    gradient.setColorAt(1, last);
    p.setBrush(gradient);
    p.drawRoundedRect(card, radius, radius);
    QPainterPath clip;
    clip.addRoundedRect(card, radius, radius);
    p.setClipPath(clip);
    if (config_["artwork_background"].toBool(true) && !displayedArtwork_.isNull()) {
        p.save();
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        p.setCompositionMode(QPainter::CompositionMode_SourceAtop);
        p.setOpacity(config_["artwork_background_strength"].toDouble(.75) * (1 - dockProgress_));
        p.drawImage(card, displayedArtwork_);
        p.restore();
    }
    const double border = config_["border_width"].toDouble(0);
    if (border > 0) {
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(ink(config_["border_color"].toString("#9B8CFF"), config_["border_opacity"].toDouble(.22)), border));
        const double borderInset = border / 2;
        p.drawRoundedRect(card.adjusted(borderInset, borderInset, -borderInset, -borderInset), radius, radius);
    }
    if (dockProgress_ > 0) {
        const double visible = std::min(card.height(), config_["compact_visible_height"].toDouble(8));
        const double gripWidth = std::min(28.0, card.width() - 16);
        p.setPen(Qt::NoPen);
        p.setBrush(ink(config_["accent_color"].toString(), .6 * dockProgress_));
        p.drawRoundedRect(QRectF(card.center().x() - gripWidth / 2, card.bottom() - visible / 2 - 1,
                                 gripWidth, 2), 1, 1);
    }
    const double contentOpacity = std::max(0.0, 1 - dockProgress_ * 2);
    if (contentOpacity == 0) return;
    p.setOpacity(contentOpacity);
    const auto rects = elementRects();
    for (auto it = rects.cbegin(); it != rects.cend(); ++it) {
        paintElement(p, it.key(), it.value());
        if (editing_) {
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(ink(config_["accent_color"].toString(), it.key() == hover_ ? .9 : .35), 1, Qt::DashLine));
            p.drawRoundedRect(it.value(), 4, 4);
        }
    }
}

void HudWindow::paintElement(QPainter& p, const QString& name, QRectF rect) {
    p.save();
    p.setClipRect(rect.adjusted(-1, -1, 1, 1), Qt::IntersectClip);
    QFont font(this->font());
    const int fontSize = font.pixelSize();
    p.setFont(font);
    p.setPen(ink(config_["text_color"].toString()));
    const auto secondary = config_["secondary_color"].toString();
    if (name == "cover") {
        paintCover(p, rect, oldCover_, 1);
        paintCover(p, rect, cover_, coverAlpha_);
    } else if (name == "title" || name == "artist" || name == "album" || name == "source") {
        QString text;
        if (name == "title") {
            text = snapshot_.title.isEmpty() ? (snapshot_.active ? QStringLiteral("Без названия") : QStringLiteral("Музыка рядом")) : snapshot_.title;
            font.setWeight(static_cast<QFont::Weight>(std::min(900, std::max(700, font.weight() + 100))));
            p.setOpacity(p.opacity() * titleAlpha_);
        } else {
            font.setPixelSize(std::max(8, fontSize - (name == "source" ? 3 : 1)));
            p.setPen(ink(secondary));
            if (name == "artist") text = snapshot_.artist.isEmpty() ? (snapshot_.active ? QStringLiteral("Неизвестный исполнитель") : QStringLiteral("Запустите любимый плеер")) : snapshot_.artist;
            if (name == "album") text = snapshot_.album;
            if (name == "source") {
                text = snapshot_.active ? snapshot_.source : "ISLAND";
                p.setPen(Qt::NoPen); p.setBrush(ink(config_["accent_color"].toString(), .85));
                p.drawEllipse(QRectF(rect.left(), rect.center().y() - 2.5, 5, 5));
                rect.adjust(10, 0, 0, 0); p.setPen(ink(secondary));
            }
        }
        p.setFont(font);
        p.drawText(rect, Qt::AlignVCenter | Qt::AlignLeft, QFontMetrics(font).elidedText(text, Qt::ElideRight, qRound(rect.width())));
    } else if (Transport.contains(name)) {
        if (name == "play") {
            p.setPen(Qt::NoPen); p.setBrush(ink(config_["accent_color"].toString(), .12 + .1 * playAlpha_));
            p.drawRoundedRect(rect, rect.height() / 2, rect.height() / 2);
        }
        if (name == hover_) {
            p.setPen(Qt::NoPen); p.setBrush(ink(config_["icon_color"].toString(), .12 * hoverAlpha_));
            p.drawRoundedRect(rect, rect.height() / 2, rect.height() / 2);
        }
        const bool enabled = snapshot_.active && (name == "play" ? snapshot_.canPlayPause : name == "next" ? snapshot_.canNext : snapshot_.canPrevious);
        icon(p, name, rect, ink(config_["icon_color"].toString(), enabled ? 1 : .3), config_["icon_size"].toDouble(18), snapshot_.playing);
    } else if (name == "progress") {
        const double thickness = config_["progress_height"].toDouble(3);
        QRectF bar(rect.x(), rect.center().y() - thickness / 2, rect.width(), thickness);
        p.setPen(Qt::NoPen); p.setBrush(ink(secondary, .22));
        p.drawRoundedRect(bar, thickness / 2, thickness / 2);
        const double fraction = snapshot_.duration > 0 ? displayedPosition() / snapshot_.duration : 0;
        bar.setWidth(bar.width() * std::clamp(fraction, 0.0, 1.0));
        p.setBrush(ink(config_["progress_color"].toString()));
        p.drawRoundedRect(bar, thickness / 2, thickness / 2);
        if ((name == hover_ || seekPreview_) && snapshot_.canSeek) p.drawEllipse(QPointF(bar.right(), bar.center().y()), 3, 3);
    } else if (name == "time") {
        font.setPixelSize(std::max(9, fontSize - 3)); p.setFont(font); p.setPen(ink(secondary));
        p.drawText(rect, Qt::AlignLeft | Qt::AlignVCenter, Layout::formatTime(displayedPosition()));
        p.drawText(rect, Qt::AlignRight | Qt::AlignVCenter, snapshot_.duration > 0 ? Layout::formatTime(snapshot_.duration) : "--:--");
    } else if (name == "volume") {
        if (!volumeAvailable_) p.setOpacity(p.opacity() * 0.35);
        icon(p, name, QRectF(rect.x(), rect.y(), 24, rect.height()), ink(config_["icon_color"].toString()), std::min(20, config_["icon_size"].toInt(18)));
        p.setPen(QPen(ink(secondary, .25), 3, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(rect.x() + 31, rect.center().y()), QPointF(rect.right() - 2, rect.center().y()));
        p.setPen(QPen(ink(config_["icon_color"].toString(), .8), 3, Qt::SolidLine, Qt::RoundCap));
        if (volumeAvailable_)
            p.drawLine(QPointF(rect.x() + 31, rect.center().y()), QPointF(rect.x() + 31 + (rect.width() - 33) * volume_, rect.center().y()));
    }
    p.restore();
}

void HudWindow::paintCover(QPainter& p, const QRectF& rect, const QPixmap& image, double opacity) {
    p.save(); p.setOpacity(p.opacity() * opacity);
    QPainterPath clip;
    const double radius = std::min(14.0, config_["radius"].toDouble(30) / 2);
    clip.addRoundedRect(rect, radius, radius); p.setClipPath(clip, Qt::IntersectClip);
    if (image.isNull()) {
        QLinearGradient gradient(rect.topLeft(), rect.bottomRight());
        gradient.setColorAt(0, ink(config_["accent_color"].toString(), .5));
        gradient.setColorAt(1, ink(config_["gradient_color"].toString()));
        p.fillRect(rect, gradient);
        icon(p, "note", rect, ink(config_["text_color"].toString(), .9), rect.width() * .45);
    } else {
        const double side = std::min(image.width(), image.height());
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        p.drawPixmap(rect, image, QRectF((image.width() - side) / 2, (image.height() - side) / 2, side, side));
    }
    p.restore();
}

QPointF HudWindow::localPoint(const QPointF& point) const {
    const double scale = config_["scale"].toDouble(1);
    const int inset = qCeil(SurfaceInset * scale);
    return (point - QPointF(inset, inset)) / scale;
}
QString HudWindow::hitTest(const QPointF& point) const {
    const auto rects = elementRects();
    for (const auto& name : {QString("volume"), QString("play"), QString("next"), QString("previous"), QString("progress")})
        if (rects.contains(name) && rects[name].contains(point)) return name;
    for (auto it = rects.cbegin(); it != rects.cend(); ++it)
        if (it.value().contains(point)) return it.key();
    return {};
}
void HudWindow::mousePressEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) return;
    cancelSeek();
    if (collapsed_ || dockProgress_ > 0) { reveal(); return; }
    hideTimer_.stop();
    const auto point = localPoint(e->position()); const auto hit = hitTest(point);
    moved_ = false;
    if (editing_ && !hit.isEmpty()) { dragElement_ = hit; dragOrigin_ = point - elementRects()[hit].topLeft(); }
    else if (hit == "volume" && !volumeAvailable_) dragElement_.clear();
    else if (hit == "progress") {
        dragElement_.clear();
        if (seekEnabled()) { dragElement_ = hit; previewSeekAt(point); }
    }
    else if (!Transport.contains(hit) && hit != "progress" && hit != "volume") dragWindow_ = e->globalPosition().toPoint() - pos();
    else dragElement_ = hit;
}
void HudWindow::mouseMoveEvent(QMouseEvent* e) {
    if (seekPreview_ && !e->buttons().testFlag(Qt::LeftButton)) cancelSeek();
    if (dockProgress_ > 0) return;
    const auto point = localPoint(e->position()); const auto hit = hitTest(point);
    if (hover_ != hit) {
        hover_ = hit;
        const QMap<QString, QString> tips{{"play", QStringLiteral("Воспроизведение / пауза")}, {"next", QStringLiteral("Следующий трек")},
            {"previous", QStringLiteral("Предыдущий трек")}, {"progress", QStringLiteral("Перемотка")},
            {"volume", volumeAvailable_ ? QStringLiteral("Громкость текущего приложения. Для сайтов - всего браузера.")
                                        : QStringLiteral("У этого источника пока нет доступной звуковой сессии.")}};
        setToolTip(tips.value(hit, QStringLiteral("Перетащите островок. Правая кнопка - меню.")));
        setCursor(editing_ ? Qt::SizeAllCursor : Transport.contains(hit) || hit == "progress" || (hit == "volume" && volumeAvailable_) ? Qt::PointingHandCursor : Qt::ArrowCursor);
        animate("hover", 0, 1, [this](double a) { hoverAlpha_ = a; update(); });
    }
    if (!dragElement_.isEmpty() && editing_ && dragOrigin_) {
        const auto target = point - *dragOrigin_; const auto rects = elementRects(); const auto rect = rects[dragElement_];
        auto positions = config_["element_positions"].toObject();
        if (config_["layout"].toString() != "custom")
            for (auto it = rects.cbegin(); it != rects.cend(); ++it) positions[it.key()] = QJsonArray{it.value().x(), it.value().y()};
        positions[dragElement_] = QJsonArray{std::clamp(target.x(), 0.0, config_["width"].toDouble() - rect.width()),
                                            std::clamp(target.y(), 0.0, config_["height"].toDouble() - rect.height())};
        config_["element_positions"] = positions; config_["layout"] = "custom"; moved_ = true; update();
    } else if (dragWindow_) {
        move(e->globalPosition().toPoint() - *dragWindow_);
        expandedGeometry_ = geometry();
        updateNativeRegion();
        moved_ = true;
    }
    else if (seekPreview_) previewSeekAt(point);
    else if (dragElement_ == "volume") volumeAt(point);
}
void HudWindow::mouseReleaseEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) return;
    const auto point = localPoint(e->position()); const auto hit = hitTest(point);
    if (editing_ && moved_ && dragOrigin_) emit configChanged(config_);
    else if (dragWindow_ && moved_) {
        auto* target = QGuiApplication::screenAt(frameGeometry().center()); if (!target) target = screen();
        const auto area = target->availableGeometry(); auto positions = config_["monitor_positions"].toObject();
        positions[target->name()] = QJsonArray{std::clamp(x() - area.x(), 0, std::max(0, area.width() - width())),
                                               std::clamp(y() - area.y(), 0, std::max(0, area.height() - height()))};
        config_["monitor_positions"] = positions; config_["monitor"] = target->name(); config_["anchor"] = "free";
        placeOnScreen(); emit configChanged(config_);
    } else if (seekPreview_) {
        previewSeekAt(point);
        const auto target = seekPreview_;
        cancelSeek();
        if (target) emit command("seek", *target);
    } else if (!editing_ && hit == dragElement_) {
        if (Transport.contains(hit) && snapshot_.active) {
            const bool enabled = hit == "play" ? snapshot_.canPlayPause : hit == "next" ? snapshot_.canNext : snapshot_.canPrevious;
            if (enabled) emit command(hit == "play" ? "play_pause" : hit, 0);
        } else if (hit == "volume") volumeAt(point);
    }
    dragElement_.clear(); dragOrigin_.reset(); dragWindow_.reset(); restartHideTimer();
}
bool HudWindow::seekEnabled() const {
    return !editing_ && !fadingOut_ && !collapsed_ && dockProgress_ == 0 && isVisible()
        && snapshot_.active && snapshot_.canSeek && std::isfinite(snapshot_.duration) && snapshot_.duration > 0
        && elementRects().value("progress").isValid();
}
void HudWindow::previewSeekAt(const QPointF& point) {
    if (!seekEnabled()) { cancelSeek(); return; }
    const auto rect = elementRects().value("progress");
    seekPreview_ = std::clamp((point.x() - rect.left()) / rect.width(), 0.0, 1.0) * snapshot_.duration;
    update();
}
void HudWindow::cancelSeek() {
    if (!seekPreview_) return;
    seekPreview_.reset();
    if (dragElement_ == "progress") dragElement_.clear();
    restartHideTimer();
    update();
}
double HudWindow::displayedPosition() const {
    return seekPreview_ ? *seekPreview_ : snapshot_.estimatedPosition();
}
void HudWindow::volumeAt(const QPointF& point) {
    if (!volumeAvailable_) return;
    const auto rect = elementRects().value("volume");
    if (rect.isValid()) emit volumeChanged(std::clamp((point.x() - rect.x() - 31) / (rect.width() - 33), 0.0, 1.0));
}
void HudWindow::wheelEvent(QWheelEvent* e) {
    if (collapsed_ || dockProgress_ > 0) return;
    if (!editing_ && volumeAvailable_ && hitTest(localPoint(e->position())) == "volume") {
        emit volumeChanged(std::clamp(volume_ + e->angleDelta().y() / 120.0 * .02, 0.0, 1.0)); e->accept();
    }
}
void HudWindow::contextMenuEvent(QContextMenuEvent* e) {
    cancelSeek();
    menuOpen_ = true;
    hideTimer_.stop();
    QMenu menu(this);
    menu.addAction(QStringLiteral("Настройки SCARP ISLAND"), this, &HudWindow::settingsRequested);
    auto* edit = menu.addAction(QStringLiteral("Редактировать расположение")); edit->setCheckable(true); edit->setChecked(editing_);
    connect(edit, &QAction::triggered, this, &HudWindow::editingChanged);
    menu.addAction(QStringLiteral("Скрыть островок"), this, [this] { conceal(true); });
    menu.exec(e->globalPos());
    menuOpen_ = false;
    restartHideTimer();
}

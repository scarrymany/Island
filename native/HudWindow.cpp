#include "HudWindow.h"
#include "AppAssets.h"
#include "HudIcons.h"
#include "IslandMenu.h"
#include "Layout.h"
#include "Squircle.h"
#include "WindowsIntegration.h"

#include <QApplication>
#include <QCloseEvent>
#include <QContextMenuEvent>
#include <QCursor>
#include <QDateTime>
#include <QJsonArray>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QScreen>
#include <QWheelEvent>
#include <QWindow>
#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <utility>

namespace {
constexpr int SurfaceInset = 14;
constexpr int ArtworkSize = 96;
constexpr int ArtworkBlurRadius = 10;
constexpr int ArtworkBlurPasses = 3;
constexpr double ArtworkMaxLuminance = 0.035;
constexpr int SeekConfirmationTimeoutMs = 3000;
constexpr double SeekConfirmationTolerance = 1.0;
constexpr int SentryIntervalMs = 90;
constexpr int InactiveGraceMs = 650;
constexpr double DockOvershoot = 0.05;
constexpr double UnionSlack = 150;
constexpr double CornerSmoothing = 0.6;
constexpr double PeekWidth = 7;
constexpr double PeekHeight = 3;
constexpr double MarqueeSpeed = 30;
constexpr double MarqueeGap = 36;
constexpr double MarqueeRest = 1.6;
constexpr double TextFade = 18;
const QStringList Transport{"previous", "play", "next"};

QImage blurredArtwork(const QImage& cover) {
    if (cover.isNull()) return {};
    QImage image(ArtworkSize, ArtworkSize, QImage::Format_RGB32);
    image.fill(Qt::black);
    {
        QPainter painter(&image);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.drawImage(image.rect(), cover);
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

// The most saturated hue family of the cover, used to tint progress and highlights.
QColor vibrantColor(const QImage& cover) {
    if (cover.isNull()) return {};
    const QImage small = cover.scaled(24, 24, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
        .convertToFormat(QImage::Format_RGB32);
    std::array<double, 12> weight{};
    std::array<std::array<double, 3>, 12> sum{};
    double total = 0;
    for (int y = 0; y < small.height(); ++y) {
        const auto* row = reinterpret_cast<const QRgb*>(small.constScanLine(y));
        for (int x = 0; x < small.width(); ++x) {
            const QColor color(row[x]);
            const double saturation = color.hsvSaturationF(), value = color.valueF();
            if (color.hsvHue() < 0 || saturation < 0.18 || value < 0.18) continue;
            const double w = saturation * saturation * value;
            const int bucket = std::clamp(color.hsvHue() / 30, 0, 11);
            weight[bucket] += w;
            sum[bucket][0] += color.redF() * w; sum[bucket][1] += color.greenF() * w; sum[bucket][2] += color.blueF() * w;
            total += w;
        }
    }
    const auto best = std::max_element(weight.begin(), weight.end());
    if (total < 8 || *best <= 0) return {};
    const int index = static_cast<int>(best - weight.begin());
    return QColor::fromRgbF(sum[index][0] / *best, sum[index][1] / *best, sum[index][2] / *best);
}

double luminance(const QColor& color) {
    return color.redF() * 0.2126 + color.greenF() * 0.7152 + color.blueF() * 0.0722;
}

// Makes an artwork hue legible on the current surface without losing its character.
QColor readableAccent(const QColor& raw, const QColor& surface) {
    float hue = 0, saturation = 0, lightness = 0, alpha = 1;
    raw.getHslF(&hue, &saturation, &lightness, &alpha);
    const bool lightSurface = luminance(surface) > 0.5;
    saturation = std::clamp(saturation, 0.42f, 0.86f);
    lightness = lightSurface ? std::clamp(lightness, 0.30f, 0.42f) : std::clamp(lightness, 0.64f, 0.78f);
    return QColor::fromHslF(std::max(0.0f, hue), saturation, lightness);
}

QColor ink(const QString& value, double alpha = 1) {
    QColor c(value);
    c.setAlphaF(std::clamp(alpha, 0.0, 1.0));
    return c;
}

QColor withAlpha(QColor color, double alpha) {
    color.setAlphaF(std::clamp(color.alphaF() * alpha, 0.0, 1.0));
    return color;
}

QColor mixColor(const QColor& from, const QColor& to, double progress) {
    progress = std::clamp(progress, 0.0, 1.0);
    return QColor::fromRgbF(from.redF() + (to.redF() - from.redF()) * progress,
                            from.greenF() + (to.greenF() - from.greenF()) * progress,
                            from.blueF() + (to.blueF() - from.blueF()) * progress,
                            from.alphaF() + (to.alphaF() - from.alphaF()) * progress);
}

double lerp(double from, double to, double progress) { return from + (to - from) * progress; }

double smoothstep(double edge0, double edge1, double value) {
    const double t = std::clamp((value - edge0) / (edge1 - edge0), 0.0, 1.0);
    return t * t * (3 - 2 * t);
}

// Even deceleration: quick to respond without throwing most of the motion into the
// first frame, so short transitions never read as a jump.
QEasingCurve emphasized() {
    QEasingCurve curve(QEasingCurve::BezierSpline);
    curve.addCubicBezierSegment(QPointF(0.25, 0.8), QPointF(0.3, 1.0), QPointF(1, 1));
    return curve;
}

QEasingCurve standard() {
    QEasingCurve curve(QEasingCurve::BezierSpline);
    curve.addCubicBezierSegment(QPointF(0.2, 0.0), QPointF(0.0, 1.0), QPointF(1, 1));
    return curve;
}

// Draws one line of text; overflow fades out instead of an ellipsis, and a positive
// offset scrolls the line with a seamless repeat for the marquee.
void drawLine(QPainter& p, const QRectF& rect, const QString& text, const QFont& font, const QColor& color,
              double offset = 0, bool fade = true) {
    const QFontMetricsF metrics(font);
    const double width = metrics.horizontalAdvance(text);
    p.setFont(font);
    p.setPen(color);
    if (width <= rect.width() + 0.5) {
        p.drawText(rect, Qt::AlignVCenter | Qt::AlignLeft, text);
        return;
    }
    if (!fade) {
        p.drawText(rect, Qt::AlignVCenter | Qt::AlignLeft, metrics.elidedText(text, Qt::ElideRight, rect.width()));
        return;
    }
    const QTransform world = p.worldTransform();
    const double ratio = std::max(0.5, std::hypot(world.m11(), world.m12()) * p.device()->devicePixelRatioF());
    QImage layer(QSize(qCeil(rect.width() * ratio) + 2, qCeil(rect.height() * ratio) + 2),
                 QImage::Format_ARGB32_Premultiplied);
    layer.setDevicePixelRatio(ratio);
    layer.fill(Qt::transparent);
    {
        QPainter painter(&layer);
        painter.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing);
        painter.setFont(font);
        painter.setPen(color);
        const QRectF line(-offset, 0, width + 2, rect.height());
        painter.drawText(line, Qt::AlignVCenter | Qt::AlignLeft, text);
        if (offset > 0) painter.drawText(line.translated(width + MarqueeGap, 0), Qt::AlignVCenter | Qt::AlignLeft, text);
        painter.setCompositionMode(QPainter::CompositionMode_DestinationIn);
        QLinearGradient mask(0, 0, rect.width(), 0);
        const double edge = std::min(0.45, TextFade / std::max(1.0, rect.width()));
        const double leading = offset > 0 ? std::min(edge, offset / std::max(1.0, rect.width())) : 0;
        mask.setColorAt(0, QColor(0, 0, 0, leading > 0 ? 0 : 255));
        mask.setColorAt(std::max(0.0, leading), Qt::black);
        mask.setColorAt(1 - edge, Qt::black);
        mask.setColorAt(1, Qt::transparent);
        painter.fillRect(QRectF(0, 0, rect.width() + 2, rect.height() + 2), mask);
    }
    p.drawImage(rect.topLeft(), layer);
}
}

HudWindow::HudWindow(const QJsonObject& config)
    : QWidget(nullptr, Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool | Qt::WindowDoesNotAcceptFocus),
      config_(config) {
    AppAssets::settingsFontFamily();
    setWindowTitle("SCARP ISLAND");
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
    // The overlay never becomes the active window; its hints must still appear.
    setAttribute(Qt::WA_AlwaysShowToolTips);
    setMouseTracking(true);
    setAccessibleName(QStringLiteral("SCARP ISLAND - музыкальный оверлей"));
    clock_.start();
    artworkPool_.setMaxThreadCount(1);
    progressTimer_.setSingleShot(true);
    progressTimer_.setTimerType(Qt::PreciseTimer);
    connect(&progressTimer_, &QChronoTimer::timeout, this, [this] {
        updateElement("progress"); updateElement("time"); syncFrameTimer();
    });
    ticker_.setTimerType(Qt::PreciseTimer);
    connect(&ticker_, &QChronoTimer::timeout, this, &HudWindow::tick);
    hideTimer_.setSingleShot(true);
    seekTimer_.setSingleShot(true);
    seekTimer_.setTimerType(Qt::PreciseTimer);
    connect(&seekTimer_, &QTimer::timeout, this, [this] { pendingSeek_.reset(); update(); });
    connect(&hideTimer_, &QTimer::timeout, this, [this] {
        if (editing_ || menuOpen_ || dragWindow_ || !dragElement_.isEmpty()) return;
        // Ask where the pointer really is: Qt's enter/leave state goes stale after
        // popups, drags and windows appearing under a resting cursor.
        if (pointerKeepsOpen()) { restartHideTimer(); return; }
        hoverFromDock_ = false;
        if (config_["idle_collapse"].toBool(true)) setCollapsed(true);
        else conceal();
    });
    screenPlacementTimer_.setSingleShot(true);
    connect(&screenPlacementTimer_, &QTimer::timeout, this, &HudWindow::placeOnScreen);
    sentryTimer_.setInterval(SentryIntervalMs);
    connect(&sentryTimer_, &QTimer::timeout, this, &HudWindow::sampleSentry);
    intentTimer_.setSingleShot(true);
    connect(&intentTimer_, &QTimer::timeout, this, &HudWindow::finishRevealIntent);
    inactiveTimer_.setSingleShot(true);
    connect(&inactiveTimer_, &QTimer::timeout, this, [this] { present(snapshot_); syncFrameTimer(); update(); });
    connect(qApp, &QGuiApplication::screenAdded, this, [this](QScreen* s) { watchScreen(s); scheduleScreenPlacement(); });
    connect(qApp, &QGuiApplication::screenRemoved, this, &HudWindow::scheduleScreenPlacement);
    connect(qApp, &QGuiApplication::primaryScreenChanged, this, &HudWindow::scheduleScreenPlacement);
    for (auto* screen : QGuiApplication::screens()) watchScreen(screen);
    applyConfig(config);
}

HudWindow::~HudWindow() {
    progressTimer_.stop();
    ticker_.stop();
    hideTimer_.stop();
    seekTimer_.stop();
    sentryTimer_.stop();
    intentTimer_.stop();
    inactiveTimer_.stop();
    screenPlacementTimer_.stop();
    animations_.stopAll();
    artworkPool_.clear();
    artworkPool_.waitForDone();
    if (QGuiApplication::platformName() == "windows") WindowsIntegration::releaseOverlayBackdrop(winId());
}

void HudWindow::watchScreen(QScreen* screen) {
    connect(screen, &QScreen::availableGeometryChanged, this, &HudWindow::scheduleScreenPlacement);
    connect(screen, &QScreen::geometryChanged, this, &HudWindow::scheduleScreenPlacement);
    connect(screen, &QScreen::logicalDotsPerInchChanged, this, &HudWindow::scheduleScreenPlacement);
    connect(screen, &QScreen::refreshRateChanged, this, [this] { syncRefreshRate(); });
}

void HudWindow::scheduleScreenPlacement() {
    // Screen removal and DPI changes arrive in bursts, before Qt has finished changing screens.
    // Reconcile once with the final screen list, without showing an intentionally hidden HUD.
    screenPlacementTimer_.start(0);
}

bool HudWindow::event(QEvent* event) {
    // Focus loss or a modal window ends an unfinished gesture, but cannot undo a
    // command already sent to the player. Keep its confirmation until reply/timeout.
    if (event->type() == QEvent::WindowBlocked || event->type() == QEvent::WindowDeactivate) cancelScrub();
    const bool handled = QWidget::event(event);
    // A drag moves the window every frame; screen changes arrive through screenChanged.
    if ((event->type() == QEvent::Move && !dragWindow_) || event->type() == QEvent::DevicePixelRatioChange) syncRefreshRate();
    if (event->type() == QEvent::DevicePixelRatioChange) updateNativeRegion();
    return handled;
}

void HudWindow::syncRefreshRate() {
    const auto* target = isVisible() && windowHandle() ? windowHandle()->screen() : targetScreen();
    animations_.setRefreshRate(target ? target->refreshRate() : 0);
    syncFrameTimer();
    syncTicker();
}

QMap<QString, QRectF> HudWindow::elementRects() const {
    // Animations query the layout several times per frame; it only changes with config_.
    if (!rects_) rects_ = Layout::elements(config_);
    return *rects_;
}

void HudWindow::invalidateLayout() {
    rects_.reset();
    titleWidth_ = -1;
}

double HudWindow::titleWidth() const {
    if (titleWidth_ < 0) {
        QFont font(this->font());
        font.setWeight(static_cast<QFont::Weight>(std::min(900, std::max(700, font.weight() + 100))));
        titleWidth_ = QFontMetricsF(font).horizontalAdvance(displayText(presented_, "title"));
    }
    return titleWidth_;
}

QScreen* HudWindow::targetScreen() const {
    auto* target = QGuiApplication::primaryScreen();
    for (auto* screen : QGuiApplication::screens())
        if (screen->name() == config_["monitor"].toString()) target = screen;
    return target;
}

QPointF HudWindow::cardOrigin() const { return expandedCard_.topLeft() - QPointF(pos()); }

QRect HudWindow::captureRect() const {
    const int inset = qCeil(SurfaceInset * config_["scale"].toDouble(1));
    return card_.translated(-QPointF(pos())).toAlignedRect().adjusted(-inset, -inset, inset, inset).intersected(rect());
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
    expandedCard_ = QRectF(expandedGeometry_.adjusted(inset, inset, -inset, -inset));
    compactCard_ = QRectF(compactGeometry_.adjusted(inset, inset, -inset, -inset));
    frameSize_ = expandedSize.expandedTo(compactSize);
    // When the dock and the card are close (the default top-centre placement), one
    // stable window covers both and the morph only repaints: no per-frame window moves.
    unionFrame_ = expandedGeometry_.united(compactGeometry_);
    const double slack = UnionSlack * scale;
    frameMode_ = config_["idle_collapse"].toBool(true) && unionFrame_.width() <= frameSize_.width() + slack
        && unionFrame_.height() <= frameSize_.height() + slack ? FrameMode::Union : FrameMode::Moving;
    stopAnimation("dock");
    applyDockGeometry(collapsed_ ? 1 : 0);
    if (isVisible()) applyNative();
    syncRefreshRate();
    restartHideTimer();
}

void HudWindow::resetPosition() {
    auto* target = targetScreen();
    if (!target) return;
    auto restored = config_;
    auto positions = restored["monitor_positions"].toObject();
    positions.remove(target->name());
    restored["monitor_positions"] = positions;
    restored["monitor"] = target->name();
    restored["anchor"] = "top_center";
    restored["offset_y"] = 12;
    applyConfig(restored);
    reveal(true);
    emit configChanged(config_);
}

void HudWindow::applyConfig(const QJsonObject& config) {
    cancelSeek();
    dragElement_.clear(); dragOrigin_.reset(); dragWindow_.reset(); pendingDrag_.reset(); stopAnimation("drag");
    setHover({}); setToolTip({}); setCursor(Qt::ArrowCursor);
    config_ = config;
    invalidateLayout();
    QFont font(config_["font_family"].toString("Inter"));
    font.setPixelSize(config_["font_size"].toInt(14));
    font.setWeight(static_cast<QFont::Weight>(std::clamp(config_["font_weight"].toInt(600), 100, 900)));
    font.setHintingPreference(QFont::PreferNoHinting);
    setFont(font);
    titleWidth_ = -1;
    if (!config_["idle_collapse"].toBool(true) || editing_) collapsed_ = false;
    if (!collapsed_) cancelRevealIntent();
    placeOnScreen();
    // Turning full auto-hide off must recover an already auto-hidden (or fading) card.
    // Manual hiding stays authoritative across settings, profiles and display changes.
    if (autoHidden_ && !manualHidden_
        && (config_["idle_collapse"].toBool(true) || config_["auto_hide_seconds"].toInt() == 0)) reveal();
    syncFrameTimer(); syncTicker(); restartHideTimer(); update();
}

QRectF HudWindow::cardAt(double progress) const {
    // Opening may overshoot slightly; docking never pushes the strip past its resting edge.
    const double t = std::clamp(progress, -DockOvershoot, 1.0);
    const QRectF& from = expandedCard_;
    const QRectF& to = compactCard_;
    QRectF card(lerp(from.left(), to.left(), t), lerp(from.top(), to.top(), t),
                lerp(from.width(), to.width(), t), lerp(from.height(), to.height(), t));
    const double peek = peek_ * smoothstep(0.6, 1.0, t);
    if (peek > 0) {
        const double scale = config_["scale"].toDouble(1);
        card.adjust(-PeekWidth * scale * peek, 0, PeekWidth * scale * peek, PeekHeight * scale * peek);
    }
    return card;
}

QRect HudWindow::frameFor(const QRectF& card) const {
    if (frameMode_ == FrameMode::Union) return unionFrame_;
    const int inset = qCeil(SurfaceInset * config_["scale"].toDouble(1));
    return QRect(QPoint(qRound(card.center().x() - frameSize_.width() / 2.0), qRound(card.top()) - inset), frameSize_);
}

void HudWindow::updateCard() {
    card_ = cardAt(dockProgress_);
    // The frame follows the card only in moving mode; its size never changes mid-morph,
    // so the layered surface is never reallocated while it animates.
    const QRect frame = dockProgress_ <= 0 && frameMode_ == FrameMode::Moving
        ? QRect(QPoint(qRound(expandedCard_.center().x() - frameSize_.width() / 2.0),
                       qRound(expandedCard_.top()) - qCeil(SurfaceInset * config_["scale"].toDouble(1))), frameSize_)
        : frameFor(card_);
    if (geometry() != frame) setGeometry(frame);
}

double HudWindow::cardRadius() const {
    const double scale = config_["scale"].toDouble(1);
    const double t = std::clamp(dockProgress_, 0.0, 1.0);
    const double radius = lerp(config_["radius"].toDouble(30), config_["compact_radius"].toDouble(16), t) * scale;
    return std::min({radius, card_.width() / 2, card_.height() / 2});
}

void HudWindow::applyDockGeometry(double progress) {
    dockProgress_ = progress;
    updateCard();
    updateNativeRegion();
    syncFrameTimer();
    syncTicker();
    update();
}

void HudWindow::setCollapsed(bool collapsed) {
    if (collapsed && (editing_ || menuOpen_ || manualHidden_ || !isVisible()
        || !config_["idle_collapse"].toBool(true))) return;
    const double target = collapsed ? 1.0 : 0.0;
    if (collapsed_ == collapsed && dockProgress_ == target && !animations_.isRunning("dock")) return;
    cancelSeek();
    cancelRevealIntent();
    collapsed_ = collapsed;
    if (collapsed) { hoverFromDock_ = false; setHover({}); pressed_.clear(); }
    setToolTip({});
    hideTimer_.stop();
    const auto settle = [this] {
        if (!collapsed_) peek_ = 0;
        updateNativeRegion(); applyHitTesting(); restartHideTimer(); syncTicker();
    };
    if (!motionEnabled("dock")) {
        stopAnimation("dock");
        if (!collapsed) peek_ = 0;
        applyDockGeometry(target);
        settle();
    } else {
        const double velocity = animations_.velocity("dock");
        const double response = std::max(0.12, duration().count() / 1000.0 * 1.3);
        // Opening blooms with a soft overshoot; closing settles without bouncing past the edge.
        const AnimationClock::Spring spring{collapsed ? response * 0.85 : response, collapsed ? 0.92 : 0.8};
        syncRefreshRate();
        animations_.spring("dock", dockProgress_, target, spring, [this](double value) { applyDockGeometry(value); },
                           settle, velocity);
    }
    if (isVisible()) applyNative();
}

bool HudWindow::pointerInDockArea() const {
    if (!hoverFromDock_ || !config_["idle_collapse"].toBool(true)) return false;
    const auto* target = targetScreen();
    if (!target) return false;
    const QRectF card = expandedCard_;
    const QRectF handle = compactCard_.intersected(QRectF(target->geometry()));
    const QPointF cursor = QCursor::pos();
    if (card.contains(cursor) || handle.contains(cursor)) return true;
    // Keep the path from the edge handle to the opening card reachable during its transition.
    QPainterPath corridor;
    corridor.moveTo(handle.bottomLeft()); corridor.lineTo(handle.bottomRight());
    corridor.lineTo(card.topRight()); corridor.lineTo(card.topLeft()); corridor.closeSubpath();
    return corridor.contains(cursor);
}

bool HudWindow::pointerKeepsOpen() const {
    if (!isVisible() || fadingOut_ || pointerHidden_) return false;
    if (pointerInDockArea()) return true;
    if (config_["click_through"].toBool()) return false;
    return card_.adjusted(-2, -2, 2, 2).contains(QPointF(QCursor::pos()));
}

QRectF HudWindow::dockHandle() const {
    const auto* target = targetScreen();
    const QRectF handle = card_.adjusted(-2, 0, 2, 0);
    return target ? handle.intersected(QRectF(target->geometry())) : handle;
}

void HudWindow::sampleSentry() {
    if (!isVisible() || QGuiApplication::platformName() != "windows") return;
    const QPointF cursor = QCursor::pos();
    // Over our own card Windows reports our cursor; sample the app underneath instead.
    if (passThrough_ || !card_.adjusted(-1, -1, 1, 1).contains(cursor))
        cursorShownOutside_ = WindowsIntegration::cursorVisible();
    const bool fullscreen = WindowsIntegration::foregroundIsFullscreen(winId());
    const bool hidden = fullscreen && !cursorShownOutside_;
    if (hidden == pointerHidden_ && fullscreen == fullscreenForeground_) return;
    pointerHidden_ = hidden;
    fullscreenForeground_ = fullscreen;
    applyHitTesting();
    if (!revealGestureAllowed()) cancelRevealIntent();
}

bool HudWindow::revealGestureAllowed() const {
    if (QGuiApplication::platformName() != "windows") return true;
    if (pointerHidden_) return false;
    if (fullscreenForeground_ && !config_["dock_hover_fullscreen"].toBool(false)) return false;
    // A held button means a drag (window snapping, selection, shooting), not a hover.
    return !WindowsIntegration::mouseButtonsDown();
}

void HudWindow::beginRevealIntent(bool fromEnter) {
    if (manualHidden_ || !collapsed_ || !isVisible()) return;
    if (!revealGestureAllowed()) return;
    intentFromEnter_ = fromEnter;
    setPeek(true);
    const int delay = config_["dock_hover_delay"].toInt(160);
    if (delay <= 0) { finishRevealIntent(); return; }
    if (!intentTimer_.isActive()) intentTimer_.start(delay);
}

void HudWindow::finishRevealIntent() {
    intentTimer_.stop();
    if (!collapsed_ || manualHidden_ || !isVisible()) return;
    if (!revealGestureAllowed()) { cancelRevealIntent(); return; }
    hoverFromDock_ = true;
    reveal();
}

void HudWindow::cancelRevealIntent() {
    intentTimer_.stop();
    intentFromEnter_ = false;
    setPeek(false);
}

void HudWindow::setPeek(bool engaged) {
    const double target = engaged && collapsed_ ? 1.0 : 0.0;
    if (peek_ == target && !animations_.isRunning("peek")) return;
    if (!motionEnabled("dock") || !isVisible()) {
        stopAnimation("peek");
        peek_ = target;
        updateCard(); updateNativeRegion(); update();
        return;
    }
    if (animations_.retarget("peek", target)) return;
    animations_.spring("peek", peek_, target, {0.26, 0.72}, [this](double value) {
        peek_ = value; updateCard(); updateNativeRegion(); update();
    });
}

void HudWindow::applyNative() {
    if (QGuiApplication::platformName() != "windows" || !isVisible()) return;
    const QRectF card = card_.translated(-QPointF(pos()));
    const auto* target = targetScreen();
    const QRectF bounds = target && dockProgress_ > 0 ? QRectF(target->geometry()).translated(-QPointF(pos())) : QRectF{};
    const bool moving = animations_.isRunning("dock") || animations_.isRunning("peek");
    WindowsIntegration::applyOverlayBackdrop(winId(), config_["blur"].toBool(true), card, cardRadius(), devicePixelRatioF(),
        config_[collapsed_ ? "compact_background" : "background"].toString("#10121B"),
        config_[collapsed_ ? "compact_opacity" : "opacity"].toDouble(.94), bounds, moving ? QRectF(rect()) : QRectF{});
    applyHitTesting();
    WindowsIntegration::ensureTopmost(winId());
    WindowsIntegration::setOverlayOpacity(winId(), windowOpacity());
}

void HudWindow::applyHitTesting() {
    if (QGuiApplication::platformName() != "windows") return;
    // A hidden cursor over a fullscreen app must never be intercepted: the app keeps
    // its input and the strip cannot flash an arrow cursor or swallow a click.
    const bool hiddenPointer = pointerHidden_ && !editing_;
    const bool fullscreenDock = collapsed_ && fullscreenForeground_ && !editing_
        && !config_["dock_hover_fullscreen"].toBool(false);
    const bool configured = config_["click_through"].toBool() && !editing_ && !collapsed_;
    passThrough_ = hiddenPointer || fullscreenDock || configured;
    WindowsIntegration::setClickThrough(winId(), passThrough_);
}

void HudWindow::setHudOpacity(double opacity) {
    setWindowOpacity(opacity);
    if (QGuiApplication::platformName() == "windows") WindowsIntegration::setOverlayOpacity(winId(), opacity);
}

void HudWindow::updateNativeRegion() {
    if (!isVisible() || QGuiApplication::platformName() != "windows") return;
    const QRectF card = card_.translated(-QPointF(pos()));
    const auto* target = targetScreen();
    const QRectF bounds = target && dockProgress_ > 0 ? QRectF(target->geometry()).translated(-QPointF(pos())) : QRectF{};
    // While the card moves the whole frame stays hittable, so a shrinking card never
    // clips the previous frame's antialiased edge; at rest only the card is hit tested.
    const bool moving = animations_.isRunning("dock") || animations_.isRunning("peek");
    WindowsIntegration::updateOverlayRegion(winId(), card, cardRadius(), devicePixelRatioF(), bounds,
                                            moving ? QRectF(rect()) : QRectF{});
}

void HudWindow::showEvent(QShowEvent* e) {
    QWidget::showEvent(e);
    // A native hide/minimize can interrupt a fade. A subsequent show must not keep
    // its last transparent frame once that animation has been cancelled.
    if (!animations_.isRunning("appear") && !fadingOut_) { setHudOpacity(1); appear_ = 1; }
    if (!animations_.isRunning("dock")) applyDockGeometry(collapsed_ ? 1 : 0);
    if (windowHandle())
        connect(windowHandle(), &QWindow::screenChanged, this, &HudWindow::syncRefreshRate, Qt::UniqueConnection);
    if (QGuiApplication::platformName() == "windows") { sentryTimer_.start(); sampleSentry(); }
    syncRefreshRate(); applyNative(); syncFrameTimer(); restartHideTimer();
}

void HudWindow::hideEvent(QHideEvent* e) {
    cancelSeek();
    progressTimer_.stop(); ticker_.stop(); hideTimer_.stop(); sentryTimer_.stop(); intentTimer_.stop();
    animations_.stopAll();
    fadingOut_ = false;
    coverAlpha_ = textAlpha_ = appear_ = 1;
    progressExpand_ = volumeExpand_ = peek_ = 0;
    playMorph_ = presented_.active && presented_.playing ? 1 : 0;
    equalizer_ = playMorph_;
    shownVolume_ = volume_;
    hoverLevels_.clear(); pressLevels_.clear(); pressed_.clear();
    marqueeOffset_ = 0; marqueePause_ = MarqueeRest; marqueeBudget_ = 0;
    displayedArtwork_ = artwork_;
    dragElement_.clear(); dragOrigin_.reset(); dragWindow_.reset(); pendingDrag_.reset();
    hover_.clear(); hoverFromDock_ = false; cardHovered_ = false;
    QWidget::hideEvent(e);
}

void HudWindow::closeEvent(QCloseEvent* e) { e->ignore(); conceal(true); }

void HudWindow::enterEvent(QEnterEvent* e) {
    if (!manualHidden_) {
        if (collapsed_ && dockProgress_ < 1 && animations_.isRunning("dock")) {
            // Catching the card while it leaves reverses the motion immediately.
            if (revealGestureAllowed()) { hoverFromDock_ = true; reveal(); }
        } else if (collapsed_) {
            beginRevealIntent(true);
        } else if (dockProgress_ > 0 || fadingOut_) {
            hoverFromDock_ = hoverFromDock_ || dockProgress_ > 0;
            reveal();
        }
        if (!collapsed_) cardHovered_ = true;
        hideTimer_.stop();
        syncTicker();
    }
    QWidget::enterEvent(e);
}

void HudWindow::leaveEvent(QEvent* e) {
    if (intentFromEnter_) cancelRevealIntent();
    cardHovered_ = false;
    setHover({});
    restartHideTimer();
    syncTicker();
    QWidget::leaveEvent(e);
}

void HudWindow::stopAnimation(const QString& name) {
    animations_.stop(name);
}

bool HudWindow::motionEnabled(const QString& name) const {
    return config_["animations"].toObject()[name].toBool(true) && config_["animation_duration"].toInt(260) > 0;
}

std::chrono::milliseconds HudWindow::duration(double factor) const {
    return std::chrono::milliseconds(qRound(config_["animation_duration"].toInt(260) * factor));
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
    const double factor = name == "title" ? 1.35 : name == "cover" ? 1.6 : 1.0;
    animations_.start(name, from, to, duration(factor), name == "disappear" ? standard() : emphasized(),
                      std::move(callback), std::move(finished));
}

void HudWindow::reveal(bool manual) {
    if (manual) manualHidden_ = false;
    if (manualHidden_) return;
    const bool visible = isVisible();
    autoHidden_ = false;
    fadingOut_ = false;
    stopAnimation("disappear");
    intentTimer_.stop();
    if (visible) setCollapsed(false);
    else {
        stopAnimation("dock");
        collapsed_ = false;
        hoverFromDock_ = false;
        peek_ = 0;
        applyDockGeometry(0);
    }
    // Start before showEvent so it can distinguish a requested appearance from a
    // stale opacity left by an interrupted hide. Repeated reveals keep their deadline.
    if (!visible || (windowOpacity() < 1 && !animations_.isRunning("appear"))) {
        const double from = visible ? windowOpacity() : 0;
        animate("appear", from, 1, [this](double a) {
            appear_ = a; setHudOpacity(a); update();
        }, [this] { restartHideTimer(); });
    }
    show();
    applyNative();
    restartHideTimer();
}

void HudWindow::conceal(bool manual) {
    cancelSeek();
    if (manual) manualHidden_ = true;
    autoHidden_ = !manualHidden_;
    hideTimer_.stop(); stopAnimation("appear"); stopAnimation("dock"); cancelRevealIntent();
    if (isVisible() && !fadingOut_) {
        fadingOut_ = true;
        animate("disappear", windowOpacity(), 0, [this](double a) { appear_ = a; setHudOpacity(a); update(); },
            [this] { if (fadingOut_) hide(); });
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
    const auto rects = elementRects();
    const bool timeline = rects.contains("progress") || rects.contains("time");
    if (!isVisible() || !presented_.active || !presented_.playing || dockProgress_ >= 1 || seekPreview_ || !timeline) {
        progressTimer_.stop();
        return;
    }
    // Repaint when the time label changes or the bar has moved a third of a device
    // pixel, instead of redrawing the whole overlay on every display refresh.
    const double rate = std::isfinite(presented_.playbackRate) && presented_.playbackRate > 0 ? presented_.playbackRate : 1;
    const double position = displayedPosition();
    double interval = (std::floor(position) + 1 - position) / rate;
    if (rects.contains("progress") && presented_.duration > 0 && config_["animations"].toObject()["progress"].toBool(true)) {
        const double pixels = rects["progress"].width() * config_["scale"].toDouble(1) * devicePixelRatioF() * 3;
        interval = std::min(interval, presented_.duration / std::max(1.0, pixels) / rate);
    }
    const double frame = animations_.frameInterval().count() / 1e9;
    interval = std::clamp(interval, frame, 1.0);
    const auto next = std::chrono::nanoseconds(std::llround(interval * 1e9)) + std::chrono::milliseconds(1);
    // Frequent resyncs (window moves, hover) must not keep postponing the next repaint.
    if (progressTimer_.isActive() && progressTimer_.remainingTime() <= next) return;
    progressTimer_.setInterval(next);
    progressTimer_.start();
}

bool HudWindow::marqueeWanted() const {
    if (!motionEnabled("marquee")) return false;
    const auto rects = elementRects();
    if (!rects.contains("title")) return false;
    if (titleWidth() <= rects["title"].width() + 0.5) return false;
    return marqueeOffset_ > 0 || cardHovered_ || marqueeBudget_ > 0;
}

void HudWindow::syncTicker() {
    const bool contentVisible = isVisible() && dockProgress_ < 0.5 && !fadingOut_;
    const bool equalizer = motionEnabled("equalizer") && elementRects().contains("source") && equalizer_ > 0.001;
    const bool marquee = marqueeWanted();
    if (contentVisible && (equalizer || marquee)) {
        // Scrolling text follows the display; the organic equalizer is calm at 60 fps.
        const auto interval = marquee ? animations_.frameInterval()
            : std::max<std::chrono::nanoseconds>(animations_.frameInterval(), std::chrono::nanoseconds(16'666'667));
        if (ticker_.interval() != interval) ticker_.setInterval(interval);
        if (!ticker_.isActive()) { tickerLast_ = clock_.nsecsElapsed(); ticker_.start(); }
    } else {
        ticker_.stop();
    }
}

void HudWindow::tick() {
    const qint64 now = clock_.nsecsElapsed();
    const double dt = std::clamp((now - tickerLast_) / 1e9, 0.0, 0.1);
    tickerLast_ = now;
    equalizerPhase_ += dt;
    if (marqueeWanted() && textAlpha_ >= 1) {
        if (marqueePause_ > 0) marqueePause_ -= dt;
        else {
            marqueeOffset_ += MarqueeSpeed * dt;
            if (marqueeOffset_ >= titleWidth() + MarqueeGap) {
                marqueeOffset_ = 0;
                marqueePause_ = MarqueeRest;
                if (!cardHovered_ && marqueeBudget_ > 0) --marqueeBudget_;
            }
        }
        updateElement("title");
    }
    updateElement("source");
    syncTicker();
}

void HudWindow::setEditing(bool enabled) {
    if (editing_ != enabled) cancelSeek();
    if (editing_ != enabled) { dragElement_.clear(); dragOrigin_.reset(); dragWindow_.reset(); }
    editing_ = enabled;
    if (enabled) reveal(true);
    applyNative(); restartHideTimer(); updateExpansion(); update();
}

void HudWindow::setVolume(double value) {
    volume_ = std::clamp(value, 0.0, 1.0);
    if (!motionEnabled("hover") || !isVisible() || dockProgress_ >= 1) {
        stopAnimation("volume");
        shownVolume_ = volume_;
        updateElement("volume");
        return;
    }
    if (animations_.retarget("volume", volume_)) return;
    animations_.spring("volume", shownVolume_, volume_, {0.24, 0.92}, [this](double v) {
        shownVolume_ = v; updateElement("volume");
    });
}

void HudWindow::setVolumeAvailable(bool available) {
    if (volumeAvailable_ == available) return;
    volumeAvailable_ = available;
    if (!available && !editing_ && dragElement_ == "volume") dragElement_.clear();
    if (hover_ == "volume") {
        setHover({});
        setToolTip({});
        setCursor(Qt::ArrowCursor);
    }
    updateExpansion();
    update();
}

void HudWindow::setSnapshot(const MediaSnapshot& snapshot) {
    const bool changed = snapshot.sourceId != snapshot_.sourceId || snapshot.title != snapshot_.title
        || snapshot.artist != snapshot_.artist || snapshot.album != snapshot_.album;
    const bool activity = snapshot.active && (changed || !snapshot_.active || (snapshot.playing && !snapshot_.playing));
    if (changed || !snapshot.active || !snapshot.canSeek || !std::isfinite(snapshot.duration)
        || snapshot.duration <= 0 || snapshot.duration != snapshot_.duration) cancelSeek();
    if (pendingSeek_) {
        const double expected = pendingSeek_->estimatedPosition();
        if (std::abs(snapshot.estimatedPosition() - expected) <= SeekConfirmationTolerance) {
            pendingSeek_.reset();
            seekTimer_.stop();
        } else {
            pendingSeek_->position = expected;
            pendingSeek_->updatedAt = QDateTime::currentMSecsSinceEpoch();
            pendingSeek_->playing = snapshot.playing;
            pendingSeek_->playbackRate = snapshot.playbackRate;
        }
    }
    snapshot_ = snapshot;
    // Players briefly drop their session while switching tracks or sources. Hold the
    // last picture for a moment so that gap never flashes the empty placeholder.
    if (!snapshot.active && presented_.active) {
        if (!inactiveTimer_.isActive()) inactiveTimer_.start(InactiveGraceMs);
    } else {
        inactiveTimer_.stop();
        present(snapshot);
    }
    // Activity also wins while the old idle fade is still visible. An unchanged
    // paused snapshot must not reopen the HUD on every media poll, nor expand a dock.
    if (activity && (!isVisible() || fadingOut_)) reveal();
    syncFrameTimer(); update();
}

QString HudWindow::displayText(const MediaSnapshot& snapshot, const QString& name) const {
    if (name == "title")
        return snapshot.title.isEmpty() ? (snapshot.active ? QStringLiteral("Без названия") : QStringLiteral("Музыка рядом")) : snapshot.title;
    if (name == "artist")
        return snapshot.artist.isEmpty() ? (snapshot.active ? QStringLiteral("Неизвестный исполнитель") : QStringLiteral("Запустите любимый плеер")) : snapshot.artist;
    if (name == "album") return snapshot.album;
    if (name == "source") return snapshot.active ? snapshot.source : QStringLiteral("ISLAND");
    return {};
}

void HudWindow::present(const MediaSnapshot& next) {
    bool textChanged = next.sourceId != presented_.sourceId || next.active != presented_.active;
    for (const auto& name : {QStringLiteral("title"), QStringLiteral("artist"), QStringLiteral("album"), QStringLiteral("source")})
        textChanged = textChanged || displayText(next, name) != displayText(presented_, name);
    if (textChanged) {
        // Rapid changes keep fading out whatever text is actually visible right now.
        if (textAlpha_ >= 0.5 || !animations_.isRunning("title")) previous_ = presented_;
        marqueeOffset_ = 0;
        marqueePause_ = MarqueeRest;
        marqueeBudget_ = 1;
        animate("title", 0, 1, [this](double a) {
            textAlpha_ = a;
            for (const auto& name : {QStringLiteral("title"), QStringLiteral("artist"), QStringLiteral("album"), QStringLiteral("source")})
                updateElement(name);
        }, [this] { syncTicker(); });
    }
    if (next.cover != presented_.cover) {
        const quint64 generation = ++artworkGeneration_;
        if (config_["animations"].toObject()["cover"].toBool(true) && isVisible() && dockProgress_ < 1) {
            // Decoding, blurring and colour analysis can take tens of milliseconds for large
            // covers; doing it here would stall the text transition that has just started.
            QPointer<HudWindow> self(this);
            artworkPool_.start([self, generation, bytes = next.cover] {
                const Artwork artwork = processArtwork(bytes);
                QMetaObject::invokeMethod(qApp, [self, generation, artwork] {
                    if (self && self->artworkGeneration_ == generation) self->applyArtwork(artwork);
                }, Qt::QueuedConnection);
            });
        } else {
            applyArtwork(processArtwork(next.cover));
        }
    }
    const double playing = next.active && next.playing ? 1.0 : 0.0;
    if (playing != (presented_.active && presented_.playing ? 1.0 : 0.0) || !presentedOnce_) {
        if (!presentedOnce_ || !motionEnabled("play") || !isVisible() || dockProgress_ >= 1) {
            stopAnimation("morph");
            playMorph_ = playing;
        } else if (!animations_.retarget("morph", playing)) {
            animations_.spring("morph", playMorph_, playing, {0.36, 0.7}, [this](double v) {
                playMorph_ = v; updateElement("play");
            });
        }
        if (!presentedOnce_ || !motionEnabled("equalizer") || !isVisible()) {
            stopAnimation("equalizer");
            equalizer_ = playing;
        } else {
            animations_.start("equalizer", equalizer_, playing, duration(1.6), standard(), [this](double v) {
                equalizer_ = v; updateElement("source");
            }, [this] { syncTicker(); });
        }
    }
    presented_ = next;
    presentedOnce_ = true;
    if (textChanged) titleWidth_ = -1;
    syncTicker();
}

HudWindow::Artwork HudWindow::processArtwork(const QByteArray& bytes) {
    Artwork artwork;
    if (!bytes.isEmpty()) artwork.image.loadFromData(bytes);
    artwork.blurred = blurredArtwork(artwork.image);
    artwork.accent = vibrantColor(artwork.image);
    return artwork;
}

void HudWindow::applyArtwork(const Artwork& artwork) {
    accentFrom_ = accentColor();
    progressFrom_ = progressColor();
    oldCover_ = cover_;
    cover_ = artwork.image.isNull() ? QPixmap() : QPixmap::fromImage(artwork.image);
    oldArtwork_ = displayedArtwork_;
    artwork_ = artwork.blurred;
    artworkAccent_ = artwork.accent;
    animate("cover", 0, 1, [this](double a) {
        coverAlpha_ = a;
        displayedArtwork_ = blendArtwork(oldArtwork_, artwork_, a);
        update();
    });
}

QColor HudWindow::accentColor() const {
    const QColor configured(config_["accent_color"].toString("#9B8CFF"));
    const QColor resolved = config_["artwork_accent"].toBool(true) && artworkAccent_.isValid()
        ? readableAccent(artworkAccent_, QColor(config_["background"].toString("#10121B"))) : configured;
    return coverAlpha_ < 1 && accentFrom_.isValid() ? mixColor(accentFrom_, resolved, coverAlpha_) : resolved;
}

QColor HudWindow::progressColor() const {
    const QColor configured(config_["progress_color"].toString("#9B8CFF"));
    const QColor resolved = config_["artwork_accent"].toBool(true) && artworkAccent_.isValid()
        ? readableAccent(artworkAccent_, QColor(config_["background"].toString("#10121B"))) : configured;
    return coverAlpha_ < 1 && progressFrom_.isValid() ? mixColor(progressFrom_, resolved, coverAlpha_) : resolved;
}

void HudWindow::updateElement(const QString& name) {
    if (!isVisible()) return;
    if (dockProgress_ > 0 || appear_ < 1) { update(); return; }
    const auto rects = elementRects();
    const auto found = rects.constFind(name);
    if (found == rects.cend()) return;
    const double scale = config_["scale"].toDouble(1);
    const QRectF local(cardOrigin() + found->topLeft() * scale, found->size() * scale);
    update(local.adjusted(-10 * scale, -10 * scale, 10 * scale, 12 * scale).toAlignedRect());
}

void HudWindow::setHover(const QString& name) {
    if (hover_ == name) return;
    const QString old = hover_;
    hover_ = name;
    const auto level = [this](const QString& key, double target) {
        if (key.isEmpty()) return;
        const QString track = QStringLiteral("hover:") + key;
        if (!motionEnabled("hover") || !isVisible() || dockProgress_ >= 1) {
            stopAnimation(track);
            if (target <= 0) hoverLevels_.remove(key); else hoverLevels_[key] = target;
            updateElement(key);
            return;
        }
        animations_.start(track, hoverLevels_.value(key), target, duration(target > 0 ? 0.55 : 0.85), standard(),
            [this, key](double v) { hoverLevels_[key] = v; updateElement(key); });
    };
    level(old, 0);
    level(name, 1);
    updateExpansion();
}

void HudWindow::updateExpansion() {
    const bool progress = !editing_ && presented_.canSeek && (hover_ == "progress" || seekPreview_.has_value());
    const bool volume = !editing_ && volumeAvailable_ && (hover_ == "volume" || dragElement_ == "volume");
    const auto drive = [this](const QString& track, double& value, double target, const QString& element) {
        if (!motionEnabled("hover") || !isVisible() || dockProgress_ >= 1) {
            stopAnimation(track);
            value = target;
            updateElement(element);
            return;
        }
        if (animations_.retarget(track, target)) return;
        animations_.spring(track, value, target, {0.3, 0.78}, [this, &value, element](double v) {
            value = v; updateElement(element);
        });
    };
    drive(QStringLiteral("expand:progress"), progressExpand_, progress ? 1 : 0, QStringLiteral("progress"));
    drive(QStringLiteral("expand:volume"), volumeExpand_, volume ? 1 : 0, QStringLiteral("volume"));
}

void HudWindow::paintEvent(QPaintEvent* event) {
    QPainter p(this);
    p.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing | QPainter::SmoothPixmapTransform);
    const double scale = config_["scale"].toDouble(1);
    const double t = std::clamp(dockProgress_, 0.0, 1.0);
    QRectF card = card_.translated(-QPointF(pos()));
    double radius = cardRadius();
    // Appearance grows the card from slightly smaller, anchored near its top edge.
    const double grow = appear_ < 1 ? 0.94 + 0.06 * std::clamp(appear_, 0.0, 1.0) : 1.0;
    if (grow < 1) {
        const QPointF anchor(card.center().x(), card.top() + card.height() * 0.2);
        card = QRectF(anchor.x() - (anchor.x() - card.left()) * grow, anchor.y() - (anchor.y() - card.top()) * grow,
                      card.width() * grow, card.height() * grow);
        radius *= grow;
    }
    if (dockProgress_ > 0) {
        if (const auto* target = targetScreen()) p.setClipRect(QRectF(target->geometry()).translated(-QPointF(pos())));
    }
    paintSurface(p, card, radius);
    const QPainterPath shape = Squircle::path(card, radius, CornerSmoothing);

    if (t > 0.5) {
        const double alpha = smoothstep(0.55, 1.0, t);
        QRectF visible = card;
        if (const auto* target = targetScreen())
            visible = card.intersected(QRectF(target->geometry()).translated(-QPointF(pos())));
        const double gripWidth = std::min((26.0 + 10.0 * peek_) * scale, card.width() - 16);
        const double gripHeight = std::max(2.0, 2.0 * scale);
        if (visible.height() > gripHeight && gripWidth > 4) {
            p.save();
            p.setPen(Qt::NoPen);
            p.setBrush(withAlpha(accentColor(), (0.62 + 0.3 * peek_) * alpha));
            p.drawRoundedRect(QRectF(visible.center().x() - gripWidth / 2, visible.center().y() - gripHeight / 2,
                                     gripWidth, gripHeight), gripHeight / 2, gripHeight / 2);
            p.restore();
        }
    }

    const double contentOpacity = 1 - smoothstep(0.04, 0.48, t);
    if (contentOpacity <= 0.002) return;
    p.save();
    p.setClipPath(shape, Qt::IntersectClip);
    p.setOpacity(contentOpacity);
    if (t > 0 || dockProgress_ < 0) {
        // The content scales with the morphing card instead of being cut by its edge.
        const double layoutWidth = config_["width"].toDouble(560), layoutHeight = config_["height"].toDouble(132);
        const double widthRatio = expandedCard_.width() > 0 ? card_.width() / expandedCard_.width() : 1;
        const double heightRatio = expandedCard_.height() > 0 ? card_.height() / expandedCard_.height() : 1;
        const double shrink = std::clamp(std::min(widthRatio, heightRatio), 0.5, 1.03);
        const double factor = scale * grow * shrink;
        p.translate(card.center().x(), card.top() + (card.height() - layoutHeight * factor) / 2);
        p.scale(factor, factor);
        p.translate(-layoutWidth / 2, 0);
    } else {
        p.translate(card.topLeft());
        p.scale(scale * grow, scale * grow);
    }
    const auto rects = elementRects();
    const QRectF dirty = p.worldTransform().inverted().mapRect(QRectF(event->rect())).adjusted(-12, -12, 12, 12);
    for (auto it = rects.cbegin(); it != rects.cend(); ++it) {
        if (!dirty.intersects(it.value().adjusted(-10, -10, 10, 12))) continue;
        paintElement(p, it.key(), it.value());
        if (editing_) {
            p.save();
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(withAlpha(accentColor(), it.key() == hover_ ? .9 : .35), 1, Qt::DashLine));
            p.drawRoundedRect(it.value(), 4, 4);
            p.restore();
        }
    }
    p.restore();
}

void HudWindow::paintSurface(QPainter& p, const QRectF& card, double radius) {
    const double t = std::clamp(dockProgress_, 0.0, 1.0);
    const QPainterPath shape = Squircle::path(card, radius, CornerSmoothing);
    const QColor compactColor(config_["compact_background"].toString("#10121B"));
    QColor first = mixColor(QColor(config_["background"].toString("#10121B")), compactColor, t);
    QColor last = mixColor(QColor(config_[config_["gradient_enabled"].toBool(true) ? "gradient_color" : "background"].toString("#10121B")),
                           compactColor, t);
    const double opacity = lerp(config_["opacity"].toDouble(.94), config_["compact_opacity"].toDouble(.94), t);
    first.setAlphaF(opacity); last.setAlphaF(opacity);
    QLinearGradient gradient(card.topLeft(), card.bottomRight());
    gradient.setColorAt(0, first);
    gradient.setColorAt(1, last);
    p.save();
    p.setPen(Qt::NoPen);
    p.fillPath(shape, gradient);
    p.setClipPath(shape, Qt::IntersectClip);
    if (config_["artwork_background"].toBool(true) && !displayedArtwork_.isNull()) {
        p.save();
        p.setCompositionMode(QPainter::CompositionMode_SourceAtop);
        p.setOpacity(config_["artwork_background_strength"].toDouble(.75) * (1 - t));
        p.drawImage(card, displayedArtwork_);
        p.restore();
    }
    const bool highlight = config_["surface_highlight"].toBool(true) && config_["border_width"].toDouble(0) <= 0;
    const bool lightSurface = luminance(first) > 0.55;
    if (highlight) {
        // A faint top sheen and hairline give the glass its depth without an outer glow.
        QLinearGradient sheen(card.topLeft(), QPointF(card.left(), card.top() + card.height() * 0.4));
        sheen.setColorAt(0, QColor(255, 255, 255, lightSurface ? 40 : 15));
        sheen.setColorAt(1, QColor(255, 255, 255, 0));
        p.fillRect(card, sheen);
    }
    p.restore();
    if (highlight) {
        QLinearGradient edge(card.topLeft(), card.bottomLeft());
        const QColor tone = lightSurface ? QColor(0, 0, 0) : QColor(255, 255, 255);
        edge.setColorAt(0, withAlpha(tone, lightSurface ? 0.10 : 0.13 * (1 - t * 0.4)));
        edge.setColorAt(0.5, withAlpha(tone, lightSurface ? 0.06 : 0.05));
        edge.setColorAt(1, withAlpha(tone, lightSurface ? 0.08 : 0.035));
        p.save();
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(QBrush(edge), 1));
        p.drawPath(Squircle::path(card.adjusted(0.5, 0.5, -0.5, -0.5), std::max(0.0, radius - 0.5), CornerSmoothing));
        p.restore();
    }
    const double border = config_["border_width"].toDouble(0);
    if (border > 0) {
        p.save();
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(ink(config_["border_color"].toString("#9B8CFF"), config_["border_opacity"].toDouble(.22)), border));
        const double borderInset = border / 2;
        p.drawPath(Squircle::path(card.adjusted(borderInset, borderInset, -borderInset, -borderInset),
                                  std::max(0.0, radius - borderInset), CornerSmoothing));
        p.restore();
    }
}

void HudWindow::paintElement(QPainter& p, const QString& name, QRectF rect) {
    if (name == "cover") {
        const double radius = std::min(14.0, config_["radius"].toDouble(30) / 2);
        // Soft contact shadow lifts the artwork off the glass.
        const double ratio = std::hypot(p.worldTransform().m11(), p.worldTransform().m12()) * devicePixelRatioF();
        const int blur = std::max(2, qRound(7 * ratio));
        const QSize device = (rect.size() * ratio).toSize();
        if (!device.isEmpty()) {
            const QImage shadow = Squircle::shadow(device, radius * ratio, blur);
            const double spread = blur * 2 / ratio;
            p.save();
            p.setOpacity(p.opacity() * 0.38);
            p.drawImage(QRectF(rect.left() - spread, rect.top() - spread + 2.5, rect.width() + spread * 2, rect.height() + spread * 2), shadow);
            p.restore();
        }
        p.save();
        p.setClipRect(rect.adjusted(-1, -1, 1, 1), Qt::IntersectClip);
        if (coverAlpha_ < 1) paintCover(p, rect, oldCover_, 1, 1);
        paintCover(p, rect, cover_, coverAlpha_, 0.94 + 0.06 * coverAlpha_);
        p.restore();
        return;
    }
    p.save();
    p.setClipRect(rect.adjusted(-1, -1, 1, 1), Qt::IntersectClip);
    if (name == "title" || name == "artist" || name == "album" || name == "source") paintText(p, name, rect);
    else if (Transport.contains(name)) paintTransport(p, name, rect);
    else if (name == "progress") paintProgress(p, rect);
    else if (name == "time") {
        QFont font(this->font());
        font.setPixelSize(std::max(9, font.pixelSize() - 3));
        font.setFeature(QFont::Tag("tnum"), 1);
        p.setFont(font);
        p.setPen(ink(config_["secondary_color"].toString()));
        p.drawText(rect, Qt::AlignLeft | Qt::AlignVCenter, Layout::formatTime(displayedPosition()));
        p.drawText(rect, Qt::AlignRight | Qt::AlignVCenter, presented_.duration > 0 ? Layout::formatTime(presented_.duration) : "--:--");
    } else if (name == "volume") paintVolume(p, rect);
    p.restore();
}

void HudWindow::paintText(QPainter& p, const QString& name, const QRectF& rect) {
    QFont font(this->font());
    const int fontSize = font.pixelSize();
    QColor color = ink(config_["text_color"].toString());
    if (name == "title") {
        font.setWeight(static_cast<QFont::Weight>(std::min(900, std::max(700, font.weight() + 100))));
    } else {
        font.setPixelSize(std::max(8, fontSize - (name == "source" ? 3 : 1)));
        color = ink(config_["secondary_color"].toString());
    }
    QRectF area = rect;
    if (name == "source") {
        // A live equalizer replaces the static source dot while music plays.
        const double unit = std::max(2.0, font.pixelSize() / 5.5);
        const double maxHeight = std::min(rect.height() - 2, font.pixelSize() * 0.95);
        const QColor bar = withAlpha(accentColor(), 0.92);
        p.save();
        p.setPen(Qt::NoPen);
        p.setBrush(bar);
        const double level = std::clamp(equalizer_, 0.0, 1.0);
        static constexpr std::array<double, 3> speed{7.3, 9.7, 6.1};
        static constexpr std::array<double, 3> phase{0.0, 1.9, 3.7};
        for (int index = 0; index < 3; ++index) {
            const double wave = 0.5 + 0.5 * std::sin(equalizerPhase_ * speed[index] + phase[index])
                * std::cos(equalizerPhase_ * speed[index] * 0.37 + phase[index] * 0.5);
            const double height = lerp(unit, maxHeight * (0.32 + 0.68 * wave), level);
            p.drawRoundedRect(QRectF(rect.left() + index * unit * 1.75, rect.center().y() - height / 2, unit, height),
                              unit / 2, unit / 2);
        }
        p.restore();
        area.adjust(unit * 1.75 * 2 + unit + 6, 0, 0, 0);
    }
    const QString current = displayText(presented_, name);
    const QString before = displayText(previous_, name);
    const bool fade = name != "source";
    const double offset = name == "title" ? marqueeOffset_ : 0;
    const double travel = std::max(4.0, rect.height() * 0.32);
    const double base = p.opacity();
    if (textAlpha_ < 1 && before != current) {
        // Outgoing text lifts away while the new line rises into place.
        const double a = std::clamp(textAlpha_, 0.0, 1.0);
        p.setOpacity(base * (1 - smoothstep(0.0, 0.7, a)));
        drawLine(p, area.translated(0, -travel * a), before, font, color, 0, fade);
        p.setOpacity(base * smoothstep(0.2, 1.0, a));
        drawLine(p, area.translated(0, travel * (1 - a)), current, font, color, 0, fade);
        p.setOpacity(base);
        return;
    }
    drawLine(p, area, current, font, color, offset, fade);
}

void HudWindow::paintTransport(QPainter& p, const QString& name, const QRectF& rect) {
    const bool enabled = presented_.active && (name == "play" ? presented_.canPlayPause
        : name == "next" ? presented_.canNext : presented_.canPrevious);
    const double hover = enabled ? hoverLevels_.value(name) : 0;
    const double press = pressLevels_.value(name);
    const QPointF center = rect.center();
    const double scaleBy = 1 - 0.1 * std::clamp(press, -0.5, 1.5) + 0.03 * hover;
    p.translate(center);
    p.scale(scaleBy, scaleBy);
    p.translate(-center);
    const double radius = rect.height() / 2;
    p.setPen(Qt::NoPen);
    if (name == "play") {
        p.setBrush(withAlpha(accentColor(), 0.16 + 0.1 * hover + 0.06 * press));
        p.drawEllipse(center, radius, radius);
    } else if (hover > 0.001) {
        p.setBrush(ink(config_["icon_color"].toString(), 0.1 * hover));
        p.drawEllipse(center, radius, radius);
    }
    const QColor color = ink(config_["icon_color"].toString(), enabled ? 1 : .3);
    const double size = config_["icon_size"].toDouble(18);
    if (name == "play") HudIcons::playPauseIcon(p, center, size, color, playMorph_);
    else HudIcons::skipIcon(p, center, size, color, name == "next");
}

void HudWindow::paintProgress(QPainter& p, const QRectF& rect) {
    const double base = config_["progress_height"].toDouble(3);
    const double expand = std::clamp(progressExpand_, 0.0, 1.2);
    const double thickness = std::min(rect.height() - 4, base + std::min(4.0, std::max(2.0, base)) * expand);
    QRectF bar(rect.x(), rect.center().y() - thickness / 2, rect.width(), thickness);
    const QString secondary = config_["secondary_color"].toString();
    p.setPen(Qt::NoPen);
    p.setBrush(ink(secondary, .2 + .06 * std::clamp(expand, 0.0, 1.0)));
    p.drawRoundedRect(bar, thickness / 2, thickness / 2);
    const double fraction = presented_.duration > 0 ? std::clamp(displayedPosition() / presented_.duration, 0.0, 1.0) : 0;
    const double filled = bar.width() * fraction;
    if (filled > 0.01) {
        p.setBrush(progressColor());
        p.drawRoundedRect(QRectF(bar.left(), bar.top(), std::max(filled, std::min(thickness, filled * 4)), thickness),
                          thickness / 2, thickness / 2);
    }
    if (presented_.canSeek && expand > 0.01) {
        const double knob = (thickness / 2 + 2.6) * std::clamp(expand, 0.0, 1.1);
        const QPointF point(bar.left() + filled, bar.center().y());
        p.setBrush(QColor(0, 0, 0, qRound(50 * std::clamp(expand, 0.0, 1.0))));
        p.drawEllipse(point + QPointF(0, 0.6), knob + 0.8, knob + 0.8);
        p.setBrush(mixColor(progressColor(), QColor(config_["text_color"].toString("#F5F5FA")), 0.65));
        p.drawEllipse(point, knob, knob);
    }
}

void HudWindow::paintVolume(QPainter& p, const QRectF& rect) {
    if (!volumeAvailable_) p.setOpacity(p.opacity() * 0.35);
    const QColor icon = ink(config_["icon_color"].toString());
    const double level = volumeAvailable_ ? shownVolume_ : 0.6;
    HudIcons::volumeIcon(p, QRectF(rect.x(), rect.y(), 24, rect.height()), icon, std::min(20, config_["icon_size"].toInt(18)), level);
    const double expand = std::clamp(volumeExpand_, 0.0, 1.2);
    const double thickness = 3 + 2 * expand;
    const double left = rect.x() + 31, right = rect.right() - 3;
    const QRectF track(left, rect.center().y() - thickness / 2, right - left, thickness);
    p.setPen(Qt::NoPen);
    p.setBrush(ink(config_["secondary_color"].toString(), .25));
    p.drawRoundedRect(track, thickness / 2, thickness / 2);
    if (!volumeAvailable_) return;
    const double filled = track.width() * std::clamp(shownVolume_, 0.0, 1.0);
    if (filled > 0.01) {
        p.setBrush(ink(config_["icon_color"].toString(), .85));
        p.drawRoundedRect(QRectF(track.left(), track.top(), std::max(filled, std::min(thickness, filled * 4)), thickness),
                          thickness / 2, thickness / 2);
    }
    if (expand > 0.01) {
        const double knob = (thickness / 2 + 2) * std::clamp(expand, 0.0, 1.1);
        const QPointF point(track.left() + filled, track.center().y());
        p.setBrush(QColor(0, 0, 0, qRound(46 * std::clamp(expand, 0.0, 1.0))));
        p.drawEllipse(point + QPointF(0, 0.6), knob + 0.8, knob + 0.8);
        p.setBrush(icon);
        p.drawEllipse(point, knob, knob);
    }
}

void HudWindow::paintCover(QPainter& p, const QRectF& rect, const QPixmap& image, double opacity, double scale) {
    if (opacity <= 0.001) return;
    p.save(); p.setOpacity(p.opacity() * opacity);
    QRectF area = rect;
    if (scale != 1) {
        const QPointF center = rect.center();
        area = QRectF(center.x() - rect.width() * scale / 2, center.y() - rect.height() * scale / 2,
                      rect.width() * scale, rect.height() * scale);
    }
    const double radius = std::min(14.0, config_["radius"].toDouble(30) / 2) * scale;
    const QPainterPath clip = Squircle::path(area, radius, CornerSmoothing);
    p.setClipPath(clip, Qt::IntersectClip);
    if (image.isNull()) {
        QLinearGradient gradient(area.topLeft(), area.bottomRight());
        gradient.setColorAt(0, withAlpha(accentColor(), .55));
        gradient.setColorAt(1, ink(config_["gradient_color"].toString()));
        p.fillRect(area, gradient);
        QRadialGradient glow(area.topLeft() + QPointF(area.width() * 0.3, area.height() * 0.25), area.width() * 0.8);
        glow.setColorAt(0, QColor(255, 255, 255, 38));
        glow.setColorAt(1, QColor(255, 255, 255, 0));
        p.fillRect(area, glow);
        HudIcons::noteIcon(p, area, ink(config_["text_color"].toString(), .9), area.width() * .45);
    } else {
        const double side = std::min(image.width(), image.height());
        p.drawPixmap(area, image, QRectF((image.width() - side) / 2, (image.height() - side) / 2, side, side));
    }
    p.setClipping(false);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QColor(255, 255, 255, 26), 1));
    p.drawPath(Squircle::path(area.adjusted(0.5, 0.5, -0.5, -0.5), std::max(0.0, radius - 0.5), CornerSmoothing));
    p.restore();
}

QPointF HudWindow::localPoint(const QPointF& point) const {
    const double scale = config_["scale"].toDouble(1);
    return (point - cardOrigin()) / scale;
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
    cancelScrub();
    dragElement_.clear(); dragOrigin_.reset(); dragWindow_.reset(); pendingDrag_.reset(); stopAnimation("drag");
    if (collapsed_ || dockProgress_ > 0) { cancelRevealIntent(); hoverFromDock_ = true; reveal(); return; }
    hideTimer_.stop();
    const auto point = localPoint(e->position()); const auto hit = hitTest(point);
    moved_ = false;
    if (editing_ && !hit.isEmpty()) { dragElement_ = hit; dragOrigin_ = point - elementRects()[hit].topLeft(); }
    else if (hit == "volume" && !volumeAvailable_) dragElement_.clear();
    else if (hit == "progress") {
        dragElement_.clear();
        if (seekEnabled()) { dragElement_ = hit; previewSeekAt(point); }
    }
    else if (!Transport.contains(hit) && hit != "progress" && hit != "volume") {
        if (!config_["position_locked"].toBool(false)) dragWindow_ = e->globalPosition().toPoint() - pos();
    }
    else dragElement_ = hit;
    if (Transport.contains(hit) && !editing_) {
        pressed_ = hit;
        const QString track = QStringLiteral("press:") + hit;
        if (!motionEnabled("hover")) pressLevels_[hit] = 1;
        else animations_.spring(track, pressLevels_.value(hit), 1, {0.16, 0.9},
            [this, hit](double v) { pressLevels_[hit] = v; updateElement(hit); });
        updateElement(hit);
    }
    updateExpansion();
}

void HudWindow::mouseMoveEvent(QMouseEvent* e) {
    if (seekPreview_ && !e->buttons().testFlag(Qt::LeftButton)) cancelScrub();
    if (dockProgress_ > 0) return;
    const auto point = localPoint(e->position()); const auto hit = hitTest(point);
    if (!cardHovered_ && !collapsed_) { cardHovered_ = true; syncTicker(); }
    if (hover_ != hit) {
        setHover(hit);
        const QMap<QString, QString> tips{{"play", QStringLiteral("Воспроизведение / пауза")}, {"next", QStringLiteral("Следующий трек")},
            {"previous", QStringLiteral("Предыдущий трек")}, {"progress", QStringLiteral("Перемотка")},
            {"volume", volumeAvailable_ ? QStringLiteral("Громкость текущего приложения. Для сайтов - всего браузера.")
                                        : QStringLiteral("У этого источника пока нет доступной звуковой сессии.")}};
        const QString hint = config_["position_locked"].toBool(false)
            ? QStringLiteral("Островок закреплён. Правая кнопка - меню.")
            : QStringLiteral("Перетащите островок. Правая кнопка - меню.");
        setToolTip(tips.value(hit, hint));
        setCursor(editing_ ? Qt::SizeAllCursor : Transport.contains(hit) || hit == "progress" || (hit == "volume" && volumeAvailable_) ? Qt::PointingHandCursor : Qt::ArrowCursor);
    }
    if (!dragElement_.isEmpty() && editing_ && dragOrigin_) {
        const auto target = point - *dragOrigin_; const auto rects = elementRects(); const auto rect = rects[dragElement_];
        auto positions = config_["element_positions"].toObject();
        if (config_["layout"].toString() != "custom")
            for (auto it = rects.cbegin(); it != rects.cend(); ++it) positions[it.key()] = QJsonArray{it.value().x(), it.value().y()};
        positions[dragElement_] = QJsonArray{std::clamp(target.x(), 0.0, config_["width"].toDouble() - rect.width()),
                                            std::clamp(target.y(), 0.0, config_["height"].toDouble() - rect.height())};
        config_["element_positions"] = positions; config_["layout"] = "custom"; moved_ = true;
        invalidateLayout(); update();
    } else if (dragWindow_ && !config_["position_locked"].toBool(false)) {
        // Mice report up to 1000 positions a second; move the window once per display frame.
        const QPoint destination = e->globalPosition().toPoint() - *dragWindow_;
        if (destination != pos() || pendingDrag_) {
            pendingDrag_ = destination;
            moved_ = true;
            animations_.requestFrame(QStringLiteral("drag"), [this] { applyPendingDrag(); });
        }
    }
    else if (seekPreview_) previewSeekAt(point);
    else if (dragElement_ == "volume") volumeAt(point);
}

void HudWindow::applyPendingDrag() {
    if (!pendingDrag_ || !dragWindow_) { pendingDrag_.reset(); return; }
    const QPoint destination = *pendingDrag_;
    pendingDrag_.reset();
    const QPoint delta = destination - pos();
    if (delta.isNull()) return;
    expandedGeometry_.translate(delta);
    expandedCard_.translate(delta);
    card_.translate(delta);
    unionFrame_.translate(delta);
    move(destination);
    updateNativeRegion();
}

void HudWindow::mouseReleaseEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) return;
    if (pendingDrag_) { stopAnimation("drag"); applyPendingDrag(); }
    const auto point = localPoint(e->position()); const auto hit = hitTest(point);
    if (!pressed_.isEmpty()) {
        const QString released = pressed_;
        pressed_.clear();
        if (!motionEnabled("hover")) { pressLevels_.remove(released); updateElement(released); }
        else animations_.spring(QStringLiteral("press:") + released, pressLevels_.value(released), 0, {0.34, 0.58},
            [this, released](double v) { pressLevels_[released] = v; updateElement(released); });
    }
    if (editing_ && moved_ && dragOrigin_) emit configChanged(config_);
    else if (dragWindow_ && moved_) {
        auto* target = QGuiApplication::screenAt(expandedCard_.center().toPoint()); if (!target) target = targetScreen();
        if (!target) { dragWindow_.reset(); restartHideTimer(); return; }
        const auto area = target->availableGeometry(); auto positions = config_["monitor_positions"].toObject();
        positions[target->name()] = QJsonArray{
            std::clamp(expandedGeometry_.x() - area.x(), 0, std::max(0, area.width() - expandedGeometry_.width())),
            std::clamp(expandedGeometry_.y() - area.y(), 0, std::max(0, area.height() - expandedGeometry_.height()))};
        config_["monitor_positions"] = positions; config_["monitor"] = target->name(); config_["anchor"] = "free";
        placeOnScreen(); emit configChanged(config_);
    } else if (seekPreview_) {
        previewSeekAt(point);
        const auto target = seekPreview_;
        cancelScrub();
        if (target) {
            pendingSeek_ = snapshot_;
            pendingSeek_->position = *target;
            pendingSeek_->updatedAt = QDateTime::currentMSecsSinceEpoch();
            seekTimer_.start(SeekConfirmationTimeoutMs);
            emit command("seek", *target);
        }
    } else if (!editing_ && hit == dragElement_) {
        if (Transport.contains(hit) && snapshot_.active) {
            const bool enabled = hit == "play" ? snapshot_.canPlayPause : hit == "next" ? snapshot_.canNext : snapshot_.canPrevious;
            if (enabled) emit command(hit == "play" ? "play_pause" : hit, 0);
        } else if (hit == "volume") volumeAt(point);
    }
    dragElement_.clear(); dragOrigin_.reset(); dragWindow_.reset(); restartHideTimer();
    updateExpansion();
    syncFrameTimer();
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
    progressTimer_.stop();
    updateElement("progress"); updateElement("time");
}

void HudWindow::cancelScrub() {
    if (!seekPreview_) return;
    seekPreview_.reset();
    if (dragElement_ == "progress") dragElement_.clear();
    restartHideTimer();
    updateExpansion();
    syncFrameTimer();
    update();
}

void HudWindow::cancelSeek() {
    cancelScrub();
    pendingSeek_.reset();
    seekTimer_.stop();
    update();
}

double HudWindow::displayedPosition() const {
    if (seekPreview_) return *seekPreview_;
    if (pendingSeek_) return pendingSeek_->estimatedPosition();
    return snapshot_.active ? snapshot_.estimatedPosition() : presented_.estimatedPosition();
}

void HudWindow::volumeAt(const QPointF& point) {
    if (!volumeAvailable_) return;
    const auto rect = elementRects().value("volume");
    if (rect.isValid()) emit volumeChanged(std::clamp((point.x() - rect.x() - 31) / (rect.width() - 34), 0.0, 1.0));
}

void HudWindow::wheelEvent(QWheelEvent* e) {
    if (collapsed_ || dockProgress_ > 0) return;
    if (!editing_ && volumeAvailable_ && hitTest(localPoint(e->position())) == "volume") {
        emit volumeChanged(std::clamp(volume_ + e->angleDelta().y() / 120.0 * .02, 0.0, 1.0)); e->accept();
        restartHideTimer();
    }
}

void HudWindow::contextMenuEvent(QContextMenuEvent* e) {
    cancelSeek();
    menuOpen_ = true;
    hideTimer_.stop();
    IslandMenu menu(this);
    menu.addAction(QStringLiteral("Настройки SCARP ISLAND"), this, &HudWindow::settingsRequested);
    menu.addAction(QStringLiteral("Вернуть в исходное положение"), this, &HudWindow::resetPosition);
    menu.addSeparator();
    auto* locked = menu.addAction(QStringLiteral("Закрепить островок"));
    locked->setCheckable(true); locked->setChecked(config_["position_locked"].toBool(false));
    connect(locked, &QAction::triggered, this, [this](bool enabled) {
        auto config = config_; config["position_locked"] = enabled;
        applyConfig(config); emit configChanged(config_);
    });
    auto* edit = menu.addAction(QStringLiteral("Редактировать расположение")); edit->setCheckable(true); edit->setChecked(editing_);
    connect(edit, &QAction::triggered, this, &HudWindow::editingChanged);
    menu.addSeparator();
    menu.addAction(QStringLiteral("Скрыть островок"), this, [this] { conceal(true); });
    menu.exec(e->globalPos());
    menuOpen_ = false;
    restartHideTimer();
}

#include "HudWindow.h"
#include "Layout.h"
#include "WindowsIntegration.h"

#include <QApplication>
#include <QCloseEvent>
#include <QContextMenuEvent>
#include <QJsonArray>
#include <QLinearGradient>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>
#include <QWheelEvent>
#include <algorithm>

namespace {
constexpr int Shadow = 14;
constexpr int FrameMs = 33;
const QStringList Transport{"previous", "play", "next"};
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
    setWindowTitle("Island HUD");
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setMouseTracking(true);
    setAccessibleName(QStringLiteral("Island - музыкальный оверлей"));
    frameTimer_.setInterval(FrameMs);
    connect(&frameTimer_, &QTimer::timeout, this, qOverload<>(&QWidget::update));
    hideTimer_.setSingleShot(true);
    connect(&hideTimer_, &QTimer::timeout, this, [this] {
        if (!editing_ && !underMouse()) conceal();
    });
    connect(qApp, &QGuiApplication::screenAdded, this, [this](QScreen* s) { watchScreen(s); placeOnScreen(); });
    connect(qApp, &QGuiApplication::screenRemoved, this, [this] { placeOnScreen(); });
    for (auto* screen : QGuiApplication::screens()) watchScreen(screen);
    applyConfig(config);
}

void HudWindow::watchScreen(QScreen* screen) {
    connect(screen, &QScreen::availableGeometryChanged, this, [this] { placeOnScreen(); });
}

QMap<QString, QRectF> HudWindow::elementRects() const { return Layout::elements(config_); }

void HudWindow::placeOnScreen() {
    auto* target = QGuiApplication::primaryScreen();
    for (auto* screen : QGuiApplication::screens())
        if (screen->name() == config_["monitor"].toString()) target = screen;
    if (!target) return;
    const auto saved = config_["monitor_positions"].toObject()[target->name()].toArray();
    QPoint point;
    if (saved.size() == 2) point = QPoint(saved[0].toInt(), saved[1].toInt());
    move(Layout::screenPosition(target->availableGeometry(), size(), config_["anchor"].toString(),
                                 config_["offset_y"].toInt(12), saved.size() == 2 ? &point : nullptr));
}

void HudWindow::applyConfig(const QJsonObject& config) {
    config_ = config;
    const double scale = config_["scale"].toDouble(1);
    const int inset = qCeil(Shadow * scale);
    setFixedSize(qRound(config_["width"].toDouble(560) * scale) + inset * 2,
                 qRound(config_["height"].toDouble(132) * scale) + inset * 2);
    placeOnScreen();
    if (isVisible()) applyNative();
    syncFrameTimer(); restartHideTimer(); update();
}

void HudWindow::applyNative() {
    if (QGuiApplication::platformName() != "windows") return;
    WindowsIntegration::applyBackdrop(winId(), config_["blur"].toBool(true),
                                      config_["background"].toString(), config_["opacity"].toDouble(.94));
    WindowsIntegration::setClickThrough(winId(), config_["click_through"].toBool() && !editing_);
    WindowsIntegration::ensureTopmost(winId());
}
void HudWindow::showEvent(QShowEvent* e) { QWidget::showEvent(e); applyNative(); syncFrameTimer(); }
void HudWindow::hideEvent(QHideEvent* e) { frameTimer_.stop(); QWidget::hideEvent(e); }
void HudWindow::closeEvent(QCloseEvent* e) { e->ignore(); conceal(true); }
void HudWindow::enterEvent(QEnterEvent* e) { hideTimer_.stop(); QWidget::enterEvent(e); }
void HudWindow::leaveEvent(QEvent* e) { hover_.clear(); update(); restartHideTimer(); QWidget::leaveEvent(e); }

void HudWindow::stopAnimation(const QString& name) {
    if (auto* previous = animations_.take(name)) { previous->stop(); previous->deleteLater(); }
}

void HudWindow::animate(const QString& name, double from, double to,
                        std::function<void(double)> callback, std::function<void()> finished) {
    stopAnimation(name);
    if (!config_["animations"].toObject()[name].toBool(true)) {
        callback(to); if (finished) finished(); return;
    }
    auto* animation = new QVariantAnimation(this);
    animation->setDuration(config_["animation_duration"].toInt(260));
    animation->setStartValue(from); animation->setEndValue(to);
    animation->setEasingCurve(QEasingCurve::OutCubic);
    connect(animation, &QVariantAnimation::valueChanged, this, [callback](const QVariant& value) { callback(value.toDouble()); });
    if (finished) connect(animation, &QVariantAnimation::finished, this, finished);
    animations_[name] = animation;
    callback(from);
    animation->start();
}

void HudWindow::reveal(bool manual) {
    if (manual) manualHidden_ = false;
    if (manualHidden_) return;
    const bool visible = isVisible();
    fadingOut_ = false;
    stopAnimation("disappear");
    if (!visible) setWindowOpacity(config_["animations"].toObject()["appear"].toBool(true) ? 0 : 1);
    show();
    if (!visible || windowOpacity() < 1)
        animate("appear", visible ? windowOpacity() : 0, 1, [this](double a) { setWindowOpacity(a); });
    restartHideTimer();
}
void HudWindow::conceal(bool manual) {
    if (manual) manualHidden_ = true;
    hideTimer_.stop(); stopAnimation("appear");
    if (isVisible()) {
        fadingOut_ = true;
        animate("disappear", windowOpacity(), 0, [this](double a) { setWindowOpacity(a); }, [this] { hide(); });
    }
}
void HudWindow::toggle() { if (isVisible() && !fadingOut_) conceal(true); else reveal(true); }
void HudWindow::restartHideTimer() {
    hideTimer_.stop();
    const int delay = config_["auto_hide_seconds"].toInt();
    if (delay > 0 && !editing_ && isVisible()) hideTimer_.start(delay * 1000);
}
void HudWindow::syncFrameTimer() {
    if (isVisible() && snapshot_.playing) {
        frameTimer_.start(config_["animations"].toObject()["progress"].toBool(true) ? FrameMs : 1000);
    } else frameTimer_.stop();
}
void HudWindow::setEditing(bool enabled) {
    editing_ = enabled;
    if (enabled) reveal(true);
    applyNative(); restartHideTimer(); update();
}
void HudWindow::setVolume(double value) { volume_ = std::clamp(value, 0.0, 1.0); update(); }

void HudWindow::setSnapshot(const MediaSnapshot& snapshot) {
    const bool changed = snapshot.sourceId != snapshot_.sourceId || snapshot.title != snapshot_.title
        || snapshot.artist != snapshot_.artist || snapshot.album != snapshot_.album;
    if (snapshot.cover != snapshot_.cover) {
        oldCover_ = cover_; cover_ = QPixmap();
        if (!snapshot.cover.isEmpty()) cover_.loadFromData(snapshot.cover);
        animate("cover", 0, 1, [this](double a) { coverAlpha_ = a; update(); });
    }
    if (changed) {
        animate("title", .15, 1, [this](double a) { titleAlpha_ = a; update(); });
        if (snapshot.active) reveal();
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
    const int inset = qCeil(Shadow * scale);
    p.translate(inset, inset);
    p.scale(scale, scale);
    const QRectF card(0, 0, config_["width"].toDouble(560), config_["height"].toDouble(132));
    const double radius = config_["radius"].toDouble(30);
    p.setPen(Qt::NoPen);
    for (int spread = 10; spread > 0; spread -= 2) {
        p.setBrush(ink(config_["accent_color"].toString(), .016));
        p.drawRoundedRect(card.adjusted(-spread, -spread / 2.0, spread, spread), radius + spread, radius + spread);
    }
    QLinearGradient gradient(card.topLeft(), card.bottomRight());
    const double opacity = config_["opacity"].toDouble(.94);
    gradient.setColorAt(0, ink(config_["background"].toString(), opacity));
    gradient.setColorAt(1, ink(config_[config_["gradient_enabled"].toBool(true) ? "gradient_color" : "background"].toString(), opacity));
    p.setBrush(gradient);
    p.setPen(QPen(ink(config_["accent_color"].toString(), .22), 1));
    p.drawRoundedRect(card.adjusted(.5, .5, -.5, -.5), radius, radius);
    QPainterPath clip;
    clip.addRoundedRect(card, radius, radius);
    p.setClipPath(clip);
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
    QFont font(config_["font_family"].toString("Segoe UI"));
    const int fontSize = config_["font_size"].toInt(13);
    font.setPixelSize(fontSize); p.setFont(font);
    p.setPen(ink(config_["text_color"].toString()));
    const auto secondary = config_["secondary_color"].toString();
    if (name == "cover") {
        paintCover(p, rect, oldCover_, 1);
        paintCover(p, rect, cover_, coverAlpha_);
    } else if (name == "title" || name == "artist" || name == "album" || name == "source") {
        QString text;
        if (name == "title") {
            text = snapshot_.title.isEmpty() ? (snapshot_.active ? QStringLiteral("Без названия") : QStringLiteral("Музыка рядом")) : snapshot_.title;
            font.setWeight(QFont::DemiBold); p.setOpacity(titleAlpha_);
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
        const double fraction = snapshot_.duration > 0 ? snapshot_.estimatedPosition() / snapshot_.duration : 0;
        bar.setWidth(bar.width() * std::clamp(fraction, 0.0, 1.0));
        p.setBrush(ink(config_["progress_color"].toString()));
        p.drawRoundedRect(bar, thickness / 2, thickness / 2);
        if (name == hover_ && snapshot_.canSeek) p.drawEllipse(QPointF(bar.right(), bar.center().y()), 3, 3);
    } else if (name == "time") {
        font.setPixelSize(std::max(9, fontSize - 3)); p.setFont(font); p.setPen(ink(secondary));
        p.drawText(rect, Qt::AlignLeft | Qt::AlignVCenter, Layout::formatTime(snapshot_.estimatedPosition()));
        p.drawText(rect, Qt::AlignRight | Qt::AlignVCenter, snapshot_.duration > 0 ? Layout::formatTime(snapshot_.duration) : "--:--");
    } else if (name == "volume") {
        icon(p, name, QRectF(rect.x(), rect.y(), 24, rect.height()), ink(config_["icon_color"].toString()), std::min(20, config_["icon_size"].toInt(18)));
        p.setPen(QPen(ink(secondary, .25), 3, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(rect.x() + 31, rect.center().y()), QPointF(rect.right() - 2, rect.center().y()));
        p.setPen(QPen(ink(config_["icon_color"].toString(), .8), 3, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(rect.x() + 31, rect.center().y()), QPointF(rect.x() + 31 + (rect.width() - 33) * volume_, rect.center().y()));
    }
    p.restore();
}

void HudWindow::paintCover(QPainter& p, const QRectF& rect, const QPixmap& image, double opacity) {
    p.save(); p.setOpacity(opacity);
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
    const int inset = qCeil(Shadow * scale);
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
    const auto point = localPoint(e->position()); const auto hit = hitTest(point);
    moved_ = false;
    if (editing_ && !hit.isEmpty()) { dragElement_ = hit; dragOrigin_ = point - elementRects()[hit].topLeft(); }
    else if (!Transport.contains(hit) && hit != "progress" && hit != "volume") dragWindow_ = e->globalPosition().toPoint() - pos();
    else dragElement_ = hit;
}
void HudWindow::mouseMoveEvent(QMouseEvent* e) {
    const auto point = localPoint(e->position()); const auto hit = hitTest(point);
    if (hover_ != hit) {
        hover_ = hit;
        const QMap<QString, QString> tips{{"play", QStringLiteral("Воспроизведение / пауза")}, {"next", QStringLiteral("Следующий трек")},
            {"previous", QStringLiteral("Предыдущий трек")}, {"progress", QStringLiteral("Перемотка")}, {"volume", QStringLiteral("Системная громкость. Нажатие или колесо мыши.")}};
        setToolTip(tips.value(hit, QStringLiteral("Перетащите островок. Правая кнопка - меню.")));
        setCursor(editing_ ? Qt::SizeAllCursor : Transport.contains(hit) || hit == "progress" || hit == "volume" ? Qt::PointingHandCursor : Qt::ArrowCursor);
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
    } else if (dragWindow_) { move(e->globalPosition().toPoint() - *dragWindow_); moved_ = true; }
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
    } else if (!editing_ && hit == dragElement_) {
        if (Transport.contains(hit) && snapshot_.active) {
            const bool enabled = hit == "play" ? snapshot_.canPlayPause : hit == "next" ? snapshot_.canNext : snapshot_.canPrevious;
            if (enabled) emit command(hit == "play" ? "play_pause" : hit, 0);
        } else if (hit == "progress" && snapshot_.canSeek && snapshot_.duration > 0) {
            const auto rect = elementRects()[hit];
            emit command("seek", std::clamp((point.x() - rect.x()) / rect.width(), 0.0, 1.0) * snapshot_.duration);
        } else if (hit == "volume") volumeAt(point);
    }
    dragElement_.clear(); dragOrigin_.reset(); dragWindow_.reset(); restartHideTimer();
}
void HudWindow::volumeAt(const QPointF& point) {
    const auto rect = elementRects().value("volume");
    if (rect.isValid()) emit volumeChanged(std::clamp((point.x() - rect.x() - 31) / (rect.width() - 33), 0.0, 1.0));
}
void HudWindow::wheelEvent(QWheelEvent* e) {
    if (!editing_ && hitTest(localPoint(e->position())) == "volume") {
        emit volumeChanged(std::clamp(volume_ + e->angleDelta().y() / 120.0 * .02, 0.0, 1.0)); e->accept();
    }
}
void HudWindow::contextMenuEvent(QContextMenuEvent* e) {
    QMenu menu(this);
    menu.addAction(QStringLiteral("Настройки Island"), this, &HudWindow::settingsRequested);
    auto* edit = menu.addAction(QStringLiteral("Редактировать расположение")); edit->setCheckable(true); edit->setChecked(editing_);
    connect(edit, &QAction::triggered, this, &HudWindow::editingChanged);
    menu.addAction(QStringLiteral("Скрыть островок"), this, [this] { conceal(true); });
    menu.exec(e->globalPos());
}

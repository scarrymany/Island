#include "SmoothScroll.h"
#include "AnimationClock.h"

#include <QAbstractScrollArea>
#include <QEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QScrollBar>
#include <QTimer>
#include <QWheelEvent>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace {
constexpr int BarWidth = 12;
constexpr int TrackInset = 4;
constexpr double IdleThickness = 4;
constexpr double HoverThickness = 8;
constexpr double MinimumThumb = 34;
constexpr int IdleDelayMs = 900;
constexpr double WheelStep = 104;
// Time constant of the wheel glide is GlideResponse / 2pi (about 53 ms): a notch settles in ~0.28 s.
constexpr double GlideResponse = 0.33;

class OverlayScrollBar final : public QWidget {
public:
    explicit OverlayScrollBar(QAbstractScrollArea* area) : QWidget(area), area_(area), motion_(this) {
        setObjectName(QStringLiteral("overlayScrollBar"));
        setAttribute(Qt::WA_Hover);
        setMouseTracking(true);
        setCursor(Qt::ArrowCursor);
        area->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        auto* bar = area->verticalScrollBar();
        connect(bar, &QScrollBar::rangeChanged, this, [this] { place(); });
        connect(bar, &QScrollBar::valueChanged, this, [this](int value) {
            if (!gliding_) target_ = value;
            // Someone else scrolled (keyboard, focus, script): continue gliding from there.
            else if (std::abs(value - position_) > 1.5) position_ = value;
            wake();
            update();
        });
        idle_.setSingleShot(true);
        idle_.setInterval(IdleDelayMs);
        connect(&idle_, &QTimer::timeout, this, [this] { fadeTo(underMouse() || dragging_ ? 1 : 0); });
        area->installEventFilter(this);
        area->viewport()->installEventFilter(this);
        place();
    }

    void setColor(const QColor& color) { color_ = color; update(); }
    void setMotion(bool enabled) {
        motionEnabled_ = enabled;
        if (!enabled) { motion_.stopAll(); gliding_ = false; position_ = -1; activity_ = 0; hover_ = underMouse() ? 1 : 0; update(); }
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (watched == area_ && (event->type() == QEvent::Resize || event->type() == QEvent::Show
                                 || event->type() == QEvent::LayoutRequest)) place();
        if (watched == area_->viewport() && event->type() == QEvent::Resize) place();
        if ((watched == area_->viewport() || watched == area_) && event->type() == QEvent::Wheel)
            return wheel(static_cast<QWheelEvent*>(event));
        return QWidget::eventFilter(watched, event);
    }

    bool event(QEvent* event) override {
        if (event->type() == QEvent::HoverEnter) hoverTo(1);
        else if (event->type() == QEvent::HoverLeave && !dragging_) hoverTo(0);
        return QWidget::event(event);
    }

    void paintEvent(QPaintEvent*) override {
        const QRectF thumb = thumbRect();
        if (thumb.isEmpty()) return;
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const double hover = std::clamp(hover_, 0.0, 1.0);
        const double emphasis = std::max(hover, std::clamp(activity_, 0.0, 1.0));
        QColor color = color_;
        color.setAlphaF(std::clamp(0.16 + 0.22 * emphasis + 0.16 * hover + (dragging_ ? 0.12 : 0.0), 0.0, 1.0));
        if (hover > 0.01) {
            QColor groove = color_;
            groove.setAlphaF(0.05 * hover);
            painter.setPen(Qt::NoPen);
            painter.setBrush(groove);
            const double width = thumb.width();
            painter.drawRoundedRect(QRectF(thumb.left(), track().top(), width, track().height()), width / 2, width / 2);
        }
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        painter.drawRoundedRect(thumb, thumb.width() / 2, thumb.width() / 2);
    }

    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() != Qt::LeftButton) return;
        const QRectF thumb = thumbRect();
        if (thumb.adjusted(-4, 0, 4, 0).contains(event->position())) {
            dragging_ = true;
            grabOffset_ = event->position().y() - thumb.top();
            stopGlide();
        } else {
            auto* bar = area_->verticalScrollBar();
            const double direction = event->position().y() < thumb.top() ? -1 : 1;
            glideTo(target_ + direction * bar->pageStep());
        }
        update();
    }

    void mouseMoveEvent(QMouseEvent* event) override {
        if (!dragging_) return;
        auto* bar = area_->verticalScrollBar();
        const QRectF thumb = thumbRect();
        const double travel = track().height() - thumb.height();
        if (travel <= 0) return;
        const double ratio = std::clamp((event->position().y() - grabOffset_ - track().top()) / travel, 0.0, 1.0);
        bar->setValue(qRound(bar->minimum() + ratio * (bar->maximum() - bar->minimum())));
    }

    void mouseReleaseEvent(QMouseEvent* event) override {
        if (event->button() != Qt::LeftButton) return;
        dragging_ = false;
        if (!underMouse()) hoverTo(0);
        wake();
        update();
    }

private:
    QRectF track() const { return QRectF(rect()).adjusted(0, TrackInset, 0, -TrackInset); }

    QRectF thumbRect() const {
        const auto* bar = area_->verticalScrollBar();
        const int range = bar->maximum() - bar->minimum();
        if (range <= 0) return {};
        const QRectF area = track();
        const double length = std::max(MinimumThumb, area.height() * bar->pageStep() / (range + bar->pageStep()));
        const double offset = (area.height() - length) * (bar->value() - bar->minimum()) / range;
        const double thickness = IdleThickness + (HoverThickness - IdleThickness) * std::clamp(hover_, 0.0, 1.0);
        return QRectF(width() - thickness - 2, area.top() + offset, thickness, length);
    }

    void place() {
        const QRect viewport = area_->viewport()->geometry();
        setGeometry(viewport.right() - BarWidth + 1, viewport.top(), BarWidth, viewport.height());
        const auto* bar = area_->verticalScrollBar();
        setVisible(bar->maximum() > bar->minimum());
        raise();
        update();
    }

    void hoverTo(double target) {
        if (!motionEnabled_) { hover_ = target; update(); return; }
        if (motion_.retarget(QStringLiteral("hover"), target)) return;
        motion_.spring(QStringLiteral("hover"), hover_, target, {0.26, 0.9}, [this](double value) { hover_ = value; update(); });
        if (target > 0) fadeTo(1);
        else idle_.start();
    }

    void fadeTo(double target) {
        if (!motionEnabled_) { activity_ = target; update(); return; }
        motion_.start(QStringLiteral("activity"), activity_, target, std::chrono::milliseconds(target > 0 ? 120 : 420),
                      QEasingCurve::OutCubic, [this](double value) { activity_ = value; update(); });
    }

    void wake() {
        if (activity_ < 1 && !motion_.isRunning(QStringLiteral("activity"))) fadeTo(1);
        idle_.start();
    }

    void stopGlide() {
        motion_.stop(QStringLiteral("glide"));
        gliding_ = false;
        position_ = -1;
        target_ = area_->verticalScrollBar()->value();
    }

    void glideTo(double value) {
        auto* bar = area_->verticalScrollBar();
        target_ = std::clamp(value, static_cast<double>(bar->minimum()), static_cast<double>(bar->maximum()));
        if (!motionEnabled_ || !isVisible()) { bar->setValue(qRound(target_)); return; }
        // A critically damped spring launched at omega * distance decays exponentially: the
        // page moves on the very next frame and slows down smoothly. Every further notch
        // re-aims the same motion from the current position, so rapid wheeling never
        // restarts an easing curve with a visible jolt.
        const double from = gliding_ && position_ >= 0 ? position_ : bar->value();
        const double omega = 2 * std::numbers::pi / GlideResponse;
        gliding_ = true;
        motion_.spring(QStringLiteral("glide"), from, target_, {GlideResponse, 1.0}, [this](double position) {
            position_ = position;
            area_->verticalScrollBar()->setValue(qRound(position));
        }, [this] { gliding_ = false; position_ = -1; }, omega * (target_ - from));
    }

    bool wheel(QWheelEvent* event) {
        auto* bar = area_->verticalScrollBar();
        if (bar->maximum() <= bar->minimum() || event->modifiers() != Qt::NoModifier) return false;
        // Touchpads already deliver smooth pixel deltas; only coarse wheel notches glide.
        if (!event->pixelDelta().isNull() || !motionEnabled_) {
            stopGlide();
            return false;
        }
        const int notches = event->angleDelta().y();
        if (notches == 0) return false;
        const double origin = gliding_ ? target_ : bar->value();
        const double next = std::clamp(origin - WheelStep * notches / 120.0,
                                       static_cast<double>(bar->minimum()), static_cast<double>(bar->maximum()));
        if (next == origin && !gliding_) return false;
        glideTo(next);
        event->accept();
        return true;
    }

    QAbstractScrollArea* area_;
    AnimationClock motion_;
    QTimer idle_;
    QColor color_{"#D4D4D4"};
    double hover_ = 0;
    double activity_ = 0;
    double target_ = 0;
    double position_ = -1;
    double grabOffset_ = 0;
    bool dragging_ = false;
    bool gliding_ = false;
    bool motionEnabled_ = true;
};

OverlayScrollBar* overlay(QAbstractScrollArea* area) {
    // The overlay has no meta-object of its own; its object name identifies it.
    auto* widget = area ? area->findChild<QWidget*>(QStringLiteral("overlayScrollBar"), Qt::FindDirectChildrenOnly) : nullptr;
    return static_cast<OverlayScrollBar*>(widget);
}
}

void SmoothScroll::install(QAbstractScrollArea* area) {
    if (!area || overlay(area)) return;
    new OverlayScrollBar(area);
}

void SmoothScroll::setColor(QAbstractScrollArea* area, const QColor& color) {
    if (auto* bar = overlay(area)) bar->setColor(color);
}

void SmoothScroll::setMotion(QAbstractScrollArea* area, bool enabled) {
    if (auto* bar = overlay(area)) bar->setMotion(enabled);
}

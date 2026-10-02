#include "IslandToolTip.h"
#include "AnimationClock.h"
#include "IslandMenu.h"
#include "Squircle.h"

#include <QApplication>
#include <QCursor>
#include <QEvent>
#include <QHelpEvent>
#include <QPainter>
#include <QScreen>
#include <QTextLayout>
#include <QWidget>

#include <algorithm>

namespace {
constexpr int Margin = 10;
constexpr int PaddingX = 11;
constexpr int PaddingY = 7;
constexpr int MaximumTextWidth = 300;
constexpr double Radius = 9;
QPointer<IslandToolTip> instance;
}

class ToolTipBubble final : public QWidget {
public:
    ToolTipBubble()
        : QWidget(nullptr, Qt::ToolTip | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint
                  | Qt::WindowTransparentForInput | Qt::WindowDoesNotAcceptFocus),
          motion_(this) {
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_ShowWithoutActivating);
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setObjectName(QStringLiteral("islandToolTip"));
    }

    void present(const QString& text, const QPoint& cursor, bool animate) {
        const auto theme = IslandMenu::theme();
        QFont font(theme.fontFamily);
        font.setPixelSize(12);
        font.setWeight(QFont::Medium);
        setFont(font);
        text_ = text;
        const QFontMetrics metrics(font);
        const QRect bounds = metrics.boundingRect(QRect(0, 0, MaximumTextWidth, 2000), Qt::TextWordWrap, text);
        resize(bounds.width() + (PaddingX + Margin) * 2, bounds.height() + (PaddingY + Margin) * 2);
        QScreen* screen = QGuiApplication::screenAt(cursor);
        if (!screen) screen = QGuiApplication::primaryScreen();
        const QRect area = screen ? screen->availableGeometry() : QRect(cursor, size());
        QPoint position = cursor + QPoint(-Margin + 4, 22 - Margin);
        if (position.y() + height() - Margin > area.bottom()) position.setY(cursor.y() - height() + Margin - 8);
        position.setX(std::clamp(position.x(), area.left() - Margin, area.right() - width() + Margin));
        final_ = position;
        const bool motion = animate && theme.motion;
        if (!isVisible()) {
            move(final_ + QPoint(0, motion ? 4 : 0));
            setWindowOpacity(motion ? 0 : 1);
            show();
            raise();
        }
        update();
        if (!motion) { motion_.stopAll(); setWindowOpacity(1); move(final_); return; }
        const QPoint start = pos();
        const double from = windowOpacity();
        motion_.start(QStringLiteral("tip"), 0, 1, std::chrono::milliseconds(150), QEasingCurve::OutCubic,
            [this, start, from](double value) {
                setWindowOpacity(from + (1 - from) * value);
                move(start + (final_ - start) * value);
            });
    }

    void dismiss() {
        if (!isVisible()) return;
        if (!IslandMenu::theme().motion) { motion_.stopAll(); hide(); return; }
        const double from = windowOpacity();
        motion_.start(QStringLiteral("tip"), from, 0, std::chrono::milliseconds(100), QEasingCurve::OutCubic,
            [this](double value) { setWindowOpacity(value); }, [this] { hide(); });
    }

protected:
    void paintEvent(QPaintEvent*) override {
        const auto theme = IslandMenu::theme();
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QRectF panel = QRectF(rect()).adjusted(Margin, Margin - 1, -Margin, -Margin - 1);
        const double ratio = devicePixelRatioF();
        const int blur = qRound(7 * ratio);
        const double spread = blur * 2 / ratio;
        painter.setOpacity(0.45);
        painter.drawImage(panel.adjusted(-spread, -spread + 3, spread, spread + 3),
                          Squircle::shadow((panel.size() * ratio).toSize(), Radius * ratio, blur));
        painter.setOpacity(1);
        QColor background = theme.background;
        background.setAlphaF(0.98f);
        painter.fillPath(Squircle::path(panel, Radius), background);
        const bool light = theme.background.lightnessF() > 0.6;
        painter.setPen(QPen(light ? QColor(0, 0, 0, 30) : QColor(255, 255, 255, 26), 1));
        painter.drawPath(Squircle::path(panel.adjusted(0.5, 0.5, -0.5, -0.5), Radius - 0.5));
        painter.setPen(theme.text);
        painter.setFont(font());
        painter.drawText(panel.adjusted(PaddingX, PaddingY, -PaddingX, -PaddingY), Qt::TextWordWrap | Qt::AlignLeft | Qt::AlignVCenter, text_);
    }

private:
    AnimationClock motion_;
    QString text_;
    QPoint final_;
};

IslandToolTip::IslandToolTip(QObject* parent) : QObject(parent) {}

void IslandToolTip::install() {
    if (!qobject_cast<QApplication*>(QCoreApplication::instance())) return;
    if (instance) return;
    instance = new IslandToolTip(qApp);
    qApp->installEventFilter(instance);
}

void IslandToolTip::hideText() {
    if (instance) instance->hide();
}

void IslandToolTip::show(QWidget* target, const QPoint& globalPosition) {
    if (!bubble_) bubble_ = new ToolTipBubble;
    const bool retarget = bubble_->isVisible() && target_ == target;
    target_ = target;
    bubble_->present(target->toolTip(), globalPosition, !retarget);
}

void IslandToolTip::hide() {
    target_.clear();
    if (bubble_) bubble_->dismiss();
}

bool IslandToolTip::eventFilter(QObject* watched, QEvent* event) {
    switch (event->type()) {
    case QEvent::ToolTip: {
        auto* widget = qobject_cast<QWidget*>(watched);
        if (!widget) break;
        QWidget* owner = widget;
        while (owner && owner->toolTip().isEmpty() && !owner->isWindow()) owner = owner->parentWidget();
        if (!owner || owner->toolTip().isEmpty()) { hide(); break; }
        show(owner, static_cast<QHelpEvent*>(event)->globalPos());
        return true;
    }
    case QEvent::ToolTipChange:
        if (watched == target_ && bubble_ && bubble_->isVisible()) {
            if (target_->toolTip().isEmpty()) hide();
            else bubble_->present(target_->toolTip(), QCursor::pos(), false);
        }
        break;
    case QEvent::Leave:
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonDblClick:
    case QEvent::Wheel:
    case QEvent::KeyPress:
    case QEvent::Hide:
    case QEvent::Close:
    case QEvent::WindowDeactivate:
    case QEvent::FocusOut:
        if (target_ && (watched == target_ || (watched->isWidgetType() && static_cast<QWidget*>(watched)->isAncestorOf(target_))))
            hide();
        break;
    case QEvent::DeferredDelete:
        if (watched == target_) hide();
        break;
    default:
        break;
    }
    return QObject::eventFilter(watched, event);
}

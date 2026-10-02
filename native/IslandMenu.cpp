#include "IslandMenu.h"
#include "Squircle.h"

#include <QAction>
#include <QApplication>
#include <QCursor>
#include <QEvent>
#include <QPainter>
#include <QPointer>
#include <QProxyStyle>
#include <QStyleFactory>
#include <QStyleOptionMenuItem>

#include <algorithm>
#include <cmath>

namespace {
constexpr int Padding = 6;
constexpr int ItemHeight = 34;
constexpr int SeparatorHeight = 11;
constexpr double PanelRadius = 12;
constexpr double ItemRadius = 8;
constexpr int CheckColumn = 24;

IslandMenu::Theme& currentTheme() {
    static IslandMenu::Theme theme;
    return theme;
}

QColor withAlpha(QColor color, double alpha) {
    color.setAlphaF(std::clamp(alpha, 0.0, 1.0));
    return color;
}

QColor mix(const QColor& from, const QColor& to, double amount) {
    return QColor::fromRgbF(from.redF() + (to.redF() - from.redF()) * amount,
                            from.greenF() + (to.greenF() - from.greenF()) * amount,
                            from.blueF() + (to.blueF() - from.blueF()) * amount);
}

class MenuStyle final : public QProxyStyle {
public:
    MenuStyle() : QProxyStyle(QStyleFactory::create(QStringLiteral("fusion"))) {}

    int pixelMetric(PixelMetric metric, const QStyleOption* option, const QWidget* widget) const override {
        switch (metric) {
        case PM_MenuPanelWidth: return 0;
        case PM_MenuHMargin:
        case PM_MenuVMargin: return IslandMenu::ShadowMargin + Padding;
        case PM_SubMenuOverlap: return IslandMenu::ShadowMargin * 2 - 2;
        case PM_SmallIconSize: return 16;
        case PM_MenuDesktopFrameWidth: return 0;
        default: return QProxyStyle::pixelMetric(metric, option, widget);
        }
    }

    int styleHint(StyleHint hint, const QStyleOption* option, const QWidget* widget, QStyleHintReturn* data) const override {
        switch (hint) {
        case SH_Menu_Mask:
        case SH_Menu_FlashTriggeredItem:
        case SH_Menu_FadeOutOnHide:
        case SH_Menu_Scrollable: return 0;
        case SH_Menu_SubMenuPopupDelay: return 150;
        case SH_Menu_MouseTracking: return 1;
        case SH_Menu_KeyboardSearch: return 1;
        default: return QProxyStyle::styleHint(hint, option, widget, data);
        }
    }

    QSize sizeFromContents(ContentsType type, const QStyleOption* option, const QSize& size, const QWidget* widget) const override {
        if (type != CT_MenuItem) return QProxyStyle::sizeFromContents(type, option, size, widget);
        const auto* item = qstyleoption_cast<const QStyleOptionMenuItem*>(option);
        if (!item) return size;
        if (item->menuItemType == QStyleOptionMenuItem::Separator) return {size.width(), SeparatorHeight};
        const QFontMetrics metrics(item->font);
        const QString label = item->text.section(QLatin1Char('\t'), 0, 0);
        const QString shortcut = item->text.section(QLatin1Char('\t'), 1);
        int width = 12 + (item->menuHasCheckableItems ? CheckColumn : 0) + metrics.horizontalAdvance(label) + 28;
        if (!shortcut.isEmpty()) width += metrics.horizontalAdvance(shortcut) + 18;
        if (item->menuItemType == QStyleOptionMenuItem::SubMenu) width += 14;
        return {std::max(width, 168), std::max(ItemHeight, metrics.height() + 16)};
    }

    void drawPrimitive(PrimitiveElement element, const QStyleOption* option, QPainter* painter, const QWidget* widget) const override {
        if (element == PE_PanelMenu || element == PE_FrameMenu) return;
        QProxyStyle::drawPrimitive(element, option, painter, widget);
    }

    void drawControl(ControlElement element, const QStyleOption* option, QPainter* painter, const QWidget* widget) const override {
        if (element == CE_MenuEmptyArea) return;
        if (element != CE_MenuItem) { QProxyStyle::drawControl(element, option, painter, widget); return; }
        const auto* item = qstyleoption_cast<const QStyleOptionMenuItem*>(option);
        if (!item) return;
        const auto& theme = currentTheme();
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        const QRectF rect(item->rect);
        if (item->menuItemType == QStyleOptionMenuItem::Separator) {
            painter->setPen(QPen(withAlpha(theme.text, 0.09), 1));
            const double y = std::floor(rect.center().y()) + 0.5;
            painter->drawLine(QPointF(rect.left() + 10, y), QPointF(rect.right() - 10, y));
            painter->restore();
            return;
        }
        const bool enabled = item->state.testFlag(State_Enabled);
        const QColor color = enabled ? theme.text : withAlpha(theme.text, 0.36);
        double left = rect.left() + 12;
        if (item->menuHasCheckableItems) {
            if (item->checked) {
                const QPointF center(left + 7, rect.center().y());
                painter->setPen(QPen(enabled ? theme.accent : color, 1.7, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                painter->drawPolyline(QPolygonF{center + QPointF(-4.2, 0.2), center + QPointF(-1.3, 3.1), center + QPointF(4.6, -3.4)});
            }
            left += CheckColumn;
        }
        const QString label = item->text.section(QLatin1Char('\t'), 0, 0);
        const QString shortcut = item->text.section(QLatin1Char('\t'), 1);
        double right = rect.right() - 14;
        if (item->menuItemType == QStyleOptionMenuItem::SubMenu) {
            const QPointF tip(rect.right() - 13, rect.center().y());
            painter->setPen(QPen(withAlpha(color, enabled ? 0.7 : 0.36), 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            painter->drawPolyline(QPolygonF{tip + QPointF(-2.5, -4), tip + QPointF(1.5, 0), tip + QPointF(-2.5, 4)});
            right -= 14;
        }
        painter->setFont(item->font);
        if (!shortcut.isEmpty()) {
            painter->setPen(withAlpha(color, 0.5));
            painter->drawText(QRectF(left, rect.top(), right - left, rect.height()), Qt::AlignVCenter | Qt::AlignRight, shortcut);
            right -= QFontMetricsF(item->font).horizontalAdvance(shortcut) + 18;
        }
        painter->setPen(color);
        const QRectF text(left, rect.top(), std::max(0.0, right - left), rect.height());
        painter->drawText(text, Qt::AlignVCenter | Qt::AlignLeft | Qt::TextShowMnemonic,
                          QFontMetricsF(item->font).elidedText(label, Qt::ElideRight, text.width(), Qt::TextShowMnemonic));
        painter->restore();
    }
};

QStyle* menuStyle() {
    static QPointer<QStyle> style;
    if (!style) {
        style = new MenuStyle;
        style->setParent(qApp);
    }
    return style;
}
}

IslandMenu::IslandMenu(QWidget* parent) : QMenu(parent), motion_(this) { initialize(); }

IslandMenu::IslandMenu(const QString& title, QWidget* parent) : QMenu(title, parent), motion_(this) { initialize(); }

IslandMenu::~IslandMenu() { motion_.stopAll(); }

void IslandMenu::initialize() {
    setStyle(menuStyle());
    setWindowFlags(windowFlags() | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint);
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_Hover);
    QFont font(currentTheme().fontFamily);
    font.setPixelSize(13);
    font.setWeight(QFont::Medium);
    setFont(font);
    connect(this, &QMenu::hovered, this, [this] { update(); });
}

IslandMenu* IslandMenu::addIslandMenu(const QString& title) {
    auto* menu = new IslandMenu(title, this);
    addMenu(menu);
    return menu;
}

void IslandMenu::setTheme(const Theme& theme) { currentTheme() = theme; }
IslandMenu::Theme IslandMenu::theme() { return currentTheme(); }

QRectF IslandMenu::panelRect() const {
    return QRectF(rect()).adjusted(ShadowMargin, ShadowMargin - 2, -ShadowMargin, -ShadowMargin - 2);
}

void IslandMenu::trackHighlight() {
    QAction* active = activeAction();
    const QRectF next = active && !active->isSeparator() && active->isEnabled() && actionGeometry(active).isValid()
        ? QRectF(actionGeometry(active)).adjusted(0, 1, 0, -1) : QRectF();
    if (next == target_) return;
    target_ = next;
    const bool motion = currentTheme().motion && isVisible();
    if (!next.isValid()) {
        if (!motion) { highlightAlpha_ = 0; return; }
        motion_.start(QStringLiteral("alpha"), highlightAlpha_, 0, std::chrono::milliseconds(140),
                      [this](double value) { highlightAlpha_ = value; update(); });
        return;
    }
    if (!motion || highlightAlpha_ <= 0.01 || !highlight_.isValid()) {
        motion_.stop(QStringLiteral("slide"));
        highlight_ = next;
        if (!motion) { highlightAlpha_ = 1; return; }
    } else {
        origin_ = highlight_;
        motion_.start(QStringLiteral("slide"), 0, 1, std::chrono::milliseconds(150), [this](double value) {
            highlight_ = QRectF(origin_.topLeft() + (target_.topLeft() - origin_.topLeft()) * value,
                                origin_.size() + (target_.size() - origin_.size()) * value);
            update();
        });
    }
    motion_.start(QStringLiteral("alpha"), highlightAlpha_, 1, std::chrono::milliseconds(110),
                  [this](double value) { highlightAlpha_ = value; update(); });
}

void IslandMenu::paintEvent(QPaintEvent* event) {
    trackHighlight();
    {
        const auto& theme = currentTheme();
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QRectF panel = panelRect();
        const double ratio = devicePixelRatioF();
        const int blur = qRound(9 * ratio);
        const QImage shadow = Squircle::shadow((panel.size() * ratio).toSize(), PanelRadius * ratio, blur);
        const double spread = blur * 2 / ratio;
        painter.setOpacity(0.5);
        painter.drawImage(panel.adjusted(-spread, -spread + 4, spread, spread + 4), shadow);
        painter.setOpacity(1);
        const QPainterPath shape = Squircle::path(panel, PanelRadius);
        QColor background = theme.background;
        background.setAlphaF(0.975f);
        painter.fillPath(shape, background);
        const bool light = theme.background.lightnessF() > 0.6;
        painter.setPen(QPen(light ? QColor(0, 0, 0, 30) : QColor(255, 255, 255, 24), 1));
        painter.drawPath(Squircle::path(panel.adjusted(0.5, 0.5, -0.5, -0.5), PanelRadius - 0.5));
        if (highlightAlpha_ > 0.001 && highlight_.isValid()) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(withAlpha(mix(theme.background, theme.text, light ? 0.1 : 0.11), highlightAlpha_));
            painter.drawPath(Squircle::path(highlight_, ItemRadius));
        }
    }
    QMenu::paintEvent(event);
}

void IslandMenu::showEvent(QShowEvent* event) {
    QMenu::showEvent(event);
    target_ = {};
    highlight_ = {};
    highlightAlpha_ = 0;
    // Qt anchors the window corner at the pointer; anchor the visible panel instead.
    const QPoint cursor = QCursor::pos();
    const bool upward = cursor.y() > geometry().center().y();
    QPoint final = pos();
    if (!qobject_cast<QMenu*>(parentWidget())) {
        const bool leftward = cursor.x() > geometry().center().x();
        final += QPoint(leftward ? ShadowMargin : -ShadowMargin, upward ? ShadowMargin + 2 : -ShadowMargin + 2);
        move(final);
    }
    if (!currentTheme().motion) { setWindowOpacity(1); return; }
    // Fade in with a short drift away from the pointer, like a native flyout.
    const int drift = upward ? 6 : -6;
    setWindowOpacity(0);
    motion_.start(QStringLiteral("open"), 0, 1, std::chrono::milliseconds(170), QEasingCurve::OutCubic, [this, final, drift](double value) {
        setWindowOpacity(value);
        move(final + QPoint(0, qRound(drift * (1 - value))));
    });
}

void IslandMenu::hideEvent(QHideEvent* event) {
    motion_.stopAll();
    setWindowOpacity(1);
    QMenu::hideEvent(event);
}

void IslandMenu::changeEvent(QEvent* event) {
    if (event->type() == QEvent::StyleChange && style() != menuStyle()) setStyle(menuStyle());
    QMenu::changeEvent(event);
}

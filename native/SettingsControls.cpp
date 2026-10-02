#include "SettingsControls.h"
#include "SmoothScroll.h"

#include <QAbstractItemView>
#include <QAccessible>
#include <QAccessibleWidget>
#include <QApplication>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFocusEvent>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QKeyEvent>
#include <QListView>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QScreen>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSlider>
#include <QStyledItemDelegate>
#include <QStyleOptionSlider>

#include <algorithm>
#include <cmath>

namespace {
constexpr int MaximumSliderSteps = 1'000'000;
constexpr int MaximumDecimals = 12;
constexpr int ControlHeight = 38;
constexpr double CornerRadius = 8;
constexpr double SwitchWidth = 40;
constexpr double SwitchHeight = 22;
constexpr double SwitchInset = 3;
constexpr double SwitchMargin = 4;
constexpr int PopupPadding = 6;
constexpr int PopupGap = 6;
constexpr int PopupScreenMargin = 8;
constexpr int PopupMaximumRows = 10;
constexpr int PopupTravel = 6;
const QString ThumbTrack = QStringLiteral("thumb");
const QString NavigationTrack = QStringLiteral("navigation");
const QString PopupTrack = QStringLiteral("popup");

void installChoiceAccessibility();

QColor mix(const QColor& from, const QColor& to, double progress)
{
    progress = std::clamp(progress, 0.0, 1.0);
    return QColor::fromRgbF(
        from.redF() + (to.redF() - from.redF()) * progress,
        from.greenF() + (to.greenF() - from.greenF()) * progress,
        from.blueF() + (to.blueF() - from.blueF()) * progress,
        from.alphaF() + (to.alphaF() - from.alphaF()) * progress);
}

QColor contrasting(const QColor& background)
{
    const double luminance = background.redF() * 0.2126 + background.greenF() * 0.7152 + background.blueF() * 0.0722;
    return QColor(luminance > 0.55 ? "#121214" : "#FAFAFA");
}

bool visualEvent(QEvent::Type type)
{
    return type == QEvent::Enter || type == QEvent::Leave || type == QEvent::HoverEnter
        || type == QEvent::HoverLeave || type == QEvent::FocusIn || type == QEvent::FocusOut
        || type == QEvent::EnabledChange || type == QEvent::PaletteChange
        || type == QEvent::FontChange || type == QEvent::LayoutDirectionChange;
}

class TrackSlider final : public QSlider {
public:
    explicit TrackSlider(QWidget* parent) : QSlider(Qt::Horizontal, parent), motion_(this)
    {
        setAttribute(Qt::WA_Hover);
        setFocusPolicy(Qt::StrongFocus);
        setMinimumHeight(ControlHeight);
        connect(this, &QSlider::sliderPressed, this, [this] { emphasize(); });
        connect(this, &QSlider::sliderReleased, this, [this] { emphasize(); });
    }

    void setMotion(bool enabled) { motionEnabled_ = enabled; if (!enabled) { motion_.stopAll(); emphasis_ = target(); update(); } }

    void setColors(const QColor& accent, const QColor& track, const QColor& text)
    {
        accent_ = accent;
        track_ = track;
        text_ = text;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QStyleOptionSlider option;
        initStyleOption(&option);
        const QRect handle = style()->subControlRect(QStyle::CC_Slider, &option, QStyle::SC_SliderHandle, this);
        const double centerY = height() / 2.0;
        const double inset = std::max(7.0, handle.width() / 2.0);
        const double left = inset;
        const double right = std::max(left, width() - inset);
        const double centerX = std::clamp(static_cast<double>(handle.center().x()), left, right);
        const QColor color = isEnabled() ? accent_ : mix(track_, text_, 0.3);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(mix(track_, text_, 0.12));
        painter.drawRoundedRect(QRectF(left, centerY - 1.5, right - left, 3), 1.5, 1.5);
        const double fillLeft = option.upsideDown ? centerX : left;
        const double fillRight = option.upsideDown ? right : centerX;
        painter.setBrush(color);
        painter.drawRoundedRect(QRectF(fillLeft, centerY - 1.5, fillRight - fillLeft, 3), 1.5, 1.5);
        if (hasFocus()) {
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(mix(track_, color, 0.6), 1));
            painter.drawEllipse(QPointF(centerX, centerY), 10, 10);
            painter.setPen(Qt::NoPen);
        }
        const double emphasis = std::clamp(emphasis_, 0.0, 1.4);
        if (emphasis > 0.01 && isEnabled()) {
            QColor halo = color;
            halo.setAlphaF(0.16f * static_cast<float>(std::min(1.0, emphasis)));
            painter.setBrush(halo);
            painter.drawEllipse(QPointF(centerX, centerY), 6 + 6 * emphasis, 6 + 6 * emphasis);
        }
        painter.setBrush(color);
        const double knob = 6.0 + 1.6 * emphasis;
        painter.drawEllipse(QPointF(centerX, centerY), knob, knob);
    }

    bool event(QEvent* event) override
    {
        const bool result = QSlider::event(event);
        if (event->type() == QEvent::HoverEnter || event->type() == QEvent::HoverLeave || event->type() == QEvent::EnabledChange)
            emphasize();
        if (visualEvent(event->type()))
            update();
        return result;
    }

private:
    double target() const { return !isEnabled() ? 0.0 : isSliderDown() ? 1.0 : underMouse() ? 0.55 : 0.0; }

    void emphasize()
    {
        const double goal = target();
        if (!motionEnabled_ || !isVisible()) { motion_.stopAll(); emphasis_ = goal; update(); return; }
        if (motion_.retarget(QStringLiteral("emphasis"), goal)) return;
        motion_.spring(QStringLiteral("emphasis"), emphasis_, goal, {0.24, 0.72}, [this](double value) {
            emphasis_ = value;
            update();
        });
    }

    AnimationClock motion_;
    double emphasis_ = 0;
    bool motionEnabled_ = true;
    QColor accent_{"#F4F4F5"};
    QColor track_{"#333338"};
    QColor text_{"#F4F4F5"};
};

void configureChoice(QComboBox* choice)
{
    installChoiceAccessibility();
    choice->setAttribute(Qt::WA_Hover);
    choice->setFocusPolicy(Qt::StrongFocus);
    choice->setMinimumHeight(ControlHeight);
    choice->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
}

void styleChoice(QComboBox* choice, const QColor& accent, const QColor& track, const QColor& text)
{
    QPalette palette = choice->palette();
    palette.setColor(QPalette::Text, text);
    palette.setColor(QPalette::WindowText, text);
    palette.setColor(QPalette::Base, track);
    palette.setColor(QPalette::Button, track);
    palette.setColor(QPalette::ButtonText, text);
    palette.setColor(QPalette::Highlight, mix(track, accent, 0.18));
    palette.setColor(QPalette::HighlightedText, text);
    choice->setPalette(palette);
    choice->view()->setPalette(palette);
    choice->setStyleSheet(QStringLiteral(
        "QComboBox::drop-down { border: none; width: 32px; }"
        "QComboBox::down-arrow { image: none; }"
        "QComboBox QLineEdit { background: transparent; border: none; }"));
    choice->update();
}

void paintChoice(QComboBox* choice, const QColor& accent, const QColor& track, const QColor& text,
                 bool keyboardFocus, bool popupVisible)
{
    QPainter painter(choice);
    painter.setRenderHint(QPainter::Antialiasing);
    const bool enabled = choice->isEnabled();
    const QColor foreground = enabled ? text : mix(track, text, 0.4);
    const QColor surface = enabled && (choice->underMouse() || popupVisible) ? mix(track, text, 0.035) : track;
    const bool focused = keyboardFocus && choice->hasFocus();
    painter.setPen(QPen(focused ? mix(track, accent, 0.7) : mix(track, text, popupVisible ? 0.2 : 0.09), 1));
    painter.setBrush(surface);
    painter.drawRoundedRect(QRectF(choice->rect()).adjusted(0.5, 0.5, -0.5, -0.5), CornerRadius, CornerRadius);

    const bool rightToLeft = choice->layoutDirection() == Qt::RightToLeft;
    const double arrowX = rightToLeft ? 17.0 : choice->width() - 17.0;
    const double arrowY = choice->height() / 2.0;
    painter.setPen(QPen(foreground, 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    QPolygonF chevron;
    const double direction = popupVisible ? -1.0 : 1.0;
    chevron << QPointF(arrowX - 4, arrowY - 2 * direction) << QPointF(arrowX, arrowY + 2 * direction)
            << QPointF(arrowX + 4, arrowY - 2 * direction);
    painter.drawPolyline(chevron);

    if (choice->isEditable())
        return;
    QRect content = choice->rect().adjusted(rightToLeft ? 34 : 12, 0, rightToLeft ? -12 : -34, 0);
    const QIcon icon = choice->currentIndex() >= 0 ? choice->itemIcon(choice->currentIndex()) : QIcon{};
    if (!icon.isNull()) {
        const QSize size = choice->iconSize().boundedTo(QSize(22, 22));
        const int x = rightToLeft ? content.right() - size.width() : content.left();
        icon.paint(&painter, QRect(QPoint(x, (choice->height() - size.height()) / 2), size),
                   Qt::AlignCenter, enabled ? QIcon::Normal : QIcon::Disabled);
        if (rightToLeft)
            content.adjust(0, 0, -size.width() - 8, 0);
        else
            content.adjust(size.width() + 8, 0, 0, 0);
    }
    painter.setFont(choice->font());
    painter.setPen(foreground);
    const QString value = choice->currentIndex() < 0 ? choice->placeholderText() : choice->currentText();
    const QString elided = choice->fontMetrics().elidedText(value, Qt::ElideRight, std::max(0, content.width()));
    painter.drawText(content, Qt::AlignVCenter | (rightToLeft ? Qt::AlignRight : Qt::AlignLeft), elided);
}
}

namespace {
class ChoiceClosingFrame final : public QWidget {
public:
    explicit ChoiceClosingFrame(QWidget* parent)
        : QWidget(parent, Qt::ToolTip | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint
                  | Qt::WindowTransparentForInput | Qt::WindowDoesNotAcceptFocus)
    {
        setObjectName(QStringLiteral("settingsChoiceClosingFrame"));
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_ShowWithoutActivating);
        setAttribute(Qt::WA_TransparentForMouseEvents);
    }

    QPixmap image;

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.drawPixmap(0, 0, image);
    }
};

class ChoiceItemDelegate final : public QStyledItemDelegate {
public:
    explicit ChoiceItemDelegate(QComboBox* choice, QObject* parent)
        : QStyledItemDelegate(parent), choice_(choice) {}

    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override
    {
        return {100, std::max(32, choice_->fontMetrics().height() + 14)};
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override
    {
        const bool enabled = index.flags().testFlag(Qt::ItemIsEnabled);
        const bool selected = index.row() == choice_->currentIndex();
        const bool active = option.state.testFlag(QStyle::State_Selected)
            || option.state.testFlag(QStyle::State_MouseOver);
        const QColor track = option.palette.color(QPalette::Base);
        const QColor foreground = option.palette.color(QPalette::Text);
        const QRectF row = QRectF(option.rect).adjusted(0, 1, 0, -1);
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setPen(Qt::NoPen);
        if (active && enabled) {
            painter->setBrush(option.palette.color(QPalette::Highlight));
            painter->drawRoundedRect(row, 5, 5);
        } else if (selected) {
            painter->setBrush(mix(track, foreground, 0.055));
            painter->drawRoundedRect(row, 5, 5);
        }
        if (!enabled) painter->setOpacity(0.4);
        const bool rtl = option.direction == Qt::RightToLeft;
        QRect content = option.rect.adjusted(rtl ? 28 : 9, 0, rtl ? -9 : -28, 0);
        const QIcon icon = qvariant_cast<QIcon>(index.data(Qt::DecorationRole));
        if (!icon.isNull()) {
            const QSize size = choice_->iconSize().boundedTo(QSize(18, 18));
            const int x = rtl ? content.right() - size.width() + 1 : content.left();
            icon.paint(painter, QRect(x, content.center().y() - size.height() / 2, size.width(), size.height()));
            if (rtl) content.adjust(0, 0, -size.width() - 8, 0);
            else content.adjust(size.width() + 8, 0, 0, 0);
        }
        painter->setFont(choice_->font());
        painter->setPen(foreground);
        painter->drawText(content, Qt::AlignVCenter | Qt::TextSingleLine | (rtl ? Qt::AlignRight : Qt::AlignLeft),
            choice_->fontMetrics().elidedText(index.data(Qt::DisplayRole).toString(), Qt::ElideRight,
                                             std::max(0, content.width())));
        if (selected) {
            const double x = rtl ? option.rect.left() + 13 : option.rect.right() - 13;
            const double y = option.rect.center().y();
            painter->setPen(QPen(foreground, 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            painter->drawPolyline(QPolygonF{QPointF(x - 3, y), QPointF(x - 1, y + 2), QPointF(x + 4, y - 3)});
        }
        painter->restore();
    }

private:
    QComboBox* choice_;
};
}

class SettingsChoicePopup final : public QWidget {
public:
    explicit SettingsChoicePopup(QComboBox* choice)
        : QWidget(choice, Qt::Popup | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint),
          choice_(choice), animation_(this), closingFrame_(new ChoiceClosingFrame(choice))
    {
        setObjectName(QStringLiteral("settingsChoicePopup"));
        setAttribute(Qt::WA_TranslucentBackground);
        setFocusPolicy(Qt::NoFocus);
        list_ = new QListView(this);
        list_->setObjectName(QStringLiteral("settingsChoiceList"));
        list_->setItemDelegate(new ChoiceItemDelegate(choice, list_));
        list_->setFrameShape(QFrame::NoFrame);
        list_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        list_->setSelectionMode(QAbstractItemView::SingleSelection);
        list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        list_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        list_->setUniformItemSizes(true);
        list_->setMouseTracking(true);
        list_->installEventFilter(this);
        list_->viewport()->installEventFilter(this);
        SmoothScroll::install(list_);
        connect(list_, &QListView::clicked, this, [this](const QModelIndex& index) { accept(index); });
        connect(list_, &QListView::entered, this, [this](const QModelIndex& index) {
            if (selectable(index)) list_->setCurrentIndex(index);
        });
        connect(choice_, &QComboBox::currentIndexChanged, this, [this] {
            if (isVisible()) closePopup(false);
        });
        connect(qApp, &QGuiApplication::fontDatabaseChanged, this, [this] { closePopup(false); });
    }

    ~SettingsChoicePopup() override
    {
        animation_.stopAll();
        delete closingFrame_;
    }

    void setMotion(bool enabled, int durationMs, double refreshRate)
    {
        motionEnabled_ = enabled;
        durationMs_ = std::clamp(durationMs, 0, 10000);
        animation_.setRefreshRate(refreshRate);
        SmoothScroll::setMotion(list_, enabled && durationMs_ > 0);
        if (!enabled || durationMs_ == 0) {
            animation_.stopAll();
            closingFrame_->hide();
            if (isVisible()) setProgress(1);
        }
    }

    void setColors(const QColor& accent, const QColor& track, const QColor& text)
    {
        track_ = mix(track, text, 0.025);
        border_ = mix(track, text, 0.14);
        QPalette colors = choice_->palette();
        colors.setColor(QPalette::Base, track_);
        colors.setColor(QPalette::Text, text);
        colors.setColor(QPalette::Highlight, mix(track_, accent, 0.13));
        colors.setColor(QPalette::HighlightedText, text);
        list_->setPalette(colors);
        list_->setStyleSheet(QStringLiteral(
            "QListView { background: transparent; border: none; outline: 0; padding: 0; }"
            "QScrollBar:vertical { background: transparent; width: 7px; margin: 2px 0; }"
            "QScrollBar::handle:vertical { background: %1; border-radius: 3px; min-height: 28px; }"
            "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }"
            "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: transparent; }")
            .arg(mix(track, text, 0.2).name()));
        SmoothScroll::setColor(list_, text);
        update();
    }

    void openPopup()
    {
        if (!choice_->isVisible() || !choice_->isEnabled() || choice_->count() == 0) return;
        if (isVisible()) return;
        if (active_ && active_ != this) active_->closePopup(false);
        if (closing_ && closing_ != this) closing_->closePopup(false);
        active_ = this;
        animation_.stopAll();
        closingFrame_->hide();
        if (closing_ == this) closing_.clear();
        list_->setFont(choice_->font());
        list_->setLayoutDirection(choice_->layoutDirection());
        list_->setAccessibleName(choice_->accessibleName());
        list_->setModel(choice_->model());
        list_->setModelColumn(choice_->modelColumn());
        list_->setRootIndex(choice_->rootModelIndex());
        list_->setEnabled(true);
        const QModelIndex current = choice_->model()->index(choice_->currentIndex(), choice_->modelColumn(), choice_->rootModelIndex());
        list_->setCurrentIndex(current);
        connections_.append(connect(list_->selectionModel(), &QItemSelectionModel::currentChanged, this,
            [this](const QModelIndex& index) {
                if (!selectable(index)) return;
                QPointer<QComboBox> choice = choice_;
                const QString text = index.data(Qt::DisplayRole).toString();
                emit choice->highlighted(index.row());
                if (choice) emit choice->textHighlighted(text);
            }));
        auto* model = choice_->model();
        connections_.append(connect(model, &QObject::destroyed, this, [this] { closePopup(false); }));
        connections_.append(connect(model, &QAbstractItemModel::modelAboutToBeReset, this, [this] { closePopup(false); }));
        connections_.append(connect(model, &QAbstractItemModel::rowsAboutToBeRemoved, this, [this] { closePopup(false); }));
        connections_.append(connect(model, &QAbstractItemModel::rowsAboutToBeInserted, this, [this] { closePopup(false); }));
        connections_.append(connect(model, &QAbstractItemModel::layoutAboutToBeChanged, this, [this] { closePopup(false); }));
        observedWindow_ = choice_->window();
        observedWindow_->installEventFilter(this);
        placePopup();
        progress_ = motionEnabled_ && durationMs_ > 0 ? 0 : 1;
        setProgress(progress_);
        show();
        list_->doItemsLayout();
        list_->scrollTo(current, QAbstractItemView::PositionAtCenter);
        const int rowHeight = list_->visualRect(current).height();
        if (rowHeight > 0) {
            auto* scroll = list_->verticalScrollBar();
            scroll->setValue(scroll->value() - scroll->value() % rowHeight);
        }
        list_->setFocus(Qt::PopupFocusReason);
        choice_->update();
        QAccessibleEvent opened(list_, QAccessible::PopupMenuStart);
        QAccessible::updateAccessibility(&opened);
        notifyExpandedState();
        if (progress_ < 1) {
            animation_.start(PopupTrack, progress_, 1, std::chrono::milliseconds(durationMs_),
                             [this](double value) { setProgress(value); });
        }
    }

    void closePopup(bool animate)
    {
        suppressCloseAnimation_ = !animate;
        if (isVisible()) hide();
        if (!animate) {
            animation_.stopAll();
            closingFrame_->hide();
            if (closing_ == this) closing_.clear();
        }
        suppressCloseAnimation_ = false;
    }

protected:
    void mousePressEvent(QMouseEvent* event) override
    {
        const QRect field(choice_->mapToGlobal(QPoint(0, 0)), choice_->size());
        setAttribute(Qt::WA_NoMouseReplay, field.contains(event->globalPosition().toPoint()));
        QWidget::mousePressEvent(event);
    }

    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(border_, 1));
        painter.setBrush(track_);
        painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), CornerRadius, CornerRadius);
    }

    void resizeEvent(QResizeEvent* event) override
    {
        QWidget::resizeEvent(event);
        list_->setGeometry(rect().adjusted(PopupPadding, PopupPadding, -PopupPadding, -PopupPadding));
    }

    void hideEvent(QHideEvent* event) override
    {
        animation_.stopAll();
        for (const auto& connection : connections_) disconnect(connection);
        connections_.clear();
        if (observedWindow_) observedWindow_->removeEventFilter(this);
        observedWindow_.clear();
        if (active_ == this) active_.clear();
        choice_->QComboBox::hidePopup();
        choice_->update();
        QAccessibleEvent closed(list_, QAccessible::PopupMenuEnd);
        QAccessible::updateAccessibility(&closed);
        notifyExpandedState();
        if (!suppressCloseAnimation_ && motionEnabled_ && durationMs_ > 0 && choice_->isVisible() && progress_ > 0) {
            // Release the popup grab first, then fade a snapshot that cannot intercept input.
            closing_ = this;
            closingFrame_->image = grab();
            closingFrame_->setGeometry(geometry());
            closingFrame_->setWindowOpacity(progress_);
            closingFrame_->show();
            const QRect origin = geometry();
            const double opacity = progress_;
            animation_.start(PopupTrack, 0, 1, std::chrono::milliseconds(std::max(1, durationMs_ * 2 / 3)),
                [this, origin, opacity](double value) {
                    closingFrame_->setWindowOpacity(opacity * (1 - value));
                    closingFrame_->move(origin.topLeft() + QPoint(0, qRound((above_ ? PopupTravel : -PopupTravel) * value)));
                }, [this] {
                    closingFrame_->hide();
                    closingFrame_->image = {};
                    if (closing_ == this) closing_.clear();
                });
        }
        QWidget::hideEvent(event);
    }

    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (watched == observedWindow_ && (event->type() == QEvent::Hide || event->type() == QEvent::Close
            || event->type() == QEvent::Move || event->type() == QEvent::Resize || event->type() == QEvent::WindowStateChange)) {
            closePopup(false);
        }
        if (watched == list_ && event->type() == QEvent::KeyPress) {
            const auto* key = static_cast<QKeyEvent*>(event);
            if (key->key() == Qt::Key_Escape || key->key() == Qt::Key_F4
                || (key->key() == Qt::Key_Up && key->modifiers().testFlag(Qt::AltModifier))) {
                closePopup(true);
                return true;
            }
            if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter || key->key() == Qt::Key_Space) {
                accept(list_->currentIndex());
                return true;
            }
            if (key->key() == Qt::Key_Tab || key->key() == Qt::Key_Backtab) {
                closePopup(true);
                QKeyEvent next(QEvent::KeyPress, key->key(), key->modifiers());
                QCoreApplication::sendEvent(choice_, &next);
                return true;
            }
        }
        return QWidget::eventFilter(watched, event);
    }

private:
    void notifyExpandedState()
    {
        QAccessible::State changes;
        changes.expanded = true;
        changes.collapsed = true;
        QAccessibleStateChangeEvent event(choice_, changes);
        QAccessible::updateAccessibility(&event);
    }

    static bool selectable(const QModelIndex& index)
    {
        return index.isValid() && index.flags().testFlag(Qt::ItemIsEnabled) && index.flags().testFlag(Qt::ItemIsSelectable);
    }

    void accept(const QModelIndex& index)
    {
        if (!selectable(index) || !isVisible()) return;
        QPointer<QComboBox> choice = choice_;
        const int row = index.row();
        const QString text = index.data(Qt::DisplayRole).toString();
        closePopup(true);
        choice->setCurrentIndex(row);
        if (!choice) return;
        emit choice->activated(row);
        if (choice) emit choice->textActivated(text);
    }

    void placePopup()
    {
        const QRect field(choice_->mapToGlobal(QPoint(0, 0)), choice_->size());
        QScreen* screen = QGuiApplication::screenAt(field.center());
        if (!screen) screen = choice_->screen();
        const QRect available = screen->availableGeometry().adjusted(PopupScreenMargin, PopupScreenMargin,
                                                                    -PopupScreenMargin, -PopupScreenMargin);
        const int rowHeight = std::max(32, choice_->fontMetrics().height() + 14);
        const int rows = std::min({choice_->count(), std::max(1, choice_->maxVisibleItems()), PopupMaximumRows});
        const int desiredHeight = rows * rowHeight + PopupPadding * 2;
        const int below = std::max(0, available.bottom() - field.bottom() - PopupGap);
        const int above = std::max(0, field.top() - PopupGap - available.top());
        above_ = below < desiredHeight && above > below;
        const int height = std::max(1, std::min(desiredHeight, above_ ? above : below));
        const int width = std::min(choice_->width(), available.width());
        const int x = std::clamp(field.left(), available.left(), available.right() - width + 1);
        const int y = above_ ? field.top() - PopupGap - height : field.bottom() + PopupGap + 1;
        target_ = QRect(x, std::clamp(y, available.top(), available.bottom() - height + 1), width, height);
        setGeometry(target_);
    }

    void setProgress(double value)
    {
        progress_ = value;
        setWindowOpacity(value);
        move(target_.topLeft() + QPoint(0, qRound((above_ ? PopupTravel : -PopupTravel) * (1 - value))));
    }

    inline static QPointer<SettingsChoicePopup> active_;
    inline static QPointer<SettingsChoicePopup> closing_;
    QComboBox* choice_;
    AnimationClock animation_;
    ChoiceClosingFrame* closingFrame_;
    QListView* list_ = nullptr;
    QPointer<QWidget> observedWindow_;
    QList<QMetaObject::Connection> connections_;
    QRect target_;
    QColor track_{"#1E1E21"};
    QColor border_{"#36363A"};
    bool motionEnabled_ = true;
    bool suppressCloseAnimation_ = false;
    bool above_ = false;
    int durationMs_ = 180;
    double progress_ = 0;
};

namespace {
class AccessibleSettingsPopup final : public QAccessibleWidget {
public:
    explicit AccessibleSettingsPopup(QWidget* popup) : QAccessibleWidget(popup, QAccessible::PopupMenu) {}
    QAccessibleInterface* parent() const override
    {
        return QAccessible::queryAccessibleInterface(widget()->parentWidget());
    }
};

class AccessibleSettingsChoice final : public QAccessibleWidget {
public:
    explicit AccessibleSettingsChoice(QComboBox* choice) : QAccessibleWidget(choice, QAccessible::ComboBox) {}

    QString text(QAccessible::Text type) const override
    {
        if (type == QAccessible::Value) return choice()->currentText();
        return QAccessibleWidget::text(type);
    }

    QAccessible::State state() const override
    {
        auto result = QAccessibleWidget::state();
        const auto* list = popupList();
        result.hasPopup = true;
        result.expandable = true;
        result.expanded = list && list->isVisible();
        result.collapsed = !result.expanded;
        return result;
    }

    int childCount() const override { return popup() ? 1 : 0; }
    QAccessibleInterface* child(int index) const override
    {
        return index == 0 ? QAccessible::queryAccessibleInterface(popup()) : nullptr;
    }
    int indexOfChild(const QAccessibleInterface* candidate) const override
    {
        return candidate && candidate->object() == popup() ? 0 : -1;
    }
    QAccessibleInterface* childAt(int x, int y) const override
    {
        auto* list = child(0);
        return list && !list->state().invisible && list->rect().contains(x, y) ? list : nullptr;
    }
    QAccessibleInterface* focusChild() const override
    {
        auto* list = QAccessible::queryAccessibleInterface(popupList());
        return list && !list->state().invisible ? list : nullptr;
    }

    QStringList actionNames() const override { return {showMenuAction(), setFocusAction()}; }
    void doAction(const QString& action) override
    {
        if (!choice()->isEnabled()) return;
        if (action == showMenuAction()) {
            if (state().expanded) choice()->hidePopup();
            else choice()->showPopup();
        } else {
            QAccessibleWidget::doAction(action);
        }
    }
    QStringList keyBindingsForAction(const QString& action) const override
    {
        return action == showMenuAction() ? QStringList{QStringLiteral("Alt+Down"), QStringLiteral("F4")}
                                         : QAccessibleWidget::keyBindingsForAction(action);
    }

private:
    QComboBox* choice() const { return static_cast<QComboBox*>(widget()); }
    QWidget* popup() const { return widget()->findChild<QWidget*>(QStringLiteral("settingsChoicePopup")); }
    QListView* popupList() const { return widget()->findChild<QListView*>(QStringLiteral("settingsChoiceList")); }
};

void installChoiceAccessibility()
{
    static const bool installed = [] {
        QAccessible::installFactory([](const QString& name, QObject* object) -> QAccessibleInterface* {
            if (name == QStringLiteral("SettingsChoice") || name == QStringLiteral("SettingsFontChoice"))
                return new AccessibleSettingsChoice(static_cast<QComboBox*>(object));
            if (name == QStringLiteral("QWidget") && object->objectName() == QStringLiteral("settingsChoicePopup"))
                return new AccessibleSettingsPopup(static_cast<QWidget*>(object));
            return nullptr;
        });
        return true;
    }();
    Q_UNUSED(installed);
}

bool handleChoiceEvent(QComboBox* choice, SettingsChoicePopup* popup, bool& keyboardFocus, QEvent* event)
{
    if (!popup) return false;
    if (event->type() == QEvent::FocusIn) {
        const auto reason = static_cast<QFocusEvent*>(event)->reason();
        if (reason != Qt::PopupFocusReason)
            keyboardFocus = reason == Qt::TabFocusReason || reason == Qt::BacktabFocusReason || reason == Qt::ShortcutFocusReason;
    } else if (event->type() == QEvent::FocusOut) {
        if (static_cast<QFocusEvent*>(event)->reason() != Qt::PopupFocusReason) keyboardFocus = false;
    } else if (event->type() == QEvent::MouseButtonPress) {
        keyboardFocus = false;
        const auto* mouse = static_cast<QMouseEvent*>(event);
        if (choice->isEnabled() && mouse->button() == Qt::LeftButton) {
            choice->setFocus(Qt::MouseFocusReason);
            if (popup->isVisible()) choice->hidePopup();
            else choice->showPopup();
            return true;
        }
    } else if (event->type() == QEvent::MouseButtonRelease) {
        if (static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton) return true;
    } else if (event->type() == QEvent::KeyPress) {
        keyboardFocus = true;
        const auto* key = static_cast<QKeyEvent*>(event);
        if (choice->isEnabled() && (key->key() == Qt::Key_F4 || key->key() == Qt::Key_Space
            || (key->key() == Qt::Key_Down && key->modifiers().testFlag(Qt::AltModifier)))) {
            if (popup->isVisible()) choice->hidePopup();
            else choice->showPopup();
            return true;
        }
    } else if (event->type() == QEvent::Hide || event->type() == QEvent::Close || event->type() == QEvent::FontChange
        || event->type() == QEvent::Move || event->type() == QEvent::Resize || event->type() == QEvent::LayoutDirectionChange
        || (event->type() == QEvent::EnabledChange && !choice->isEnabled())) {
        popup->closePopup(false);
    }
    return false;
}
}

class SettingsNavigationDelegate final : public QStyledItemDelegate {
public:
    explicit SettingsNavigationDelegate(SettingsNavigation* navigation)
        : QStyledItemDelegate(navigation), navigation_(navigation) {}

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override
    {
        QStyleOptionViewItem item(option);
        initStyleOption(&item, index);
        const auto* navigation = navigation_;
        const QRectF overlap = navigation->pill_.intersected(item.rect);
        const double highlight = item.rect.height() > 0 ? overlap.height() / item.rect.height() : 0;
        QColor foreground = mix(mix(navigation->track_, navigation->text_, 0.64), navigation->accent_, highlight);
        if (item.state.testFlag(QStyle::State_MouseOver))
            foreground = mix(foreground, navigation->text_, 0.45);
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        if (!item.state.testFlag(QStyle::State_Enabled)) painter->setOpacity(0.4);
        if (navigation->keyboardFocus_ && item.state.testFlag(QStyle::State_HasFocus)) {
            painter->setPen(QPen(mix(navigation->track_, navigation->accent_, 0.55), 1));
            painter->setBrush(Qt::NoBrush);
            painter->drawRoundedRect(QRectF(item.rect).adjusted(0.5, 0.5, -0.5, -0.5), CornerRadius, CornerRadius);
        }
        const bool rtl = item.direction == Qt::RightToLeft;
        QRect content = item.rect.adjusted(11, 0, -11, 0);
        if (!item.icon.isNull()) {
            const QSize size = item.decorationSize.boundedTo(content.size());
            QPixmap icon = item.icon.pixmap(size, navigation->devicePixelRatioF());
            QPainter tint(&icon);
            tint.setCompositionMode(QPainter::CompositionMode_SourceIn);
            tint.fillRect(icon.rect(), foreground);
            tint.end();
            const int x = rtl ? content.right() - size.width() + 1 : content.left();
            painter->drawPixmap(QRect(x, content.center().y() - size.height() / 2, size.width(), size.height()), icon);
            if (rtl) content.adjust(0, 0, -size.width() - 10, 0);
            else content.adjust(size.width() + 10, 0, 0, 0);
        }
        painter->setFont(item.font);
        painter->setPen(foreground);
        painter->drawText(content, Qt::AlignVCenter | (rtl ? Qt::AlignRight : Qt::AlignLeft),
            QFontMetrics(item.font).elidedText(item.text, Qt::ElideRight, std::max(0, content.width())));
        painter->restore();
    }

private:
    SettingsNavigation* navigation_;
};

SettingsNavigation::SettingsNavigation(QWidget* parent) : QListWidget(parent), animation_(this)
{
    setItemDelegate(new SettingsNavigationDelegate(this));
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setFrameShape(QFrame::NoFrame);
}

void SettingsNavigation::setMotion(bool enabled, int durationMs, double refreshRate)
{
    animation_.setRefreshRate(refreshRate);
    durationMs = std::clamp(durationMs, 0, 10000);
    if (motionEnabled_ == enabled && durationMs_ == durationMs) return;
    motionEnabled_ = enabled;
    durationMs_ = durationMs;
    if (!enabled || durationMs == 0) settlePill();
}

void SettingsNavigation::setColors(const QColor& accent, const QColor& track, const QColor& text)
{
    if (!accent.isValid() || !track.isValid() || !text.isValid()) return;
    if (accent_ == accent && track_ == track && text_ == text) return;
    accent_ = accent;
    track_ = track;
    text_ = text;
    viewport()->update();
}

void SettingsNavigation::settlePill()
{
    animation_.stopAll();
    target_ = currentItem() ? visualItemRect(currentItem()) : QRect{};
    pill_ = target_;
    viewport()->update();
}

void SettingsNavigation::movePill(bool animate)
{
    const QRectF destination = currentItem() ? visualItemRect(currentItem()) : QRect{};
    if (target_ == destination && pill_.isValid()) return;
    animation_.stopAll();
    target_ = destination;
    if (!animate || !motionEnabled_ || durationMs_ == 0 || !isVisible() || !pill_.isValid() || !target_.isValid()) {
        pill_ = target_;
        viewport()->update();
        return;
    }
    const QRectF origin = pill_;
    animation_.start(NavigationTrack, 0, 1, std::chrono::milliseconds(durationMs_), [this, origin, destination](double value) {
        pill_ = QRectF(origin.topLeft() + (destination.topLeft() - origin.topLeft()) * value,
            origin.size() + (destination.size() - origin.size()) * value);
        viewport()->update();
    });
}

void SettingsNavigation::currentChanged(const QModelIndex& current, const QModelIndex& previous)
{
    QListWidget::currentChanged(current, previous);
    movePill(!signalsBlocked());
}

void SettingsNavigation::paintEvent(QPaintEvent* event)
{
    const QRectF destination = currentItem() ? visualItemRect(currentItem()) : QRect{};
    if (target_ != destination) settlePill();
    {
        QPainter painter(viewport());
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        if (!isEnabled()) painter.setOpacity(0.4);
        painter.setBrush(track_);
        painter.drawRoundedRect(pill_, CornerRadius, CornerRadius);
    }
    QListWidget::paintEvent(event);
}

void SettingsNavigation::resizeEvent(QResizeEvent* event)
{
    QListWidget::resizeEvent(event);
    settlePill();
}

void SettingsNavigation::scrollContentsBy(int dx, int dy)
{
    QListWidget::scrollContentsBy(dx, dy);
    settlePill();
}

void SettingsNavigation::hideEvent(QHideEvent* event)
{
    settlePill();
    QListWidget::hideEvent(event);
}

void SettingsNavigation::focusInEvent(QFocusEvent* event)
{
    keyboardFocus_ = event->reason() == Qt::TabFocusReason || event->reason() == Qt::BacktabFocusReason;
    QListWidget::focusInEvent(event);
    viewport()->update();
}

void SettingsNavigation::focusOutEvent(QFocusEvent* event)
{
    keyboardFocus_ = false;
    QListWidget::focusOutEvent(event);
    viewport()->update();
}

SettingsSlider::SettingsSlider(QWidget* parent) : QWidget(parent)
{
    slider_ = new TrackSlider(this);
    editor_ = new QDoubleSpinBox(this);
    editor_->setButtonSymbols(QAbstractSpinBox::NoButtons);
    editor_->setKeyboardTracking(false);
    editor_->setAlignment(Qt::AlignRight);
    editor_->setAccelerated(true);
    editor_->setRange(0, 100);
    editor_->setDecimals(0);
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(16);
    layout->addWidget(slider_, 1);
    layout->addWidget(editor_);
    setMinimumWidth(200);
    setFocusProxy(slider_);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    connect(this, &QObject::objectNameChanged, this, [this](const QString& name) {
        slider_->setObjectName(name + ".slider");
        editor_->setObjectName(name + ".editor");
    });
    setObjectName("settingsSlider");
    connect(editor_, &QDoubleSpinBox::valueChanged, this, [this](double value) {
        syncSlider();
        emit valueChanged(value);
    });
    connect(slider_, &QSlider::valueChanged, this, [this](int position) {
        const double span = editor_->maximum() - editor_->minimum();
        const double progress = slider_->maximum() > 0 ? static_cast<double>(position) / slider_->maximum() : 0;
        editor_->setValue(editor_->minimum() + span * progress);
    });
    setColors(QColor("#F4F4F5"), QColor("#333338"), QColor("#F4F4F5"));
    rebuildSlider();
}

void SettingsSlider::setRange(double minimum, double maximum)
{
    if (!std::isfinite(minimum) || !std::isfinite(maximum) || minimum > maximum || !std::isfinite(maximum - minimum))
        return;
    const double before = value();
    {
        const QSignalBlocker blocker(editor_);
        editor_->setRange(minimum, maximum);
    }
    rebuildSlider();
    if (value() != before)
        emit valueChanged(value());
}

void SettingsSlider::setDecimals(int decimals)
{
    decimals = std::clamp(decimals, 0, MaximumDecimals);
    if (editor_->decimals() == decimals)
        return;
    const double before = value();
    {
        const QSignalBlocker blocker(editor_);
        editor_->setDecimals(decimals);
    }
    rebuildSlider();
    if (value() != before)
        emit valueChanged(value());
}

void SettingsSlider::setSingleStep(double step)
{
    if (!std::isfinite(step) || step <= 0)
        return;
    editor_->setSingleStep(step);
    rebuildSlider();
}

void SettingsSlider::setSuffix(const QString& suffix)
{
    editor_->setSuffix(suffix);
    updateEditorWidth();
}

void SettingsSlider::setAccessibleName(const QString& name)
{
    QWidget::setAccessibleName(name);
    updateAccessibleNames();
}

void SettingsSlider::setColors(const QColor& accent, const QColor& track, const QColor& text)
{
    if (!accent.isValid() || !track.isValid() || !text.isValid())
        return;
    if (accent_ == accent && track_ == track && text_ == text)
        return;
    accent_ = accent;
    track_ = track;
    text_ = text;
    static_cast<TrackSlider*>(slider_)->setColors(accent, track, text);
    QPalette palette = editor_->palette();
    palette.setColor(QPalette::Base, track);
    palette.setColor(QPalette::Text, text);
    palette.setColor(QPalette::Disabled, QPalette::Text, mix(track, text, 0.4));
    editor_->setPalette(palette);
    editor_->setStyleSheet(QStringLiteral(
        "QDoubleSpinBox { background: %5; color: %1; border: 1px solid %2; border-radius: 8px;"
        "padding: 6px 8px; min-height: 22px; }"
        "QDoubleSpinBox:focus { border-color: %3; }"
        "QDoubleSpinBox:disabled { color: %4; }")
        .arg(text.name(), mix(track, text, 0.08).name(), mix(track, accent, 0.7).name(), mix(track, text, 0.4).name(), track.name()));
    updateEditorWidth();
}

void SettingsSlider::setMotion(bool enabled) { static_cast<TrackSlider*>(slider_)->setMotion(enabled); }

double SettingsSlider::value() const { return editor_->value(); }
QDoubleSpinBox* SettingsSlider::editor() const { return editor_; }
QSlider* SettingsSlider::slider() const { return slider_; }

void SettingsSlider::setValue(double value)
{
    if (std::isfinite(value) && value != editor_->value())
        editor_->setValue(value);
}

void SettingsSlider::rebuildSlider()
{
    const double span = editor_->maximum() - editor_->minimum();
    const double scale = std::pow(10.0, std::min(editor_->decimals(), 6));
    const double desiredSteps = span * scale;
    const int steps = span > 0 ? static_cast<int>(std::clamp(std::round(desiredSteps), 1.0, static_cast<double>(MaximumSliderSteps))) : 0;
    const QSignalBlocker blocker(slider_);
    slider_->setRange(0, steps);
    if (steps > 0) {
        const double desiredStep = editor_->singleStep() / span * steps;
        const int singleStep = static_cast<int>(std::clamp(std::round(desiredStep), 1.0, static_cast<double>(steps)));
        slider_->setSingleStep(singleStep);
        slider_->setPageStep(std::max(singleStep, steps / 10));
    }
    slider_->setEnabled(steps > 0);
    syncSlider();
    updateEditorWidth();
}

void SettingsSlider::syncSlider()
{
    const double span = editor_->maximum() - editor_->minimum();
    const double progress = span > 0 ? (editor_->value() - editor_->minimum()) / span : 0;
    const QSignalBlocker blocker(slider_);
    slider_->setValue(qRound(progress * slider_->maximum()));
}

void SettingsSlider::updateEditorWidth()
{
    const auto number = [this](double value) { return editor_->locale().toString(value, 'f', editor_->decimals()) + editor_->suffix(); };
    const int textWidth = std::max(editor_->fontMetrics().horizontalAdvance(number(editor_->minimum())),
                                   editor_->fontMetrics().horizontalAdvance(number(editor_->maximum())));
    editor_->setFixedWidth(std::max(82, textWidth + 24));
}

void SettingsSlider::updateAccessibleNames()
{
    const QString name = accessibleName().isEmpty() ? objectName() : accessibleName();
    slider_->setAccessibleName(name);
    editor_->setAccessibleName(name + QStringLiteral(" - точное значение"));
}

bool SettingsSlider::event(QEvent* event)
{
    const bool result = QWidget::event(event);
    if (!slider_ || !editor_)
        return result;
    if (event->type() == QEvent::Polish || event->type() == QEvent::Show) {
        updateAccessibleNames();
        updateEditorWidth();
    } else if (event->type() == QEvent::FontChange || event->type() == QEvent::LocaleChange) {
        updateEditorWidth();
    }
    return result;
}

SettingsChoice::SettingsChoice(QWidget* parent) : QComboBox(parent)
{
    configureChoice(this);
    popup_ = new SettingsChoicePopup(this);
    styleChoice(this, accent_, track_, text_);
    popup_->setColors(accent_, track_, text_);
}

SettingsChoice::~SettingsChoice() { delete popup_; popup_ = nullptr; }
void SettingsChoice::setMotion(bool enabled, int durationMs, double refreshRate) { popup_->setMotion(enabled, durationMs, refreshRate); }
void SettingsChoice::showPopup() { popup_->openPopup(); }
void SettingsChoice::hidePopup() { popup_->closePopup(true); QComboBox::hidePopup(); }
void SettingsChoice::setModel(QAbstractItemModel* model)
{
    if (popup_) popup_->closePopup(false);
    QComboBox::setModel(model);
}

void SettingsChoice::setColors(const QColor& accent, const QColor& track, const QColor& text)
{
    if (!accent.isValid() || !track.isValid() || !text.isValid())
        return;
    if (accent_ == accent && track_ == track && text_ == text)
        return;
    accent_ = accent;
    track_ = track;
    text_ = text;
    styleChoice(this, accent_, track_, text_);
    popup_->setColors(accent_, track_, text_);
}

QSize SettingsChoice::sizeHint() const { return QComboBox::sizeHint().expandedTo(QSize(100, ControlHeight)); }
QSize SettingsChoice::minimumSizeHint() const { return QComboBox::minimumSizeHint().expandedTo(QSize(100, ControlHeight)); }
void SettingsChoice::paintEvent(QPaintEvent*) { paintChoice(this, accent_, track_, text_, keyboardFocus_, popup_->isVisible()); }

bool SettingsChoice::event(QEvent* event)
{
    if (handleChoiceEvent(this, popup_, keyboardFocus_, event)) { update(); return true; }
    const bool result = QComboBox::event(event);
    if (visualEvent(event->type()))
        update();
    return result;
}

SettingsFontChoice::SettingsFontChoice(QWidget* parent) : QFontComboBox(parent)
{
    configureChoice(this);
    setEditable(false);
    popup_ = new SettingsChoicePopup(this);
    styleChoice(this, accent_, track_, text_);
    popup_->setColors(accent_, track_, text_);
}

SettingsFontChoice::~SettingsFontChoice() { delete popup_; popup_ = nullptr; }
void SettingsFontChoice::setMotion(bool enabled, int durationMs, double refreshRate) { popup_->setMotion(enabled, durationMs, refreshRate); }
void SettingsFontChoice::showPopup() { popup_->openPopup(); }
void SettingsFontChoice::hidePopup() { popup_->closePopup(true); QFontComboBox::hidePopup(); }
void SettingsFontChoice::setModel(QAbstractItemModel* model)
{
    if (popup_) popup_->closePopup(false);
    QFontComboBox::setModel(model);
}

void SettingsFontChoice::setColors(const QColor& accent, const QColor& track, const QColor& text)
{
    if (!accent.isValid() || !track.isValid() || !text.isValid())
        return;
    if (accent_ == accent && track_ == track && text_ == text)
        return;
    accent_ = accent;
    track_ = track;
    text_ = text;
    styleChoice(this, accent_, track_, text_);
    popup_->setColors(accent_, track_, text_);
}

QSize SettingsFontChoice::sizeHint() const { return QFontComboBox::sizeHint().expandedTo(QSize(100, ControlHeight)); }
QSize SettingsFontChoice::minimumSizeHint() const { return QFontComboBox::minimumSizeHint().expandedTo(QSize(100, ControlHeight)); }
void SettingsFontChoice::paintEvent(QPaintEvent*) { paintChoice(this, accent_, track_, text_, keyboardFocus_, popup_->isVisible()); }

bool SettingsFontChoice::event(QEvent* event)
{
    if (handleChoiceEvent(this, popup_, keyboardFocus_, event)) { update(); return true; }
    const bool result = QFontComboBox::event(event);
    if (visualEvent(event->type()))
        update();
    return result;
}

SettingsToggle::SettingsToggle(QWidget* parent) : SettingsToggle(QString{}, parent) {}

SettingsToggle::SettingsToggle(const QString& text, QWidget* parent)
    : QCheckBox(text, parent), animation_(this)
{
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_Hover);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    setMinimumWidth(static_cast<int>(SwitchWidth + SwitchMargin * 2));
    setMinimumHeight(static_cast<int>(SwitchHeight + SwitchMargin * 2));
}

void SettingsToggle::setMotion(bool enabled, int durationMs, double refreshRate)
{
    durationMs = std::clamp(durationMs, 0, 10000);
    animation_.setRefreshRate(refreshRate);
    if (motionEnabled_ == enabled && durationMs_ == durationMs)
        return;
    motionEnabled_ = enabled;
    durationMs_ = durationMs;
    if (!motionEnabled_ || durationMs_ == 0) {
        animation_.stopAll();
        target_ = checkedPosition();
        position_ = target_;
        update();
    } else if (animation_.isRunning(ThumbTrack)) {
        animation_.start(ThumbTrack, position_, target_, std::chrono::milliseconds(durationMs_), [this](double value) {
            position_ = value;
            update();
        });
    }
}

void SettingsToggle::setColors(const QColor& accent, const QColor& track, const QColor& text)
{
    if (!accent.isValid() || !track.isValid() || !text.isValid())
        return;
    if (accent_ == accent && track_ == track && text_ == text)
        return;
    accent_ = accent;
    track_ = track;
    text_ = text;
    update();
}

QSize SettingsToggle::sizeHint() const
{
    const int labelWidth = text().isEmpty() ? 0 : fontMetrics().horizontalAdvance(text()) + 12;
    return {static_cast<int>(SwitchWidth + SwitchMargin * 2) + labelWidth,
            std::max(static_cast<int>(SwitchHeight + SwitchMargin * 2), fontMetrics().height() + 8)};
}

QSize SettingsToggle::minimumSizeHint() const { return sizeHint(); }

double SettingsToggle::checkedPosition() const
{
    return checkState() == Qt::PartiallyChecked ? 0.5 : isChecked() ? 1.0 : 0.0;
}

void SettingsToggle::syncState(bool animate)
{
    const double desired = checkedPosition();
    if (desired == target_)
        return;
    target_ = desired;
    animation_.stop(ThumbTrack);
    if (!animate || !motionEnabled_ || durationMs_ == 0 || !isVisible() || !isEnabled()) {
        position_ = target_;
        update();
        return;
    }
    animation_.start(ThumbTrack, position_, target_, std::chrono::milliseconds(durationMs_), [this](double value) {
        position_ = value;
        update();
    });
}

void SettingsToggle::checkStateSet()
{
    QCheckBox::checkStateSet();
    syncState(!signalsBlocked());
}

void SettingsToggle::nextCheckState()
{
    QCheckBox::nextCheckState();
    syncState(!signalsBlocked());
}

bool SettingsToggle::hitButton(const QPoint& position) const { return rect().contains(position); }

void SettingsToggle::paintEvent(QPaintEvent*)
{
    if (checkedPosition() != target_)
        syncState(false);
    const bool rightToLeft = layoutDirection() == Qt::RightToLeft;
    const double x = rightToLeft ? width() - SwitchWidth - SwitchMargin : SwitchMargin;
    const QRectF track(x, (height() - SwitchHeight) / 2.0, SwitchWidth, SwitchHeight);
    const QColor fill = mix(track_, accent_, position_);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    if (!isEnabled())
        painter.setOpacity(0.42);
    if (hasFocus()) {
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(mix(track_, accent_, 0.7), 1));
        painter.drawRoundedRect(track.adjusted(-2.5, -2.5, 2.5, 2.5), SwitchHeight / 2.0 + 2.5, SwitchHeight / 2.0 + 2.5);
    }
    painter.setPen(Qt::NoPen);
    painter.setBrush(underMouse() ? mix(fill, text_, 0.035) : fill);
    painter.drawRoundedRect(track, SwitchHeight / 2.0, SwitchHeight / 2.0);
    const double diameter = SwitchHeight - SwitchInset * 2;
    const double progress = rightToLeft ? 1.0 - position_ : position_;
    const double thumbX = track.left() + SwitchInset + progress * (SwitchWidth - SwitchInset * 2 - diameter);
    painter.setBrush(mix(mix(track_, text_, 0.85), contrasting(accent_), position_));
    painter.drawEllipse(QRectF(thumbX, track.top() + SwitchInset, diameter, diameter));
    if (!text().isEmpty()) {
        const int textStart = static_cast<int>(SwitchWidth + SwitchMargin + 12);
        const QRect label = rect().adjusted(rightToLeft ? 0 : textStart, 0, rightToLeft ? -textStart : 0, 0);
        painter.setFont(font());
        painter.setPen(text_);
        painter.drawText(label, Qt::AlignVCenter | Qt::TextShowMnemonic | (rightToLeft ? Qt::AlignRight : Qt::AlignLeft), text());
    }
}

bool SettingsToggle::event(QEvent* event)
{
    const bool result = QCheckBox::event(event);
    if (visualEvent(event->type())) {
        if (event->type() == QEvent::EnabledChange && !isEnabled()) {
            animation_.stopAll();
            target_ = checkedPosition();
            position_ = target_;
        }
        update();
        if (event->type() == QEvent::FontChange)
            updateGeometry();
    }
    return result;
}

void SettingsToggle::hideEvent(QHideEvent* event)
{
    animation_.stopAll();
    target_ = checkedPosition();
    position_ = target_;
    QCheckBox::hideEvent(event);
}

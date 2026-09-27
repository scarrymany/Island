#include "SettingsControls.h"

#include <QAbstractItemView>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFocusEvent>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QPainter>
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
const QString ThumbTrack = QStringLiteral("thumb");
const QString NavigationTrack = QStringLiteral("navigation");

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
    explicit TrackSlider(QWidget* parent) : QSlider(Qt::Horizontal, parent)
    {
        setAttribute(Qt::WA_Hover);
        setFocusPolicy(Qt::StrongFocus);
        setMinimumHeight(ControlHeight);
    }

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
        painter.setBrush(color);
        painter.drawEllipse(QPointF(centerX, centerY), isSliderDown() ? 7.0 : 6.0, isSliderDown() ? 7.0 : 6.0);
    }

    bool event(QEvent* event) override
    {
        const bool result = QSlider::event(event);
        if (visualEvent(event->type()))
            update();
        return result;
    }

private:
    QColor accent_{"#F4F4F5"};
    QColor track_{"#333338"};
    QColor text_{"#F4F4F5"};
};

void configureChoice(QComboBox* choice)
{
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
        "QComboBox QLineEdit { background: transparent; border: none; }"
        "QComboBox QAbstractItemView { background: %1; color: %2; border: 1px solid %3;"
        "selection-background-color: %4; selection-color: %2; outline: 0; padding: 4px; }")
        .arg(track.name(), text.name(), mix(track, text, 0.12).name(), mix(track, accent, 0.18).name()));
    choice->update();
}

void paintChoice(QComboBox* choice, const QColor& accent, const QColor& track, const QColor& text)
{
    QPainter painter(choice);
    painter.setRenderHint(QPainter::Antialiasing);
    const bool enabled = choice->isEnabled();
    const QColor foreground = enabled ? text : mix(track, text, 0.4);
    const QColor surface = enabled && choice->underMouse() ? mix(track, text, 0.035) : track;
    const bool focused = choice->hasFocus();
    painter.setPen(QPen(focused ? mix(track, accent, 0.7) : mix(track, text, 0.09), 1));
    painter.setBrush(surface);
    painter.drawRoundedRect(QRectF(choice->rect()).adjusted(0.5, 0.5, -0.5, -0.5), CornerRadius, CornerRadius);

    const bool rightToLeft = choice->layoutDirection() == Qt::RightToLeft;
    const double arrowX = rightToLeft ? 17.0 : choice->width() - 17.0;
    const double arrowY = choice->height() / 2.0;
    painter.setPen(QPen(foreground, 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    QPolygonF chevron;
    chevron << QPointF(arrowX - 4, arrowY - 2) << QPointF(arrowX, arrowY + 2) << QPointF(arrowX + 4, arrowY - 2);
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
    styleChoice(this, accent_, track_, text_);
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
}

QSize SettingsChoice::sizeHint() const { return QComboBox::sizeHint().expandedTo(QSize(100, ControlHeight)); }
QSize SettingsChoice::minimumSizeHint() const { return QComboBox::minimumSizeHint().expandedTo(QSize(100, ControlHeight)); }
void SettingsChoice::paintEvent(QPaintEvent*) { paintChoice(this, accent_, track_, text_); }

bool SettingsChoice::event(QEvent* event)
{
    const bool result = QComboBox::event(event);
    if (visualEvent(event->type()))
        update();
    return result;
}

SettingsFontChoice::SettingsFontChoice(QWidget* parent) : QFontComboBox(parent)
{
    configureChoice(this);
    styleChoice(this, accent_, track_, text_);
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
}

QSize SettingsFontChoice::sizeHint() const { return QFontComboBox::sizeHint().expandedTo(QSize(100, ControlHeight)); }
QSize SettingsFontChoice::minimumSizeHint() const { return QFontComboBox::minimumSizeHint().expandedTo(QSize(100, ControlHeight)); }
void SettingsFontChoice::paintEvent(QPaintEvent*) { paintChoice(this, accent_, track_, text_); }

bool SettingsFontChoice::event(QEvent* event)
{
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

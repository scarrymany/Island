#include "IslandDialogs.h"
#include "IslandMenu.h"
#include "Squircle.h"

#include <QApplication>
#include <QEvent>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpressionValidator>
#include <QScreen>

#include <algorithm>
#include <cmath>
#include <optional>

namespace {
constexpr int ShadowMargin = 18;
constexpr double PanelRadius = 16;
constexpr int PickerWidth = 300;
constexpr int PickerPadding = 16;

void paintPanel(QWidget* widget, const QRectF& panel, double grow = 1) {
    const auto theme = IslandMenu::theme();
    QPainter painter(widget);
    painter.setRenderHint(QPainter::Antialiasing);
    QRectF shape = panel;
    if (grow < 1) {
        const QPointF center = panel.center();
        shape = QRectF(center.x() - panel.width() * grow / 2, center.y() - panel.height() * grow / 2,
                       panel.width() * grow, panel.height() * grow);
    }
    const double ratio = widget->devicePixelRatioF();
    const int blur = qRound(12 * ratio);
    const double spread = blur * 2 / ratio;
    painter.setOpacity(0.55);
    painter.drawImage(shape.adjusted(-spread, -spread + 6, spread, spread + 6),
                      Squircle::shadow((shape.size() * ratio).toSize(), PanelRadius * ratio, blur));
    painter.setOpacity(1);
    QColor background = theme.background;
    background.setAlphaF(0.985f);
    painter.fillPath(Squircle::path(shape, PanelRadius), background);
    const bool light = theme.background.lightnessF() > 0.6;
    painter.setPen(QPen(light ? QColor(0, 0, 0, 34) : QColor(255, 255, 255, 26), 1));
    painter.drawPath(Squircle::path(shape.adjusted(0.5, 0.5, -0.5, -0.5), PanelRadius - 0.5));
}

// Gives a stock dialog the overlay chrome: no system frame, a soft squircle panel,
// a quick fade-in and dragging by its background.
class DialogChrome final : public QObject {
public:
    explicit DialogChrome(QDialog* dialog) : QObject(dialog), dialog_(dialog), motion_(this) {
        dialog->setWindowFlags((dialog->windowFlags() & ~Qt::WindowContextHelpButtonHint)
            | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint);
        dialog->setAttribute(Qt::WA_TranslucentBackground);
        dialog->setContentsMargins(ShadowMargin + 10, ShadowMargin + 8, ShadowMargin + 10, ShadowMargin + 10);
        const auto theme = IslandMenu::theme();
        dialog->setStyleSheet(QStringLiteral(
            "QDialog, QMessageBox, QInputDialog { background: transparent; }"
            "QLabel { color: %1; background: transparent; }"
            "QLabel#qt_msgbox_informativelabel { color: %2; font-weight: 400; }").arg(theme.text.name(),
                QColor::fromRgbF(theme.text.redF() * 0.62 + theme.background.redF() * 0.38,
                                 theme.text.greenF() * 0.62 + theme.background.greenF() * 0.38,
                                 theme.text.blueF() * 0.62 + theme.background.blueF() * 0.38).name()));
        dialog->installEventFilter(this);
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (watched != dialog_) return false;
        switch (event->type()) {
        case QEvent::Paint:
            paintPanel(dialog_, QRectF(dialog_->rect()).adjusted(ShadowMargin, ShadowMargin, -ShadowMargin, -ShadowMargin));
            return false;
        case QEvent::Show:
            if (auto* parent = dialog_->parentWidget()) {
                const QRect frame = parent->window()->geometry();
                dialog_->adjustSize();
                dialog_->move(frame.center() - QPoint(dialog_->width() / 2, dialog_->height() / 2));
            }
            if (IslandMenu::theme().motion) {
                dialog_->setWindowOpacity(0);
                motion_.start(QStringLiteral("show"), 0, 1, std::chrono::milliseconds(160), QEasingCurve::OutCubic,
                              [this](double value) { dialog_->setWindowOpacity(value); });
            }
            return false;
        case QEvent::MouseButtonPress: {
            const auto* mouse = static_cast<QMouseEvent*>(event);
            if (mouse->button() == Qt::LeftButton) drag_ = mouse->globalPosition().toPoint() - dialog_->pos();
            return false;
        }
        case QEvent::MouseMove:
            if (drag_ && (static_cast<QMouseEvent*>(event)->buttons() & Qt::LeftButton))
                dialog_->move(static_cast<QMouseEvent*>(event)->globalPosition().toPoint() - *drag_);
            return false;
        case QEvent::MouseButtonRelease:
            drag_.reset();
            return false;
        default:
            return false;
        }
    }

private:
    QDialog* dialog_;
    AnimationClock motion_;
    std::optional<QPoint> drag_;
};

// QMessageBox sizes itself from its grid and indents text for a missing icon.
// Drop that indent and give the prompt a comfortable reading width.
void shape(QMessageBox& box) {
    auto* grid = qobject_cast<QGridLayout*>(box.layout());
    if (!grid) return;
    for (int index = 0; index < grid->count(); ++index) {
        if (auto* spacer = grid->itemAt(index)->spacerItem()) spacer->changeSize(0, 0, QSizePolicy::Fixed, QSizePolicy::Fixed);
    }
    grid->setVerticalSpacing(10);
    if (auto* label = box.findChild<QLabel*>(QStringLiteral("qt_msgbox_label"))) {
        int row = 0, column = 0, rows = 0, columns = 0;
        grid->getItemPosition(grid->indexOf(label), &row, &column, &rows, &columns);
        grid->setColumnMinimumWidth(column, 360);
        grid->setColumnStretch(column, 1);
    }
    grid->invalidate();
}

QString heading(const QString& title) {
    return QStringLiteral("<div style='font-size:15px; font-weight:700;'>%1</div>").arg(title.toHtmlEscaped());
}
}

QMessageBox::StandardButton IslandDialogs::question(QWidget* parent, const QString& title, const QString& text) {
    QMessageBox box(QMessageBox::NoIcon, title, heading(title), QMessageBox::Yes | QMessageBox::No, parent);
    box.setTextFormat(Qt::RichText);
    box.setInformativeText(text);
    box.button(QMessageBox::Yes)->setText(QStringLiteral("Да"));
    box.button(QMessageBox::No)->setText(QStringLiteral("Отмена"));
    box.button(QMessageBox::Yes)->setObjectName(QStringLiteral("accentButton"));
    box.setDefaultButton(QMessageBox::Yes);
    new DialogChrome(&box);
    shape(box);
    return static_cast<QMessageBox::StandardButton>(box.exec());
}

void IslandDialogs::warning(QWidget* parent, const QString& title, const QString& text) {
    QMessageBox box(QMessageBox::NoIcon, title, heading(title), QMessageBox::Ok, parent);
    box.setTextFormat(Qt::RichText);
    box.setInformativeText(text);
    box.button(QMessageBox::Ok)->setText(QStringLiteral("Понятно"));
    box.button(QMessageBox::Ok)->setObjectName(QStringLiteral("accentButton"));
    new DialogChrome(&box);
    shape(box);
    box.exec();
}

QString IslandDialogs::getText(QWidget* parent, const QString& title, const QString& label, const QString& value, bool* accepted) {
    QInputDialog dialog(parent);
    dialog.setWindowTitle(title);
    dialog.setLabelText(QStringLiteral("%1<div style='margin-top:6px; font-weight:400;'>%2</div>")
        .arg(heading(title), label.toHtmlEscaped()));
    dialog.setTextValue(value);
    dialog.setOkButtonText(QStringLiteral("Сохранить"));
    dialog.setCancelButtonText(QStringLiteral("Отмена"));
    dialog.setMinimumWidth(380);
    for (auto* button : dialog.findChildren<QPushButton*>()) {
        if (button->text() == QStringLiteral("Сохранить")) button->setObjectName(QStringLiteral("accentButton"));
    }
    new DialogChrome(&dialog);
    const bool ok = dialog.exec() == QDialog::Accepted;
    if (accepted) *accepted = ok;
    return ok ? dialog.textValue() : QString{};
}

ColorPickerDialog::ColorPickerDialog(const QColor& initial, QWidget* parent, const QString& title)
    : QDialog(parent), initial_(initial.isValid() ? initial : QColor(Qt::white)), title_(title), motion_(this) {
    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint);
    setAttribute(Qt::WA_TranslucentBackground);
    setWindowTitle(title.isEmpty() ? QStringLiteral("Цвет") : title);
    setObjectName(QStringLiteral("colorPicker"));
    setModal(true);
    setMouseTracking(true);
    const auto theme = IslandMenu::theme();
    accent_ = theme.accent;
    setStyleSheet(QStringLiteral("QDialog { background: transparent; }"));
    hex_ = new QLineEdit(this);
    hex_->setObjectName(QStringLiteral("colorHex"));
    hex_->setAccessibleName(QStringLiteral("Шестнадцатеричный код цвета"));
    hex_->setValidator(new QRegularExpressionValidator(QRegularExpression(QStringLiteral("#?[0-9A-Fa-f]{0,6}")), hex_));
    hex_->setMaxLength(7);
    hex_->installEventFilter(this);
    connect(hex_, &QLineEdit::textEdited, this, [this](const QString& text) {
        QString digits = text;
        if (!digits.startsWith(u'#')) digits.prepend(u'#');
        const QColor color(digits);
        if (digits.size() == 7 && color.isValid()) {
            float h = 0, s = 0, v = 0;
            color.getHsvF(&h, &s, &v);
            exact_ = color;
            apply(h >= 0 ? h : hue_, s, v, false);
        }
    });
    auto* cancel = new QPushButton(QStringLiteral("Отмена"), this);
    auto* done = new QPushButton(QStringLiteral("Готово"), this);
    cancel->setObjectName(QStringLiteral("colorCancel"));
    done->setObjectName(QStringLiteral("accentButton"));
    done->setDefault(true);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(done, &QPushButton::clicked, this, &QDialog::accept);
    const int top = ShadowMargin + PickerPadding;
    const int inner = PickerWidth - PickerPadding * 2;
    hex_->setGeometry(ShadowMargin + PickerPadding + 52, top + 238, 116, 34);
    done->setGeometry(ShadowMargin + PickerWidth - PickerPadding - 104, top + 330, 104, 36);
    cancel->setGeometry(done->x() - 98 - 8, top + 330, 98, 36);
    setFixedSize(PickerWidth + ShadowMargin * 2, top + 330 + 36 + PickerPadding + ShadowMargin);
    Q_UNUSED(inner);
    swatches_ = {QColor("#F5F5FA"), QColor("#9B8CFF"), QColor("#70CDFF"), QColor("#71DBC8"), QColor("#A0E8B5"),
                 QColor("#FFD27A"), QColor("#FFAD87"), QColor("#FF7A9A"), QColor("#10121B"), QColor("#242344")};
    setCurrentColor(initial_);
}

void ColorPickerDialog::setSwatches(const QList<QColor>& swatches) { swatches_ = swatches.mid(0, 10); update(); }
void ColorPickerDialog::setAccent(const QColor& accent) { accent_ = accent; update(); }

QColor ColorPickerDialog::currentColor() const {
    if (exact_.isValid()) return exact_;
    return QColor::fromHsvF(std::clamp(hue_, 0.0, 1.0), std::clamp(saturation_, 0.0, 1.0), std::clamp(value_, 0.0, 1.0));
}

void ColorPickerDialog::setCurrentColor(const QColor& color) {
    if (!color.isValid()) return;
    float h = 0, s = 0, v = 0;
    color.getHsvF(&h, &s, &v);
    // Exact entries (swatches, hex, the initial value) must survive the HSV round trip.
    exact_ = color;
    apply(h >= 0 ? h : hue_, s, v, false);
    hex_->setText(color.name().toUpper());
}

void ColorPickerDialog::apply(double hue, double saturation, double value, bool updateEditor) {
    hue_ = std::clamp(hue, 0.0, 1.0);
    saturation_ = std::clamp(saturation, 0.0, 1.0);
    value_ = std::clamp(value, 0.0, 1.0);
    if (updateEditor && hex_ && !hex_->hasFocus()) hex_->setText(currentColor().name().toUpper());
    emit currentColorChanged(currentColor());
    update();
}

QRectF ColorPickerDialog::panel() const {
    return QRectF(rect()).adjusted(ShadowMargin, ShadowMargin, -ShadowMargin, -ShadowMargin);
}

QRectF ColorPickerDialog::field() const {
    const QRectF area = panel();
    return QRectF(area.left() + PickerPadding, area.top() + PickerPadding + 34, area.width() - PickerPadding * 2, 156);
}

QRectF ColorPickerDialog::hueStrip() const {
    const QRectF area = field();
    return QRectF(area.left(), area.bottom() + 14, area.width(), 14);
}

QRectF ColorPickerDialog::previewRect() const {
    return QRectF(field().left(), panel().top() + PickerPadding + 238, 42, 34);
}

QRectF ColorPickerDialog::swatchRect(int index) const {
    const QRectF area = field();
    const double size = 20;
    const int count = std::max<qsizetype>(1, swatches_.size());
    const double gap = count > 1 ? (area.width() - size * count) / (count - 1) : 0;
    return QRectF(area.left() + index * (size + gap), panel().top() + PickerPadding + 290, size, size);
}

void ColorPickerDialog::paintEvent(QPaintEvent*) {
    const double grow = 0.97 + 0.03 * appear_;
    paintPanel(this, panel(), grow);
    const auto theme = IslandMenu::theme();
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    if (grow < 1) {
        const QPointF center = panel().center();
        painter.translate(center);
        painter.scale(grow, grow);
        painter.translate(-center);
    }
    QFont font(theme.fontFamily);
    font.setPixelSize(14);
    font.setWeight(QFont::Bold);
    painter.setFont(font);
    painter.setPen(theme.text);
    painter.drawText(QRectF(field().left(), panel().top() + PickerPadding - 2, field().width(), 24),
                     Qt::AlignLeft | Qt::AlignVCenter, windowTitle());

    const QRectF area = field();
    const QPainterPath shape = Squircle::path(area, 10);
    painter.save();
    painter.setClipPath(shape);
    QLinearGradient horizontal(area.topLeft(), area.topRight());
    horizontal.setColorAt(0, Qt::white);
    horizontal.setColorAt(1, QColor::fromHsvF(hue_, 1, 1));
    painter.fillRect(area, horizontal);
    QLinearGradient vertical(area.topLeft(), area.bottomLeft());
    vertical.setColorAt(0, QColor(0, 0, 0, 0));
    vertical.setColorAt(1, Qt::black);
    painter.fillRect(area, vertical);
    painter.restore();
    painter.setPen(QPen(QColor(255, 255, 255, 30), 1));
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(Squircle::path(area.adjusted(0.5, 0.5, -0.5, -0.5), 9.5));

    const auto knob = [&painter](const QPointF& center, const QColor& fill, double radius) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0, 0, 0, 70));
        painter.drawEllipse(center + QPointF(0, 1), radius + 1.5, radius + 1.5);
        painter.setBrush(Qt::white);
        painter.drawEllipse(center, radius, radius);
        painter.setBrush(fill);
        painter.drawEllipse(center, radius - 2.5, radius - 2.5);
    };
    knob(QPointF(area.left() + saturation_ * area.width(), area.top() + (1 - value_) * area.height()), currentColor(), 8);

    const QRectF strip = hueStrip();
    QLinearGradient spectrum(strip.topLeft(), strip.topRight());
    for (int step = 0; step <= 12; ++step) spectrum.setColorAt(step / 12.0, QColor::fromHsvF(step / 12.0 * 0.9999, 1, 1));
    painter.setPen(Qt::NoPen);
    painter.setBrush(spectrum);
    painter.drawRoundedRect(strip, strip.height() / 2, strip.height() / 2);
    knob(QPointF(strip.left() + hue_ * strip.width(), strip.center().y()), QColor::fromHsvF(hue_, 1, 1), 9);

    const QRectF preview = previewRect();
    painter.save();
    painter.setClipPath(Squircle::path(preview, 9));
    painter.fillRect(QRectF(preview.left(), preview.top(), preview.width() / 2, preview.height()), initial_);
    painter.fillRect(QRectF(preview.center().x(), preview.top(), preview.width() / 2, preview.height()), currentColor());
    painter.restore();
    painter.setPen(QPen(QColor(255, 255, 255, 34), 1));
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(Squircle::path(preview.adjusted(0.5, 0.5, -0.5, -0.5), 8.5));

    for (int index = 0; index < swatches_.size(); ++index) {
        const QRectF swatch = swatchRect(index);
        const bool selected = swatches_[index].name().compare(currentColor().name(), Qt::CaseInsensitive) == 0;
        painter.setPen(QPen(QColor(255, 255, 255, selected ? 230 : 40), selected ? 2 : 1));
        painter.setBrush(swatches_[index]);
        painter.drawEllipse(selected ? swatch.adjusted(1, 1, -1, -1) : swatch.adjusted(0.5, 0.5, -0.5, -0.5));
    }
}

void ColorPickerDialog::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    if (!IslandMenu::theme().motion) { appear_ = 1; setWindowOpacity(1); return; }
    appear_ = 0;
    setWindowOpacity(0);
    motion_.start(QStringLiteral("show"), 0, 1, std::chrono::milliseconds(180), QEasingCurve::OutCubic, [this](double value) {
        appear_ = value;
        setWindowOpacity(std::min(1.0, value * 1.4));
        update();
    });
}

void ColorPickerDialog::pick(const QPointF& point) {
    exact_ = {};
    if (drag_ == Drag::Field) {
        const QRectF area = field();
        apply(hue_, (point.x() - area.left()) / area.width(), 1 - (point.y() - area.top()) / area.height());
    } else if (drag_ == Drag::Hue) {
        const QRectF strip = hueStrip();
        apply((point.x() - strip.left()) / strip.width(), saturation_, value_);
    }
}

void ColorPickerDialog::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) return QDialog::mousePressEvent(event);
    const QPointF point = event->position();
    hex_->clearFocus();
    if (field().adjusted(-6, -6, 6, 6).contains(point)) drag_ = Drag::Field;
    else if (hueStrip().adjusted(-4, -8, 4, 8).contains(point)) drag_ = Drag::Hue;
    else {
        for (int index = 0; index < swatches_.size(); ++index) {
            if (swatchRect(index).adjusted(-3, -3, 3, 3).contains(point)) { setCurrentColor(swatches_[index]); return; }
        }
        windowDrag_ = event->globalPosition().toPoint() - pos();
        return;
    }
    pick(point);
}

void ColorPickerDialog::mouseMoveEvent(QMouseEvent* event) {
    if (drag_ != Drag::None) pick(event->position());
    else if (windowDrag_ && (event->buttons() & Qt::LeftButton)) move(event->globalPosition().toPoint() - *windowDrag_);
    const QPointF point = event->position();
    const bool interactive = field().contains(point) || hueStrip().adjusted(-4, -8, 4, 8).contains(point);
    setCursor(interactive || drag_ != Drag::None ? Qt::CrossCursor : Qt::ArrowCursor);
}

void ColorPickerDialog::mouseReleaseEvent(QMouseEvent* event) {
    drag_ = Drag::None;
    windowDrag_.reset();
    QDialog::mouseReleaseEvent(event);
}

void ColorPickerDialog::keyPressEvent(QKeyEvent* event) {
    const double step = event->modifiers().testFlag(Qt::ShiftModifier) ? 0.1 : 0.01;
    const int key = event->key();
    if (key == Qt::Key_Left || key == Qt::Key_Right || key == Qt::Key_Up || key == Qt::Key_Down
        || key == Qt::Key_PageUp || key == Qt::Key_PageDown) exact_ = {};
    switch (key) {
    case Qt::Key_Left: apply(hue_, saturation_ - step, value_); return;
    case Qt::Key_Right: apply(hue_, saturation_ + step, value_); return;
    case Qt::Key_Up: apply(hue_, saturation_, value_ + step); return;
    case Qt::Key_Down: apply(hue_, saturation_, value_ - step); return;
    case Qt::Key_PageUp: apply(hue_ - step, saturation_, value_); return;
    case Qt::Key_PageDown: apply(hue_ + step, saturation_, value_); return;
    default: QDialog::keyPressEvent(event);
    }
}

bool ColorPickerDialog::eventFilter(QObject* watched, QEvent* event) {
    if (watched == hex_ && event->type() == QEvent::FocusOut) hex_->setText(currentColor().name().toUpper());
    return QDialog::eventFilter(watched, event);
}

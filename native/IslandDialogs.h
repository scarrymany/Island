#pragma once

#include "AnimationClock.h"

#include <QColor>
#include <QDialog>
#include <QMessageBox>
#include <optional>

class QLineEdit;

// Frameless, themed replacements for the stock modal prompts. They keep the Qt
// dialog classes (QMessageBox, QInputDialog) so keyboard handling, accessibility
// and automation behave exactly like the platform ones.
namespace IslandDialogs {
QMessageBox::StandardButton question(QWidget* parent, const QString& title, const QString& text);
void warning(QWidget* parent, const QString& title, const QString& text);
QString getText(QWidget* parent, const QString& title, const QString& label, const QString& value, bool* accepted);
}

// Compact colour picker: saturation/value field, hue strip, hex entry and
// curated swatches. currentColorChanged fires while dragging for live preview.
class ColorPickerDialog final : public QDialog {
    Q_OBJECT

public:
    explicit ColorPickerDialog(const QColor& initial, QWidget* parent = nullptr, const QString& title = {});
    void setCurrentColor(const QColor& color);
    [[nodiscard]] QColor currentColor() const;
    void setSwatches(const QList<QColor>& swatches);
    void setAccent(const QColor& accent);

signals:
    void currentColorChanged(const QColor& color);

protected:
    void paintEvent(QPaintEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    enum class Drag { None, Field, Hue };
    QRectF panel() const;
    QRectF field() const;
    QRectF hueStrip() const;
    QRectF swatchRect(int index) const;
    QRectF previewRect() const;
    void pick(const QPointF& point);
    void apply(double hue, double saturation, double value, bool updateEditor = true);

    QColor initial_;
    QString title_;
    double hue_ = 0, saturation_ = 0, value_ = 0;
    QList<QColor> swatches_;
    QColor accent_{"#FFFFFF"};
    Drag drag_ = Drag::None;
    std::optional<QPoint> windowDrag_;
    QColor exact_;
    QLineEdit* hex_ = nullptr;
    AnimationClock motion_;
    double appear_ = 1;
};

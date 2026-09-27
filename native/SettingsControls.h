#pragma once

#include "AnimationClock.h"

#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QFontComboBox>
#include <QWidget>

class QDoubleSpinBox;
class QHideEvent;
class QSlider;

class SettingsSlider final : public QWidget {
    Q_OBJECT
    Q_PROPERTY(double value READ value WRITE setValue NOTIFY valueChanged)

public:
    explicit SettingsSlider(QWidget* parent = nullptr);

    void setRange(double minimum, double maximum);
    void setDecimals(int decimals);
    void setSingleStep(double step);
    void setSuffix(const QString& suffix);
    void setAccessibleName(const QString& name);
    void setColors(const QColor& accent, const QColor& track, const QColor& text);

    [[nodiscard]] double value() const;
    [[nodiscard]] QDoubleSpinBox* editor() const;
    [[nodiscard]] QSlider* slider() const;

public slots:
    void setValue(double value);

signals:
    void valueChanged(double value);

protected:
    bool event(QEvent* event) override;

private:
    void rebuildSlider();
    void syncSlider();
    void updateEditorWidth();
    void updateAccessibleNames();

    QSlider* slider_ = nullptr;
    QDoubleSpinBox* editor_ = nullptr;
    QColor accent_;
    QColor track_;
    QColor text_;
};

class SettingsChoice final : public QComboBox {
    Q_OBJECT

public:
    explicit SettingsChoice(QWidget* parent = nullptr);
    void setColors(const QColor& accent, const QColor& track, const QColor& text);
    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;
    bool event(QEvent* event) override;

private:
    QColor accent_{"#F4F4F5"};
    QColor track_{"#18181B"};
    QColor text_{"#F4F4F5"};
};

class SettingsToggle final : public QCheckBox {
    Q_OBJECT

public:
    explicit SettingsToggle(QWidget* parent = nullptr);
    explicit SettingsToggle(const QString& text, QWidget* parent = nullptr);
    void setMotion(bool enabled, int durationMs, double refreshRate);
    void setColors(const QColor& accent, const QColor& track, const QColor& text);
    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;
    void checkStateSet() override;
    void nextCheckState() override;
    bool hitButton(const QPoint& position) const override;
    bool event(QEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    void syncState(bool animate);
    [[nodiscard]] double checkedPosition() const;

    AnimationClock animation_;
    QColor accent_{"#F4F4F5"};
    QColor track_{"#333338"};
    QColor text_{"#F4F4F5"};
    bool motionEnabled_ = true;
    int durationMs_ = 180;
    double position_ = 0;
    double target_ = 0;
};

class SettingsFontChoice final : public QFontComboBox {
    Q_OBJECT

public:
    explicit SettingsFontChoice(QWidget* parent = nullptr);
    void setColors(const QColor& accent, const QColor& track, const QColor& text);
    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;
    bool event(QEvent* event) override;

private:
    QColor accent_{"#F4F4F5"};
    QColor track_{"#18181B"};
    QColor text_{"#F4F4F5"};
};

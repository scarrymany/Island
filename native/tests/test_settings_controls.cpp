#include "SettingsControls.h"

#include <QAccessible>
#include <QDoubleSpinBox>
#include <QFontDatabase>
#include <QSignalBlocker>
#include <QSlider>
#include <QtTest>

#include <limits>

class SettingsControlsTest final : public QObject {
    Q_OBJECT

private slots:
    void sliderPreservesExactDecimalInput()
    {
        SettingsSlider control;
        control.setDecimals(2);
        control.setRange(0.1, 1.0);
        control.setSingleStep(0.05);
        QSignalSpy changed(&control, &SettingsSlider::valueChanged);
        control.setValue(0.73);
        QCOMPARE(control.value(), 0.73);
        QCOMPARE(control.editor()->value(), 0.73);
        QCOMPARE(changed.size(), 1);
        control.editor()->setValue(0.87);
        QCOMPARE(control.value(), 0.87);
        QCOMPARE(changed.size(), 2);
        control.slider()->setValue(control.slider()->maximum());
        QCOMPARE(control.value(), 1.0);
        QCOMPARE(changed.size(), 3);
        control.setValue(1.0);
        QCOMPARE(changed.size(), 3);
    }

    void sliderClampsRangeAndDoesNotEmitDuringRefresh()
    {
        SettingsSlider control;
        control.setRange(0, 100);
        control.setDecimals(0);
        control.setValue(70);
        QSignalSpy changed(&control, &SettingsSlider::valueChanged);
        control.setRange(10, 50);
        QCOMPARE(control.value(), 50.0);
        QCOMPARE(changed.size(), 1);
        {
            const QSignalBlocker blocker(&control);
            control.setValue(23);
        }
        QCOMPARE(control.value(), 23.0);
        QCOMPARE(control.editor()->value(), 23.0);
        QCOMPARE(changed.size(), 1);
        control.slider()->setValue(control.slider()->minimum());
        QCOMPARE(control.value(), 10.0);
        QCOMPARE(changed.size(), 2);
    }

    void numericEditorSupportsKeyboardAndAccessibleNames()
    {
        SettingsSlider control;
        control.setObjectName("opacity");
        control.setAccessibleName(QStringLiteral("Непрозрачность"));
        control.setRange(0, 1);
        control.setDecimals(2);
        control.editor()->setLocale(QLocale::c());
        control.show();
        control.editor()->setFocus();
        control.editor()->selectAll();
        QTest::keyClicks(control.editor(), "0.67");
        QTest::keyClick(control.editor(), Qt::Key_Return);
        QCOMPARE(control.value(), 0.67);
        QCOMPARE(control.slider()->objectName(), QStringLiteral("opacity.slider"));
        QCOMPARE(control.editor()->objectName(), QStringLiteral("opacity.editor"));
        QCOMPARE(control.slider()->accessibleName(), QStringLiteral("Непрозрачность"));
        QVERIFY(control.editor()->accessibleName().contains(QStringLiteral("Непрозрачность")));
        QCOMPARE(control.editor()->buttonSymbols(), QAbstractSpinBox::NoButtons);
    }

    void sliderRejectsNonfiniteValues()
    {
        SettingsSlider control;
        control.setValue(12);
        QSignalSpy changed(&control, &SettingsSlider::valueChanged);
        control.setValue(std::numeric_limits<double>::quiet_NaN());
        control.setValue(std::numeric_limits<double>::infinity());
        control.setRange(10, std::numeric_limits<double>::infinity());
        QCOMPARE(control.value(), 12.0);
        QCOMPARE(changed.size(), 0);
    }

    void numericEditorRemainsReadableWithLightTheme()
    {
        SettingsSlider control;
        control.setColors(QColor("#111111"), QColor("#ECECEE"), QColor("#111111"));
        control.show();
        control.editor()->ensurePolished();
        const auto palette = control.editor()->palette();
        QVERIFY(palette.color(QPalette::Base).lightnessF() - palette.color(QPalette::Text).lightnessF() > 0.5);
    }

    void choiceRetainsKeyboardSelectionAndItemData()
    {
        SettingsChoice choice;
        choice.addItem(QStringLiteral("Автоматически"), "auto");
        choice.addItem(QStringLiteral("Основной монитор"), "primary");
        choice.show();
        choice.setFocus();
        QSignalSpy changed(&choice, &QComboBox::currentIndexChanged);
        QTest::keyClick(&choice, Qt::Key_Down);
        QCOMPARE(choice.currentData().toString(), QStringLiteral("primary"));
        QCOMPARE(changed.size(), 1);
        choice.setColors(QColor("#FFFFFF"), QColor("#18181B"), QColor("#F4F4F5"));
        QVERIFY(!choice.grab().isNull());
    }

    void fontChoiceRetainsSelectedFontAndNativeSignal()
    {
        const QStringList families = QFontDatabase::families();
        if (families.size() < 2)
            QSKIP("This platform does not expose enough fonts");
        SettingsFontChoice choice;
        QSignalSpy changed(&choice, &QFontComboBox::currentFontChanged);
        const QString family = families.first() == choice.currentFont().family() ? families.last() : families.first();
        choice.setCurrentFont(QFont(family));
        QCOMPARE(choice.currentFont().family(), family);
        QVERIFY(changed.size() >= 1);
        choice.setColors(QColor("#7EB2FF"), QColor("#18181B"), QColor("#F4F4F5"));
        choice.show();
        QVERIFY(!choice.grab().isNull());
    }

    void toggleRetainsMouseKeyboardAndAccessibility()
    {
        SettingsToggle toggle(QStringLiteral("Плавные переходы"));
        toggle.setMotion(false, 200, 144);
        toggle.resize(toggle.sizeHint());
        toggle.show();
        QSignalSpy changed(&toggle, &QCheckBox::toggled);
        QTest::mouseClick(&toggle, Qt::LeftButton, Qt::NoModifier, toggle.rect().center());
        QVERIFY(toggle.isChecked());
        QCOMPARE(changed.size(), 1);
        auto* accessible = QAccessible::queryAccessibleInterface(&toggle);
        QVERIFY(accessible);
        QCOMPARE(accessible->role(), QAccessible::CheckBox);
        QVERIFY(accessible->state().checked);
        toggle.setFocus();
        QTest::keyClick(&toggle, Qt::Key_Space);
        QVERIFY(!toggle.isChecked());
        QCOMPARE(changed.size(), 2);
        toggle.setEnabled(false);
        QVERIFY(accessible->state().disabled);
        QTest::mouseClick(&toggle, Qt::LeftButton);
        QCOMPARE(changed.size(), 2);
    }

    void toggleBlockedRefreshAndReducedMotionMatchFinalAppearance()
    {
        SettingsToggle toggle;
        toggle.resize(56, 34);
        toggle.show();
        toggle.clearFocus();
        toggle.setMotion(true, 200, 144);
        QSignalSpy changed(&toggle, &QCheckBox::toggled);
        {
            const QSignalBlocker blocker(&toggle);
            toggle.setChecked(true);
        }
        const QImage refreshed = toggle.grab().toImage();
        QCOMPARE(changed.size(), 0);
        toggle.setMotion(false, 200, 144);
        const QImage withoutMotion = toggle.grab().toImage();
        QCOMPARE(refreshed, withoutMotion);
        toggle.setChecked(false);
        QCOMPARE(changed.size(), 1);
        QVERIFY(toggle.grab().toImage() != refreshed);
    }

    void toggleAnimationCanReverseAndFinishWithoutExtraSignals()
    {
        SettingsToggle toggle;
        toggle.resize(56, 34);
        toggle.show();
        toggle.clearFocus();
        toggle.setMotion(true, 70, 144);
        QSignalSpy changed(&toggle, &QCheckBox::toggled);
        toggle.setChecked(true);
        QTest::qWait(25);
        toggle.setChecked(false);
        QTest::qWait(100);
        const QImage animated = toggle.grab().toImage();
        QCOMPARE(changed.size(), 2);
        toggle.setMotion(false, 70, 144);
        QCOMPARE(animated, toggle.grab().toImage());
        toggle.hide();
        toggle.setChecked(true);
        toggle.show();
        QVERIFY(toggle.isChecked());
        QCOMPARE(changed.size(), 3);
    }
};

QTEST_MAIN(SettingsControlsTest)
#include "test_settings_controls.moc"

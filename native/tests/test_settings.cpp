#include "AppAssets.h"
#include "ConfigStore.h"
#include "SettingsControls.h"
#include "SettingsWindow.h"

#include <QGraphicsOpacityEffect>
#include <QFontInfo>
#include <QLabel>
#include <QKeySequenceEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <QtTest>

class SettingsTest : public QObject {
    Q_OBJECT
private slots:
    void captionMouseFocusDoesNotRemainHighlighted() {
        QTemporaryDir temp;
        ConfigStore store(temp.filePath("config.json"));
        SettingsWindow settings(&store);
        settings.show();
        const auto buttons = settings.findChildren<QPushButton*>("captionButton");
        QVERIFY(!buttons.isEmpty());
        auto* minimize = buttons.first();
        settings.findChild<QListWidget*>("navigation")->setFocus();
        minimize->setFocus(Qt::MouseFocusReason);
        QTest::qWait(20);
        const QImage mouseFocus = minimize->grab().toImage();
        minimize->clearFocus();
        QCOMPARE(minimize->grab().toImage(), mouseFocus);
        minimize->setFocus(Qt::TabFocusReason);
        QVERIFY(minimize->grab().toImage() != mouseFocus);
        settings.showMinimized();
        settings.showNormal();
        minimize->clearFocus();
        QTest::qWait(20);
        QCOMPARE(minimize->grab().toImage(), mouseFocus);
    }

    void hudFontWeightSelectionStoresANumber() {
        QTemporaryDir temp;
        ConfigStore store(temp.filePath("config.json"));
        SettingsWindow settings(&store);
        auto* weight = settings.findChild<QComboBox*>("font_weight");
        QVERIFY(weight);
        weight->setCurrentIndex(weight->findData("700"));
        QCOMPARE(store.config()["font_weight"].toInt(), 700);
        auto config = store.config();
        config["font_weight"] = 650;
        QVERIFY(store.update(config));
        QCOMPARE(weight->currentData().toString(), QString("650"));
    }
    void resizingTextPreservesTheSelectedFamily() {
        QTemporaryDir temp;
        ConfigStore store(temp.filePath("config.json"));
        SettingsWindow settings(&store);
        settings.show();
        auto* heading = settings.findChild<QLabel*>("heading");
        QVERIFY(heading);
        QCOMPARE(QFontInfo(heading->font()).family(), QString("Inter"));
        for (int size : {18, 10}) {
            auto config = store.config();
            config["settings_font_size"] = size;
            QVERIFY(store.update(config));
            QCOMPARE(QFontInfo(heading->font()).family(), QString("Inter"));
            QCOMPARE(store.config()["settings_font_family"].toString(), QString("Inter"));
            for (auto* label : settings.findChildren<QLabel*>())
                QCOMPARE(QFontInfo(label->font()).family(), QString("Inter"));
        }
    }
    void restoresAccidentallyClearedShortcut() {
        QTemporaryDir temp;
        ConfigStore store(temp.filePath("config.json"));
        auto config = store.config();
        config["hotkey"] = "";
        QVERIFY(store.update(config));
        SettingsWindow settings(&store);
        auto* restore = settings.findChild<QPushButton*>("restoreHotkey");
        auto* hotkey = settings.findChild<QKeySequenceEdit*>("hotkey");
        QVERIFY(restore && hotkey);
        QVERIFY(hotkey->keySequence().isEmpty());
        restore->click();
        QCOMPARE(store.config()["hotkey"].toString(), QString("Ctrl+Alt+M"));
        QCOMPARE(hotkey->keySequence().toString(QKeySequence::PortableText), QString("Ctrl+Alt+M"));
        QVERIFY(!hotkey->isClearButtonEnabled());
    }

    void delayPresetsAndCustomValueRoundTrip() {
        QTemporaryDir temp;
        ConfigStore store(temp.filePath("config.json"));
        SettingsWindow settings(&store);
        auto* preset = settings.findChild<QComboBox*>("auto_hide_seconds");
        auto* exact = settings.findChild<QSpinBox*>("customHideDelay");
        QVERIFY(preset && exact);
        QVERIFY(!preset->isEditable());
        QCOMPARE(preset->currentData().toInt(), 0);
        preset->setCurrentIndex(preset->findData(30));
        QCOMPARE(store.config()["auto_hide_seconds"].toInt(), 30);
        preset->setCurrentIndex(preset->findData(-1));
        exact->setValue(47);
        QCOMPARE(store.config()["auto_hide_seconds"].toInt(), 47);
        settings.refresh();
        QCOMPARE(preset->currentData().toInt(), -1);
        QCOMPARE(exact->value(), 47);
        preset->setCurrentIndex(preset->findData(0));
        QCOMPARE(store.config()["auto_hide_seconds"].toInt(), 0);
        QVERIFY(exact->isHidden());
        auto config = store.config();
        config["auto_hide_seconds"] = 91;
        QVERIFY(store.update(config));
        QCOMPARE(preset->currentData().toInt(), -1);
        QCOMPARE(exact->value(), 91);
        QCOMPARE(exact->buttonSymbols(), QAbstractSpinBox::NoButtons);
    }

    void numericSettingsUsePreciseSliders() {
        QTemporaryDir temp;
        ConfigStore store(temp.filePath("config.json"));
        SettingsWindow settings(&store);
        auto* opacity = settings.findChild<SettingsSlider*>("opacity");
        auto* width = settings.findChild<SettingsSlider*>("width");
        QVERIFY(opacity && width);
        opacity->setValue(0.73);
        width->setValue(618);
        QCOMPARE(store.config()["opacity"].toDouble(), 0.73);
        QCOMPARE(store.config()["width"].toInt(), 618);
        for (auto* editor : settings.findChildren<QAbstractSpinBox*>())
            QCOMPARE(editor->buttonSymbols(), QAbstractSpinBox::NoButtons);
    }

    void pageMotionCanBeInterruptedAndDisabled() {
        QTemporaryDir temp;
        ConfigStore store(temp.filePath("config.json"));
        SettingsWindow settings(&store);
        settings.show();
        auto* navigation = settings.findChild<QListWidget*>("navigation");
        auto* pages = settings.findChild<QStackedWidget*>("settingsPages");
        QVERIFY(navigation && pages);
        auto* opacity = qobject_cast<QGraphicsOpacityEffect*>(pages->graphicsEffect());
        QVERIFY(opacity);
        navigation->setCurrentRow(3);
        QVERIFY(opacity->opacity() < 1.0);
        QTest::qWait(35);
        navigation->setCurrentRow(5);
        QCOMPARE(pages->currentIndex(), 5);
        settings.resize(900, 600);
        QCOMPARE(opacity->opacity(), 1.0);
        navigation->setCurrentRow(1);
        auto config = store.config();
        config["settings_animations"] = false;
        QVERIFY(store.update(config));
        QCOMPARE(opacity->opacity(), 1.0);
        navigation->setCurrentRow(6);
        QCOMPARE(opacity->opacity(), 1.0);
        settings.close();
        QVERIFY(!settings.isVisible());
        settings.show();
        QCOMPARE(opacity->opacity(), 1.0);
        QCOMPARE(pages->currentIndex(), 6);
    }
};

QTEST_MAIN(SettingsTest)
#include "test_settings.moc"

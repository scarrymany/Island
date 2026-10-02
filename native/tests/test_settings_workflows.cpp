#include "ConfigStore.h"
#include "SettingsControls.h"
#include "SettingsWindow.h"

#include <QApplication>
#include <QColorDialog>
#include "IslandDialogs.h"
#include <QDoubleSpinBox>
#include <QDesktopServices>
#include <QFile>
#include <QFileDialog>
#include <QFontDatabase>
#include <QInputDialog>
#include <QLineEdit>
#include <QJsonArray>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalSpy>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>
#include <QtTest>

#include <algorithm>
#include <functional>
#include <memory>

namespace {
constexpr int ModalTimeoutMs = 4000;
constexpr int PageCount = 7;

QJsonValue setting(const QJsonObject& config, const QString& key)
{
    const auto parts = key.split('/');
    return parts.size() == 2 ? config[parts[0]].toObject()[parts[1]] : config[key];
}

QPushButton* button(QWidget* parent, const QString& text)
{
    for (auto* candidate : parent->findChildren<QPushButton*>()) {
        if (candidate->text() == text)
            return candidate;
    }
    return nullptr;
}

QWidget* group(QWidget* parent, const QString& title)
{
    for (auto* heading : parent->findChildren<QLabel*>("groupHeading")) {
        if (heading->text() == title)
            return heading->parentWidget();
    }
    return nullptr;
}

struct DialogStep {
    enum Kind { Input, Question, Color, File } kind;
    QString text;
    int result = QDialog::Accepted;
};

QString runDialogs(const std::function<void()>& action, const QList<DialogStep>& steps)
{
    int next = 0;
    QString error;
    QTimer responder;
    QTimer watchdog;
    watchdog.setSingleShot(true);
    QObject::connect(&responder, &QTimer::timeout, &responder, [&] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (!dialog)
            return;
        if (next >= steps.size()) {
            error = QStringLiteral("Unexpected dialog: %1").arg(dialog->windowTitle());
            dialog->reject();
            return;
        }
        const auto step = steps[next++];
        bool matched = false;
        if (step.kind == DialogStep::Input) {
            if (auto* input = qobject_cast<QInputDialog*>(dialog)) {
                matched = true;
                input->setTextValue(step.text);
            }
        } else if (step.kind == DialogStep::Question) {
            matched = qobject_cast<QMessageBox*>(dialog) != nullptr;
        } else if (step.kind == DialogStep::Color) {
            if (auto* color = qobject_cast<ColorPickerDialog*>(dialog)) {
                matched = true;
                color->setCurrentColor(QColor(step.text));
            }
        } else if (auto* file = qobject_cast<QFileDialog*>(dialog)) {
            matched = true;
            file->selectFile(step.text);
            // selectFile() leaves an already focused file-name field untouched, and focus
            // arrives asynchronously; type the path the way a user would.
            if (auto* name = file->findChild<QLineEdit*>(QStringLiteral("fileNameEdit")))
                name->setText(step.text);
        }
        if (!matched) {
            error = QStringLiteral("Wrong dialog at step %1: %2").arg(next).arg(dialog->metaObject()->className());
            dialog->reject();
        } else if (step.kind == DialogStep::Question) {
            auto* message = qobject_cast<QMessageBox*>(dialog);
            auto* response = message->button(static_cast<QMessageBox::StandardButton>(step.result));
            if (response)
                response->click();
            else {
                error = QStringLiteral("Requested message button is unavailable");
                message->reject();
            }
        } else if (step.result == QDialog::Accepted) {
            dialog->accept();
        } else {
            dialog->done(step.result);
        }
    });
    QObject::connect(&watchdog, &QTimer::timeout, &watchdog, [&] {
        error = QStringLiteral("Dialog workflow exceeded %1 ms").arg(ModalTimeoutMs);
        if (auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget()))
            dialog->reject();
    });
    responder.start(10);
    watchdog.start(ModalTimeoutMs);
    action();
    if (next != steps.size() && error.isEmpty())
        error = QStringLiteral("Expected %1 dialogs, observed %2").arg(steps.size()).arg(next);
    return error;
}
}

class SettingsWorkflowsTest final : public QObject {
    Q_OBJECT

private:
    std::unique_ptr<QTemporaryDir> temp_;
    std::unique_ptr<ConfigStore> store_;
    std::unique_ptr<SettingsWindow> window_;
    QUrl openedUrl_;

    QWidget* page(int index) const
    {
        return window_->findChild<QStackedWidget*>("settingsPages")->widget(index);
    }

    void showPage(int index)
    {
        window_->findChild<QListWidget*>("navigation")->setCurrentRow(index);
        window_->show();
        QCoreApplication::processEvents();
    }

    bool save(const QString& key, const QJsonValue& value)
    {
        auto config = store_->config();
        config[key] = value;
        return store_->update(config);
    }

    void checkPersisted()
    {
        ConfigStore reloaded(store_->path());
        QVERIFY(reloaded.loadError().isEmpty());
        QCOMPARE(reloaded.config(), store_->config());
    }

private slots:
    void captureUrl(const QUrl& url)
    {
        openedUrl_ = url;
    }

    void init()
    {
        temp_ = std::make_unique<QTemporaryDir>();
        QVERIFY(temp_->isValid());
        store_ = std::make_unique<ConfigStore>(temp_->filePath("config.json"));
        auto config = store_->config();
        config["settings_animations"] = false;
        QVERIFY(store_->update(config));
        window_ = std::make_unique<SettingsWindow>(store_.get());
    }

    void cleanup()
    {
        QVERIFY(QApplication::activeModalWidget() == nullptr);
        window_.reset();
        store_.reset();
        temp_.reset();
    }

    void everyNumberUsesItsDeclaredRangeAndPersists()
    {
        const auto numbers = window_->findChildren<SettingsSlider*>();
        QCOMPARE(numbers.size(), 25);
        for (auto* control : numbers) {
            const QString key = control->objectName();
            const auto range = ConfigStore::numericRange(key);
            QCOMPARE(control->editor()->minimum(), range.first);
            QCOMPARE(control->editor()->maximum(), range.second);
            for (double value : {range.second, range.first}) {
                control->setValue(value);
                QCOMPARE(setting(store_->config(), key).toDouble(), value);
                QCOMPARE(control->editor()->value(), value);
                checkPersisted();
            }
            const double middle = control->editor()->decimals() == 0
                ? qRound((range.first + range.second) / 2) : (range.first + range.second) / 2;
            control->editor()->setValue(middle);
            QCOMPARE(setting(store_->config(), key).toDouble(), control->editor()->value());
        }
    }

    void everyToggleRoundTripsWithMouseAndKeyboard()
    {
        const auto toggles = window_->findChildren<SettingsToggle*>();
        QCOMPARE(toggles.size(), 34);
        for (auto* toggle : toggles) {
            const bool original = toggle->isChecked();
            QTest::mouseClick(toggle, Qt::LeftButton, Qt::NoModifier, toggle->rect().center());
            QCOMPARE(setting(store_->config(), toggle->objectName()).toBool(), !original);
            QTest::keyClick(toggle, Qt::Key_Space);
            QCOMPARE(setting(store_->config(), toggle->objectName()).toBool(), original);
        }
        checkPersisted();
    }

    void dependencyStatesFollowTheStoredConfiguration()
    {
        const QList<QPair<QString, QStringList>> dependencies = {
            {"gradient_enabled", {"gradient_color"}},
            {"artwork_background", {"artwork_background_strength"}},
            {"idle_collapse", {"idle_collapse_seconds"}},
            {"settings_animations", {"settings_animation_duration"}}
        };
        for (const auto& dependency : dependencies) {
            for (bool enabled : {false, true}) {
                QVERIFY(save(dependency.first, enabled));
                for (const auto& key : dependency.second) {
                    auto* control = window_->findChild<QWidget*>(key);
                    QVERIFY(control);
                    QCOMPARE(control->isEnabled(), enabled);
                }
            }
        }
        for (double width : {0.0, 0.25, 4.0, 0.0}) {
            QVERIFY(save("border_width", width));
            QCOMPARE(window_->findChild<QWidget*>("border_color")->isEnabled(), width > 0);
            QCOMPARE(window_->findChild<QWidget*>("border_opacity")->isEnabled(), width > 0);
        }
    }

    void layoutEditingAndCoordinatesFollowSelectedElementsAndMonitors()
    {
        showPage(1);
        auto* layout = window_->findChild<QComboBox*>("layout");
        auto* edit = button(page(1), QStringLiteral("Перетаскивать элементы HUD"));
        auto* reset = button(page(1), QStringLiteral("Сбросить позиции элементов"));
        QVERIFY(layout && edit && reset);
        QSignalSpy editing(window_.get(), &SettingsWindow::editLayoutChanged);
        QTest::mouseClick(edit, Qt::LeftButton);
        QCOMPARE(store_->config()["layout"].toString(), QString("custom"));
        QVERIFY(edit->isChecked());
        QCOMPARE(editing.size(), 1);
        auto* layoutGroup = group(page(1), QStringLiteral("Расположение элементов"));
        const auto coordinateEditors = layoutGroup->findChildren<QDoubleSpinBox*>();
        QCOMPARE(coordinateEditors.size(), 2);
        QComboBox* element = nullptr;
        for (auto* choice : layoutGroup->findChildren<QComboBox*>()) {
            if (choice != layout)
                element = choice;
        }
        QVERIFY(element);
        for (int index = 0; index < element->count(); ++index) {
            element->setCurrentIndex(index);
            coordinateEditors[0]->setValue(21.5 + index);
            coordinateEditors[1]->setValue(32.5 + index);
            QCOMPARE(store_->config()["element_positions"].toObject()[element->currentData().toString()].toArray(),
                     (QJsonArray{21.5 + index, 32.5 + index}));
        }
        reset->click();
        QVERIFY(store_->config()["element_positions"].toObject().isEmpty());
        layout->setCurrentIndex(layout->findData("stacked"));
        QVERIFY(!edit->isChecked());
        QVERIFY(!element->isEnabled());
        QVERIFY(!coordinateEditors[0]->isEnabled());
        QCOMPARE(editing.size(), 2);
        QVERIFY(store_->config()["height"].toInt() >= 240);
        layout->setCurrentIndex(layout->findData("island"));
        QCOMPARE(store_->config()["layout"].toString(), QString("island"));

        auto* anchor = window_->findChild<QComboBox*>("anchor");
        auto* monitor = window_->findChild<QComboBox*>("monitor");
        const auto monitorEditors = group(page(1), QStringLiteral("Монитор и положение"))->findChildren<QSpinBox*>();
        QCOMPARE(monitorEditors.size(), 2);
        anchor->setCurrentIndex(anchor->findData("free"));
        QVERIFY(!window_->findChild<QWidget*>("offset_y")->isEnabled());
        QVERIFY(monitorEditors[0]->isEnabled());
        for (const QString& name : {QStringLiteral("QA-DISPLAY-A"), QStringLiteral("QA-DISPLAY-B")}) {
            QVERIFY(save("monitor", name));
            QCOMPARE(monitor->currentData().toString(), name);
            monitorEditors[0]->setValue(-110);
            monitorEditors[1]->setValue(78);
            QCOMPARE(store_->config()["monitor_positions"].toObject()[name].toArray(), (QJsonArray{-110, 78}));
        }
        anchor->setCurrentIndex(anchor->findData("top_center"));
        QVERIFY(!monitorEditors[0]->isEnabled());
        QVERIFY(window_->findChild<QWidget*>("offset_y")->isEnabled());
        checkPersisted();
    }

    void sourceSelectionSurvivesDisconnectionAndRefresh()
    {
        auto* sources = window_->findChild<QComboBox*>("source_id");
        QSignalSpy changed(window_.get(), &SettingsWindow::sourceChanged);
        window_->setSources({{"player-a", "Player A"}, {"player-a", "Duplicate"}, {"player-b", ""}, {"", "Ignored"}});
        QCOMPARE(sources->count(), 3);
        sources->setCurrentIndex(sources->findData("player-b"));
        QCOMPARE(changed.size(), 1);
        QCOMPARE(changed.first().first().toString(), QString("player-b"));
        window_->setSources({});
        QCOMPARE(sources->currentData().toString(), QString("player-b"));
        QVERIFY(sources->currentText().startsWith(QStringLiteral("Недоступен:")));
        window_->refresh();
        QCOMPARE(changed.size(), 1);
        window_->setSources({{"player-b", "Reconnected player"}});
        QCOMPARE(sources->currentText(), QString("Reconnected player"));
        sources->setCurrentIndex(0);
        QCOMPARE(changed.size(), 2);
        QVERIFY(store_->config()["source_id"].toString().isEmpty());
        checkPersisted();
    }

    void everyFontWeightAndHideDelayChoicePersists()
    {
        auto* weight = window_->findChild<QComboBox*>("font_weight");
        auto* delay = window_->findChild<QComboBox*>("auto_hide_seconds");
        auto* custom = window_->findChild<QSpinBox*>("customHideDelay");
        QVERIFY(weight && delay && custom);
        for (int index = 0; index < weight->count(); ++index) {
            weight->setCurrentIndex(index);
            QCOMPARE(store_->config()["font_weight"].toInt(), weight->itemData(index).toInt());
            QVERIFY(store_->config()["font_weight"].isDouble());
        }
        for (int index = 0; index < delay->count(); ++index) {
            const int seconds = delay->itemData(index).toInt();
            delay->setCurrentIndex(index);
            if (seconds < 0) {
                for (int value : {1, 3600}) {
                    custom->setValue(value);
                    QCOMPARE(store_->config()["auto_hide_seconds"].toInt(), value);
                }
            } else {
                QCOMPARE(store_->config()["auto_hide_seconds"].toInt(), seconds);
                QVERIFY(custom->isHidden());
            }
        }
        checkPersisted();
    }

    void everyColorDialogAppliesAndCancels()
    {
        const QStringList colors = {"background", "gradient_color", "accent_color", "border_color", "compact_background",
            "text_color", "secondary_color", "icon_color", "progress_color", "settings_background", "settings_accent", "settings_text"};
        QVERIFY(save("border_width", 1.0));
        for (const auto& key : colors) {
            auto* control = window_->findChild<QPushButton*>(key);
            QVERIFY(control);
            QString error = runDialogs([control] { control->click(); }, {{DialogStep::Color, "#B16238"}});
            QVERIFY2(error.isEmpty(), qPrintable(error));
            QCOMPARE(store_->config()[key].toString(), QString("#B16238"));
            QCOMPARE(control->text(), QString("#B16238"));
            error = runDialogs([control] { control->click(); }, {{DialogStep::Color, "#12ABCD", QDialog::Rejected}});
            QVERIFY2(error.isEmpty(), qPrintable(error));
            QCOMPARE(store_->config()[key].toString(), QString("#B16238"));
        }
        checkPersisted();
    }

    void profileSaveReplaceApplyDeleteAndCancel()
    {
        showPage(4);
        auto* profiles = group(page(4), QStringLiteral("Профили HUD"));
        QVERIFY(profiles);
        auto* saveButton = button(profiles, QStringLiteral("Сохранить как..."));
        auto* apply = button(profiles, QStringLiteral("Применить"));
        auto* remove = button(profiles, QStringLiteral("Удалить"));
        QVERIFY(saveButton && apply && remove);
        QString error = runDialogs([&] { saveButton->click(); }, {{DialogStep::Input, " QA profile "}});
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(store_->profiles(), QStringList{"QA profile"});
        const int savedWidth = store_->config()["width"].toInt();
        QVERIFY(save("width", 700));
        apply->click();
        QCOMPARE(store_->config()["width"].toInt(), savedWidth);
        QVERIFY(save("width", 700));
        error = runDialogs([&] { saveButton->click(); }, {{DialogStep::Input, "QA profile"}, {DialogStep::Question, {}, QMessageBox::No}});
        QVERIFY2(error.isEmpty(), qPrintable(error));
        apply->click();
        QCOMPARE(store_->config()["width"].toInt(), savedWidth);
        QVERIFY(save("width", 700));
        error = runDialogs([&] { saveButton->click(); }, {{DialogStep::Input, "QA profile"}, {DialogStep::Question, {}, QMessageBox::Yes}});
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(save("width", 900));
        apply->click();
        QCOMPARE(store_->config()["width"].toInt(), 700);
        error = runDialogs([&] { saveButton->click(); }, {{DialogStep::Input, "Discarded", QDialog::Rejected}});
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(store_->profiles().size(), 1);
        error = runDialogs([&] { remove->click(); }, {{DialogStep::Question, {}, QMessageBox::No}});
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(store_->profiles().size(), 1);
        error = runDialogs([&] { remove->click(); }, {{DialogStep::Question, {}, QMessageBox::Yes}});
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(store_->profiles().isEmpty());
        QVERIFY(profiles->findChild<QComboBox*>()->currentText().isEmpty());
        apply->click();
        remove->click();
        checkPersisted();
    }

    void themeSaveReplaceApplyDeleteAndBuiltinProtection()
    {
        showPage(4);
        auto* themes = group(page(4), QStringLiteral("Темы оформления"));
        QVERIFY(themes);
        auto* choice = themes->findChild<QComboBox*>();
        auto* saveButton = button(themes, QStringLiteral("Сохранить тему..."));
        auto* apply = button(themes, QStringLiteral("Применить"));
        auto* remove = button(themes, QStringLiteral("Удалить"));
        QVERIFY(choice && saveButton && apply && remove);
        QVERIFY(save("accent_color", "#123456"));
        QString error = runDialogs([&] { saveButton->click(); }, {{DialogStep::Input, "QA theme"}});
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(store_->themes().contains("QA theme"));
        QVERIFY(save("accent_color", "#ABCDEF"));
        QVERIFY(save("width", 812));
        choice->setCurrentText("QA theme");
        apply->click();
        QCOMPARE(store_->config()["accent_color"].toString(), QString("#123456"));
        QCOMPARE(store_->config()["width"].toInt(), 812);
        QVERIFY(save("accent_color", "#654321"));
        error = runDialogs([&] { saveButton->click(); }, {{DialogStep::Input, "QA theme"}, {DialogStep::Question, {}, QMessageBox::Yes}});
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(save("accent_color", "#ABCDEF"));
        choice->setCurrentText("QA theme");
        apply->click();
        QCOMPARE(store_->config()["accent_color"].toString(), QString("#654321"));
        error = runDialogs([&] { remove->click(); }, {{DialogStep::Question, {}, QMessageBox::No}});
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(store_->themes().contains("QA theme"));
        error = runDialogs([&] { remove->click(); }, {{DialogStep::Question, {}, QMessageBox::Yes}});
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(!store_->themes().contains("QA theme"));
        for (const QString& name : {QString("Lunar"), QString("Midnight"), QString("Ember"), QString("Mono")}) {
            choice->setCurrentText(name);
            const auto before = store_->config();
            error = runDialogs([&] { remove->click(); }, {{DialogStep::Question, {}, QMessageBox::Ok}});
            QVERIFY2(error.isEmpty(), qPrintable(error));
            QCOMPARE(store_->config(), before);
            QVERIFY(store_->themes().contains(name));
            auto* quickTheme = button(page(0), name);
            QVERIFY(quickTheme);
            quickTheme->click();
            QCOMPARE(store_->config()["width"].toInt(), 812);
        }
        checkPersisted();
    }

    void exportImportAndInvalidImportUseTemporaryFiles()
    {
        showPage(4);
        auto* exportButton = button(page(4), QStringLiteral("Экспорт JSON"));
        auto* importButton = button(page(4), QStringLiteral("Импорт JSON"));
        QVERIFY(exportButton && importButton);
        QVERIFY(store_->saveProfile("Export profile"));
        QVERIFY(store_->saveTheme("Export theme"));
        const auto exported = store_->config();
        const QString path = temp_->filePath("exported.json");
        QString error = runDialogs([&] { exportButton->click(); }, {{DialogStep::File, path}});
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(QFile::exists(path));
        QVERIFY(save("width", 899));
        error = runDialogs([&] { importButton->click(); }, {{DialogStep::File, path}});
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(store_->config(), exported);
        QVERIFY(store_->profiles().contains("Export profile"));
        QVERIFY(store_->themes().contains("Export theme"));
        QFile invalid(temp_->filePath("invalid.json"));
        QVERIFY(invalid.open(QIODevice::WriteOnly));
        QCOMPARE(invalid.write("{broken"), qint64(7));
        invalid.close();
        error = runDialogs([&] { importButton->click(); }, {{DialogStep::File, invalid.fileName()}, {DialogStep::Question, {}, QMessageBox::Ok}});
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(store_->config(), exported);
        error = runDialogs([&] { importButton->click(); }, {{DialogStep::File, {}, QDialog::Rejected}});
        QVERIFY2(error.isEmpty(), qPrintable(error));
        error = runDialogs([&] { exportButton->click(); }, {{DialogStep::File, {}, QDialog::Rejected}});
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(store_->config(), exported);
        checkPersisted();
    }

    void updateActionsRespectBusyAvailableAndRepositoryStates()
    {
        showPage(6);
        auto* check = button(page(6), QStringLiteral("Проверить обновления"));
        auto* install = button(page(6), QStringLiteral("Установить обновление"));
        auto* repository = window_->findChild<QPushButton*>("projectRepository");
        QVERIFY(check && install && repository);
        QSignalSpy checked(window_.get(), &SettingsWindow::checkUpdates);
        QSignalSpy installed(window_.get(), &SettingsWindow::updateInstallRequested);
        QVERIFY(check->isEnabled());
        QVERIFY(!install->isEnabled());
        QTest::mouseClick(check, Qt::LeftButton);
        QCOMPARE(checked.size(), 1);
        for (bool busy : {false, true}) {
            for (bool available : {false, true}) {
                window_->setUpdateState("QA update state", available, busy);
                QCOMPARE(check->isEnabled(), !busy);
                QCOMPARE(install->isEnabled(), available && !busy);
            }
        }
        window_->setUpdateState("QA ready", true, false);
        install->click();
        QCOMPARE(installed.size(), 1);
        QDesktopServices::setUrlHandler("https", this, "captureUrl");
        repository->click();
        QDesktopServices::unsetUrlHandler("https");
        QCOMPARE(openedUrl_, QUrl("https://github.com/scarrymany/Island"));
        QVERIFY(save("update_repository", ""));
        QCOMPARE(store_->config()["update_repository"].toString(), QString("scarrymany/Island"));
        QVERIFY(check->isEnabled());
        checkPersisted();
    }

    void navigationCaptionAndHudActionsHaveCompleteLifecycles()
    {
        showPage(0);
        auto* navigation = window_->findChild<QListWidget*>("navigation");
        auto* pages = window_->findChild<QStackedWidget*>("settingsPages");
        QCOMPARE(navigation->count(), PageCount);
        QCOMPARE(pages->count(), PageCount);
        for (int index = 0; index < PageCount; ++index) {
            QTest::mouseClick(navigation->viewport(), Qt::LeftButton, Qt::NoModifier,
                navigation->visualItemRect(navigation->item(index)).center());
            QCOMPARE(pages->currentIndex(), index);
            QCOMPARE(window_->findChild<QLabel*>("heading")->text(), navigation->item(index)->text());
        }
        navigation->setFocus();
        QTest::keyClick(navigation, Qt::Key_Home);
        QCOMPARE(pages->currentIndex(), 0);
        QTest::keyClick(navigation, Qt::Key_End);
        QCOMPARE(pages->currentIndex(), 6);
        QVERIFY(!button(window_.get(), "HUD"));
        QPushButton* minimize = nullptr;
        QPushButton* maximize = nullptr;
        for (auto* control : window_->findChildren<QPushButton*>()) {
            if (control->toolTip() == QStringLiteral("Свернуть"))
                minimize = control;
            if (control->toolTip() == QStringLiteral("Развернуть / восстановить"))
                maximize = control;
        }
        QVERIFY(minimize && maximize);
        maximize->click();
        QVERIFY(window_->isMaximized());
        maximize->click();
        QVERIFY(!window_->isMaximized());
        minimize->click();
        QVERIFY(window_->isMinimized());
        window_->showNormal();
        QVERIFY(!window_->isMinimized());
        window_->setEditing(true);
        QSignalSpy editing(window_.get(), &SettingsWindow::editLayoutChanged);
        window_->findChild<QPushButton*>("windowClose")->click();
        QVERIFY(!window_->isVisible());
        QCOMPARE(editing.size(), 1);
        QCOMPARE(editing.first().first().toBool(), false);
        window_->show();
        QCOMPARE(pages->currentIndex(), 6);
    }

    void allPagesRemainReachableAcrossThemeFontAndWindowSizes()
    {
        const QStringList palettes = {"#0A0A0A", "#F3F4F6"};
        const QList<QSize> sizes = {QSize(820, 520), QSize(1080, 800), QSize(1400, 950)};
        for (const auto& background : palettes) {
            QVERIFY(save("settings_background", background));
            QVERIFY(save("settings_text", background == "#0A0A0A" ? "#D4D4D4" : "#202124"));
            for (int fontSize : {8, 10, 18}) {
                QVERIFY(save("settings_font_size", fontSize));
                for (const auto& size : sizes) {
                    window_->resize(size);
                    for (int index = 0; index < PageCount; ++index) {
                        showPage(index);
                        auto* scroll = qobject_cast<QScrollArea*>(page(index));
                        QVERIFY(scroll);
                        QVERIFY(scroll->viewport()->width() > 100);
                        QVERIFY(scroll->viewport()->height() > 100);
                        const QString context = QStringLiteral("Page %1, font %2, requested %3x%4: content %5, viewport %6")
                            .arg(index).arg(fontSize).arg(size.width()).arg(size.height())
                            .arg(scroll->widget()->width()).arg(scroll->viewport()->width());
                        QVERIFY2(scroll->widget()->width() <= scroll->viewport()->width(), qPrintable(context));
                        for (auto* toggle : scroll->findChildren<SettingsToggle*>()) {
                            const QString toggleContext = QStringLiteral("%1: toggle %2 width %3, minimum hint %4")
                                .arg(context, toggle->objectName()).arg(toggle->width()).arg(toggle->minimumSizeHint().width());
                            QVERIFY2(toggle->width() >= toggle->minimumSizeHint().width(), qPrintable(toggleContext));
                        }
                        auto* vertical = scroll->verticalScrollBar();
                        vertical->setValue(vertical->maximum());
                        QCOMPARE(vertical->value(), vertical->maximum());
                        int lastGroupBottom = 0;
                        for (auto* child : scroll->widget()->findChildren<QWidget*>("settingsGroup", Qt::FindDirectChildrenOnly))
                            lastGroupBottom = std::max(lastGroupBottom, child->geometry().bottom());
                        const QString scrollContext = QStringLiteral("Page %1, font %2, requested %3x%4: last group ends %5, scroll position %6")
                            .arg(index).arg(fontSize).arg(size.width()).arg(size.height())
                            .arg(lastGroupBottom).arg(vertical->value());
                        QVERIFY2(vertical->value() < lastGroupBottom, qPrintable(scrollContext));
                        vertical->setValue(0);
                        QVERIFY(!window_->grab().isNull());
                    }
                }
            }
        }
        for (const QString& key : {QString("font_family"), QString("settings_font_family")}) {
            auto* font = window_->findChild<QFontComboBox*>(key);
            QVERIFY(font);
            for (const auto& family : {QString("Inter"), QFontDatabase::systemFont(QFontDatabase::FixedFont).family()}) {
                font->setCurrentFont(QFont(family));
                QCOMPARE(store_->config()[key].toString(), font->currentFont().family());
                window_->refresh();
                QCOMPARE(store_->config()[key].toString(), font->currentFont().family());
            }
        }
        checkPersisted();
    }
};

int main(int argc, char** argv)
{
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication application(argc, argv);
    QApplication::setQuitOnLastWindowClosed(false);
    SettingsWorkflowsTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_settings_workflows.moc"

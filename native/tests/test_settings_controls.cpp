#include "AppAssets.h"
#include "SettingsControls.h"

#include <QAccessible>
#include <QDoubleSpinBox>
#include <QFontDatabase>
#include <QListView>
#include <QPushButton>
#include <QScreen>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSlider>
#include <QStandardItemModel>
#include <QStringListModel>
#include <QWindow>
#include <QtTest>

#include <Windows.h>

#include <limits>

namespace {
void prepareNavigation(SettingsNavigation& navigation)
{
    navigation.resize(220, 220);
    navigation.setStyleSheet("QListWidget { background: #101010; border: none; }");
    navigation.setColors(QColor("#FFFFFF"), QColor("#A02020"), QColor("#FFFFFF"));
    for (int row = 0; row < 6; ++row) {
        auto* item = new QListWidgetItem(QString::number(row), &navigation);
        item->setSizeHint(QSize(180, 40));
    }
    navigation.setCurrentRow(0);
    navigation.show();
    navigation.clearFocus();
    QCoreApplication::processEvents();
}

int navigationPillCenter(SettingsNavigation& navigation)
{
    const QImage image = navigation.viewport()->grab().toImage();
    const int x = qRound((navigation.visualItemRect(navigation.currentItem()).left() + 6) * image.devicePixelRatio());
    int first = -1;
    int last = -1;
    for (int y = 0; y < image.height(); ++y) {
        const QColor color = image.pixelColor(x, y);
        if (color.red() > color.green() + 15) {
            if (first < 0) first = y;
            last = y;
        }
    }
    return first < 0 ? -1 : qRound((first + last) / (2 * image.devicePixelRatio()));
}
}

class SettingsControlsTest final : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QApplication::setFont(QFont(AppAssets::settingsFontFamily()));
    }

    void navigationUsesProvidedFillAndAccent()
    {
        SettingsNavigation navigation;
        prepareNavigation(navigation);
        navigation.setColors(QColor("#89B4FA"), QColor("#1A1A1A"), QColor("#F4F4F5"));
        navigation.setIconSize(QSize(16, 16));
        QPixmap icon(16, 16);
        icon.fill(Qt::white);
        navigation.currentItem()->setIcon(QIcon(icon));
        const QImage image = navigation.viewport()->grab().toImage();
        const QRect row = navigation.visualItemRect(navigation.currentItem());
        const auto pixel = [&image](int x, int y) {
            return image.pixelColor(qRound(x * image.devicePixelRatio()), qRound(y * image.devicePixelRatio()));
        };
        QCOMPARE(pixel(row.left() + 6, row.center().y()), QColor("#1A1A1A"));
        QCOMPARE(pixel(row.left() + 19, row.center().y()), QColor("#89B4FA"));
    }

    void navigationPillGlidesAndReversesWithoutChangingSelection()
    {
        SettingsNavigation navigation;
        prepareNavigation(navigation);
        navigation.setMotion(true, 220, 144);
        const int initial = navigationPillCenter(navigation);
        QVERIFY(initial >= 0);
        QSignalSpy changed(&navigation, &QListWidget::currentRowChanged);
        navigation.setCurrentRow(3);
        QCOMPARE(navigationPillCenter(navigation), initial);
        QTest::qWait(45);
        const int middle = navigationPillCenter(navigation);
        QVERIFY(middle > initial);
        QVERIFY(middle < navigation.visualItemRect(navigation.currentItem()).center().y());
        navigation.setCurrentRow(0);
        QCOMPARE(navigationPillCenter(navigation), middle);
        // The spring carries its speed into the reversal, so it settles a little later.
        QTRY_COMPARE_WITH_TIMEOUT(navigationPillCenter(navigation), initial, 1000);
        QCOMPARE(changed.size(), 2);
        QCOMPARE(navigation.currentRow(), 0);
    }

    void navigationSettlesForReducedMotionResizeScrollAndHide()
    {
        SettingsNavigation navigation;
        prepareNavigation(navigation);
        navigation.setMotion(true, 300, 144);
        navigation.setCurrentRow(2);
        navigation.setMotion(false, 300, 144);
        QVERIFY(qAbs(navigationPillCenter(navigation) - navigation.visualItemRect(navigation.currentItem()).center().y()) <= 1);
        navigation.setMotion(true, 300, 144);
        navigation.setCurrentRow(3);
        navigation.resize(230, 210);
        QVERIFY(qAbs(navigationPillCenter(navigation) - navigation.visualItemRect(navigation.currentItem()).center().y()) <= 1);
        const int scrollBefore = navigation.verticalScrollBar()->value();
        navigation.setCurrentRow(5);
        navigation.scrollToItem(navigation.currentItem(), QAbstractItemView::PositionAtBottom);
        QVERIFY(navigation.verticalScrollBar()->value() > scrollBefore);
        QVERIFY(qAbs(navigationPillCenter(navigation) - navigation.visualItemRect(navigation.currentItem()).center().y()) <= 1);
        navigation.setCurrentRow(3);
        navigation.hide();
        navigation.show();
        navigation.clearFocus();
        QVERIFY(qAbs(navigationPillCenter(navigation) - navigation.visualItemRect(navigation.currentItem()).center().y()) <= 1);
    }

    void navigationRetainsKeyboardAndAccessibleItems()
    {
        SettingsNavigation navigation;
        prepareNavigation(navigation);
        navigation.setMotion(false, 200, 60);
        navigation.setFocus(Qt::TabFocusReason);
        QSignalSpy changed(&navigation, &QListWidget::currentRowChanged);
        QTest::keyClick(&navigation, Qt::Key_Down);
        QCOMPARE(navigation.currentRow(), 1);
        QCOMPARE(changed.size(), 1);
        auto* accessible = QAccessible::queryAccessibleInterface(&navigation);
        QVERIFY(accessible);
        QCOMPARE(accessible->role(), QAccessible::List);
        QVERIFY(accessible->childCount() >= navigation.count());
        navigation.clear();
        QCOMPARE(navigationPillCenter(navigation), -1);
    }

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

    void choicesUseCustomPopupInsteadOfNativeContainer()
    {
        SettingsChoice choice;
        choice.addItems({"First", "Second"});
        choice.resize(240, 38);
        choice.show();
        choice.showPopup();
        auto* popup = choice.findChild<QWidget*>(QStringLiteral("settingsChoicePopup"));
        QVERIFY(popup);
        QVERIFY(popup->isVisible());
        QCOMPARE(popup->width(), choice.width());
        QVERIFY(popup->testAttribute(Qt::WA_TranslucentBackground));
        choice.hidePopup();
    }

    void fontChoiceUsesReadOnlyField()
    {
        SettingsFontChoice choice;
        QVERIFY(!choice.isEditable());
    }

    void mouseFocusDoesNotLeaveChoiceBorderHighlighted()
    {
        SettingsChoice choice;
        choice.addItem("First");
        choice.resize(240, 38);
        choice.show();
        choice.activateWindow();
        QCoreApplication::processEvents();
        choice.clearFocus();
        QCoreApplication::processEvents();
        const QImage unfocused = choice.grab().toImage();
        choice.setFocus(Qt::MouseFocusReason);
        QVERIFY(choice.hasFocus());
        QCOMPARE(choice.grab().toImage(), unfocused);
        choice.clearFocus();
        choice.setFocus(Qt::TabFocusReason);
        QVERIFY(choice.grab().toImage() != unfocused);
    }

    void choicePopupKeepsSelectionVisibleAndCommitsOnlyOnActivation()
    {
        SettingsChoice choice;
        choice.setMotion(false, 200, 144);
        for (int row = 0; row < 60; ++row) choice.addItem(QString::number(row), row);
        choice.setCurrentIndex(45);
        choice.resize(240, 38);
        choice.show();
        QSignalSpy changed(&choice, &QComboBox::currentIndexChanged);
        QSignalSpy activated(&choice, &QComboBox::activated);
        QSignalSpy highlighted(&choice, &QComboBox::highlighted);
        choice.showPopup();
        auto* list = choice.findChild<QListView*>(QStringLiteral("settingsChoiceList"));
        QVERIFY(list);
        QCOMPARE(list->currentIndex().row(), 45);
        QVERIFY(list->viewport()->rect().contains(list->visualRect(list->currentIndex())));
        const QRect viewport = list->viewport()->rect();
        const QModelIndex firstVisible = list->indexAt(QPoint(viewport.center().x(), viewport.top()));
        const QModelIndex lastVisible = list->indexAt(QPoint(viewport.center().x(), viewport.bottom()));
        QVERIFY(firstVisible.isValid());
        QVERIFY(lastVisible.isValid());
        QVERIFY(list->visualRect(firstVisible).top() >= viewport.top());
        QVERIFY(list->visualRect(lastVisible).bottom() <= viewport.bottom());
        QVERIFY(list->verticalScrollBar()->maximum() > 0);
        QTest::keyClick(list, Qt::Key_Down);
        QCOMPARE(choice.currentIndex(), 45);
        QCOMPARE(changed.size(), 0);
        QCOMPARE(list->currentIndex().row(), 46);
        QCOMPARE(highlighted.size(), 1);
        QTest::keyClick(list, Qt::Key_Return);
        QCOMPARE(choice.currentData().toInt(), 46);
        QCOMPARE(changed.size(), 1);
        QCOMPARE(activated.size(), 1);
        QVERIFY(!list->isVisible());
        choice.showPopup();
        QTest::keyClick(list, Qt::Key_Down);
        QTest::keyClick(list, Qt::Key_Escape);
        QCOMPARE(choice.currentIndex(), 46);
        QCOMPARE(activated.size(), 1);
        choice.showPopup();
        QTest::keyClick(list, Qt::Key_Return);
        QCOMPARE(activated.size(), 2);
        QCOMPARE(changed.size(), 1);
    }

    void choicePopupMouseSelectionSkipsDisabledItems()
    {
        SettingsChoice choice;
        choice.setMotion(false, 0, 60);
        choice.addItems({"First", "Disabled", "Last"});
        auto* model = qobject_cast<QStandardItemModel*>(choice.model());
        QVERIFY(model);
        model->item(1)->setEnabled(false);
        choice.show();
        choice.showPopup();
        auto* list = choice.findChild<QListView*>(QStringLiteral("settingsChoiceList"));
        QTest::keyClick(list, Qt::Key_Down);
        QCOMPARE(list->currentIndex().row(), 2);
        QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier, list->visualRect(model->index(1, 0)).center());
        QCOMPARE(choice.currentIndex(), 0);
        QVERIFY(list->isVisible());
        QSignalSpy activated(&choice, &QComboBox::activated);
        QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier, list->visualRect(model->index(2, 0)).center());
        QCOMPARE(choice.currentIndex(), 2);
        QCOMPARE(activated.size(), 1);
        QVERIFY(!list->isVisible());
    }

    void choicePopupFitsScreenAndOpensAboveBottomField()
    {
        SettingsChoice choice;
        choice.setMotion(false, 0, 60);
        for (int row = 0; row < 50; ++row) choice.addItem(QString::number(row));
        const QRect available = choice.screen()->availableGeometry();
        choice.setGeometry(available.left() + 30, available.bottom() - 70, 240, 38);
        choice.show();
        choice.showPopup();
        auto* popup = choice.findChild<QWidget*>(QStringLiteral("settingsChoicePopup"));
        QVERIFY(available.contains(popup->geometry()));
        QVERIFY(popup->geometry().bottom() < choice.mapToGlobal(QPoint(0, 0)).y());
        QCOMPARE(popup->width(), choice.width());
        choice.hidePopup();
    }

    void fontPopupUsesUniformRowsAndPreservesExactFamily()
    {
        SettingsFontChoice choice;
        choice.setMotion(false, 0, 60);
        choice.resize(240, 38);
        choice.setCurrentIndex(choice.count() / 2);
        choice.show();
        choice.showPopup();
        auto* list = choice.findChild<QListView*>(QStringLiteral("settingsChoiceList"));
        QVERIFY(list);
        QCOMPARE(list->font(), choice.font());
        const QModelIndex firstVisible = list->indexAt(QPoint(list->viewport()->width() / 2, 0));
        QVERIFY(firstVisible.isValid());
        QVERIFY(list->visualRect(firstVisible).top() >= 0);
        QVERIFY(list->itemDelegate() != choice.itemDelegate());
        QStyleOptionViewItem option;
        option.font = choice.font();
        const int height = list->itemDelegate()->sizeHint(option, choice.model()->index(0, 0)).height();
        for (int row = 1; row < choice.count(); ++row)
            QCOMPARE(list->itemDelegate()->sizeHint(option, choice.model()->index(row, 0)).height(), height);
        if (choice.count() < 2) QSKIP("This platform does not expose enough fonts");
        const int row = choice.currentIndex() == 0 ? choice.count() - 1 : 0;
        const QString family = choice.itemText(row);
        list->setCurrentIndex(choice.model()->index(row, 0));
        QSignalSpy changed(&choice, &QFontComboBox::currentFontChanged);
        QTest::keyClick(list, Qt::Key_Return);
        QCOMPARE(choice.currentFont().family(), family);
        QCOMPARE(changed.size(), 1);
    }

    void choicePopupStopsForModelChangesWindowHideAndDisable()
    {
        QWidget window;
        SettingsChoice choice(&window);
        choice.setMotion(true, 200, 144);
        QStringListModel model({"First", "Second", "Third"});
        choice.setModel(&model);
        window.show();
        choice.showPopup();
        auto* popup = choice.findChild<QWidget*>(QStringLiteral("settingsChoicePopup"));
        QVERIFY(popup->isVisible());
        model.setStringList({"New first", "New second"});
        QVERIFY(!popup->isVisible());
        choice.showPopup();
        choice.setEnabled(false);
        QVERIFY(!popup->isVisible());
        choice.setEnabled(true);
        choice.showPopup();
        choice.setFont(QFont(choice.font().family(), 15));
        QVERIFY(!popup->isVisible());
        choice.showPopup();
        window.hide();
        QVERIFY(!popup->isVisible());
        auto* closing = choice.findChild<QWidget*>(QStringLiteral("settingsChoiceClosingFrame"));
        QVERIFY(closing);
        QVERIFY(!closing->isVisible());
    }

    void choicePopupAnimationClosesWithoutHoldingInputAndCanReopen()
    {
        SettingsChoice choice;
        choice.setMotion(true, 180, 144);
        choice.addItems({"First", "Second"});
        choice.resize(240, 38);
        choice.show();
        choice.showPopup();
        auto* popup = choice.findChild<QWidget*>(QStringLiteral("settingsChoicePopup"));
        auto* closing = choice.findChild<QWidget*>(QStringLiteral("settingsChoiceClosingFrame"));
        QVERIFY(popup->windowOpacity() < 0.1);
        QTest::qWait(65);
        QVERIFY(popup->windowOpacity() > 0);
        QVERIFY(popup->windowOpacity() < 1);
        choice.hidePopup();
        QVERIFY(!popup->isVisible());
        QVERIFY(QApplication::activePopupWidget() != popup);
        QVERIFY(closing->isVisible());
        QVERIFY(closing->windowFlags().testFlag(Qt::WindowTransparentForInput));
        choice.showPopup();
        QVERIFY(popup->isVisible());
        QVERIFY(!closing->isVisible());
        choice.setMotion(false, 180, 144);
        QCOMPARE(popup->windowOpacity(), 1.0);
        choice.hidePopup();
        QVERIFY(!closing->isVisible());
        choice.setMotion(true, 90, 144);
        choice.showPopup();
        QTest::qWait(120);
        choice.hidePopup();
        QTRY_VERIFY(!closing->isVisible());
    }

    void choiceAccessibleStateAndActionUseVisiblePopup()
    {
        SettingsChoice choice;
        choice.setMotion(false, 0, 60);
        choice.setAccessibleName("Audio device");
        choice.addItems({"First", "Second"});
        choice.show();
        auto* accessible = QAccessible::queryAccessibleInterface(&choice);
        QVERIFY(accessible);
        QCOMPARE(accessible->role(), QAccessible::ComboBox);
        QCOMPARE(accessible->text(QAccessible::Name), QStringLiteral("Audio device"));
        QCOMPARE(accessible->text(QAccessible::Value), QStringLiteral("First"));
        QVERIFY(accessible->state().collapsed);
        QVERIFY(!accessible->state().expanded);
        accessible->actionInterface()->doAction(QAccessibleActionInterface::showMenuAction());
        QVERIFY(accessible->state().expanded);
        QVERIFY(!accessible->state().collapsed);
        QCOMPARE(accessible->childCount(), 1);
        QCOMPARE(accessible->child(0)->role(), QAccessible::PopupMenu);
        QVERIFY(!accessible->child(0)->state().invisible);
        QCOMPARE(accessible->child(0)->parent(), accessible);
        QCOMPARE(accessible->child(0)->childCount(), 1);
        auto* list = accessible->child(0)->child(0);
        QCOMPARE(list->role(), QAccessible::List);
        QCOMPARE(list->parent(), accessible->child(0));
        accessible->actionInterface()->doAction(QAccessibleActionInterface::showMenuAction());
        QVERIFY(accessible->state().collapsed);
    }

    void openingAnotherChoiceClosesPreviousPopup()
    {
        QWidget window;
        SettingsChoice first(&window);
        SettingsChoice second(&window);
        first.addItems({"First", "Second"});
        second.addItems({"One", "Two"});
        second.move(0, 50);
        window.resize(300, 150);
        window.show();
        first.showPopup();
        auto* firstPopup = first.findChild<QWidget*>(QStringLiteral("settingsChoicePopup"));
        second.showPopup();
        auto* secondPopup = second.findChild<QWidget*>(QStringLiteral("settingsChoicePopup"));
        QVERIFY(!firstPopup->isVisible());
        QVERIFY(secondPopup->isVisible());
        second.hidePopup();
    }

    void choicePopupSearchesByPrefixWithoutCommitting()
    {
        SettingsChoice choice;
        choice.setMotion(false, 0, 60);
        choice.addItems({"Alpha", "Beta", "Gamma"});
        choice.show();
        choice.showPopup();
        auto* list = choice.findChild<QListView*>(QStringLiteral("settingsChoiceList"));
        QTest::keyClicks(list, "be");
        QCOMPARE(list->currentIndex().row(), 1);
        QCOMPARE(choice.currentIndex(), 0);
        QTest::keyClick(list, Qt::Key_Return);
        QCOMPARE(choice.currentText(), QStringLiteral("Beta"));
    }

    void choicePopupClosesWhenExternalModelIsDestroyed()
    {
        SettingsChoice choice;
        auto* model = new QStringListModel({"First", "Second"});
        choice.setModel(model);
        choice.show();
        choice.showPopup();
        auto* popup = choice.findChild<QWidget*>(QStringLiteral("settingsChoicePopup"));
        QVERIFY(popup->isVisible());
        delete model;
        QVERIFY(!popup->isVisible());
    }

    void choiceClosingAnimationStopsOnOtherPopupAndParentHide()
    {
        QWidget window;
        SettingsChoice first(&window);
        SettingsChoice second(&window);
        first.addItems({"First", "Second"});
        second.addItems({"One", "Two"});
        first.setMotion(true, 180, 144);
        second.move(0, 50);
        window.resize(300, 150);
        window.show();
        first.showPopup();
        QTest::qWait(40);
        first.hidePopup();
        auto* closing = first.findChild<QWidget*>(QStringLiteral("settingsChoiceClosingFrame"));
        QVERIFY(closing->isVisible());
        second.showPopup();
        QVERIFY(!closing->isVisible());
        second.hidePopup();
        first.showPopup();
        QTest::qWait(40);
        first.hidePopup();
        QVERIFY(closing->isVisible());
        window.hide();
        QVERIFY(!closing->isVisible());
    }

    void nativePopupReplaysOutsideClickAndClosesOnFieldClick()
    {
        if (QGuiApplication::platformName() != QStringLiteral("windows"))
            QSKIP("Native popup event routing requires the Windows platform");
        QWidget window;
        struct CursorRestore {
            POINT position{};
            CursorRestore() { GetCursorPos(&position); }
            ~CursorRestore() { SetCursorPos(position.x, position.y); }
        } restoreCursor;
        SettingsChoice first(&window);
        SettingsChoice second(&window);
        QPushButton button("Outside", &window);
        first.addItems({"First", "Second"});
        second.addItems({"One", "Two"});
        first.setMotion(true, 200, 144);
        second.setMotion(true, 200, 144);
        first.setGeometry(20, 20, 180, 38);
        second.setGeometry(220, 20, 180, 38);
        button.setGeometry(420, 20, 100, 38);
        window.resize(550, 220);
        window.show();
        const auto nativeWindow = reinterpret_cast<HWND>(window.winId());
        // A hidden test process can carry SW_HIDE into the first ShowWindow call.
        ShowWindow(nativeWindow, SW_SHOWNORMAL);
        SetForegroundWindow(nativeWindow);
        window.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&window));
        QVERIFY(IsWindowVisible(nativeWindow));
        QCOMPARE(GetForegroundWindow(), nativeWindow);
        auto* firstPopup = first.findChild<QWidget*>(QStringLiteral("settingsChoicePopup"));
        auto* secondPopup = second.findChild<QWidget*>(QStringLiteral("settingsChoicePopup"));
        QSignalSpy clicked(&button, &QPushButton::clicked);
        const auto click = [&window](const QRect& rectangle) {
            const double scale = window.devicePixelRatioF();
            POINT point{qRound(rectangle.center().x() * scale), qRound(rectangle.center().y() * scale)};
            if (!ClientToScreen(reinterpret_cast<HWND>(window.winId()), &point)) return false;
            const int width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
            const int height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
            if (width <= 1 || height <= 1) return false;
            INPUT input[3]{};
            for (auto& event : input) event.type = INPUT_MOUSE;
            input[0].mi.dx = MulDiv(point.x - GetSystemMetrics(SM_XVIRTUALSCREEN), 65535, width - 1);
            input[0].mi.dy = MulDiv(point.y - GetSystemMetrics(SM_YVIRTUALSCREEN), 65535, height - 1);
            input[0].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
            input[1].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
            input[2].mi.dwFlags = MOUSEEVENTF_LEFTUP;
            if (SendInput(2, input, sizeof(INPUT)) != 2) return false;
            // Qt posts the outside-click replay before the physical button is released.
            QTest::qWait(30);
            return SendInput(1, &input[2], sizeof(INPUT)) == 1;
        };
        QVERIFY(click(first.geometry()));
        QTRY_VERIFY(firstPopup->isVisible());
        QTest::qWait(45);
        QVERIFY(click(button.geometry()));
        QTRY_COMPARE(clicked.size(), 1);
        QVERIFY(!firstPopup->isVisible());
        QVERIFY(click(first.geometry()));
        QTRY_VERIFY(firstPopup->isVisible());
        QVERIFY(click(first.geometry()));
        QTRY_VERIFY(!firstPopup->isVisible());
        QVERIFY(click(first.geometry()));
        QTRY_VERIFY(firstPopup->isVisible());
        QVERIFY(click(second.geometry()));
        QTRY_VERIFY(secondPopup->isVisible());
        QVERIFY(!firstPopup->isVisible());
        INPUT escape[2]{};
        for (auto& event : escape) { event.type = INPUT_KEYBOARD; event.ki.wVk = VK_ESCAPE; }
        escape[1].ki.dwFlags = KEYEVENTF_KEYUP;
        QCOMPARE(SendInput(2, escape, sizeof(INPUT)), UINT(2));
        QTRY_VERIFY(!secondPopup->isVisible());
        QVERIFY(click(second.geometry()));
        QTRY_VERIFY(secondPopup->isVisible());
        second.hidePopup();
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

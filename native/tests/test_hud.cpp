#include "ConfigStore.h"
#include "HudWindow.h"
#include "SettingsWindow.h"
#include <QJsonArray>
#include <QCursor>
#include <QEnterEvent>
#include <QListWidget>
#include <QScreen>
#include <QTemporaryDir>
#include <QtTest>

class HudTest : public QObject {
    Q_OBJECT
private:
    static QJsonObject config() {
        auto result = ConfigStore::defaults();
        auto animations = result["animations"].toObject();
        for (auto it = animations.begin(); it != animations.end(); ++it) it.value() = false;
        result["animations"] = animations; result["blur"] = false;
        return result;
    }
    static QPoint point(HudWindow& hud, const QString& name) {
        return hud.elementRects()[name].center().toPoint() + QPoint(14, 14);
    }
private slots:
    void playbackAndSeekAreDispatched() {
        HudWindow hud(config()); MediaSnapshot media; media.active = true; media.duration = 200; media.canSeek = true;
        hud.setSnapshot(media); hud.reveal();
        QSignalSpy commands(&hud, &HudWindow::command);
        QTest::mouseClick(&hud, Qt::LeftButton, Qt::NoModifier, point(hud, "play"));
        QCOMPARE(commands.size(), 1); QCOMPARE(commands.at(0).at(0).toString(), QString("play_pause"));
        QTest::mouseClick(&hud, Qt::LeftButton, Qt::NoModifier, point(hud, "progress"));
        QCOMPARE(commands.size(), 2); QVERIFY(qAbs(commands.at(1).at(1).toDouble() - 100) < 2);
    }
    void disabledControlDoesNothing() {
        HudWindow hud(config()); MediaSnapshot media; media.active = true; media.canPlayPause = false;
        hud.setSnapshot(media); hud.reveal(); QSignalSpy commands(&hud, &HudWindow::command);
        QTest::mouseClick(&hud, Qt::LeftButton, Qt::NoModifier, point(hud, "play"));
        QCOMPARE(commands.size(), 0);
    }
    void manualHideSurvivesTrackChange() {
        HudWindow hud(config()); hud.reveal(); hud.conceal(true); QVERIFY(!hud.isVisible());
        MediaSnapshot media; media.active = true; media.title = "New track"; hud.setSnapshot(media);
        QVERIFY(!hud.isVisible()); hud.toggle(); QVERIFY(hud.isVisible());
    }
    void dragElementIsSaved() {
        HudWindow hud(config()); hud.setEditing(true);
        QSignalSpy changed(&hud, &HudWindow::configChanged);
        const auto start = point(hud, "title");
        QTest::mousePress(&hud, Qt::LeftButton, Qt::NoModifier, start);
        QTest::mouseMove(&hud, start + QPoint(15, 12));
        QTest::mouseRelease(&hud, Qt::LeftButton, Qt::NoModifier, start + QPoint(15, 12));
        QCOMPARE(changed.size(), 1); QCOMPARE(hud.config()["layout"].toString(), QString("custom"));
        QVERIFY(hud.config()["element_positions"].toObject().contains("title"));
    }
    void settingsCloseKeepsApplicationAlive() {
        QTemporaryDir temp; ConfigStore store(temp.filePath("config.json")); SettingsWindow settings(&store);
        settings.show(); settings.close(); QVERIFY(!settings.isVisible());
        settings.show(); QVERIFY(settings.isVisible());
    }
    void settingsFitAvailableScreenOnFirstShow() {
        QTemporaryDir temp; ConfigStore store(temp.filePath("config.json")); SettingsWindow settings(&store);
        settings.show();
        const auto available = settings.screen()->availableGeometry();
        QVERIFY(settings.width() <= available.width());
        QVERIFY(settings.height() <= available.height());
        settings.resize(820, 520);
        QTest::qWait(50);
        const auto* navigation = settings.findChild<QListWidget*>("navigation");
        QVERIFY(navigation);
        QVERIFY(navigation->viewport()->rect().contains(navigation->visualItemRect(navigation->item(6))));
    }
    void autoHideCanBeDisabled() {
        auto c = config(); c["auto_hide_seconds"] = 1; c["idle_collapse"] = false;
        HudWindow hud(c); hud.reveal();
        c["auto_hide_seconds"] = 0; hud.applyConfig(c);
        QTest::qWait(1100); QVERIFY(hud.isVisible());
    }
    void idleIslandDocksAtScreenEdgeAndExpandsOnHover() {
        auto c = config();
        c["idle_collapse"] = true; c["idle_collapse_seconds"] = 1;
        c["compact_width"] = 156; c["compact_height"] = 32; c["compact_visible_height"] = 8;
        auto effects = c["animations"].toObject(); effects["dock"] = false; c["animations"] = effects;
        QCursor::setPos(QGuiApplication::primaryScreen()->geometry().bottomRight());
        HudWindow hud(c); hud.reveal();
        const auto expanded = hud.geometry();
        QTRY_VERIFY_WITH_TIMEOUT(hud.width() < expanded.width(), 1800);
        QVERIFY(hud.isVisible());
        QCOMPARE(hud.size(), QSize(184, 60));
        QCOMPARE(hud.y() + hud.height() - 14, hud.screen()->geometry().top() + 8);
        MediaSnapshot media; media.active = true; media.title = "Next track";
        hud.setSnapshot(media);
        QCOMPARE(hud.size(), QSize(184, 60));
        QEnterEvent enter(QPointF(20, 40), QPointF(20, 40), QPointF(hud.pos() + QPoint(20, 40)));
        QApplication::sendEvent(&hud, &enter);
        QCOMPARE(hud.geometry(), expanded);
    }
    void editingBlocksDockingAndDisablingRestoresSize() {
        auto c = config(); c["idle_collapse"] = true; c["idle_collapse_seconds"] = 1;
        auto effects = c["animations"].toObject(); effects["dock"] = false; c["animations"] = effects;
        QCursor::setPos(QGuiApplication::primaryScreen()->geometry().bottomRight());
        HudWindow hud(c); hud.setEditing(true);
        const auto expanded = hud.geometry();
        QTest::qWait(1100); QCOMPARE(hud.geometry(), expanded);
        hud.setEditing(false);
        QTRY_VERIFY_WITH_TIMEOUT(hud.width() < expanded.width(), 1800);
        c["idle_collapse"] = false; hud.applyConfig(c);
        QCOMPARE(hud.geometry(), expanded);
    }
    void manualHideAndEditingInterruptDocking() {
        auto c = config(); c["idle_collapse"] = true; c["idle_collapse_seconds"] = 1;
        auto effects = c["animations"].toObject(); effects["dock"] = false; c["animations"] = effects;
        QCursor::setPos(QGuiApplication::primaryScreen()->geometry().bottomRight());
        HudWindow hud(c); hud.reveal(); const auto expanded = hud.geometry();
        QTRY_VERIFY_WITH_TIMEOUT(hud.width() < expanded.width(), 1800);
        hud.conceal(true);
        MediaSnapshot media; media.active = true; media.title = "Hidden track"; hud.setSnapshot(media);
        QEnterEvent enter({}, {}, {}); QApplication::sendEvent(&hud, &enter);
        QVERIFY(!hud.isVisible());
        hud.setEditing(true);
        QVERIFY(hud.isVisible()); QCOMPARE(hud.geometry(), expanded);
    }
    void pointerAtDockHandleDoesNotCauseRepeatedCollapse() {
        auto c = config(); c["idle_collapse"] = true; c["idle_collapse_seconds"] = 1;
        c["auto_hide_seconds"] = 1;
        auto effects = c["animations"].toObject(); effects["dock"] = false; c["animations"] = effects;
        const auto screen = QGuiApplication::primaryScreen()->geometry();
        QCursor::setPos(screen.bottomRight());
        HudWindow hud(c); hud.reveal(); const auto expanded = hud.geometry();
        QTRY_VERIFY_WITH_TIMEOUT(hud.width() < expanded.width(), 1800);
        QCursor::setPos(QPoint(hud.geometry().center().x(), screen.top() + 2));
        QEnterEvent enter({}, {}, QPointF(QCursor::pos())); QApplication::sendEvent(&hud, &enter);
        QEvent leave(QEvent::Leave); QApplication::sendEvent(&hud, &leave);
        QTest::qWait(1300);
        QVERIFY(hud.isVisible()); QCOMPARE(hud.geometry(), expanded);
        QCursor::setPos(screen.bottomRight());
        QTRY_VERIFY_WITH_TIMEOUT(hud.width() < expanded.width(), 1800);
        QVERIFY(hud.isVisible());
    }
    void configChangeDuringDockAnimationSettlesToExpandedGeometry() {
        auto c = config(); c["idle_collapse"] = true; c["idle_collapse_seconds"] = 1;
        auto effects = c["animations"].toObject(); effects["dock"] = true; c["animations"] = effects;
        c["animation_duration"] = 500;
        QCursor::setPos(QGuiApplication::primaryScreen()->geometry().bottomRight());
        HudWindow hud(c); hud.reveal(); const int expandedWidth = hud.width();
        QTRY_VERIFY_WITH_TIMEOUT(hud.width() < expandedWidth, 1800);
        c["idle_collapse"] = false; c["width"] = 700; hud.applyConfig(c);
        QTest::qWait(550);
        QCOMPARE(hud.width(), 728); QVERIFY(hud.isVisible());
    }
    void hidingDuringDragDoesNotLeaveIdleBlocked() {
        auto c = config(); c["idle_collapse"] = true; c["idle_collapse_seconds"] = 1;
        auto effects = c["animations"].toObject(); effects["dock"] = false; c["animations"] = effects;
        HudWindow hud(c); hud.reveal(); const int expandedWidth = hud.width();
        QTest::mousePress(&hud, Qt::LeftButton, Qt::NoModifier, QPoint(hud.width() / 2, 15));
        hud.conceal(true); hud.reveal(true);
        QCursor::setPos(QGuiApplication::primaryScreen()->geometry().bottomRight());
        QEvent leave(QEvent::Leave); QApplication::sendEvent(&hud, &leave);
        QTRY_VERIFY_WITH_TIMEOUT(hud.width() < expandedWidth, 1800);
    }
    void slowExpansionFinishesBeforeIdleTimerStarts() {
        auto c = config(); c["idle_collapse"] = true; c["idle_collapse_seconds"] = 1;
        auto effects = c["animations"].toObject(); effects["dock"] = false; c["animations"] = effects;
        QCursor::setPos(QGuiApplication::primaryScreen()->geometry().bottomRight());
        HudWindow hud(c); hud.reveal(); const auto expanded = hud.geometry();
        QTRY_VERIFY_WITH_TIMEOUT(hud.width() < expanded.width(), 1800);
        effects["dock"] = true; c["animations"] = effects; c["animation_duration"] = 1600;
        hud.applyConfig(c); hud.reveal(true);
        QTRY_COMPARE_WITH_TIMEOUT(hud.geometry(), expanded, 2000);
        QVERIFY(hud.isVisible());
    }
};
QTEST_MAIN(HudTest)
#include "test_hud.moc"

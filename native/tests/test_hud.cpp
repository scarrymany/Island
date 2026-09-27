#include "ConfigStore.h"
#include "HudWindow.h"
#include "SettingsWindow.h"
#include <QJsonArray>
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
    void autoHideCanBeDisabled() {
        auto c = config(); c["auto_hide_seconds"] = 1;
        HudWindow hud(c); hud.reveal();
        c["auto_hide_seconds"] = 0; hud.applyConfig(c);
        QTest::qWait(1100); QVERIFY(hud.isVisible());
    }
};
QTEST_MAIN(HudTest)
#include "test_hud.moc"

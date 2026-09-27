#include "ConfigStore.h"
#include "HudWindow.h"
#include "SettingsWindow.h"
#include <QJsonArray>
#include <QCursor>
#include <QEnterEvent>
#include <QBuffer>
#include <QPainter>
#include <QFontDatabase>
#include <QFontInfo>
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
        const double scale = hud.config()["scale"].toDouble(1);
        const int inset = qCeil(14 * scale);
        return (hud.elementRects()[name].center() * scale + QPointF(inset, inset)).toPoint();
    }
    static QRect pixelRect(HudWindow& hud, const QString& name) {
        const double scale = hud.config()["scale"].toDouble(1);
        const int inset = qCeil(14 * scale);
        const auto rect = hud.elementRects().value(name);
        return QRectF(rect.topLeft() * scale + QPointF(inset, inset), rect.size() * scale).toAlignedRect();
    }
    static QByteArray artwork(const QColor& left, const QColor& right = {}) {
        QImage image(100, 100, QImage::Format_RGB32);
        image.fill(left);
        if (right.isValid()) {
            QPainter painter(&image);
            painter.fillRect(50, 0, 50, 100, right);
        }
        QByteArray bytes;
        QBuffer buffer(&bytes); buffer.open(QIODevice::WriteOnly);
        image.save(&buffer, "PNG");
        return bytes;
    }
    static QJsonObject backgroundConfig() {
        auto result = config();
        auto visible = result["visible"].toObject();
        for (auto it = visible.begin(); it != visible.end(); ++it) it.value() = false;
        result["visible"] = visible;
        result["gradient_enabled"] = false;
        result["idle_collapse"] = false;
        result["background"] = "#14141C";
        result["opacity"] = 0.6;
        result["artwork_background"] = true;
        result["artwork_background_strength"] = 1.0;
        return result;
    }
    static QColor backgroundPixel(HudWindow& hud) {
        return hud.grab().toImage().pixelColor(hud.width() / 2, hud.height() / 2);
    }
private slots:
    void hudUsesBundledFontAndConfiguredWeight() {
        auto c = config();
        c["font_family"] = "Inter"; c["font_size"] = 14; c["font_weight"] = 600;
        HudWindow hud(c);
        QVERIFY(QFontDatabase::families().contains("Inter"));
        QCOMPARE(QFontInfo(hud.font()).family(), QString("Inter"));
        QCOMPARE(hud.font().pixelSize(), 14);
        QCOMPARE(hud.font().weight(), QFont::DemiBold);
        c["font_weight"] = 800; hud.applyConfig(c);
        QCOMPARE(hud.font().weight(), QFont::ExtraBold);
    }
    void scrubbingPreviewsProgressAndTimeWithoutSendingUntilRelease() {
        auto c = config();
        c["idle_collapse"] = false; c["artwork_background"] = false;
        c["gradient_enabled"] = false; c["background"] = "#000000"; c["opacity"] = 1;
        c["progress_color"] = "#FFFFFF"; c["secondary_color"] = "#FFFFFF";
        HudWindow hud(c);
        MediaSnapshot media;
        media.active = true; media.canSeek = true; media.duration = 200; media.position = 20;
        media.sourceId = "Player"; media.title = "Track";
        hud.setSnapshot(media); hud.reveal();
        const QRect progress = pixelRect(hud, "progress");
        const QPoint start(progress.left() + progress.width() / 4, progress.center().y());
        const QPoint finish(progress.left() + progress.width() * 3 / 4, progress.center().y());
        const double target = (finish.x() - 14 - hud.elementRects()["progress"].left())
            / hud.elementRects()["progress"].width() * media.duration;
        QSignalSpy commands(&hud, &HudWindow::command);
        QTest::mousePress(&hud, Qt::LeftButton, Qt::NoModifier, start);
        QTest::mouseMove(&hud, finish);
        QCOMPARE(commands.size(), 0);
        const auto dragging = hud.grab().toImage();
        QVERIFY(dragging.pixelColor((QPointF(progress.center()) * dragging.devicePixelRatio()).toPoint()).red() > 220);
        const auto draggingTime = hud.grab(pixelRect(hud, "time")).toImage();
        HudWindow reference(c);
        auto expected = media; expected.position = target;
        reference.setSnapshot(expected); reference.reveal();
        QCOMPARE(draggingTime, reference.grab(pixelRect(reference, "time")).toImage());
        media.position = 35; hud.setSnapshot(media);
        QCOMPARE(hud.grab(pixelRect(hud, "time")).toImage(), draggingTime);
        QCOMPARE(commands.size(), 0);
        QTest::mouseRelease(&hud, Qt::LeftButton, Qt::NoModifier, finish);
        QCOMPARE(commands.size(), 1);
        QCOMPARE(commands.first().first().toString(), QString("seek"));
        QVERIFY(qAbs(commands.first().at(1).toDouble() - target) < 0.001);
    }
    void scrubReleaseOutsideClampsToTrackBounds_data() {
        QTest::addColumn<bool>("pastEnd");
        QTest::addColumn<double>("scale");
        QTest::newRow("before-start") << false << 1.0;
        QTest::newRow("past-end") << true << 1.0;
        QTest::newRow("scaled-before-start") << false << 1.75;
        QTest::newRow("scaled-past-end") << true << 1.75;
    }
    void scrubReleaseOutsideClampsToTrackBounds() {
        QFETCH(bool, pastEnd);
        QFETCH(double, scale);
        auto c = config(); c["scale"] = scale;
        HudWindow hud(c);
        MediaSnapshot media; media.active = true; media.canSeek = true; media.duration = 200;
        hud.setSnapshot(media); hud.reveal();
        QSignalSpy commands(&hud, &HudWindow::command);
        QTest::mousePress(&hud, Qt::LeftButton, Qt::NoModifier, point(hud, "progress"));
        const QPoint outside(pastEnd ? hud.width() + 50 : -50, -20);
        QTest::mouseMove(&hud, outside);
        QCOMPARE(commands.size(), 0);
        QTest::mouseRelease(&hud, Qt::LeftButton, Qt::NoModifier, outside);
        QCOMPARE(commands.size(), 1);
        QCOMPARE(commands.first().at(1).toDouble(), pastEnd ? media.duration : 0.0);
    }
    void interruptedScrubCannotResumeOnAnotherTrack_data() {
        QTest::addColumn<QString>("interruption");
        for (const auto* reason : {"track", "source", "inactive", "disabled", "duration", "hide", "fade", "editing", "layout", "capture", "blocked"})
            QTest::newRow(reason) << QString::fromLatin1(reason);
    }
    void interruptedScrubCannotResumeOnAnotherTrack() {
        QFETCH(QString, interruption);
        auto c = config();
        if (interruption == "fade") {
            auto animations = c["animations"].toObject(); animations["disappear"] = true;
            c["animations"] = animations; c["animation_duration"] = 600;
        }
        HudWindow hud(c);
        MediaSnapshot media; media.active = true; media.canSeek = true; media.duration = 200;
        media.sourceId = "Player"; media.title = "First";
        hud.setSnapshot(media); hud.reveal();
        const QPoint position = point(hud, "progress");
        QSignalSpy commands(&hud, &HudWindow::command);
        QTest::mousePress(&hud, Qt::LeftButton, Qt::NoModifier, position);
        if (interruption == "hide") { hud.hide(); hud.reveal(true); }
        else if (interruption == "fade") { hud.conceal(true); hud.reveal(true); }
        else if (interruption == "editing") { hud.setEditing(true); hud.setEditing(false); }
        else if (interruption == "layout") {
            auto visible = c["visible"].toObject(); visible["progress"] = false; c["visible"] = visible; hud.applyConfig(c);
            visible["progress"] = true; c["visible"] = visible; hud.applyConfig(c);
        } else if (interruption == "capture") {
            QMouseEvent lost(QEvent::MouseMove, position, hud.mapToGlobal(position), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(&hud, &lost);
        } else if (interruption == "blocked") {
            QEvent blocked(QEvent::WindowBlocked); QApplication::sendEvent(&hud, &blocked);
            QEvent unblocked(QEvent::WindowUnblocked); QApplication::sendEvent(&hud, &unblocked);
        } else {
            auto changed = media;
            if (interruption == "track") changed.title = "Second";
            if (interruption == "source") changed.sourceId = "Other Player";
            if (interruption == "inactive") changed.active = false;
            if (interruption == "disabled") changed.canSeek = false;
            if (interruption == "duration") changed.duration = 0;
            hud.setSnapshot(changed);
            hud.setSnapshot(media);
        }
        QTest::mouseMove(&hud, position + QPoint(20, 0));
        QTest::mouseRelease(&hud, Qt::LeftButton, Qt::NoModifier, position + QPoint(20, 0));
        QCOMPARE(commands.size(), 0);
    }
    void initiallyDisabledSeekCannotStartMidGesture() {
        HudWindow hud(config());
        MediaSnapshot media; media.active = true; media.duration = 200;
        hud.setSnapshot(media); hud.reveal();
        QSignalSpy commands(&hud, &HudWindow::command);
        const QPoint position = point(hud, "progress");
        QTest::mousePress(&hud, Qt::LeftButton, Qt::NoModifier, position);
        media.canSeek = true; hud.setSnapshot(media);
        QTest::mouseRelease(&hud, Qt::LeftButton, Qt::NoModifier, position);
        QCOMPARE(commands.size(), 0);
    }
    void changingSourceCancelsThePreviousVolumeGesture() {
        HudWindow hud(config());
        hud.reveal();
        hud.setVolumeAvailable(true);
        const QPoint position = point(hud, "volume");
        QSignalSpy changes(&hud, &HudWindow::volumeChanged);
        QTest::mousePress(&hud, Qt::LeftButton, Qt::NoModifier, position);
        hud.setVolumeAvailable(false);
        hud.setVolumeAvailable(true);
        QTest::mouseRelease(&hud, Qt::LeftButton, Qt::NoModifier, position);
        QCOMPARE(changes.size(), 0);
        hud.setVolumeAvailable(false);
        QTest::mousePress(&hud, Qt::LeftButton, Qt::NoModifier, position);
        hud.setVolumeAvailable(true);
        QTest::mouseRelease(&hud, Qt::LeftButton, Qt::NoModifier, position);
        QCOMPARE(changes.size(), 0);
    }
    void unavailableSourceVolumeDoesNotDispatchInput() {
        HudWindow hud(config());
        hud.reveal();
        hud.setVolumeAvailable(false);
        const QPoint position = point(hud, "volume");
        QSignalSpy changes(&hud, &HudWindow::volumeChanged);
        QTest::mouseClick(&hud, Qt::LeftButton, Qt::NoModifier, position);
        QWheelEvent unavailable(position, hud.mapToGlobal(position), {}, QPoint(0, 120),
                                Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(&hud, &unavailable);
        QCOMPARE(changes.size(), 0);
        hud.setVolumeAvailable(true);
        hud.setVolume(0.4);
        QWheelEvent available(position, hud.mapToGlobal(position), {}, QPoint(0, 120),
                              Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(&hud, &available);
        QCOMPARE(changes.size(), 1);
        QVERIFY(qAbs(changes.first().first().toDouble() - 0.42) < 0.001);
    }
    void artworkBackgroundFollowsTrackAndPreservesOpacity() {
        auto c = backgroundConfig();
        HudWindow hud(c); hud.reveal();
        const QColor fallback = backgroundPixel(hud);
        MediaSnapshot track; track.active = true; track.cover = artwork(Qt::red);
        hud.setSnapshot(track);
        const QColor red = backgroundPixel(hud);
        QVERIFY(red.red() > red.blue() + 40);
        QCOMPARE(red.alpha(), fallback.alpha());
        track.cover = artwork(Qt::blue); hud.setSnapshot(track);
        const QColor blue = backgroundPixel(hud);
        QVERIFY(blue.blue() > blue.red() + 40);
        QCOMPARE(blue.alpha(), fallback.alpha());
        c["artwork_background"] = false; hud.applyConfig(c);
        QCOMPARE(backgroundPixel(hud), fallback);
        c["artwork_background"] = true; c["artwork_background_strength"] = 0.0; hud.applyConfig(c);
        QCOMPARE(backgroundPixel(hud), fallback);
        c["artwork_background_strength"] = 1.0; hud.applyConfig(c);
        track.cover.clear(); hud.setSnapshot(track);
        QCOMPARE(backgroundPixel(hud), fallback);
        track.cover = "invalid image"; hud.setSnapshot(track);
        QCOMPARE(backgroundPixel(hud), fallback);
    }
    void artworkBackgroundBlursEdgesAndKeepsBrightCoversReadable() {
        HudWindow hud(backgroundConfig()); hud.reveal();
        MediaSnapshot track; track.active = true; track.cover = artwork(Qt::red, Qt::blue);
        hud.setSnapshot(track);
        const QImage pixels = hud.grab().toImage();
        const int middle = hud.width() / 2;
        const int y = hud.height() / 2;
        const QColor left = pixels.pixelColor(middle - 70, y);
        const QColor right = pixels.pixelColor(middle + 70, y);
        QVERIFY(left.red() > right.red() + 10);
        QVERIFY(right.blue() > left.blue() + 10);
        for (int x = middle - 60; x < middle + 60; ++x) {
            const QColor a = pixels.pixelColor(x, y), b = pixels.pixelColor(x + 1, y);
            QVERIFY(qAbs(a.red() - b.red()) < 10);
            QVERIFY(qAbs(a.blue() - b.blue()) < 10);
        }
        track.cover = artwork(Qt::white); hud.setSnapshot(track);
        const QColor white = backgroundPixel(hud);
        QVERIFY(white.red() <= 60 && white.green() <= 60 && white.blue() <= 60);
    }
    void artworkTransitionCanBeInterruptedWithoutColorJump() {
        auto c = backgroundConfig();
        HudWindow hud(c); hud.reveal();
        MediaSnapshot track; track.active = true; track.cover = artwork(Qt::red);
        hud.setSnapshot(track);
        auto effects = c["animations"].toObject(); effects["cover"] = true;
        c["animations"] = effects; c["animation_duration"] = 600; hud.applyConfig(c);
        track.cover = artwork(Qt::blue); hud.setSnapshot(track);
        QTest::qWait(150);
        const QColor before = backgroundPixel(hud);
        QVERIFY(before.red() > 10 && before.blue() > 10);
        track.cover = artwork(Qt::green); hud.setSnapshot(track);
        const QColor after = backgroundPixel(hud);
        QVERIFY(qAbs(before.red() - after.red()) <= 2);
        QVERIFY(qAbs(before.blue() - after.blue()) <= 2);
        QTest::qWait(650);
        const QColor settled = backgroundPixel(hud);
        QVERIFY(settled.green() > settled.red() + 20);
        QVERIFY(settled.green() > settled.blue() + 20);
    }
    void hidingDuringArtworkTransitionSettlesToCurrentTrack() {
        auto c = backgroundConfig();
        HudWindow hud(c); hud.reveal();
        MediaSnapshot track; track.active = true; track.cover = artwork(Qt::red);
        hud.setSnapshot(track);
        auto effects = c["animations"].toObject(); effects["cover"] = true;
        c["animations"] = effects; c["animation_duration"] = 600; hud.applyConfig(c);
        track.cover = artwork(Qt::blue); hud.setSnapshot(track);
        QTest::qWait(100);
        hud.conceal(true); hud.reveal(true);
        const QColor current = backgroundPixel(hud);
        QVERIFY(current.blue() > current.red() + 100);
        QVERIFY(current.red() < 5);
    }
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

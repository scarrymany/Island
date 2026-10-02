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
#include <QDateTime>
#include <QElapsedTimer>
#include <QListWidget>
#include <QScreen>
#include <QTemporaryDir>
#include <QtTest>
#include <algorithm>

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
        return (hud.elementRects()[name].center() * scale + hud.cardOrigin()).toPoint();
    }
    static QRect pixelRect(HudWindow& hud, const QString& name) {
        const double scale = hud.config()["scale"].toDouble(1);
        const auto rect = hud.elementRects().value(name);
        return QRectF(rect.topLeft() * scale + hud.cardOrigin(), rect.size() * scale).toAlignedRect();
    }
    // The window is a stable transparent frame; the card is what users see and touch.
    static QPoint cardCenter(HudWindow& hud) {
        return (hud.cardOrigin() + QPointF(hud.cardGeometry().width() / 2, hud.cardGeometry().height() / 2)).toPoint();
    }
    static QPoint header(HudWindow& hud) {
        return (hud.cardOrigin() + QPointF(hud.cardGeometry().width() / 2, 1)).toPoint();
    }
    static bool docked(HudWindow& hud) { return hud.isCollapsed() && hud.dockProgress() >= 1; }
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
        result["surface_highlight"] = false;
        return result;
    }
    static QColor backgroundPixel(HudWindow& hud) {
        const QImage image = hud.grab().toImage();
        return image.pixelColor((QPointF(cardCenter(hud)) * image.devicePixelRatio()).toPoint());
    }
    static QImage timeAt(HudWindow& reference, MediaSnapshot snapshot, double position) {
        snapshot.position = position;
        snapshot.playing = false;
        reference.setSnapshot(snapshot);
        return reference.grab(pixelRect(reference, "time")).toImage();
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
        const double target = (finish.x() - hud.cardOrigin().x() - hud.elementRects()["progress"].left())
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
        QCOMPARE(hud.grab(pixelRect(hud, "time")).toImage(), draggingTime);
        media.position = 36; media.updatedAt = QDateTime::currentMSecsSinceEpoch();
        hud.setSnapshot(media);
        QCOMPARE(hud.grab(pixelRect(hud, "time")).toImage(), draggingTime);
        media.position = target; hud.setSnapshot(media);
        QCOMPARE(hud.grab(pixelRect(hud, "time")).toImage(), draggingTime);
        media.position = target + 10; hud.setSnapshot(media);
        QCOMPARE(hud.grab(pixelRect(hud, "time")).toImage(), timeAt(reference, media, media.position));
    }
    void repeatedSeeksIgnoreAnEarlierTargetAcknowledgement() {
        auto c = config(); c["idle_collapse"] = false;
        HudWindow hud(c), reference(c);
        MediaSnapshot media; media.active = true; media.canSeek = true; media.duration = 200;
        media.position = 10; media.title = "Track"; media.sourceId = "Player";
        hud.setSnapshot(media); hud.reveal();
        const QRect bar = pixelRect(hud, "progress");
        QSignalSpy commands(&hud, &HudWindow::command);
        QTest::mouseClick(&hud, Qt::LeftButton, Qt::NoModifier, QPoint(bar.left() + bar.width() / 4, bar.center().y()));
        QTest::mouseClick(&hud, Qt::LeftButton, Qt::NoModifier, QPoint(bar.left() + bar.width() * 3 / 4, bar.center().y()));
        QCOMPARE(commands.size(), 2);
        const double first = commands.first().at(1).toDouble(), last = commands.last().at(1).toDouble();
        const auto expected = timeAt(reference, media, last);
        QCOMPARE(hud.grab(pixelRect(hud, "time")).toImage(), expected);
        media.position = first; media.updatedAt = QDateTime::currentMSecsSinceEpoch(); hud.setSnapshot(media);
        QCOMPARE(hud.grab(pixelRect(hud, "time")).toImage(), expected);
        media.position = last; hud.setSnapshot(media);
        QCOMPARE(hud.grab(pixelRect(hud, "time")).toImage(), expected);
        media.position = last + 8; hud.setSnapshot(media);
        QCOMPARE(hud.grab(pixelRect(hud, "time")).toImage(), timeAt(reference, media, media.position));
    }
    void unacknowledgedPausedSeekExpiresAndShowsLatestPlayerPosition() {
        auto c = config(); c["idle_collapse"] = false;
        HudWindow hud(c), reference(c);
        MediaSnapshot media; media.active = true; media.canSeek = true; media.duration = 200; media.position = 20;
        hud.setSnapshot(media); hud.reveal();
        QSignalSpy commands(&hud, &HudWindow::command);
        QTest::mouseClick(&hud, Qt::LeftButton, Qt::NoModifier, point(hud, "progress"));
        QCOMPARE(commands.size(), 1);
        const auto target = timeAt(reference, media, commands.first().at(1).toDouble());
        media.position = 35; hud.setSnapshot(media);
        QCOMPARE(hud.grab(pixelRect(hud, "time")).toImage(), target);
        QTest::qWait(150);
        QCOMPARE(hud.grab(pixelRect(hud, "time")).toImage(), target);
        const auto actual = timeAt(reference, media, media.position);
        QTRY_COMPARE_WITH_TIMEOUT(hud.grab(pixelRect(hud, "time")).toImage(), actual, 3500);
        QCOMPARE(commands.size(), 1);
    }
    void pendingSeekAdvancesAtPlaybackRateAndFreezesOnPause() {
        auto c = config(); c["idle_collapse"] = false;
        HudWindow hud(c), reference(c);
        MediaSnapshot media; media.active = true; media.canSeek = true; media.duration = 200;
        media.position = 10; media.playing = true; media.playbackRate = 2;
        media.updatedAt = QDateTime::currentMSecsSinceEpoch();
        hud.setSnapshot(media); hud.reveal();
        QSignalSpy commands(&hud, &HudWindow::command);
        QTest::mouseClick(&hud, Qt::LeftButton, Qt::NoModifier, point(hud, "progress"));
        QCOMPARE(commands.size(), 1);
        const double target = commands.first().at(1).toDouble();
        QElapsedTimer elapsed; elapsed.start();
        QTest::qWait(600);
        const double predicted = target + elapsed.elapsed() / 1000.0 * media.playbackRate;
        media.playing = false; media.position = 12; media.updatedAt = QDateTime::currentMSecsSinceEpoch();
        hud.setSnapshot(media);
        const auto paused = hud.grab(pixelRect(hud, "time")).toImage();
        QVERIFY(paused == timeAt(reference, media, predicted - .15) || paused == timeAt(reference, media, predicted + .15));
        QTest::qWait(200);
        QCOMPARE(hud.grab(pixelRect(hud, "time")).toImage(), paused);
        media.position = predicted; hud.setSnapshot(media);
        QCOMPARE(hud.grab(pixelRect(hud, "time")).toImage(), timeAt(reference, media, predicted));
    }
    void submittedSeekKeepsItsConfirmationPreviewAfterFocusLoss_data() {
        QTest::addColumn<bool>("blocked");
        QTest::newRow("deactivated") << false;
        QTest::newRow("blocked") << true;
    }
    void submittedSeekKeepsItsConfirmationPreviewAfterFocusLoss() {
        QFETCH(bool, blocked);
        auto c = config(); c["idle_collapse"] = false;
        HudWindow hud(c), reference(c);
        MediaSnapshot media; media.active = true; media.canSeek = true; media.duration = 200;
        media.title = "Track"; media.sourceId = "Player"; media.position = 10;
        hud.setSnapshot(media); hud.reveal();
        QSignalSpy commands(&hud, &HudWindow::command);
        QTest::mouseClick(&hud, Qt::LeftButton, Qt::NoModifier, point(hud, "progress"));
        QCOMPARE(commands.size(), 1);
        const double target = commands.first().at(1).toDouble();
        const auto expected = timeAt(reference, media, target);
        QEvent focusChanged(blocked ? QEvent::WindowBlocked : QEvent::WindowDeactivate);
        QApplication::sendEvent(&hud, &focusChanged);
        media.position = 15; hud.setSnapshot(media);
        QTest::qWait(150);
        QCOMPARE(hud.grab(pixelRect(hud, "time")).toImage(), expected);
        media.position = target; hud.setSnapshot(media);
        media.position = target + 10; hud.setSnapshot(media);
        QCOMPARE(hud.grab(pixelRect(hud, "time")).toImage(), timeAt(reference, media, media.position));
        QCOMPARE(commands.size(), 1);
    }
    void pendingSeekIsCancelledWhenTrackOrSourceChanges_data() {
        QTest::addColumn<bool>("sourceChange");
        QTest::newRow("track") << false;
        QTest::newRow("source") << true;
    }
    void pendingSeekIsCancelledWhenTrackOrSourceChanges() {
        QFETCH(bool, sourceChange);
        auto c = config(); c["idle_collapse"] = false;
        HudWindow hud(c), reference(c);
        MediaSnapshot media; media.active = true; media.canSeek = true; media.duration = 200;
        media.title = "Track"; media.sourceId = "Player"; media.position = 10;
        hud.setSnapshot(media); hud.reveal();
        QTest::mouseClick(&hud, Qt::LeftButton, Qt::NoModifier, point(hud, "progress"));
        if (sourceChange) media.sourceId = "Other Player"; else media.title = "Other Track";
        media.position = 30; hud.setSnapshot(media);
        QCOMPARE(hud.grab(pixelRect(hud, "time")).toImage(), timeAt(reference, media, media.position));
    }
    void aNewScrubCannotContinueAnInterruptedWindowDrag() {
        auto c = config(); c["idle_collapse"] = false;
        HudWindow hud(c);
        MediaSnapshot media; media.active = true; media.canSeek = true; media.duration = 200;
        hud.setSnapshot(media); hud.reveal();
        const QPoint top = header(hud);
        QTest::mousePress(&hud, Qt::LeftButton, Qt::NoModifier, top);
        QTest::mouseMove(&hud, top + QPoint(20, 10));
        const QRect geometry = hud.geometry();
        const QPoint progress = point(hud, "progress");
        QSignalSpy commands(&hud, &HudWindow::command);
        QTest::mousePress(&hud, Qt::LeftButton, Qt::NoModifier, progress);
        QTest::mouseMove(&hud, progress + QPoint(30, 0));
        QCOMPARE(hud.geometry(), geometry);
        QTest::mouseRelease(&hud, Qt::LeftButton, Qt::NoModifier, progress + QPoint(30, 0));
        QCOMPARE(commands.size(), 1);
        QCOMPARE(commands.first().first().toString(), QString("seek"));
    }
    void continuousScrubbingOwnsEveryFrameAcrossPlayerUpdates() {
        auto c = config(); c["idle_collapse"] = false;
        HudWindow hud(c), reference(c);
        MediaSnapshot media; media.active = true; media.canSeek = true; media.duration = 200;
        media.title = "Track"; media.artist = "Artist"; media.sourceId = "Player";
        media.position = 10; media.playing = true; media.updatedAt = QDateTime::currentMSecsSinceEpoch();
        reference.setSnapshot(media);
        hud.setSnapshot(media); hud.reveal();
        QCoreApplication::processEvents();
        const QRect geometry = hud.geometry();
        const QRect bar = pixelRect(hud, "progress");
        const QRectF logicalBar = hud.elementRects()["progress"];
        QSignalSpy commands(&hud, &HudWindow::command);
        QTest::mousePress(&hud, Qt::LeftButton, Qt::NoModifier, point(hud, "progress"));
        QPoint cursor;
        for (int frame = 0; frame < 80; ++frame) {
            const int step = frame < 40 ? frame : 79 - frame;
            cursor = QPoint(bar.left() + qRound(bar.width() * (0.1 + step * .02)), bar.center().y());
            QTest::mouseMove(&hud, cursor);
            const double target = (cursor.x() - hud.cardOrigin().x() - logicalBar.left()) / logicalBar.width() * media.duration;
            media.position = 10 + frame * .1;
            media.playing = frame < 25 || frame >= 50;
            media.updatedAt = QDateTime::currentMSecsSinceEpoch();
            hud.setSnapshot(media);
            hud.setVolume(frame / 100.0);
            QCOMPARE(hud.geometry(), geometry);
            const auto actual = hud.grab(pixelRect(hud, "time")).toImage();
            const auto expected = timeAt(reference, media, target);
            QVERIFY2(actual == expected, qPrintable(QStringLiteral("Timeline differs from cursor preview at frame %1").arg(frame)));
            QCOMPARE(commands.size(), 0);
            QTest::qWait(1);
        }
        QTest::mouseRelease(&hud, Qt::LeftButton, Qt::NoModifier, cursor);
        QCOMPARE(commands.size(), 1);
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
        for (const auto* reason : {"track", "source", "inactive", "disabled", "duration", "hide", "fade", "editing", "layout", "capture", "blocked", "deactivated"})
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
        } else if (interruption == "deactivated") {
            QEvent deactivated(QEvent::WindowDeactivate); QApplication::sendEvent(&hud, &deactivated);
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
        const int middle = cardCenter(hud).x();
        const int y = cardCenter(hud).y();
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
    void automaticHideRecoversOnMeaningfulActivity_data() {
        QTest::addColumn<QString>("activity");
        QTest::addColumn<bool>("duringFade");
        for (const auto* activity : {"track", "resume", "reactivate"}) {
            QTest::newRow(qPrintable(QString(activity) + "-hidden")) << QString(activity) << false;
            QTest::newRow(qPrintable(QString(activity) + "-fading")) << QString(activity) << true;
        }
    }
    void automaticHideRecoversOnMeaningfulActivity() {
        QFETCH(QString, activity);
        QFETCH(bool, duringFade);
        auto c = config(); c["idle_collapse"] = false;
        auto effects = c["animations"].toObject();
        effects["disappear"] = duringFade; effects["appear"] = true;
        c["animations"] = effects; c["animation_duration"] = 300;
        QCursor::setPos(QGuiApplication::primaryScreen()->geometry().bottomRight());
        HudWindow hud(c);
        MediaSnapshot media; media.active = activity != "reactivate";
        media.title = "Same track"; media.sourceId = "Player";
        hud.setSnapshot(media); hud.reveal();
        QTRY_VERIFY_WITH_TIMEOUT(hud.windowOpacity() > .99, 700);
        hud.conceal();
        if (duringFade) { QTest::qWait(70); QVERIFY(hud.isVisible()); QVERIFY(hud.windowOpacity() < .99); }
        else QVERIFY(!hud.isVisible());
        if (activity == "track") media.title = "Next track";
        else if (activity == "resume") media.playing = true;
        else media.active = true;
        hud.setSnapshot(media);
        QTest::qWait(400);
        QVERIFY(hud.isVisible()); QVERIFY(hud.windowOpacity() > .99);
    }
    void unchangedMediaPollingDoesNotUndoAutomaticHide() {
        auto c = config(); c["idle_collapse"] = false;
        HudWindow hud(c);
        MediaSnapshot media; media.active = true; media.title = "Same track";
        hud.setSnapshot(media); hud.reveal(); hud.conceal();
        QVERIFY(!hud.isVisible());
        for (int poll = 0; poll < 4; ++poll) {
            media.position += 1;
            hud.setSnapshot(media);
            QVERIFY(!hud.isVisible());
        }
    }
    void manualHideSurvivesActivityAndDisplayChangesDuringFade() {
        auto c = config(); c["idle_collapse"] = false;
        auto effects = c["animations"].toObject(); effects["disappear"] = true;
        c["animations"] = effects; c["animation_duration"] = 250;
        HudWindow hud(c);
        MediaSnapshot media; media.active = true; media.title = "Track";
        hud.setSnapshot(media); hud.reveal(); hud.conceal(true);
        QTest::qWait(50);
        media.title = "New track"; media.playing = true; hud.setSnapshot(media);
        c["idle_collapse"] = true; hud.applyConfig(c);
        auto* screen = QGuiApplication::primaryScreen();
        QVERIFY(QMetaObject::invokeMethod(screen, "availableGeometryChanged", Qt::DirectConnection,
            Q_ARG(QRect, screen->availableGeometry())));
        QTest::qWait(350);
        QVERIFY(!hud.isVisible());
        media.active = false; hud.setSnapshot(media);
        media.active = true; hud.setSnapshot(media);
        hud.placeOnScreen();
        QVERIFY(!hud.isVisible());
        hud.toggle(); QVERIFY(hud.isVisible()); QVERIFY(hud.windowOpacity() > .99);
    }
    void disablingAutomaticHideRecoversTheCard_data() {
        QTest::addColumn<bool>("enableDock");
        QTest::newRow("disable-timer") << false;
        QTest::newRow("enable-dock") << true;
    }
    void disablingAutomaticHideRecoversTheCard() {
        QFETCH(bool, enableDock);
        auto c = config(); c["idle_collapse"] = false; c["auto_hide_seconds"] = 1;
        QCursor::setPos(QGuiApplication::primaryScreen()->geometry().bottomRight());
        HudWindow hud(c); hud.reveal();
        QTRY_VERIFY_WITH_TIMEOUT(!hud.isVisible(), 1800);
        if (enableDock) c["idle_collapse"] = true;
        else c["auto_hide_seconds"] = 0;
        hud.applyConfig(c);
        QVERIFY(hud.isVisible()); QVERIFY(hud.windowOpacity() > .99);
        hud.conceal(true); hud.applyConfig(c);
        QVERIFY(!hud.isVisible());
    }
    void rapidVisibilityReversalsCannotLeaveAStaleHide() {
        auto c = config(); c["idle_collapse"] = false;
        auto effects = c["animations"].toObject(); effects["appear"] = effects["disappear"] = true;
        c["animations"] = effects; c["animation_duration"] = 300;
        QCursor::setPos(QGuiApplication::primaryScreen()->geometry().bottomRight());
        HudWindow hud(c);
        for (int i = 0; i < 4; ++i) {
            hud.reveal(true); QTest::qWait(35);
            hud.conceal(true); QTest::qWait(25);
        }
        hud.reveal(true);
        QTest::qWait(400);
        QVERIFY(hud.isVisible()); QVERIFY(hud.windowOpacity() > .99);
    }
    void nativeHideDuringAppearanceCannotLeaveTransparentShow() {
        auto c = config(); c["idle_collapse"] = false;
        auto effects = c["animations"].toObject(); effects["appear"] = true;
        c["animations"] = effects; c["animation_duration"] = 400;
        HudWindow hud(c); hud.reveal();
        QTest::qWait(40); QVERIFY(hud.windowOpacity() < .99);
        hud.hide(); hud.show();
        QVERIFY(hud.isVisible()); QVERIFY(hud.windowOpacity() > .99);
        QTest::qWait(450);
        QVERIFY(hud.isVisible()); QVERIFY(hud.windowOpacity() > .99);
    }
    void positionLockPreservesPlaybackSeekAndVolume() {
        auto c = config(); c["position_locked"] = true; c["idle_collapse"] = false;
        HudWindow hud(c);
        MediaSnapshot media; media.active = true; media.canSeek = true; media.duration = 200;
        hud.setSnapshot(media); hud.reveal();
        const QRect initial = hud.geometry();
        const QPoint top = header(hud);
        QSignalSpy changes(&hud, &HudWindow::configChanged);
        QSignalSpy commands(&hud, &HudWindow::command);
        QSignalSpy volume(&hud, &HudWindow::volumeChanged);
        QTest::mousePress(&hud, Qt::LeftButton, Qt::NoModifier, top);
        QTest::mouseMove(&hud, top + QPoint(25, 20));
        QTest::mouseRelease(&hud, Qt::LeftButton, Qt::NoModifier, top + QPoint(25, 20));
        QCOMPARE(hud.geometry(), initial); QCOMPARE(changes.size(), 0);
        QTest::mouseClick(&hud, Qt::LeftButton, Qt::NoModifier, point(hud, "play"));
        QTest::mouseClick(&hud, Qt::LeftButton, Qt::NoModifier, point(hud, "progress"));
        QTest::mouseClick(&hud, Qt::LeftButton, Qt::NoModifier, point(hud, "volume"));
        QCOMPARE(commands.size(), 2);
        QCOMPARE(commands.at(0).first().toString(), QString("play_pause"));
        QCOMPARE(commands.at(1).first().toString(), QString("seek"));
        QCOMPARE(volume.size(), 1); QCOMPARE(hud.geometry(), initial);
    }
    void positionLockStillAllowsDeliberateLayoutEditing() {
        auto c = config(); c["position_locked"] = true;
        HudWindow hud(c); hud.setEditing(true);
        const QRect initial = hud.geometry();
        const QRectF title = hud.elementRects()["title"];
        const QPoint start = point(hud, "title");
        QSignalSpy changes(&hud, &HudWindow::configChanged);
        QTest::mousePress(&hud, Qt::LeftButton, Qt::NoModifier, start);
        QTest::mouseMove(&hud, start + QPoint(12, 10));
        QTest::mouseRelease(&hud, Qt::LeftButton, Qt::NoModifier, start + QPoint(12, 10));
        QCOMPARE(changes.size(), 1); QCOMPARE(hud.geometry(), initial);
        QVERIFY(hud.elementRects()["title"].topLeft() != title.topLeft());
        QVERIFY(hud.config()["position_locked"].toBool());
    }
    void enablingPositionLockCancelsAnInFlightWindowDrag() {
        auto c = config(); c["idle_collapse"] = false;
        HudWindow hud(c); hud.reveal();
        const QPoint top = header(hud);
        QTest::mousePress(&hud, Qt::LeftButton, Qt::NoModifier, top);
        QTest::mouseMove(&hud, top + QPoint(15, 10));
        c["position_locked"] = true; hud.applyConfig(c);
        const QRect locked = hud.geometry();
        QSignalSpy changes(&hud, &HudWindow::configChanged);
        QTest::mouseMove(&hud, top + QPoint(55, 30));
        QTest::mouseRelease(&hud, Qt::LeftButton, Qt::NoModifier, top + QPoint(55, 30));
        QCOMPARE(hud.geometry(), locked); QCOMPARE(changes.size(), 0);
    }
    void resettingPositionRecoversHiddenHudAndPreservesOtherMonitors() {
        auto c = config(); c["idle_collapse"] = false; c["position_locked"] = true;
        auto* screen = QGuiApplication::primaryScreen();
        c["monitor"] = screen->name(); c["anchor"] = "free"; c["offset_y"] = 300;
        c["monitor_positions"] = QJsonObject{{screen->name(), QJsonArray{90, 80}},
            {"Other saved monitor", QJsonArray{240, 180}}};
        HudWindow hud(c); hud.reveal(); hud.conceal(true);
        QSignalSpy changes(&hud, &HudWindow::configChanged);
        hud.resetPosition();
        QVERIFY(hud.isVisible()); QVERIFY(hud.windowOpacity() > .99);
        QCOMPARE(hud.config()["anchor"].toString(), QString("top_center"));
        QCOMPARE(hud.config()["offset_y"].toInt(), 12);
        QVERIFY(hud.config()["position_locked"].toBool());
        const auto positions = hud.config()["monitor_positions"].toObject();
        QVERIFY(!positions.contains(screen->name()));
        QCOMPARE(positions["Other saved monitor"].toArray(), (QJsonArray{240, 180}));
        const QRect area = screen->availableGeometry();
        const QRect window = hud.cardGeometry().toAlignedRect().adjusted(-14, -14, 14, 14);
        QCOMPARE(window.x(), area.x() + std::max(0, (area.width() - window.width()) / 2));
        QCOMPARE(window.y(), area.y() + std::min(12, std::max(0, area.height() - window.height())));
        QCOMPARE(changes.size(), 1);
    }
    void roundedEdgesContainPartialAlphaAtEveryScale_data() {
        QTest::addColumn<double>("scale");
        QTest::newRow("small") << 0.75;
        QTest::newRow("normal") << 1.0;
        QTest::newRow("fractional") << 1.25;
        QTest::newRow("large") << 1.75;
    }
    void roundedEdgesContainPartialAlphaAtEveryScale() {
        QFETCH(double, scale);
        auto c = backgroundConfig(); c["scale"] = scale; c["opacity"] = 1;
        c["artwork_background"] = false; c["border_width"] = 0; c["radius"] = 30;
        HudWindow hud(c); hud.reveal();
        const QImage image = hud.grab().toImage();
        const double ratio = image.devicePixelRatio();
        const QPoint corner = (hud.cardOrigin() * ratio).toPoint();
        const int radius = qRound(30 * scale * ratio);
        int partial = 0;
        for (int y = corner.y(); y < corner.y() + radius; ++y)
            for (int x = corner.x(); x < corner.x() + radius; ++x) {
                const int alpha = image.pixelColor(x, y).alpha();
                if (alpha > 0 && alpha < 255) ++partial;
            }
        QVERIFY2(partial > radius / 2, "Rounded contour lost its per-pixel antialiasing");
        QCOMPARE(image.pixelColor(corner).alpha(), 0);
        QCOMPARE(image.pixelColor((QPointF(cardCenter(hud)) * ratio).toPoint()).alpha(), 255);
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
        const auto expanded = hud.cardGeometry();
        QTRY_VERIFY_WITH_TIMEOUT(docked(hud), 1800);
        QVERIFY(hud.isVisible());
        QCOMPARE(hud.cardGeometry().size(), QSizeF(156, 32));
        QCOMPARE(hud.cardGeometry().bottom(), hud.screen()->geometry().top() + 8.0);
        MediaSnapshot media; media.active = true; media.title = "Next track";
        hud.setSnapshot(media);
        QCOMPARE(hud.cardGeometry().size(), QSizeF(156, 32));
        // Hovering the strip shows intent immediately but opens only after a short dwell.
        const QPoint handle = hud.mapFromGlobal(hud.cardGeometry().center().toPoint());
        QEnterEvent enter(handle, handle, hud.mapToGlobal(handle));
        QApplication::sendEvent(&hud, &enter);
        QVERIFY(hud.isCollapsed());
        QTRY_COMPARE_WITH_TIMEOUT(hud.cardGeometry(), expanded, 1000);
    }
    void editingBlocksDockingAndDisablingRestoresSize() {
        auto c = config(); c["idle_collapse"] = true; c["idle_collapse_seconds"] = 1;
        auto effects = c["animations"].toObject(); effects["dock"] = false; c["animations"] = effects;
        QCursor::setPos(QGuiApplication::primaryScreen()->geometry().bottomRight());
        HudWindow hud(c); hud.setEditing(true);
        const auto expanded = hud.cardGeometry();
        QTest::qWait(1100); QCOMPARE(hud.cardGeometry(), expanded); QVERIFY(!hud.isCollapsed());
        hud.setEditing(false);
        QTRY_VERIFY_WITH_TIMEOUT(docked(hud), 1800);
        c["idle_collapse"] = false; hud.applyConfig(c);
        QCOMPARE(hud.cardGeometry(), expanded);
    }
    void manualHideAndEditingInterruptDocking() {
        auto c = config(); c["idle_collapse"] = true; c["idle_collapse_seconds"] = 1;
        auto effects = c["animations"].toObject(); effects["dock"] = false; c["animations"] = effects;
        QCursor::setPos(QGuiApplication::primaryScreen()->geometry().bottomRight());
        HudWindow hud(c); hud.reveal(); const auto expanded = hud.cardGeometry();
        QTRY_VERIFY_WITH_TIMEOUT(docked(hud), 1800);
        hud.conceal(true);
        MediaSnapshot media; media.active = true; media.title = "Hidden track"; hud.setSnapshot(media);
        QEnterEvent enter({}, {}, {}); QApplication::sendEvent(&hud, &enter);
        QTest::qWait(250);
        QVERIFY(!hud.isVisible());
        hud.setEditing(true);
        QVERIFY(hud.isVisible()); QCOMPARE(hud.cardGeometry(), expanded);
    }
    void pointerAtDockHandleDoesNotCauseRepeatedCollapse() {
        auto c = config(); c["idle_collapse"] = true; c["idle_collapse_seconds"] = 1;
        c["auto_hide_seconds"] = 1;
        auto effects = c["animations"].toObject(); effects["dock"] = false; c["animations"] = effects;
        const auto screen = QGuiApplication::primaryScreen()->geometry();
        QCursor::setPos(screen.bottomRight());
        HudWindow hud(c); hud.reveal(); const auto expanded = hud.cardGeometry();
        QTRY_VERIFY_WITH_TIMEOUT(docked(hud), 1800);
        QCursor::setPos(QPoint(qRound(hud.cardGeometry().center().x()), screen.top() + 2));
        QEnterEvent enter({}, {}, QPointF(QCursor::pos())); QApplication::sendEvent(&hud, &enter);
        QTRY_COMPARE_WITH_TIMEOUT(hud.cardGeometry(), expanded, 1000);
        // The expanded card no longer covers the screen edge; the handle keeps it open.
        QEvent leave(QEvent::Leave); QApplication::sendEvent(&hud, &leave);
        QTest::qWait(1300);
        QVERIFY(hud.isVisible()); QCOMPARE(hud.cardGeometry(), expanded);
        QCursor::setPos(screen.bottomRight());
        QTRY_VERIFY_WITH_TIMEOUT(docked(hud), 1800);
        QVERIFY(hud.isVisible());
    }
    void configChangeDuringDockAnimationSettlesToExpandedGeometry() {
        auto c = config(); c["idle_collapse"] = true; c["idle_collapse_seconds"] = 1;
        auto effects = c["animations"].toObject(); effects["dock"] = true; c["animations"] = effects;
        c["animation_duration"] = 500;
        QCursor::setPos(QGuiApplication::primaryScreen()->geometry().bottomRight());
        HudWindow hud(c); hud.reveal();
        QTRY_VERIFY_WITH_TIMEOUT(hud.isCollapsed() && hud.dockProgress() > 0.1, 1800);
        c["idle_collapse"] = false; c["width"] = 700; hud.applyConfig(c);
        QTest::qWait(550);
        QCOMPARE(hud.cardGeometry().width(), 700.0); QVERIFY(!hud.isCollapsed()); QVERIFY(hud.isVisible());
    }
    void hidingDuringDragDoesNotLeaveIdleBlocked() {
        auto c = config(); c["idle_collapse"] = true; c["idle_collapse_seconds"] = 1;
        auto effects = c["animations"].toObject(); effects["dock"] = false; c["animations"] = effects;
        HudWindow hud(c); hud.reveal();
        QTest::mousePress(&hud, Qt::LeftButton, Qt::NoModifier, header(hud));
        hud.conceal(true); hud.reveal(true);
        QCursor::setPos(QGuiApplication::primaryScreen()->geometry().bottomRight());
        QEvent leave(QEvent::Leave); QApplication::sendEvent(&hud, &leave);
        QTRY_VERIFY_WITH_TIMEOUT(docked(hud), 1800);
    }
    void screenRelayoutDuringExpansionRestartsIdleTimer() {
        auto c = config(); c["idle_collapse"] = true; c["idle_collapse_seconds"] = 1;
        QCursor::setPos(QGuiApplication::primaryScreen()->geometry().bottomRight());
        HudWindow hud(c); hud.reveal(); const QRectF expanded = hud.cardGeometry();
        QTRY_VERIFY_WITH_TIMEOUT(docked(hud), 1800);
        auto effects = c["animations"].toObject(); effects["dock"] = true;
        c["animations"] = effects; c["animation_duration"] = 400;
        hud.applyConfig(c); hud.reveal(true); QTest::qWait(40);
        auto* screen = QGuiApplication::primaryScreen();
        QVERIFY(QMetaObject::invokeMethod(screen, "availableGeometryChanged", Qt::DirectConnection,
            Q_ARG(QRect, screen->availableGeometry())));
        QTRY_COMPARE_WITH_TIMEOUT(hud.cardGeometry(), expanded, 250);
        QTRY_VERIFY_WITH_TIMEOUT(docked(hud), 2500);
        QVERIFY(hud.isVisible());
    }
    void mediaInactivityAndResumeKeepDockHandleAvailable() {
        auto c = config(); c["idle_collapse"] = true; c["idle_collapse_seconds"] = 1;
        QCursor::setPos(QGuiApplication::primaryScreen()->geometry().bottomRight());
        HudWindow hud(c);
        MediaSnapshot media; media.active = true; media.playing = true; media.title = "Track";
        hud.setSnapshot(media); hud.reveal(); const QRectF expanded = hud.cardGeometry();
        QTRY_VERIFY_WITH_TIMEOUT(docked(hud), 2500);
        const QRectF strip = hud.cardGeometry();
        media.active = false; media.playing = false; hud.setSnapshot(media);
        QVERIFY(hud.isVisible()); QCOMPARE(hud.cardGeometry(), strip);
        media.active = true; media.playing = true; hud.setSnapshot(media);
        QVERIFY(hud.isVisible()); QCOMPARE(hud.cardGeometry(), strip);
        QVERIFY(hud.windowOpacity() > .99);
        hud.reveal(true); QTRY_COMPARE_WITH_TIMEOUT(hud.cardGeometry(), expanded, 1500);
    }
    void slowExpansionFinishesBeforeIdleTimerStarts() {
        auto c = config(); c["idle_collapse"] = true; c["idle_collapse_seconds"] = 1;
        auto effects = c["animations"].toObject(); effects["dock"] = false; c["animations"] = effects;
        QCursor::setPos(QGuiApplication::primaryScreen()->geometry().bottomRight());
        HudWindow hud(c); hud.reveal(); const auto expanded = hud.cardGeometry();
        QTRY_VERIFY_WITH_TIMEOUT(docked(hud), 1800);
        effects["dock"] = true; c["animations"] = effects; c["animation_duration"] = 1600;
        hud.applyConfig(c); hud.reveal(true);
        QTRY_COMPARE_WITH_TIMEOUT(hud.cardGeometry(), expanded, 4000);
        QVERIFY(hud.isVisible()); QVERIFY(!hud.isCollapsed());
    }
};
QTEST_MAIN(HudTest)
#include "test_hud.moc"

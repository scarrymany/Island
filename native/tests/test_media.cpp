#include "MediaBridge.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QSignalSpy>
#include <QTest>

#include <limits>

class MediaTests final : public QObject {
    Q_OBJECT

private slots:
    void prefersPlayingSource() {
        const QList<MediaSelection::Source> sources{{"browser", false}, {"spotify", true}};
        QCOMPARE(MediaSelection::selectSource(sources, "browser", {}, {}), QString("spotify"));
    }

    void prefersCurrentPlayingSource() {
        const QList<MediaSelection::Source> sources{{"browser", true}, {"spotify", true}};
        QCOMPARE(MediaSelection::selectSource(sources, "browser", {}, "spotify"), QString("browser"));
    }

    void retainsPlayingSourceWhenCurrentIsPaused() {
        const QList<MediaSelection::Source> sources{{"browser", false}, {"other", true}, {"spotify", true}};
        QCOMPARE(MediaSelection::selectSource(sources, "browser", {}, "spotify"), QString("spotify"));
    }

    void manualSourceCanBePaused() {
        const QList<MediaSelection::Source> sources{{"browser", true}, {"spotify", false}};
        QCOMPARE(MediaSelection::selectSource(sources, "browser", "spotify", {}), QString("spotify"));
    }

    void missingManualSourceDoesNotSelectAnotherApplication() {
        const QList<MediaSelection::Source> sources{{"browser", true}};
        QVERIFY(MediaSelection::selectSource(sources, "browser", "spotify", {}).isEmpty());
    }

    void automaticSourceRecoversAfterRemoval() {
        const QList<MediaSelection::Source> sources{{"browser", true}};
        QCOMPARE(MediaSelection::selectSource(sources, {}, {}, "spotify"), QString("browser"));
    }

    void emptySessionsHaveNoSource() {
        QVERIFY(MediaSelection::selectSource({}, {}, {}, {}).isEmpty());
    }

    void positionAdvancesAtPlaybackRate() {
        MediaSnapshot snapshot;
        snapshot.active = true;
        snapshot.playing = true;
        snapshot.position = 20;
        snapshot.duration = 100;
        snapshot.playbackRate = 1.5;
        snapshot.updatedAt = QDateTime::currentMSecsSinceEpoch() - 10'000;
        const double result = snapshot.estimatedPosition();
        QVERIFY(result >= 35.0 && result < 35.1);
    }

    void positionStopsAtTrackEnd() {
        MediaSnapshot snapshot;
        snapshot.active = true;
        snapshot.playing = true;
        snapshot.position = 55;
        snapshot.duration = 60;
        snapshot.updatedAt = QDateTime::currentMSecsSinceEpoch() - 10'000;
        QCOMPARE(snapshot.estimatedPosition(), 60.0);
    }

    void pausedPositionIsStable() {
        MediaSnapshot snapshot;
        snapshot.active = true;
        snapshot.position = 20;
        snapshot.duration = 100;
        snapshot.updatedAt = QDateTime::currentMSecsSinceEpoch() - 10'000;
        QCOMPARE(snapshot.estimatedPosition(), 20.0);
    }

    void clockChangeDoesNotRewindTrack() {
        MediaSnapshot snapshot;
        snapshot.active = true;
        snapshot.playing = true;
        snapshot.position = 20;
        snapshot.updatedAt = QDateTime::currentMSecsSinceEpoch() + 10'000;
        QCOMPARE(snapshot.estimatedPosition(), 20.0);
    }

    void inactivePositionIsZero() {
        MediaSnapshot snapshot;
        snapshot.position = 20;
        QCOMPARE(snapshot.estimatedPosition(), 0.0);
    }

    void invalidPositionIsSafe() {
        MediaSnapshot snapshot;
        snapshot.active = true;
        snapshot.position = std::numeric_limits<double>::quiet_NaN();
        QCOMPARE(snapshot.estimatedPosition(), 0.0);
    }

    void shutdownCancelsStartup() {
        MediaBridge bridge;
        QElapsedTimer timer;
        timer.start();
        bridge.start();
        bridge.stop();
        QVERIFY2(timer.elapsed() < 1500, "Media startup cancellation took too long");
    }

    void shutdownStopsQueuedSignals() {
        MediaBridge bridge;
        QSignalSpy snapshots(&bridge, &MediaBridge::snapshotChanged);
        QSignalSpy sources(&bridge, &MediaBridge::sourcesChanged);
        bridge.start();
        QTest::qWait(150);
        bridge.stop();
        const auto snapshotCount = snapshots.count();
        const auto sourceCount = sources.count();
        QTest::qWait(50);
        QCOMPARE(snapshots.count(), snapshotCount);
        QCOMPARE(sources.count(), sourceCount);
    }

    void canRestartAfterStop() {
        MediaBridge bridge;
        bridge.start();
        bridge.stop();
        bridge.start();
        bridge.stop();
    }

    void manualMissingSourcePublishesNoUnrelatedTrack() {
        MediaBridge bridge;
        bridge.setSource(QStringLiteral("island-test-nonexistent-source"));
        QSignalSpy snapshots(&bridge, &MediaBridge::snapshotChanged);
        bridge.start();
        QTest::qWait(150);
        bridge.stop();
        for (const auto& arguments : snapshots) {
            QVERIFY(!qvariant_cast<MediaSnapshot>(arguments.first()).active);
        }
    }
};

QTEST_GUILESS_MAIN(MediaTests)
#include "test_media.moc"

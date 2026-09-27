#include "AnimationClock.h"

#include <QEventLoop>
#include <QTimer>
#include <QtTest>
#include <cmath>
#include <limits>

using namespace std::chrono_literals;

namespace {
void runFor(int milliseconds) {
    QEventLoop loop;
    QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
    loop.exec();
}
}

class AnimationTest final : public QObject {
    Q_OBJECT
private slots:
    void monitorCadencePreservesFractionalIntervals() {
        AnimationClock clock;
        clock.setRefreshRate(144);
        QVERIFY(std::abs(clock.frameInterval().count() - 6'944'444) <= 1);
        clock.setRefreshRate(240);
        QVERIFY(std::abs(clock.frameInterval().count() - 4'166'667) <= 1);
        clock.setRefreshRate(59.94);
        QVERIFY(std::abs(clock.frameInterval().count() - 16'683'350) <= 1);
        QVERIFY(!clock.isActive());
    }

    void invalidRatesUseSafeCadence() {
        AnimationClock clock;
        for (const double rate : {0.0, -1.0, std::numeric_limits<double>::infinity(),
                                  std::numeric_limits<double>::quiet_NaN()}) {
            clock.setRefreshRate(rate);
            QVERIFY(std::abs(clock.frameInterval().count() - 16'666'667) <= 1);
        }
        clock.setRefreshRate(1e20);
        QVERIFY(clock.frameInterval() >= 1ms);
    }

    void completesOnceAndStops() {
        AnimationClock clock;
        clock.setRefreshRate(144);
        QList<double> values;
        int completed = 0;
        clock.start("dock", 0, 1, 80ms, [&](double value) { values.append(value); }, [&] { ++completed; });
        QCOMPARE(values, QList<double>{0});
        QVERIFY(clock.isRunning("dock"));
        runFor(150);
        QCOMPARE(completed, 1);
        QCOMPARE(values.last(), 1.0);
        QVERIFY(values.size() > 2);
        for (qsizetype i = 1; i < values.size(); ++i) QVERIFY(values[i] >= values[i - 1]);
        QVERIFY(!clock.isRunning("dock"));
        QVERIFY(!clock.isActive());
        const auto count = values.size();
        runFor(30);
        QCOMPARE(values.size(), count);
    }

    void elapsedTimeSurvivesBlockedEventLoop() {
        AnimationClock clock;
        double value = -1;
        int updates = 0;
        int completed = 0;
        clock.start("dock", 0, 1, 60ms, [&](double next) { value = next; ++updates; }, [&] { ++completed; });
        QTest::qSleep(100);
        QCOMPARE(updates, 1);
        runFor(25);
        QCOMPARE(value, 1.0);
        QCOMPARE(updates, 2);
        QCOMPARE(completed, 1);
        QVERIFY(!clock.isActive());
    }

    void refreshChangeDoesNotRestartAnimation() {
        AnimationClock clock;
        double value = 0;
        int completed = 0;
        clock.start("dock", 0, 1, 100ms, [&](double next) { value = next; }, [&] { ++completed; });
        runFor(50);
        QVERIFY(value > 0 && value < 1);
        const double before = value;
        clock.setRefreshRate(240);
        QCOMPARE(value, before);
        runFor(85);
        QCOMPARE(value, 1.0);
        QCOMPARE(completed, 1);
    }

    void cancellationDoesNotFinishOrTick() {
        AnimationClock clock;
        int updates = 0;
        int completed = 0;
        clock.start("dock", 0, 1, 100ms, [&](double) { ++updates; }, [&] { ++completed; });
        clock.stop("dock");
        runFor(40);
        QCOMPARE(updates, 1);
        QCOMPARE(completed, 0);
        QVERIFY(!clock.isActive());
    }

    void callbackCanReplaceItsTrack() {
        AnimationClock clock;
        int oldCompleted = 0;
        int newCompleted = 0;
        double replacement = 0;
        clock.start("dock", 0, 1, 30ms, [&](double value) {
            if (value == 0) return;
            clock.start("dock", 10, 20, 20ms, [&](double next) { replacement = next; }, [&] { ++newCompleted; });
        }, [&] { ++oldCompleted; });
        runFor(120);
        QCOMPARE(oldCompleted, 0);
        QCOMPARE(newCompleted, 1);
        QCOMPARE(replacement, 20.0);
        QVERIFY(!clock.isActive());
    }

    void completionCanStartAnotherTrack() {
        AnimationClock clock;
        int completed = 0;
        clock.start("first", 0, 1, 20ms, [](double) {}, [&] {
            ++completed;
            clock.start("second", 0, 1, 20ms, [](double) {}, [&] { ++completed; });
        });
        runFor(120);
        QCOMPARE(completed, 2);
        QVERIFY(!clock.isActive());
    }

    void callbackCanCancelAllTracks() {
        AnimationClock clock;
        int completed = 0;
        clock.start("first", 0, 1, 20ms, [&](double value) {
            if (value > 0) clock.stopAll();
        }, [&] { ++completed; });
        clock.start("second", 0, 1, 20ms, [](double) {}, [&] { ++completed; });
        runFor(70);
        QCOMPARE(completed, 0);
        QVERIFY(!clock.isRunning("first"));
        QVERIFY(!clock.isRunning("second"));
        QVERIFY(!clock.isActive());
    }

    void zeroDurationCompletesSynchronously() {
        AnimationClock clock;
        double value = 0;
        int completed = 0;
        clock.start("dock", 0, 1, 0ms, [&](double next) { value = next; }, [&] { ++completed; });
        QCOMPARE(value, 1.0);
        QCOMPARE(completed, 1);
        QVERIFY(!clock.isActive());
    }

    void measureHighRefreshDelivery() {
        AnimationClock clock;
        clock.setRefreshRate(200);
        QElapsedTimer elapsed;
        QEventLoop loop;
        int callbacks = 0;
        bool completed = false;
        elapsed.start();
        clock.start("measurement", 0, 1, 1000ms, [&](double) { ++callbacks; }, [&] {
            completed = true;
            loop.quit();
        });
        QTimer::singleShot(2000, &loop, &QEventLoop::quit);
        loop.exec();
        const double milliseconds = elapsed.nsecsElapsed() / 1'000'000.0;
        qInfo().nospace() << "200 Hz scheduler: " << callbacks - 1 << " timed callbacks in "
            << milliseconds << " ms; requested period=" << clock.frameInterval().count() << " ns";
        QVERIFY(completed);
        QVERIFY(!clock.isActive());
    }
};

QTEST_GUILESS_MAIN(AnimationTest)
#include "test_animation.moc"

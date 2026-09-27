#pragma once

#include <QChronoTimer>
#include <QEasingCurve>
#include <QElapsedTimer>
#include <QMap>
#include <QObject>
#include <QString>
#include <chrono>
#include <functional>

class AnimationClock final : public QObject {
public:
    using ValueCallback = std::function<void(double)>;
    using CompletionCallback = std::function<void()>;

    explicit AnimationClock(QObject* parent = nullptr);
    void setRefreshRate(double refreshRate);
    std::chrono::nanoseconds frameInterval() const { return frameInterval_; }
    bool isActive() const { return timer_.isActive(); }
    bool isRunning(const QString& name) const { return tracks_.contains(name); }
    void start(const QString& name, double from, double to, std::chrono::milliseconds duration,
               ValueCallback callback, CompletionCallback finished = {});
    void stop(const QString& name);
    void stopAll();

private:
    struct Track {
        quint64 id;
        qint64 started;
        qint64 duration;
        double from;
        double to;
        ValueCallback callback;
        CompletionCallback finished;
    };

    void tick();
    void schedule();

    QChronoTimer timer_;
    QElapsedTimer elapsed_;
    QEasingCurve easing_{QEasingCurve::OutCubic};
    QMap<QString, Track> tracks_;
    std::chrono::nanoseconds frameInterval_{};
    qint64 nextFrame_ = 0;
    quint64 nextId_ = 0;
    bool ticking_ = false;
};

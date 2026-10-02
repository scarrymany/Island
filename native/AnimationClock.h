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

    // SwiftUI-style parameters: response is the undamped period in seconds,
    // damping is the ratio (below 1 overshoots slightly, 1 settles without bounce).
    struct Spring {
        double response = 0.35;
        double damping = 1.0;
    };

    explicit AnimationClock(QObject* parent = nullptr);
    void setRefreshRate(double refreshRate);
    std::chrono::nanoseconds frameInterval() const { return frameInterval_; }
    bool isActive() const { return timer_.isActive(); }
    bool isRunning(const QString& name) const { return tracks_.contains(name); }
    void start(const QString& name, double from, double to, std::chrono::milliseconds duration,
               ValueCallback callback, CompletionCallback finished = {});
    void start(const QString& name, double from, double to, std::chrono::milliseconds duration,
               const QEasingCurve& easing, ValueCallback callback, CompletionCallback finished = {});
    // A spring keeps its velocity when it is retargeted, so an interrupted motion
    // reverses smoothly instead of restarting from rest with a visible kink.
    void spring(const QString& name, double from, double to, Spring parameters, ValueCallback callback,
                CompletionCallback finished = {}, double velocity = 0);
    // Moves a running spring to a new target. Returns false when no spring of that name runs.
    bool retarget(const QString& name, double to, CompletionCallback finished = {});
    [[nodiscard]] double velocity(const QString& name) const;
    [[nodiscard]] double target(const QString& name, double fallback) const;
    // Runs callback once on the next display frame. Repeated requests before that frame
    // coalesce, which keeps high-rate input (1000 Hz mice) at the display cadence.
    void requestFrame(const QString& name, CompletionCallback callback);
    void stop(const QString& name);
    void stopAll();

    static void evaluateSpring(double from, double velocity, double to, Spring parameters, double seconds,
                               double& position, double& currentVelocity);

private:
    enum class Kind { Eased, Spring, Frame };
    struct Track {
        quint64 id;
        Kind kind;
        qint64 started;
        qint64 duration;
        double from;
        double to;
        double velocity;
        double scale;
        Spring spring;
        QEasingCurve easing;
        ValueCallback callback;
        CompletionCallback finished;
        double current;
        double currentVelocity;
    };

    void tick();
    void schedule();
    static double springPrecision(const Track& track);
    qint64 alignToVBlank(qint64 deadline, qint64 now);

    QChronoTimer timer_;
    QElapsedTimer elapsed_;
    QEasingCurve easing_{QEasingCurve::OutCubic};
    QMap<QString, Track> tracks_;
    std::chrono::nanoseconds frameInterval_{};
    qint64 nextFrame_ = 0;
    qint64 vblank_ = 0;
    qint64 vblankPeriod_ = 0;
    qint64 vblankSampled_ = 0;
    quint64 nextId_ = 0;
    bool ticking_ = false;
};

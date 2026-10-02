#include "AnimationClock.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <utility>

namespace {
constexpr double DefaultRefreshRate = 60;
constexpr double MaximumRefreshRate = 1000;
constexpr qint64 NanosecondsPerSecond = 1'000'000'000;
constexpr double SpringRelativePrecision = 0.0008;
constexpr double SpringMaximumPeriods = 12;
}

AnimationClock::AnimationClock(QObject* parent) : QObject(parent) {
    elapsed_.start();
    timer_.setSingleShot(true);
    timer_.setTimerType(Qt::PreciseTimer);
    connect(&timer_, &QChronoTimer::timeout, this, &AnimationClock::tick);
    setRefreshRate(DefaultRefreshRate);
}

void AnimationClock::setRefreshRate(double refreshRate) {
    if (!std::isfinite(refreshRate) || refreshRate <= 0) refreshRate = DefaultRefreshRate;
    refreshRate = std::clamp(refreshRate, 1.0, MaximumRefreshRate);
    const auto interval = std::chrono::nanoseconds(std::llround(NanosecondsPerSecond / refreshRate));
    if (frameInterval_ == interval) return;
    frameInterval_ = interval;
    nextFrame_ = elapsed_.nsecsElapsed() + frameInterval_.count();
    schedule();
}

void AnimationClock::start(const QString& name, double from, double to, std::chrono::milliseconds duration,
                           ValueCallback callback, CompletionCallback finished) {
    start(name, from, to, duration, easing_, std::move(callback), std::move(finished));
}

void AnimationClock::start(const QString& name, double from, double to, std::chrono::milliseconds duration,
                           const QEasingCurve& easing, ValueCallback callback, CompletionCallback finished) {
    stop(name);
    if (!callback) return;
    if (duration <= std::chrono::milliseconds::zero() || from == to) {
        callback(to);
        if (finished) finished();
        return;
    }
    tracks_.insert(name, Track{++nextId_, Kind::Eased, elapsed_.nsecsElapsed(),
        std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count(), from, to, 0, std::abs(to - from),
        {}, easing, callback, std::move(finished), from, 0});
    callback(from);
    if (!timer_.isActive()) schedule();
}

void AnimationClock::spring(const QString& name, double from, double to, Spring parameters, ValueCallback callback,
                            CompletionCallback finished, double velocity) {
    stop(name);
    if (!callback) return;
    if (!std::isfinite(velocity)) velocity = 0;
    const bool instant = !std::isfinite(parameters.response) || parameters.response <= 0
        || !std::isfinite(parameters.damping) || parameters.damping <= 0;
    if (instant || (from == to && velocity == 0)) {
        callback(to);
        if (finished) finished();
        return;
    }
    tracks_.insert(name, Track{++nextId_, Kind::Spring, elapsed_.nsecsElapsed(), 0, from, to, velocity,
        std::max({std::abs(to - from), std::abs(velocity) * parameters.response / (2 * std::numbers::pi), 1e-9}),
        parameters, {}, callback, std::move(finished), from, velocity});
    callback(from);
    if (!timer_.isActive()) schedule();
}

bool AnimationClock::retarget(const QString& name, double to, CompletionCallback finished) {
    auto current = tracks_.find(name);
    if (current == tracks_.end() || current->kind != Kind::Spring) return false;
    if (current->to == to && !finished) return true;
    const qint64 now = elapsed_.nsecsElapsed();
    double position = current->current, speed = current->currentVelocity;
    evaluateSpring(current->from, current->velocity, current->to, current->spring,
                   static_cast<double>(now - current->started) / NanosecondsPerSecond, position, speed);
    current->id = ++nextId_;
    current->started = now;
    current->from = position;
    current->velocity = speed;
    current->scale = std::max(current->scale, std::abs(to - position));
    current->to = to;
    current->current = position;
    current->currentVelocity = speed;
    if (finished) current->finished = std::move(finished);
    return true;
}

double AnimationClock::velocity(const QString& name) const {
    const auto current = tracks_.constFind(name);
    return current == tracks_.cend() ? 0.0 : current->currentVelocity;
}

double AnimationClock::target(const QString& name, double fallback) const {
    const auto current = tracks_.constFind(name);
    return current == tracks_.cend() ? fallback : current->to;
}

void AnimationClock::stop(const QString& name) {
    tracks_.remove(name);
    if (tracks_.isEmpty()) {
        timer_.stop();
        nextFrame_ = 0;
    }
}

void AnimationClock::stopAll() {
    tracks_.clear();
    timer_.stop();
    nextFrame_ = 0;
}

void AnimationClock::evaluateSpring(double from, double velocity, double to, Spring parameters, double seconds,
                                    double& position, double& currentVelocity) {
    const double omega = 2 * std::numbers::pi / parameters.response;
    const double zeta = parameters.damping;
    const double displacement = from - to;
    const double t = std::max(0.0, seconds);
    double offset = 0, speed = 0;
    if (std::abs(zeta - 1) < 1e-6) {
        const double envelope = std::exp(-omega * t);
        const double slope = velocity + omega * displacement;
        offset = (displacement + slope * t) * envelope;
        speed = (velocity - omega * slope * t) * envelope;
    } else if (zeta < 1) {
        const double damped = omega * std::sqrt(1 - zeta * zeta);
        const double envelope = std::exp(-zeta * omega * t);
        const double a = displacement;
        const double b = (velocity + zeta * omega * displacement) / damped;
        const double cosine = std::cos(damped * t), sine = std::sin(damped * t);
        offset = envelope * (a * cosine + b * sine);
        speed = envelope * ((b * damped - zeta * omega * a) * cosine - (a * damped + zeta * omega * b) * sine);
    } else {
        const double root = std::sqrt(zeta * zeta - 1);
        const double fast = -omega * (zeta + root), slow = -omega * (zeta - root);
        const double second = (velocity - slow * displacement) / (fast - slow);
        const double first = displacement - second;
        offset = first * std::exp(slow * t) + second * std::exp(fast * t);
        speed = slow * first * std::exp(slow * t) + fast * second * std::exp(fast * t);
    }
    position = to + offset;
    currentVelocity = speed;
}

double AnimationClock::springPrecision(const Track& track) {
    return std::max(track.scale * SpringRelativePrecision, 1e-7);
}

void AnimationClock::tick() {
    const qint64 now = elapsed_.nsecsElapsed();
    if (now < nextFrame_) {
        schedule();
        return;
    }
    ticking_ = true;
    const auto names = tracks_.keys();
    for (const auto& name : names) {
        auto current = tracks_.find(name);
        if (current == tracks_.end()) continue;
        const Track track = current.value();
        bool done = false;
        double value = track.to;
        if (track.kind == Kind::Eased) {
            const double progress = std::clamp(static_cast<double>(now - track.started) / track.duration, 0.0, 1.0);
            done = progress == 1;
            if (!done) value = track.from + (track.to - track.from) * track.easing.valueForProgress(progress);
            current->current = value;
        } else {
            const double seconds = static_cast<double>(now - track.started) / NanosecondsPerSecond;
            double speed = 0;
            evaluateSpring(track.from, track.velocity, track.to, track.spring, seconds, value, speed);
            const double precision = springPrecision(track);
            const double velocityPrecision = precision * 2 * std::numbers::pi / track.spring.response;
            done = (std::abs(value - track.to) <= precision && std::abs(speed) <= velocityPrecision)
                || seconds > track.spring.response * SpringMaximumPeriods / std::min(1.0, track.spring.damping);
            if (done) { value = track.to; speed = 0; }
            current->current = value;
            current->currentVelocity = speed;
        }
        track.callback(value);
        // A value callback can cancel, replace or retarget this track while updating the widget.
        current = tracks_.find(name);
        if (current == tracks_.end() || current->id != track.id || !done) continue;
        tracks_.erase(current);
        if (track.finished) track.finished();
    }
    ticking_ = false;
    schedule();
}

void AnimationClock::schedule() {
    if (ticking_) return;
    if (tracks_.isEmpty()) {
        timer_.stop();
        nextFrame_ = 0;
        return;
    }
    const qint64 now = elapsed_.nsecsElapsed();
    const qint64 period = frameInterval_.count();
    if (nextFrame_ == 0) nextFrame_ = now + period;
    else if (nextFrame_ <= now) nextFrame_ += ((now - nextFrame_) / period + 1) * period;
    // Absolute deadlines compensate for Windows timer rounding; late frames are skipped.
    timer_.setInterval(std::chrono::nanoseconds(nextFrame_ - now));
    timer_.start();
}

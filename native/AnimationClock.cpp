#include "AnimationClock.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace {
constexpr double DefaultRefreshRate = 60;
constexpr double MaximumRefreshRate = 1000;
constexpr qint64 NanosecondsPerSecond = 1'000'000'000;
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
    stop(name);
    if (!callback) return;
    if (duration <= std::chrono::milliseconds::zero() || from == to) {
        callback(to);
        if (finished) finished();
        return;
    }
    tracks_.insert(name, Track{++nextId_, elapsed_.nsecsElapsed(),
        std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count(), from, to, callback, std::move(finished)});
    callback(from);
    if (!timer_.isActive()) schedule();
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
        const double progress = std::clamp(static_cast<double>(now - track.started) / track.duration, 0.0, 1.0);
        track.callback(progress == 1 ? track.to : track.from + (track.to - track.from) * easing_.valueForProgress(progress));
        // A value callback can cancel or replace this track while updating the widget.
        current = tracks_.find(name);
        if (current == tracks_.end() || current->id != track.id || progress < 1) continue;
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

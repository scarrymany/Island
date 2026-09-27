#include "MediaBridge.h"

#include <QDateTime>
#include <QDebug>
#include <QMetaObject>
#include <QScopeGuard>

#include <Windows.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Storage.Streams.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <stop_token>
#include <thread>
#include <utility>

using namespace std::chrono_literals;
using winrt::Windows::Foundation::AsyncStatus;
using winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSession;
using winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionManager;
using winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionMediaProperties;
using winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionPlaybackStatus;
using winrt::Windows::Storage::Streams::DataReader;
using winrt::Windows::Storage::Streams::IRandomAccessStreamReference;

namespace {
using Clock = std::chrono::steady_clock;
using Session = GlobalSystemMediaTransportControlsSession;
using SessionManager = GlobalSystemMediaTransportControlsSessionManager;
using Metadata = GlobalSystemMediaTransportControlsSessionMediaProperties;
using PlaybackStatus = GlobalSystemMediaTransportControlsSessionPlaybackStatus;

constexpr auto RecoveryInterval = 2s;
constexpr auto MetadataInterval = 15s;
constexpr auto OperationTimeout = 5s;
constexpr uint64_t MaxCoverBytes = 8 * 1024 * 1024;
constexpr size_t MaxPendingCommands = 16;
constexpr double TicksPerSecond = 10'000'000.0;

struct Cancelled final {};

QString text(const winrt::hstring& value) {
    return QString::fromWCharArray(value.c_str(), static_cast<qsizetype>(value.size()));
}

QString sourceName(const QString& sourceId) {
    static const QList<QPair<QString, QString>> names{
        {QStringLiteral("spotify"), QStringLiteral("Spotify")},
        {QStringLiteral("soundcloud"), QStringLiteral("SoundCloud")},
        {QStringLiteral("chrome"), QStringLiteral("Chrome")},
        {QStringLiteral("firefox"), QStringLiteral("Firefox")},
        {QStringLiteral("msedge"), QStringLiteral("Edge")},
        {QStringLiteral("music.ui"), QStringLiteral("Media Player")},
        {QStringLiteral("vlc"), QStringLiteral("VLC")},
        {QStringLiteral("opera"), QStringLiteral("Opera")},
        {QStringLiteral("brave"), QStringLiteral("Brave")},
    };
    for (const auto& [token, name] : names) {
        if (sourceId.contains(token, Qt::CaseInsensitive)) {
            return name;
        }
    }
    QString name = sourceId.section(u'\\', -1).section(u'!', 0, 0).section(u'_', 0, 0);
    if (name.endsWith(QStringLiteral(".exe"), Qt::CaseInsensitive)) {
        name.chop(4);
    }
    return name.isEmpty() ? sourceId : name;
}

struct Command {
    QString action;
    double value = 0.0;
    QString sourceId;
};

struct WorkerState {
    explicit WorkerState(MediaBridge* bridge) : owner(bridge) {}

    MediaBridge* owner;
    std::atomic<bool> stopping{false};
    std::mutex mutex;
    std::condition_variable_any changed;
    uint64_t revision = 0;
    bool wake = true;
    bool rebuild = false;
    QString preferred;
    QString selected;
    std::set<QString> dirtyMetadata;
    std::deque<Command> commands;

    void notify(const QString& sourceId = {}, bool metadata = false, bool sessions = false) noexcept {
        try {
            {
                std::lock_guard lock(mutex);
                if (stopping.load()) {
                    return;
                }
                ++revision;
                wake = true;
                rebuild = rebuild || sessions;
                if (metadata) {
                    dirtyMetadata.insert(sourceId);
                }
            }
            changed.notify_one();
        } catch (...) {
            qWarning() << "Could not queue a Windows media event";
        }
    }

    bool isCurrent(uint64_t expected) {
        std::lock_guard lock(mutex);
        return !stopping.load() && expected == revision;
    }
};

struct AsyncWait {
    std::mutex mutex;
    std::condition_variable_any changed;
    bool completed = false;
};

template<class Operation>
auto waitFor(const Operation& operation, std::stop_token stop) {
    if (stop.stop_requested()) {
        operation.Cancel();
        throw Cancelled{};
    }
    const auto wait = std::make_shared<AsyncWait>();
    operation.Completed([wait](const auto&, AsyncStatus) noexcept {
        {
            std::lock_guard lock(wait->mutex);
            wait->completed = true;
        }
        wait->changed.notify_one();
    });
    std::unique_lock lock(wait->mutex);
    const bool completed = wait->changed.wait_for(lock, stop, OperationTimeout, [&] { return wait->completed; });
    lock.unlock();
    if (!completed || stop.stop_requested()) {
        operation.Cancel();
        if (stop.stop_requested()) {
            throw Cancelled{};
        }
        throw winrt::hresult_error(HRESULT_FROM_WIN32(ERROR_TIMEOUT), L"Windows media request timed out");
    }
    return operation.GetResults();
}

struct SessionRecord {
    Session session{nullptr};
    Session::MediaPropertiesChanged_revoker mediaChanged;
    Session::PlaybackInfoChanged_revoker playbackChanged;
    Session::TimelinePropertiesChanged_revoker timelineChanged;
    Metadata metadata{nullptr};
    QByteArray cover;
    Clock::time_point metadataAt{};
    bool metadataDirty = true;
};

struct SessionCandidate {
    Session session{nullptr};
    bool playing = false;
    bool current = false;
};

class WindowsMediaBackend final {
public:
    WindowsMediaBackend(std::shared_ptr<WorkerState> state, std::stop_token stop)
        : state(std::move(state)), stop(stop) {}

    void run() {
        while (!stop.stop_requested()) {
            QString preferred;
            std::deque<Command> commands;
            std::set<QString> dirty;
            bool rebuild;
            uint64_t revision;
            {
                std::lock_guard lock(state->mutex);
                state->wake = false;
                preferred = state->preferred;
                revision = state->revision;
                rebuild = std::exchange(state->rebuild, false);
                dirty.swap(state->dirtyMetadata);
                commands.swap(state->commands);
            }
            try {
                if (!manager) {
                    connect();
                }
                for (const auto& id : dirty) {
                    const auto it = records.find(id);
                    if (it != records.end()) {
                        it->second.metadataDirty = true;
                    }
                }
                for (const auto& command : commands) {
                    try {
                        execute(command, preferred);
                    } catch (const winrt::hresult_error& exception) {
                        reportError(QStringLiteral("Команда воспроизведения не выполнена: %1")
                            .arg(text(exception.message())));
                    }
                }
                refresh(preferred, rebuild, revision);
                lastError.clear();
            } catch (const Cancelled&) {
                break;
            } catch (const winrt::hresult_error& exception) {
                reportError(QStringLiteral("Windows Media Control: %1").arg(text(exception.message())));
                disconnect();
                publish(MediaSnapshot{});
            } catch (const std::exception& exception) {
                reportError(QStringLiteral("Ошибка медиаисточника: %1").arg(QString::fromUtf8(exception.what())));
                disconnect();
                publish(MediaSnapshot{});
            }
            std::unique_lock lock(state->mutex);
            state->changed.wait_for(lock, stop, RecoveryInterval, [&] { return state->wake; });
        }
        disconnect();
    }

private:
    std::shared_ptr<WorkerState> state;
    std::stop_token stop;
    SessionManager manager{nullptr};
    SessionManager::SessionsChanged_revoker sessionsChanged;
    SessionManager::CurrentSessionChanged_revoker currentChanged;
    std::map<QString, SessionRecord> records;
    QString selected;
    QString lastError;
    MediaSources sources;
    MediaSnapshot snapshot;

    void connect() {
        manager = waitFor(SessionManager::RequestAsync(), stop);
        const auto weak = std::weak_ptr(state);
        sessionsChanged = manager.SessionsChanged(winrt::auto_revoke, [weak](const auto&, const auto&) noexcept {
            if (auto shared = weak.lock()) {
                shared->notify({}, false, true);
            }
        });
        currentChanged = manager.CurrentSessionChanged(winrt::auto_revoke, [weak](const auto&, const auto&) noexcept {
            if (auto shared = weak.lock()) {
                shared->notify();
            }
        });
    }

    void disconnect() {
        sessionsChanged.revoke();
        currentChanged.revoke();
        records.clear();
        manager = nullptr;
        selected.clear();
        if (!sources.isEmpty()) {
            sources.clear();
            postSources();
        }
        std::lock_guard lock(state->mutex);
        state->selected.clear();
    }

    void addSession(const QString& id, const Session& session) {
        SessionRecord record;
        record.session = session;
        const auto weak = std::weak_ptr(state);
        record.mediaChanged = session.MediaPropertiesChanged(winrt::auto_revoke,
            [weak, id](const auto&, const auto&) noexcept {
                if (auto shared = weak.lock()) {
                    shared->notify(id, true);
                }
            });
        record.playbackChanged = session.PlaybackInfoChanged(winrt::auto_revoke,
            [weak](const auto&, const auto&) noexcept {
                if (auto shared = weak.lock()) {
                    shared->notify();
                }
            });
        record.timelineChanged = session.TimelinePropertiesChanged(winrt::auto_revoke,
            [weak](const auto&, const auto&) noexcept {
                if (auto shared = weak.lock()) {
                    shared->notify();
                }
            });
        records.insert_or_assign(id, std::move(record));
    }

    void refresh(const QString& preferred, bool rebuild, uint64_t revision) {
        const auto current = manager.GetCurrentSession();
        std::map<QString, SessionCandidate> sessions;
        for (const auto& session : manager.GetSessions()) {
            try {
                const auto status = session.GetPlaybackInfo().PlaybackStatus();
                if (status == PlaybackStatus::Closed) {
                    continue;
                }
                const QString id = text(session.SourceAppUserModelId());
                const bool playing = status == PlaybackStatus::Playing;
                const bool isCurrent = session == current;
                const auto existing = sessions.find(id);
                if (existing == sessions.end() || (playing && !existing->second.playing)
                    || (playing == existing->second.playing && isCurrent && !existing->second.current)) {
                    sessions.insert_or_assign(id, SessionCandidate{session, playing, isCurrent});
                }
            } catch (const winrt::hresult_error&) {
                qDebug() << "Media session disappeared during enumeration";
            }
        }
        std::erase_if(records, [&](const auto& entry) {
            return rebuild || !sessions.contains(entry.first);
        });
        QList<MediaSelection::Source> choices;
        MediaSources newSources;
        for (const auto& [id, candidate] : sessions) {
            try {
                const auto& session = candidate.session;
                const auto existing = records.find(id);
                if (existing == records.end() || existing->second.session != session) {
                    addSession(id, session);
                }
                choices.append({id, candidate.playing});
                newSources.append({id, sourceName(id)});
            } catch (const winrt::hresult_error&) {
                records.erase(id);
            }
        }
        std::sort(newSources.begin(), newSources.end(), [](const auto& a, const auto& b) {
            return a.second < b.second;
        });
        if (sources != newSources) {
            sources = std::move(newSources);
            postSources();
        }
        const QString currentId = current ? text(current.SourceAppUserModelId()) : QString{};
        const QString next = MediaSelection::selectSource(choices, currentId, preferred, selected);
        if (next != selected) {
            selected = next;
            publish(MediaSnapshot{});
        }
        if (selected.isEmpty()) {
            publish(MediaSnapshot{});
            return;
        }
        auto& record = records.at(selected);
        if (record.metadataDirty || Clock::now() - record.metadataAt >= MetadataInterval) {
            const auto metadata = waitFor(record.session.TryGetMediaPropertiesAsync(), stop);
            if (!state->isCurrent(revision)) {
                return;
            }
            QByteArray cover = record.cover;
            if (!metadata) {
                cover.clear();
            } else if (record.metadataDirty || cover.isEmpty() || !sameTrack(metadata, record.metadata)) {
                cover = readCover(metadata.Thumbnail());
            }
            if (!state->isCurrent(revision)) {
                return;
            }
            record.metadata = metadata;
            record.cover = std::move(cover);
            record.metadataAt = Clock::now();
            record.metadataDirty = false;
        }
        if (!record.metadata) {
            publish(MediaSnapshot{});
            return;
        }
        const auto playback = record.session.GetPlaybackInfo();
        const auto timeline = record.session.GetTimelineProperties();
        const auto controls = playback.Controls();
        MediaSnapshot value;
        value.active = true;
        value.title = text(record.metadata.Title());
        value.artist = text(record.metadata.Artist());
        if (value.artist.isEmpty()) {
            value.artist = text(record.metadata.AlbumArtist());
        }
        value.album = text(record.metadata.AlbumTitle());
        value.source = sourceName(selected);
        value.sourceId = selected;
        value.playing = playback.PlaybackStatus() == PlaybackStatus::Playing;
        if (const auto rate = playback.PlaybackRate(); rate && std::isfinite(rate.Value())) {
            value.playbackRate = rate.Value();
        }
        const double start = std::chrono::duration<double>(timeline.StartTime()).count();
        value.duration = std::max(0.0, std::chrono::duration<double>(timeline.EndTime()).count() - start);
        value.position = std::chrono::duration<double>(timeline.Position()).count() - start;
        const auto updated = timeline.LastUpdatedTime();
        if (value.playing && updated.time_since_epoch().count() > 0) {
            const double elapsed = std::chrono::duration<double>(winrt::clock::now() - updated).count();
            value.position += std::max(0.0, elapsed) * value.playbackRate;
        }
        value.position = std::max(0.0, value.position);
        if (value.duration > 0.0) {
            value.position = std::min(value.position, value.duration);
        }
        value.canSeek = controls.IsPlaybackPositionEnabled() && value.duration > 0.0;
        value.canPrevious = controls.IsPreviousEnabled();
        value.canNext = controls.IsNextEnabled();
        value.canPlayPause = controls.IsPlayPauseToggleEnabled()
            || (value.playing ? controls.IsPauseEnabled() : controls.IsPlayEnabled());
        value.cover = record.cover;
        value.updatedAt = QDateTime::currentMSecsSinceEpoch();
        if (state->isCurrent(revision)) {
            publish(std::move(value));
        }
    }

    static bool sameTrack(const Metadata& a, const Metadata& b) {
        return a && b && a.Title() == b.Title() && a.Artist() == b.Artist()
            && a.AlbumTitle() == b.AlbumTitle() && a.TrackNumber() == b.TrackNumber();
    }

    QByteArray readCover(const IRandomAccessStreamReference& thumbnail) {
        if (!thumbnail) {
            return {};
        }
        try {
            const auto stream = waitFor(thumbnail.OpenReadAsync(), stop);
            const auto closeStream = qScopeGuard([&] {
                try {
                    stream.Close();
                } catch (const winrt::hresult_error&) {
                    qDebug() << "Media artwork stream already closed";
                }
            });
            const auto size = stream.Size();
            if (size == 0 || size > MaxCoverBytes) {
                return {};
            }
            const DataReader reader(stream.GetInputStreamAt(0));
            const auto closeReader = qScopeGuard([&] {
                try {
                    reader.Close();
                } catch (const winrt::hresult_error&) {
                    qDebug() << "Media artwork reader already closed";
                }
            });
            const auto loaded = waitFor(reader.LoadAsync(static_cast<uint32_t>(size)), stop);
            if (loaded != size) {
                return {};
            }
            QByteArray bytes(static_cast<qsizetype>(size), Qt::Uninitialized);
            const auto begin = reinterpret_cast<uint8_t*>(bytes.data());
            reader.ReadBytes(winrt::array_view<uint8_t>(begin, begin + size));
            return bytes;
        } catch (const winrt::hresult_error& exception) {
            qDebug() << "Media artwork unavailable:" << text(exception.message());
            return {};
        }
    }

    void execute(const Command& command, const QString& preferred) {
        const auto it = records.find(command.sourceId);
        if (it == records.end() || (!preferred.isEmpty() && command.sourceId != preferred)) {
            return;
        }
        const auto& session = it->second.session;
        const auto playback = session.GetPlaybackInfo();
        const auto controls = playback.Controls();
        winrt::Windows::Foundation::IAsyncOperation<bool> operation{nullptr};
        if (command.action == QStringLiteral("previous") && controls.IsPreviousEnabled()) {
            operation = session.TrySkipPreviousAsync();
        } else if (command.action == QStringLiteral("next") && controls.IsNextEnabled()) {
            operation = session.TrySkipNextAsync();
        } else if (command.action == QStringLiteral("play_pause")) {
            if (controls.IsPlayPauseToggleEnabled()) {
                operation = session.TryTogglePlayPauseAsync();
            } else if (playback.PlaybackStatus() == PlaybackStatus::Playing && controls.IsPauseEnabled()) {
                operation = session.TryPauseAsync();
            } else if (playback.PlaybackStatus() != PlaybackStatus::Playing && controls.IsPlayEnabled()) {
                operation = session.TryPlayAsync();
            }
        } else if (command.action == QStringLiteral("seek") && controls.IsPlaybackPositionEnabled()) {
            const auto timeline = session.GetTimelineProperties();
            const double start = std::chrono::duration<double>(timeline.StartTime()).count();
            const double end = std::chrono::duration<double>(timeline.EndTime()).count();
            if (end <= start) {
                return;
            }
            const double minimum = std::max(start, std::chrono::duration<double>(timeline.MinSeekTime()).count());
            double maximum = std::chrono::duration<double>(timeline.MaxSeekTime()).count();
            maximum = maximum > minimum ? std::min(end, maximum) : end;
            const double target = std::min(maximum, std::max(minimum, start + command.value));
            operation = session.TryChangePlaybackPositionAsync(static_cast<int64_t>(std::llround(target * TicksPerSecond)));
        }
        if (operation && !waitFor(operation, stop)) {
            reportError(QStringLiteral("Медиаприложение отклонило команду воспроизведения."));
        }
    }

    void publish(MediaSnapshot value) {
        if (!value.active && !snapshot.active) {
            return;
        }
        snapshot = std::move(value);
        uint64_t revision;
        {
            std::lock_guard lock(state->mutex);
            state->selected = snapshot.sourceId;
            revision = state->revision;
        }
        QMetaObject::invokeMethod(state->owner, [shared = state, value = snapshot, revision] {
            if (shared->isCurrent(revision)) {
                emit shared->owner->snapshotChanged(value);
            }
        }, Qt::QueuedConnection);
    }

    void postSources() {
        QMetaObject::invokeMethod(state->owner, [shared = state, value = sources] {
            if (!shared->stopping.load()) {
                emit shared->owner->sourcesChanged(value);
            }
        }, Qt::QueuedConnection);
    }

    void reportError(const QString& message) {
        if (message == lastError || stop.stop_requested()) {
            return;
        }
        lastError = message;
        qWarning().noquote() << message;
        QMetaObject::invokeMethod(state->owner, [shared = state, message] {
            if (!shared->stopping.load()) {
                emit shared->owner->error(message);
            }
        }, Qt::QueuedConnection);
    }
};
}

double MediaSnapshot::estimatedPosition() const {
    if (!active) {
        return 0.0;
    }
    double result = std::isfinite(position) ? position : 0.0;
    if (playing && updatedAt > 0) {
        const auto elapsed = std::max(qint64{0}, QDateTime::currentMSecsSinceEpoch() - updatedAt);
        result += static_cast<double>(elapsed) / 1000.0 * (std::isfinite(playbackRate) ? playbackRate : 1.0);
    }
    result = std::max(0.0, result);
    return duration > 0.0 ? std::min(result, duration) : result;
}

QString MediaSelection::selectSource(const QList<Source>& sources, const QString& current,
                                     const QString& preferred, const QString& previous) {
    const auto find = [&](const QString& id) {
        return std::find_if(sources.cbegin(), sources.cend(), [&](const auto& item) { return item.id == id; });
    };
    if (!preferred.isEmpty()) {
        return find(preferred) != sources.cend() ? preferred : QString{};
    }
    const auto active = find(current);
    const auto last = find(previous);
    if (active != sources.cend() && active->playing) {
        return current;
    }
    if (last != sources.cend() && last->playing) {
        return previous;
    }
    for (const auto& source : sources) {
        if (source.playing) {
            return source.id;
        }
    }
    if (active != sources.cend()) {
        return current;
    }
    if (last != sources.cend()) {
        return previous;
    }
    return sources.isEmpty() ? QString{} : sources.first().id;
}

struct MediaBridge::Impl {
    QString preferred;
    std::shared_ptr<WorkerState> state;
    std::jthread worker;
};

MediaBridge::MediaBridge(QObject* parent) : QObject(parent), d(std::make_unique<Impl>()) {
    qRegisterMetaType<MediaSnapshot>();
    qRegisterMetaType<MediaSources>();
}

MediaBridge::~MediaBridge() {
    stop();
}

void MediaBridge::start() {
    if (d->worker.joinable()) {
        return;
    }
    d->state = std::make_shared<WorkerState>(this);
    d->state->preferred = d->preferred;
    d->worker = std::jthread([state = d->state](std::stop_token stop) {
        try {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            const auto uninitialize = qScopeGuard([] { winrt::uninit_apartment(); });
            WindowsMediaBackend backend(state, stop);
            backend.run();
        } catch (const Cancelled&) {
        } catch (const winrt::hresult_error& exception) {
            const QString message = QStringLiteral("Медиаисточники недоступны: %1").arg(text(exception.message()));
            QMetaObject::invokeMethod(state->owner, [state, message] {
                if (!state->stopping.load()) {
                    emit state->owner->error(message);
                }
            }, Qt::QueuedConnection);
        } catch (const std::exception& exception) {
            qCritical() << "Media worker failed:" << exception.what();
        }
    });
}

void MediaBridge::stop() {
    if (!d->worker.joinable()) {
        return;
    }
    d->state->stopping.store(true);
    d->worker.request_stop();
    d->state->changed.notify_all();
    d->worker.join();
    d->state.reset();
}

void MediaBridge::setSource(const QString& sourceId) {
    if (d->preferred == sourceId) {
        return;
    }
    d->preferred = sourceId;
    if (!d->state) {
        return;
    }
    {
        std::lock_guard lock(d->state->mutex);
        d->state->preferred = sourceId;
        ++d->state->revision;
        d->state->wake = true;
    }
    d->state->changed.notify_one();
}

void MediaBridge::execute(const QString& action, double value) {
    if (action != QStringLiteral("previous") && action != QStringLiteral("play_pause")
        && action != QStringLiteral("next") && action != QStringLiteral("seek")) {
        emit error(QStringLiteral("Неизвестная команда воспроизведения: %1").arg(action));
        return;
    }
    if (!std::isfinite(value) || !d->state) {
        return;
    }
    {
        std::lock_guard lock(d->state->mutex);
        if (d->state->selected.isEmpty()) {
            return;
        }
        auto& commands = d->state->commands;
        if (action == QStringLiteral("seek") && !commands.empty()
            && commands.back().action == action && commands.back().sourceId == d->state->selected) {
            commands.back().value = value;
        } else {
            if (commands.size() >= MaxPendingCommands) {
                return;
            }
            commands.push_back({action, value, d->state->selected});
        }
        d->state->wake = true;
    }
    d->state->changed.notify_one();
}

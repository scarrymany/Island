#include "SessionVolume.h"

#include <QDir>
#include <QMetaObject>
#include <QRegularExpression>
#include <QScopeGuard>

#include <Windows.h>
#include <appmodel.h>
#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>
#include <wrl/implements.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <utility>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace std::chrono_literals;

namespace {
constexpr auto PollInterval = 2500ms;
constexpr DWORD MaxProcessPath = 32768;
constexpr UINT32 MaxIdentityLength = 512;

QString normalizedPath(QString path) {
    return QDir::cleanPath(path.replace(u'\\', u'/')).toCaseFolded();
}

QString executableName(const QString& path) {
    return normalizedPath(path).section(u'/', -1);
}

QString identityString(HANDLE process, LONG(WINAPI* query)(HANDLE, UINT32*, PWSTR)) {
    UINT32 size = 0;
    if (query(process, &size, nullptr) != ERROR_INSUFFICIENT_BUFFER || size == 0 || size > MaxIdentityLength) return {};
    std::vector<wchar_t> buffer(size);
    if (query(process, &size, buffer.data()) != ERROR_SUCCESS) return {};
    return QString::fromWCharArray(buffer.data());
}

SessionVolumeMatching::ProcessIdentity processIdentity(DWORD processId) {
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!process) return {};
    const auto close = qScopeGuard([process] { CloseHandle(process); });
    SessionVolumeMatching::ProcessIdentity identity;
    std::vector<wchar_t> path(MaxProcessPath);
    DWORD size = static_cast<DWORD>(path.size());
    if (QueryFullProcessImageNameW(process, 0, path.data(), &size))
        identity.executablePath = QString::fromWCharArray(path.data(), size);
    identity.appUserModelId = identityString(process, GetApplicationUserModelId);
    identity.packageFamily = identityString(process, GetPackageFamilyName);
    return identity;
}

QString audioError(const QString& operation, HRESULT result) {
    return QStringLiteral("%1 (Windows: 0x%2)").arg(operation).arg(static_cast<quint32>(result), 8, 16, QLatin1Char('0'));
}

struct WorkerState {
    explicit WorkerState(SessionVolume* target) : owner(target) {}
    SessionVolume* owner;
    std::atomic<bool> stopping{false};
    std::mutex mutex;
    std::condition_variable_any changed;
    QString sourceId;
    quint64 revision = 0;
    bool wake = true;
    std::optional<double> requestedVolume;
    QString lastError;
    QString lastErrorSource;

    bool current(quint64 expected) {
        std::lock_guard lock(mutex);
        return !stopping.load() && revision == expected;
    }
};

void publish(const std::shared_ptr<WorkerState>& state, quint64 revision,
             const SessionVolumeState& value, const QString& error = {}) {
    QMetaObject::invokeMethod(state->owner, [state, revision, value, error] {
        if (!state->current(revision)) return;
        emit state->owner->changed(value);
        if (!state->current(revision)) return;
        const bool repeated = error == state->lastError && value.sourceId == state->lastErrorSource;
        state->lastError = error;
        state->lastErrorSource = value.sourceId;
        if (!error.isEmpty() && !repeated) emit state->owner->error(error);
    }, Qt::QueuedConnection);
}

class SessionCollector final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IAudioSessionNotification> {
public:
    HRESULT STDMETHODCALLTYPE OnSessionCreated(IAudioSessionControl* session) noexcept override {
        if (!session) return E_POINTER;
        try {
            std::lock_guard lock(mutex_);
            sessions_.emplace_back(session);
            return S_OK;
        } catch (...) {
            return E_OUTOFMEMORY;
        }
    }

    std::vector<ComPtr<IAudioSessionControl>> take() {
        std::lock_guard lock(mutex_);
        return std::exchange(sessions_, {});
    }

private:
    std::mutex mutex_;
    std::vector<ComPtr<IAudioSessionControl>> sessions_;
};

struct Session {
    DWORD processId = 0;
    ComPtr<IAudioSessionControl2> control;
    ComPtr<ISimpleAudioVolume> volume;
};

struct Scan {
    std::vector<Session> sessions;
    HRESULT failure = S_OK;
};

Scan findSessions(const QString& sourceId, std::stop_token stop) {
    Scan result;
    if (sourceId.isEmpty() || stop.stop_requested()) return result;
    ComPtr<IMMDeviceEnumerator> devices;
    result.failure = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&devices));
    if (FAILED(result.failure)) return result;
    ComPtr<IMMDeviceCollection> endpoints;
    result.failure = devices->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &endpoints);
    if (FAILED(result.failure)) return result;
    UINT deviceCount = 0;
    result.failure = endpoints->GetCount(&deviceCount);
    if (FAILED(result.failure)) return result;
    std::map<DWORD, SessionVolumeMatching::ProcessIdentity> identities;
    std::set<QString> seen;
    for (UINT deviceIndex = 0; deviceIndex < deviceCount && !stop.stop_requested(); ++deviceIndex) {
        ComPtr<IMMDevice> endpoint;
        HRESULT status = endpoints->Item(deviceIndex, &endpoint);
        if (FAILED(status)) { result.failure = status; continue; }
        ComPtr<IAudioSessionManager2> manager;
        status = endpoint->Activate(__uuidof(IAudioSessionManager2), CLSCTX_INPROC_SERVER, nullptr,
            reinterpret_cast<void**>(manager.GetAddressOf()));
        if (FAILED(status)) { result.failure = status; continue; }
        auto collector = Microsoft::WRL::Make<SessionCollector>();
        if (!collector) { result.failure = E_OUTOFMEMORY; continue; }
        status = manager->RegisterSessionNotification(collector.Get());
        if (FAILED(status)) { result.failure = status; continue; }
        const auto unregister = qScopeGuard([&] { manager->UnregisterSessionNotification(collector.Get()); });
        ComPtr<IAudioSessionEnumerator> enumerator;
        status = manager->GetSessionEnumerator(&enumerator);
        if (FAILED(status)) { result.failure = status; continue; }
        int count = 0;
        status = enumerator->GetCount(&count);
        if (FAILED(status)) { result.failure = status; continue; }
        std::vector<ComPtr<IAudioSessionControl>> controls;
        for (int index = 0; index < count && !stop.stop_requested(); ++index) {
            ComPtr<IAudioSessionControl> control;
            if (SUCCEEDED(enumerator->GetSession(index, &control))) controls.push_back(std::move(control));
        }
        auto created = collector->take();
        controls.insert(controls.end(), created.begin(), created.end());
        for (const auto& control : controls) {
            if (stop.stop_requested()) break;
            Session session;
            if (FAILED(control.As(&session.control)) || session.control->IsSystemSoundsSession() != S_FALSE) continue;
            AudioSessionState audioState{};
            if (FAILED(control->GetState(&audioState)) || audioState == AudioSessionStateExpired) continue;
            // A shared session's creator PID cannot prove that all its audio belongs to this application.
            if (session.control->GetProcessId(&session.processId) != S_OK || session.processId == 0) continue;
            auto identity = identities.find(session.processId);
            if (identity == identities.end()) identity = identities.emplace(session.processId, processIdentity(session.processId)).first;
            if (!SessionVolumeMatching::matches(sourceId, identity->second)) continue;
            LPWSTR instance = nullptr;
            status = session.control->GetSessionInstanceIdentifier(&instance);
            const auto releaseString = qScopeGuard([&] { CoTaskMemFree(instance); });
            if (FAILED(status) || !instance) continue;
            const QString instanceId = QString::number(deviceIndex) + u':' + QString::fromWCharArray(instance);
            if (!seen.insert(instanceId).second || FAILED(control.As(&session.volume))) continue;
            result.sessions.push_back(std::move(session));
        }
    }
    return result;
}

SessionVolumeState readSessions(const QString& sourceId, const std::vector<Session>& sessions) {
    QList<SessionVolumeMatching::Reading> readings;
    for (const auto& session : sessions) {
        float volume = 0;
        BOOL muted = FALSE;
        AudioSessionState state{};
        if (FAILED(session.control->GetState(&state)) || state == AudioSessionStateExpired
            || FAILED(session.volume->GetMasterVolume(&volume)) || FAILED(session.volume->GetMute(&muted))) continue;
        readings.append({volume, muted != FALSE, state == AudioSessionStateActive});
    }
    return SessionVolumeMatching::summarize(sourceId, readings);
}

void run(const std::shared_ptr<WorkerState>& state, std::stop_token stop) {
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(initialized)) {
        quint64 revision;
        QString source;
        { std::lock_guard lock(state->mutex); revision = state->revision; source = state->sourceId; }
        publish(state, revision, SessionVolumeState{source}, audioError(QStringLiteral("Громкость приложения недоступна"), initialized));
        return;
    }
    const auto uninitialize = qScopeGuard([] { CoUninitialize(); });
    while (!stop.stop_requested()) {
        QString source;
        quint64 revision;
        std::optional<double> requested;
        {
            std::unique_lock lock(state->mutex);
            state->changed.wait_for(lock, stop, PollInterval, [&] { return state->wake; });
            if (stop.stop_requested()) break;
            source = state->sourceId;
            revision = state->revision;
            requested = std::exchange(state->requestedVolume, std::nullopt);
            state->wake = false;
        }
        const auto scan = findSessions(source, stop);
        if (!state->current(revision)) continue;
        QString failure;
        bool attemptedWrite = false;
        for (const auto& session : scan.sessions) {
            if (!requested || !state->current(revision)) break;
            DWORD currentProcess = 0;
            if (session.control->GetProcessId(&currentProcess) != S_OK || currentProcess != session.processId
                || !SessionVolumeMatching::matches(source, processIdentity(currentProcess))) continue;
            if (!state->current(revision)) break;
            attemptedWrite = true;
            HRESULT status;
            if (*requested == 0) {
                status = session.volume->SetMute(TRUE, nullptr);
                if (SUCCEEDED(status) && state->current(revision)) status = session.volume->SetMasterVolume(0, nullptr);
            } else {
                status = session.volume->SetMasterVolume(static_cast<float>(*requested), nullptr);
                if (SUCCEEDED(status) && state->current(revision)) status = session.volume->SetMute(FALSE, nullptr);
            }
            if (FAILED(status)) failure = audioError(QStringLiteral("Не удалось изменить громкость приложения"), status);
        }
        if (requested && !attemptedWrite)
            failure = QStringLiteral("Аудиосессия выбранного приложения недоступна");
        const auto value = readSessions(source, scan.sessions);
        if (!value.available && FAILED(scan.failure))
            failure = audioError(QStringLiteral("Аудиосессии выбранного приложения недоступны"), scan.failure);
        publish(state, revision, value, failure);
    }
}
}

bool SessionVolumeMatching::matches(const QString& sourceId, const ProcessIdentity& process) {
    if (sourceId.isEmpty() || sourceId != sourceId.trimmed()) return false;
    if (!process.appUserModelId.isEmpty() && sourceId == process.appUserModelId) return true;
    const QString executable = executableName(process.executablePath);
    if (sourceId.contains(u'!')) {
        const QString family = sourceId.section(u'!', 0, 0);
        return executable == "spotify.exe" && family.startsWith("SpotifyAB.SpotifyMusic_", Qt::CaseInsensitive)
            && sourceId.count(u'!') == 1 && !sourceId.section(u'!', 1).isEmpty() && family == process.packageFamily;
    }
    if (process.executablePath.isEmpty()) return false;
    if (sourceId.contains(u'/') || sourceId.contains(u'\\')) return normalizedPath(sourceId) == normalizedPath(process.executablePath);
    static const QRegularExpression executableId(QStringLiteral("^[A-Za-z0-9_.-]+\\.exe$"), QRegularExpression::CaseInsensitiveOption);
    if (executableId.match(sourceId).hasMatch()) return sourceId.compare(executable, Qt::CaseInsensitive) == 0;
    const QString source = sourceId.toCaseFolded();
    if (source == "spotify") return executable == "spotify.exe";
    // Chromium's desktop AUMIDs retain the product prefix for browser profiles and installed web apps.
    static const QRegularExpression browserId(QStringLiteral("^(chrome|msedge)(?:\\.[A-Za-z0-9_-]+)*$"));
    const auto browser = browserId.match(source);
    if (browser.hasMatch()) return executable == browser.captured(1) + ".exe";
    return false;
}

SessionVolumeState SessionVolumeMatching::summarize(const QString& sourceId, const QList<Reading>& readings) {
    SessionVolumeState state{sourceId};
    if (sourceId.isEmpty()) return state;
    const auto valid = [](const Reading& value) { return std::isfinite(value.volume) && value.volume >= 0 && value.volume <= 1; };
    const bool hasActive = std::any_of(readings.cbegin(), readings.cend(), [&](const Reading& value) { return valid(value) && value.active; });
    state.muted = true;
    for (const auto& reading : readings) {
        if (!valid(reading)) continue;
        ++state.sessionCount;
        if (hasActive && !reading.active) continue;
        state.available = true;
        state.muted = state.muted && reading.muted;
        if (!reading.muted) state.volume = std::max(state.volume, reading.volume);
    }
    if (!state.available) state.muted = false;
    return state;
}

struct SessionVolume::Impl {
    QString sourceId;
    SessionVolumeState last;
    std::shared_ptr<WorkerState> state;
    std::jthread worker;
};

SessionVolume::SessionVolume(QObject* parent) : QObject(parent), d(std::make_unique<Impl>()) {
    qRegisterMetaType<SessionVolumeState>();
    connect(this, &SessionVolume::changed, this, [this](const SessionVolumeState& state) { d->last = state; });
}

SessionVolume::~SessionVolume() { stop(); }

void SessionVolume::start() {
    if (d->worker.joinable()) return;
    d->state = std::make_shared<WorkerState>(this);
    d->state->sourceId = d->sourceId;
    d->worker = std::jthread([state = d->state](std::stop_token stop) {
        try { run(state, stop); }
        catch (const std::exception&) {
            quint64 revision;
            QString source;
            { std::lock_guard lock(state->mutex); revision = state->revision; source = state->sourceId; }
            publish(state, revision, SessionVolumeState{source}, QStringLiteral("Не удалось обработать аудиосессии приложения"));
        }
    });
}

void SessionVolume::stop() {
    if (!d->worker.joinable()) return;
    d->state->stopping.store(true);
    d->worker.request_stop();
    d->state->changed.notify_all();
    d->worker.join();
    d->state.reset();
    d->last = SessionVolumeState{d->sourceId};
}

void SessionVolume::setSource(const QString& sourceId) {
    if (d->sourceId == sourceId) return;
    d->sourceId = sourceId;
    if (d->state) {
        { std::lock_guard lock(d->state->mutex);
          d->state->sourceId = sourceId; ++d->state->revision;
          d->state->requestedVolume.reset(); d->state->wake = true; }
        d->state->changed.notify_one();
    }
    emit changed(SessionVolumeState{sourceId});
}

void SessionVolume::refresh() {
    if (!d->state) return;
    { std::lock_guard lock(d->state->mutex); d->state->wake = true; }
    d->state->changed.notify_one();
}

void SessionVolume::setVolume(double value) {
    if (!std::isfinite(value) || value < 0 || value > 1) {
        emit error(QStringLiteral("Громкость приложения должна быть от 0 до 1"));
        return;
    }
    if (!d->state || d->sourceId.isEmpty() || !d->last.available || d->last.sourceId != d->sourceId) {
        emit error(QStringLiteral("Аудиосессия выбранного приложения недоступна"));
        return;
    }
    { std::lock_guard lock(d->state->mutex);
      d->state->requestedVolume = value; ++d->state->revision; d->state->wake = true; }
    d->state->changed.notify_one();
}

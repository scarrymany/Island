#include "SessionVolume.h"

#include <QElapsedTimer>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QtTest>

#include <Windows.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <cmath>
#include <limits>

using SessionVolumeMatching::ProcessIdentity;
using SessionVolumeMatching::Reading;

class SessionVolumeTest final : public QObject {
    Q_OBJECT

private slots:
    void matchesOnlySelectedExecutable() {
        const ProcessIdentity spotify{R"(C:\Users\Music\Spotify\Spotify.exe)", {}, {}};
        const ProcessIdentity chrome{R"(C:\Program Files\Google\Chrome\Application\chrome.exe)", {}, {}};
        QVERIFY(SessionVolumeMatching::matches("Spotify.exe", spotify));
        QVERIFY(SessionVolumeMatching::matches("spotify.EXE", spotify));
        QVERIFY(SessionVolumeMatching::matches("Chrome", chrome));
        QVERIFY(SessionVolumeMatching::matches("Chrome.Profile_1", chrome));
        QVERIFY(!SessionVolumeMatching::matches("Chrome", spotify));
        QVERIFY(!SessionVolumeMatching::matches("Spotify.exe", chrome));
        QVERIFY(!SessionVolumeMatching::matches("SomeChromePlayer", chrome));
        QVERIFY(!SessionVolumeMatching::matches("ChromeBackup", chrome));
        QVERIFY(!SessionVolumeMatching::matches("YouTube", chrome));
        QVERIFY(!SessionVolumeMatching::matches("SoundCloud", chrome));
        QVERIFY(!SessionVolumeMatching::matches({}, spotify));
        QVERIFY(!SessionVolumeMatching::matches("Spotify.exe", {}));
        QVERIFY(!SessionVolumeMatching::matches("chrome.exe", {R"(C:\Apps\chrome_helper.exe)", {}, {}}));
    }

    void fullPathsDoNotFallBackToBasename() {
        const ProcessIdentity spotify{R"(C:\Music\Spotify.exe)", {}, {}};
        QVERIFY(SessionVolumeMatching::matches("c:/music/SPOTIFY.EXE", spotify));
        QVERIFY(!SessionVolumeMatching::matches(R"(D:\Another\Spotify.exe)", spotify));
        QVERIFY(!SessionVolumeMatching::matches("https://example.test/Spotify.exe", spotify));
        QVERIFY(!SessionVolumeMatching::matches("Spotify.exe --argument", spotify));
    }

    void exactAppIdentitySupportsUnknownPackagedPlayers() {
        const ProcessIdentity player{R"(C:\WindowsApps\Music\Player.exe)", "Publisher.Player_123!Main", "Publisher.Player_123"};
        QVERIFY(SessionVolumeMatching::matches("Publisher.Player_123!Main", player));
        QVERIFY(!SessionVolumeMatching::matches("Publisher.Player_123!Other", player));
        QVERIFY(!SessionVolumeMatching::matches("Publisher.Player_123", player));
    }

    void spotifyPackageSupportsChildProcessWithoutAppIdentity() {
        const ProcessIdentity spotify{R"(C:\WindowsApps\SpotifyPackage\Spotify.exe)", {}, "SpotifyAB.SpotifyMusic_123"};
        QVERIFY(SessionVolumeMatching::matches("SpotifyAB.SpotifyMusic_123!Spotify", spotify));
        QVERIFY(!SessionVolumeMatching::matches("SpotifyAB.SpotifyMusic_other!Spotify", spotify));
        QVERIFY(!SessionVolumeMatching::matches("SpotifyAB.SpotifyMusic_123!Spotify", {R"(C:\Apps\Other.exe)", {}, spotify.packageFamily}));
    }

    void aggregatePrefersActiveSessionsAndHonorsMute() {
        auto state = SessionVolumeMatching::summarize("Chrome", {{0.3, false, true}, {0.8, true, true}, {1.0, false, false}});
        QVERIFY(state.available);
        QCOMPARE(state.sourceId, QString("Chrome"));
        QCOMPARE(state.sessionCount, 3);
        QCOMPARE(state.volume, 0.3);
        QVERIFY(!state.muted);
        state = SessionVolumeMatching::summarize("Chrome", {{0.4, true, true}, {0.8, true, true}});
        QCOMPARE(state.volume, 0.0);
        QVERIFY(state.muted);
        state = SessionVolumeMatching::summarize("Spotify.exe", {{0.25, false, false}, {0.7, false, false}});
        QCOMPARE(state.volume, 0.7);
    }

    void invalidReadingsNeverMakeVolumeAvailable() {
        const auto empty = SessionVolumeMatching::summarize("Unknown", {});
        QVERIFY(!empty.available);
        QCOMPARE(empty.sessionCount, 0);
        QVERIFY(!SessionVolumeMatching::summarize({}, {{0.5, false, true}}).available);
        const auto invalid = SessionVolumeMatching::summarize("Chrome", {
            {-0.1, false, true}, {1.1, false, true}, {std::numeric_limits<double>::quiet_NaN(), false, true}});
        QVERIFY(!invalid.available);
        QCOMPARE(invalid.sessionCount, 0);
    }

    void sourceSwitchInvalidatesStateImmediately() {
        SessionVolume volume;
        QSignalSpy changes(&volume, &SessionVolume::changed);
        volume.setSource("Chrome");
        volume.setSource("Spotify.exe");
        QCOMPARE(changes.size(), 2);
        const auto state = qvariant_cast<SessionVolumeState>(changes.last().first());
        QCOMPARE(state.sourceId, QString("Spotify.exe"));
        QVERIFY(!state.available);
        volume.setSource("Spotify.exe");
        QCOMPARE(changes.size(), 2);
    }

    void unreadyAndInvalidCommandsFailWithoutWriting() {
        SessionVolume volume;
        QSignalSpy errors(&volume, &SessionVolume::error);
        for (double invalid : {-0.1, 1.1, std::numeric_limits<double>::quiet_NaN()}) volume.setVolume(invalid);
        QCOMPARE(errors.size(), 3);
        volume.setVolume(0.5);
        QCOMPARE(errors.size(), 4);
        volume.setSource("Unknown Player");
        volume.setVolume(0.0);
        QCOMPARE(errors.size(), 5);
    }

    void stoppedWorkerCannotPublishObsoleteState() {
        SessionVolume volume;
        QSignalSpy changes(&volume, &SessionVolume::changed);
        volume.start();
        volume.refresh();
        QElapsedTimer elapsed;
        elapsed.start();
        volume.stop();
        QVERIFY(elapsed.elapsed() < 1000);
        const auto count = changes.size();
        QTest::qWait(30);
        QCOMPARE(changes.size(), count);
        volume.start();
        volume.stop();
    }

    void passiveDesktopRead() {
        const QString source = qEnvironmentVariable("ISLAND_SESSION_VOLUME_DIAGNOSTIC_SOURCE");
        if (source.isEmpty()) QSKIP("Set ISLAND_SESSION_VOLUME_DIAGNOSTIC_SOURCE to explicitly enable a read-only desktop probe");
        SessionVolume volume;
        QSignalSpy changes(&volume, &SessionVolume::changed);
        QSignalSpy errors(&volume, &SessionVolume::error);
        volume.setSource(source);
        volume.start();
        QTRY_VERIFY_WITH_TIMEOUT(changes.size() >= 2 || !errors.isEmpty(), 6000);
        QVERIFY2(errors.isEmpty(), errors.isEmpty() ? "" : qPrintable(errors.first().first().toString()));
        const auto state = qvariant_cast<SessionVolumeState>(changes.last().first());
        qInfo() << "Passive session volume:" << state.sourceId << "available=" << state.available
                << "level=" << state.volume << "muted=" << state.muted << "sessions=" << state.sessionCount;
        QVERIFY(state.available);
        QVERIFY(state.sessionCount > 0);
        QVERIFY(state.volume >= 0 && state.volume <= 1);
        volume.stop();
    }

    void controlledDesktopSessionNeverChangesDeviceVolume() {
        if (qEnvironmentVariableIntValue("ISLAND_AUDIO_DESKTOP_TEST") != 1)
            QSKIP("Set ISLAND_AUDIO_DESKTOP_TEST=1 to enable the silent, owned WASAPI session test");
        using Microsoft::WRL::ComPtr;
        const auto windowsError = [](HRESULT result) {
            return QStringLiteral("Windows HRESULT 0x%1").arg(static_cast<quint32>(result), 8, 16, QLatin1Char('0'));
        };
        const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        QVERIFY(SUCCEEDED(initialized));
        const auto uninitialize = qScopeGuard([] { CoUninitialize(); });
        ComPtr<IMMDeviceEnumerator> devices;
        QVERIFY(SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&devices))));
        ComPtr<IMMDevice> endpoint;
        HRESULT result = devices->GetDefaultAudioEndpoint(eRender, eMultimedia, &endpoint);
        if (FAILED(result)) QSKIP(qPrintable(windowsError(result)));
        ComPtr<IAudioEndpointVolume> deviceVolume;
        QVERIFY(SUCCEEDED(endpoint->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_INPROC_SERVER, nullptr,
            reinterpret_cast<void**>(deviceVolume.GetAddressOf()))));
        float masterBefore = 0;
        BOOL muteBefore = FALSE;
        QVERIFY(SUCCEEDED(deviceVolume->GetMasterVolumeLevelScalar(&masterBefore)));
        QVERIFY(SUCCEEDED(deviceVolume->GetMute(&muteBefore)));
        const auto unchangedDevice = [&] {
            float scalar = -1;
            BOOL muted = FALSE;
            return SUCCEEDED(deviceVolume->GetMasterVolumeLevelScalar(&scalar)) && SUCCEEDED(deviceVolume->GetMute(&muted))
                && std::abs(scalar - masterBefore) < 0.00001f && muted == muteBefore;
        };
        ComPtr<IAudioClient> client;
        result = endpoint->Activate(__uuidof(IAudioClient), CLSCTX_INPROC_SERVER, nullptr,
            reinterpret_cast<void**>(client.GetAddressOf()));
        if (FAILED(result)) QSKIP(qPrintable(windowsError(result)));
        WAVEFORMATEX* format = nullptr;
        result = client->GetMixFormat(&format);
        const auto releaseFormat = qScopeGuard([&] { CoTaskMemFree(format); });
        if (FAILED(result)) QSKIP(qPrintable(windowsError(result)));
        GUID sessionId{};
        QVERIFY(SUCCEEDED(CoCreateGuid(&sessionId)));
        // Never start rendering. A unique, nonpersistent session keeps all writes confined to this test.
        result = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_NOPERSIST, 0, 0, format, &sessionId);
        if (FAILED(result)) QSKIP(qPrintable(windowsError(result)));
        ComPtr<IAudioSessionControl> control;
        QVERIFY(SUCCEEDED(client->GetService(IID_PPV_ARGS(&control))));
        ComPtr<IAudioSessionControl2> control2;
        QVERIFY(SUCCEEDED(control.As(&control2)));
        DWORD processId = 0;
        QCOMPARE(control2->GetProcessId(&processId), S_OK);
        QCOMPARE(processId, GetCurrentProcessId());
        ComPtr<ISimpleAudioVolume> ownVolume;
        QVERIFY(SUCCEEDED(client->GetService(IID_PPV_ARGS(&ownVolume))));
        QVERIFY(SUCCEEDED(ownVolume->SetMasterVolume(0.62f, nullptr)));
        QVERIFY(SUCCEEDED(ownVolume->SetMute(FALSE, nullptr)));
        const auto sessionMatches = [&](float expected, bool expectedMute) {
            float scalar = -1;
            BOOL muted = FALSE;
            return SUCCEEDED(ownVolume->GetMasterVolume(&scalar)) && SUCCEEDED(ownVolume->GetMute(&muted))
                && std::abs(scalar - expected) < 0.00001f && (muted != FALSE) == expectedMute;
        };

        SessionVolume volume;
        SessionVolumeState state;
        connect(&volume, &SessionVolume::changed, this, [&](SessionVolumeState value) { state = value; });
        QSignalSpy errors(&volume, &SessionVolume::error);
        volume.setSource(QCoreApplication::applicationFilePath());
        volume.start();
        QElapsedTimer discovery;
        discovery.start();
        while (!state.available && errors.isEmpty() && discovery.elapsed() < 6000) QTest::qWait(20);
        if (!state.available) QSKIP("The Windows audio engine did not expose the initialized inactive test session");
        QVERIFY(state.sessionCount > 0);
        QVERIFY(std::abs(state.volume - 0.62) < 0.00001);
        for (const double target : {0.37, 0.0, 0.58}) {
            volume.setVolume(target);
            QTRY_VERIFY_WITH_TIMEOUT(sessionMatches(static_cast<float>(target), target == 0), 3000);
            QTRY_VERIFY_WITH_TIMEOUT(std::abs(state.volume - target) < 0.00001 && state.muted == (target == 0), 3000);
            QVERIFY(unchangedDevice());
        }
        volume.setVolume(0.21);
        volume.setVolume(0.73);
        QTRY_VERIFY_WITH_TIMEOUT(sessionMatches(0.73f, false), 3000);
        QVERIFY(errors.isEmpty());
        volume.setSource("Unidentified media source");
        QVERIFY(!state.available);
        volume.setVolume(0);
        QCOMPARE(errors.size(), 1);
        QTest::qWait(100);
        QVERIFY(sessionMatches(0.73f, false));
        QVERIFY(unchangedDevice());
        volume.stop();
    }
};

QTEST_GUILESS_MAIN(SessionVolumeTest)
#include "test_session_volume.moc"

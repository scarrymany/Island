#include "WindowsIntegration.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dwmapi.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <QColor>
#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QKeySequence>
#include <QSet>

#include <algorithm>
#include <cmath>

using Microsoft::WRL::ComPtr;

namespace {
constexpr wchar_t StartupKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t StartupName[] = L"IslandMusicHUD";
constexpr int MaxStartupCommand = 260;
constexpr int FirstHotkeyId = 0x4900;
constexpr int LastHotkeyId = 0xBFFF;
constexpr DWORD SystemBackdropAttribute = 38;
constexpr DWORD DarkModeAttribute = 20;
constexpr int AcrylicBackdrop = 3;
constexpr int NoBackdrop = 1;
constexpr int AccentAttribute = 19;
constexpr int AccentDisabled = 0;
constexpr int AccentBlur = 3;
constexpr int AccentAcrylic = 4;

QSet<int> activeHotkeys;
int nextHotkeyId = FirstHotkeyId;

void setError(QString* target, const QString& message) {
    if (target) {
        *target = message;
    }
}

QString systemError(DWORD code) {
    wchar_t* message = nullptr;
    const DWORD length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM
                                           | FORMAT_MESSAGE_IGNORE_INSERTS,
                                       nullptr, code, 0, reinterpret_cast<wchar_t*>(&message), 0, nullptr);
    QString result = length ? QString::fromWCharArray(message, static_cast<int>(length)).trimmed()
                            : QStringLiteral("Windows: 0x%1").arg(code, 8, 16, QLatin1Char('0'));
    if (message) {
        LocalFree(message);
    }
    return result;
}

int allocateHotkeyId() {
    for (int attempt = FirstHotkeyId; attempt <= LastHotkeyId; ++attempt) {
        const int candidate = nextHotkeyId;
        nextHotkeyId = nextHotkeyId == LastHotkeyId ? FirstHotkeyId : nextHotkeyId + 1;
        if (!activeHotkeys.contains(candidate)) {
            return candidate;
        }
    }
    return -1;
}

ComPtr<IAudioEndpointVolume> defaultEndpoint(QString* error) {
    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT result = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(&enumerator));
    ComPtr<IMMDevice> device;
    if (SUCCEEDED(result)) {
        result = enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &device);
    }
    ComPtr<IAudioEndpointVolume> endpoint;
    if (SUCCEEDED(result)) {
        result = device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_INPROC_SERVER, nullptr,
                                  reinterpret_cast<void**>(endpoint.GetAddressOf()));
    }
    if (FAILED(result)) {
        setError(error, QStringLiteral("Аудиовыход Windows недоступен: %1").arg(systemError(result)));
    }
    return endpoint;
}

struct AccentPolicy {
    DWORD state;
    DWORD flags;
    DWORD color;
    DWORD animation;
};

struct CompositionAttribute {
    int attribute;
    void* data;
    UINT size;
};

bool setAccent(HWND hwnd, DWORD state, DWORD color) {
    using SetComposition = BOOL(WINAPI*)(HWND, const CompositionAttribute*);
    static const auto setComposition = reinterpret_cast<SetComposition>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetWindowCompositionAttribute"));
    if (!setComposition) {
        return false;
    }
    AccentPolicy policy{state, 0, color, 0};
    CompositionAttribute attribute{AccentAttribute, &policy, sizeof(policy)};
    return setComposition(hwnd, &attribute) != FALSE;
}
}

WindowsIntegration::WindowsIntegration(QObject* parent)
    : QObject(parent), nativeThreadId_(GetCurrentThreadId()) {
    comResult_ = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ownsCom_ = SUCCEEDED(comResult_);
    if (QCoreApplication::instance()) {
        QCoreApplication::instance()->installNativeEventFilter(this);
    }
}

WindowsIntegration::~WindowsIntegration() {
    if (QCoreApplication::instance()) {
        QCoreApplication::instance()->removeNativeEventFilter(this);
    }
    if (GetCurrentThreadId() == nativeThreadId_) {
        releaseHotkey();
        if (ownsCom_) {
            CoUninitialize();
        }
    } else {
        qWarning() << "WindowsIntegration destroyed outside its creating thread";
    }
}

bool WindowsIntegration::onOwnerThread(QString* error) const {
    if (GetCurrentThreadId() != nativeThreadId_) {
        setError(error, QStringLiteral("Операция Windows вызвана из другого потока"));
        return false;
    }
    return true;
}

std::optional<WindowsIntegration::Hotkey> WindowsIntegration::parseHotkey(const QString& sequence,
                                                                        QString* error) {
    const QKeySequence shortcut = QKeySequence::fromString(sequence.trimmed(), QKeySequence::PortableText);
    if (shortcut.count() != 1 || shortcut.isEmpty()) {
        setError(error, QStringLiteral("Укажите одно сочетание, например Ctrl+Alt+M"));
        return std::nullopt;
    }
    const auto combination = shortcut[0];
    const auto modifiers = combination.keyboardModifiers();
    UINT nativeModifiers = 0;
    if (modifiers.testFlag(Qt::ControlModifier)) nativeModifiers |= MOD_CONTROL;
    if (modifiers.testFlag(Qt::AltModifier)) nativeModifiers |= MOD_ALT;
    if (modifiers.testFlag(Qt::ShiftModifier)) nativeModifiers |= MOD_SHIFT;
    if (modifiers.testFlag(Qt::MetaModifier)) nativeModifiers |= MOD_WIN;
    if (modifiers.testFlag(Qt::KeypadModifier) || modifiers.testFlag(Qt::GroupSwitchModifier)) {
        setError(error, QStringLiteral("Используйте обычную клавишу с Ctrl, Alt или Shift"));
        return std::nullopt;
    }
    const int key = combination.key();
    if (key == Qt::Key_F12) {
        setError(error, QStringLiteral("F12 зарезервирована Windows для отладчика"));
        return std::nullopt;
    }
    if (key >= Qt::Key_F1 && key <= Qt::Key_F24) {
        return Hotkey{nativeModifiers, static_cast<UINT>(VK_F1 + key - Qt::Key_F1)};
    }
    if (!nativeModifiers) {
        setError(error, QStringLiteral("Добавьте Ctrl, Alt, Shift или Win к горячей клавише"));
        return std::nullopt;
    }
    if ((key >= Qt::Key_A && key <= Qt::Key_Z) || (key >= Qt::Key_0 && key <= Qt::Key_9)) {
        return Hotkey{nativeModifiers, static_cast<UINT>(key)};
    }
    UINT virtualKey = 0;
    switch (key) {
    case Qt::Key_Space: virtualKey = VK_SPACE; break;
    case Qt::Key_Tab: virtualKey = VK_TAB; break;
    case Qt::Key_Backspace: virtualKey = VK_BACK; break;
    case Qt::Key_Return:
    case Qt::Key_Enter: virtualKey = VK_RETURN; break;
    case Qt::Key_Escape: virtualKey = VK_ESCAPE; break;
    case Qt::Key_Left: virtualKey = VK_LEFT; break;
    case Qt::Key_Right: virtualKey = VK_RIGHT; break;
    case Qt::Key_Up: virtualKey = VK_UP; break;
    case Qt::Key_Down: virtualKey = VK_DOWN; break;
    case Qt::Key_Home: virtualKey = VK_HOME; break;
    case Qt::Key_End: virtualKey = VK_END; break;
    case Qt::Key_PageUp: virtualKey = VK_PRIOR; break;
    case Qt::Key_PageDown: virtualKey = VK_NEXT; break;
    case Qt::Key_Insert: virtualKey = VK_INSERT; break;
    case Qt::Key_Delete: virtualKey = VK_DELETE; break;
    default: break;
    }
    if (virtualKey) {
        return Hotkey{nativeModifiers, virtualKey};
    }
    if (key >= 0x21 && key <= 0x7E) {
        const SHORT mapped = VkKeyScanW(static_cast<wchar_t>(key));
        if (mapped != -1) {
            const BYTE shifts = HIBYTE(mapped);
            if (shifts & 1) nativeModifiers |= MOD_SHIFT;
            if (shifts & 2) nativeModifiers |= MOD_CONTROL;
            if (shifts & 4) nativeModifiers |= MOD_ALT;
            return Hotkey{nativeModifiers, LOBYTE(mapped)};
        }
    }
    setError(error, QStringLiteral("Эта клавиша не поддерживается как глобальная горячая клавиша"));
    return std::nullopt;
}

bool WindowsIntegration::releaseHotkey(QString* error) {
    if (hotkeyId_ < 0) {
        return true;
    }
    if (!UnregisterHotKey(nullptr, hotkeyId_)) {
        setError(error, systemError(GetLastError()));
        return false;
    }
    activeHotkeys.remove(hotkeyId_);
    hotkeyId_ = -1;
    hotkey_.reset();
    return true;
}

bool WindowsIntegration::setHotkey(const QString& sequence, QString* error) {
    setError(error, {});
    if (!onOwnerThread(error)) return false;
    if (sequence.trimmed().isEmpty()) return releaseHotkey(error);
    const auto parsed = parseHotkey(sequence, error);
    if (!parsed) return false;
    if (hotkey_ == parsed) return true;
    const int candidate = allocateHotkeyId();
    if (candidate < 0 || !RegisterHotKey(nullptr, candidate, parsed->modifiers | MOD_NOREPEAT, parsed->key)) {
        setError(error, QStringLiteral("Сочетание уже занято или зарезервировано Windows"));
        return false;
    }
    if (!releaseHotkey(error)) {
        UnregisterHotKey(nullptr, candidate);
        return false;
    }
    hotkeyId_ = candidate;
    hotkey_ = parsed;
    activeHotkeys.insert(candidate);
    return true;
}

bool WindowsIntegration::nativeEventFilter(const QByteArray& eventType, void* message, qintptr* result) {
    if ((eventType != "windows_generic_MSG" && eventType != "windows_dispatcher_MSG") || !message) {
        return false;
    }
    const auto* native = static_cast<MSG*>(message);
    if (native->message != WM_HOTKEY || hotkeyId_ < 0 || native->wParam != static_cast<WPARAM>(hotkeyId_)) {
        return false;
    }
    if (result) *result = 0;
    emit activated();
    return true;
}

bool WindowsIntegration::setStartup(bool enabled, QString* error) {
    setError(error, {});
    HKEY key = nullptr;
    LSTATUS result = enabled
        ? RegCreateKeyExW(HKEY_CURRENT_USER, StartupKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr)
        : RegOpenKeyExW(HKEY_CURRENT_USER, StartupKey, 0, KEY_SET_VALUE, &key);
    if (!enabled && result == ERROR_FILE_NOT_FOUND) return true;
    if (result != ERROR_SUCCESS) {
        setError(error, systemError(result));
        return false;
    }
    if (enabled) {
        const QString command = QLatin1Char('"') + QDir::toNativeSeparators(QCoreApplication::applicationFilePath())
            + QStringLiteral("\" --background");
        if (command.size() > MaxStartupCommand) {
            RegCloseKey(key);
            setError(error, QStringLiteral("Путь программы слишком длинный для автозапуска Windows"));
            return false;
        }
        const auto wide = command.toStdWString();
        result = RegSetValueExW(key, StartupName, 0, REG_SZ, reinterpret_cast<const BYTE*>(wide.c_str()),
                                static_cast<DWORD>((wide.size() + 1) * sizeof(wchar_t)));
    } else {
        result = RegDeleteValueW(key, StartupName);
        if (result == ERROR_FILE_NOT_FOUND) result = ERROR_SUCCESS;
    }
    RegCloseKey(key);
    if (result != ERROR_SUCCESS) setError(error, systemError(result));
    return result == ERROR_SUCCESS;
}

bool WindowsIntegration::isStartupEnabled() {
    wchar_t command[MaxStartupCommand + 1]{};
    DWORD size = sizeof(command);
    const LSTATUS result = RegGetValueW(HKEY_CURRENT_USER, StartupKey, StartupName,
                                        RRF_RT_REG_SZ, nullptr, command, &size);
    return result == ERROR_SUCCESS && command[0] != L'\0';
}

double WindowsIntegration::volume(QString* error) {
    setError(error, {});
    if (!onOwnerThread(error)) return -1.0;
    if (FAILED(comResult_) && comResult_ != RPC_E_CHANGED_MODE) {
        setError(error, systemError(comResult_));
        return -1.0;
    }
    const auto endpoint = defaultEndpoint(error);
    if (!endpoint) return -1.0;
    float value = 0.0F;
    const HRESULT result = endpoint->GetMasterVolumeLevelScalar(&value);
    if (FAILED(result)) {
        setError(error, systemError(result));
        return -1.0;
    }
    return std::clamp(static_cast<double>(value), 0.0, 1.0);
}

bool WindowsIntegration::setVolume(double value, QString* error) {
    setError(error, {});
    if (!std::isfinite(value) || value < 0.0 || value > 1.0) {
        setError(error, QStringLiteral("Громкость должна быть в диапазоне от 0 до 1"));
        return false;
    }
    if (!onOwnerThread(error)) return false;
    if (FAILED(comResult_) && comResult_ != RPC_E_CHANGED_MODE) {
        setError(error, systemError(comResult_));
        return false;
    }
    const auto endpoint = defaultEndpoint(error);
    if (!endpoint) return false;
    const HRESULT result = endpoint->SetMasterVolumeLevelScalar(static_cast<float>(value), nullptr);
    if (FAILED(result)) setError(error, systemError(result));
    return SUCCEEDED(result);
}

bool WindowsIntegration::applyBackdrop(WId window, bool enabled, const QString& tint, double opacity) {
    if (!window || !std::isfinite(opacity) || opacity < 0.0 || opacity > 1.0) return false;
    const QColor color(tint);
    if (!color.isValid()) return false;
    const HWND hwnd = reinterpret_cast<HWND>(window);
    const int backdrop = enabled ? AcrylicBackdrop : NoBackdrop;
    const HRESULT result = DwmSetWindowAttribute(hwnd, SystemBackdropAttribute, &backdrop, sizeof(backdrop));
    if (SUCCEEDED(result)) {
        const MARGINS margins = enabled ? MARGINS{-1, -1, -1, -1} : MARGINS{0, 0, 0, 0};
        if (SUCCEEDED(DwmExtendFrameIntoClientArea(hwnd, &margins))) {
            const BOOL dark = color.lightness() < 128;
            DwmSetWindowAttribute(hwnd, DarkModeAttribute, &dark, sizeof(dark));
            setAccent(hwnd, AccentDisabled, 0);
            return true;
        }
    }
    // Windows 10 lacks a public HWND Acrylic API; its optional accent API can be unavailable.
    if (!enabled) return setAccent(hwnd, AccentDisabled, 0);
    const DWORD abgr = (static_cast<DWORD>(std::lround(opacity * 255.0)) << 24)
        | (static_cast<DWORD>(color.blue()) << 16) | (static_cast<DWORD>(color.green()) << 8)
        | static_cast<DWORD>(color.red());
    return setAccent(hwnd, AccentAcrylic, abgr) || setAccent(hwnd, AccentBlur, abgr);
}

void WindowsIntegration::setClickThrough(WId window, bool enabled) {
    const HWND hwnd = reinterpret_cast<HWND>(window);
    if (!hwnd) return;
    SetLastError(ERROR_SUCCESS);
    const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (!style && GetLastError() != ERROR_SUCCESS) {
        qWarning() << "Cannot read overlay window style:" << systemError(GetLastError());
        return;
    }
    const LONG_PTR updated = enabled ? style | WS_EX_TRANSPARENT : style & ~WS_EX_TRANSPARENT;
    if (updated == style) return;
    SetLastError(ERROR_SUCCESS);
    if (!SetWindowLongPtrW(hwnd, GWL_EXSTYLE, updated) && GetLastError() != ERROR_SUCCESS) {
        qWarning() << "Cannot update overlay hit testing:" << systemError(GetLastError());
        return;
    }
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

void WindowsIntegration::ensureTopmost(WId window) {
    if (!window) return;
    if (!SetWindowPos(reinterpret_cast<HWND>(window), HWND_TOPMOST, 0, 0, 0, 0,
                      SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE)) {
        qWarning() << "Cannot keep overlay topmost:" << systemError(GetLastError());
    }
}

#include "WindowsIntegration.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>
#include <DispatcherQueue.h>
#include <windows.ui.composition.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.Composition.Desktop.h>

#include <QColor>
#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QEvent>
#include <QHash>
#include <QKeySequence>
#include <QSet>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <memory>

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
constexpr int AccentHostBackdrop = 5;
constexpr DWORD HostBackdropAttribute = 17;
constexpr double RegionCoordinateLimit = (1 << 26) - 2;
// Keeps the circular Composition clip inside the painted continuous-corner card edge.
constexpr double BackdropInset = 1.0;
constexpr DWORD CursorSuppressed = 0x00000002;

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

namespace composition = winrt::Windows::UI::Composition;

struct CompositionContext {
    bool ownsCom = false;
    winrt::Windows::System::DispatcherQueueController queue{nullptr};
    composition::Compositor compositor{nullptr};

    CompositionContext() {
        const HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        ownsCom = SUCCEEDED(result);
        if (FAILED(result) && result != RPC_E_CHANGED_MODE) winrt::check_hresult(result);
        try {
            if (!winrt::Windows::System::DispatcherQueue::GetForCurrentThread()) {
                const DispatcherQueueOptions options{sizeof(DispatcherQueueOptions), DQTYPE_THREAD_CURRENT, DQTAT_COM_NONE};
                winrt::check_hresult(CreateDispatcherQueueController(options,
                    reinterpret_cast<ABI::Windows::System::IDispatcherQueueController**>(winrt::put_abi(queue))));
            }
            compositor = composition::Compositor{};
        } catch (...) {
            shutdown();
            throw;
        }
    }

    ~CompositionContext() { shutdown(); }

    void shutdown() noexcept {
        compositor = nullptr;
        if (queue) {
            try {
                const auto closing = queue.ShutdownQueueAsync();
                const auto deadline = GetTickCount64() + 2000;
                // The dispatcher needs its native messages while releasing the last surface.
                while (closing.Status() == winrt::Windows::Foundation::AsyncStatus::Started
                       && GetTickCount64() < deadline) {
                    MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
                    MSG message{};
                    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                        if (message.message == WM_QUIT) { PostQuitMessage(static_cast<int>(message.wParam)); break; }
                        TranslateMessage(&message);
                        DispatchMessageW(&message);
                    }
                }
            } catch (const winrt::hresult_error& error) {
                qWarning() << "Cannot stop backdrop dispatcher:" << QString::fromWCharArray(error.message().c_str());
            }
            queue = nullptr;
        }
        if (ownsCom) { CoUninitialize(); ownsCom = false; }
    }
};

std::weak_ptr<CompositionContext> sharedComposition;

std::shared_ptr<CompositionContext> compositionContext() {
    auto context = sharedComposition.lock();
    if (!context) { context = std::make_shared<CompositionContext>(); sharedComposition = context; }
    return context;
}

LRESULT CALLBACK backdropWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCHITTEST) return HTTRANSPARENT;
    if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcW(window, message, wParam, lParam);
}

class OverlaySurface;
QHash<HWND, OverlaySurface*> overlaySurfaces;

class OverlaySurface final : public QObject {
public:
    OverlaySurface(HWND foreground, QWidget* widget)
        : QObject(widget), foreground_(foreground), context_(compositionContext()) {
        const auto instance = GetModuleHandleW(nullptr);
        constexpr wchar_t className[] = L"IslandCompositionBackdrop";
        WNDCLASSW windowClass{};
        windowClass.lpfnWndProc = backdropWindowProc;
        windowClass.hInstance = instance;
        windowClass.lpszClassName = className;
        if (!RegisterClassW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            winrt::throw_last_error();
        const DWORD topmost = (GetWindowLongPtrW(foreground, GWL_EXSTYLE) & WS_EX_TOPMOST) ? WS_EX_TOPMOST : 0;
        window_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT
                                      | WS_EX_NOREDIRECTIONBITMAP | topmost,
                                 className, L"Island Backdrop", WS_POPUP, 0, 0, 1, 1,
                                 nullptr, nullptr, instance, nullptr);
        if (!window_) winrt::throw_last_error();
        try {
            const auto interop = context_->compositor.as<ABI::Windows::UI::Composition::Desktop::ICompositorDesktopInterop>();
            winrt::check_hresult(interop->CreateDesktopWindowTarget(window_, false,
                reinterpret_cast<ABI::Windows::UI::Composition::Desktop::IDesktopWindowTarget**>(winrt::put_abi(target_))));
            root_ = context_->compositor.CreateContainerVisual();
            root_.Opacity(static_cast<float>(widget->windowOpacity()));
            sprite_ = context_->compositor.CreateSpriteVisual();
            rounded_ = context_->compositor.CreateRoundedRectangleGeometry();
            sprite_.Clip(context_->compositor.CreateGeometricClip(rounded_));
            sprite_.Brush(context_->compositor.CreateHostBackdropBrush());
            screenClip_ = context_->compositor.CreateInsetClip();
            root_.Clip(screenClip_);
            root_.Children().InsertAtTop(sprite_);
            target_.Root(root_);
            widget->installEventFilter(this);
        } catch (...) {
            DestroyWindow(window_);
            window_ = nullptr;
            throw;
        }
    }

    ~OverlaySurface() override {
        if (parent()) parent()->removeEventFilter(this);
        overlaySurfaces.remove(foreground_);
        if (window_) ShowWindow(window_, SW_HIDE);
        if (target_) { try { target_.Close(); } catch (...) {} }
        target_ = nullptr;
        sprite_ = nullptr;
        root_ = nullptr;
        rounded_ = nullptr;
        screenClip_ = nullptr;
        if (window_) DestroyWindow(window_);
    }

    bool configure(bool enabled, const QColor& color, double opacity) {
        if (!enabled) { enabled_ = false; ShowWindow(window_, SW_HIDE); return true; }
        const BOOL useHost = TRUE;
        bool supported = SUCCEEDED(DwmSetWindowAttribute(window_, HostBackdropAttribute, &useHost, sizeof(useHost)));
        if (!supported) {
            // Windows 10 exposes host backdrop only through its optional, undocumented accent API.
            const DWORD abgr = (static_cast<DWORD>(std::lround(opacity * 255.0)) << 24)
                | (static_cast<DWORD>(color.blue()) << 16) | (static_cast<DWORD>(color.green()) << 8)
                | static_cast<DWORD>(color.red());
            supported = setAccent(window_, AccentHostBackdrop, abgr);
        }
        enabled_ = supported;
        syncVisibility();
        return supported;
    }

    bool update(const QRectF& card, double radius, double ratio, const QRectF& clip) {
        bounds_ = card;
        radius_ = radius;
        ratio_ = ratio;
        clip_ = clip;
        RECT client{};
        POINT origin{};
        if (!GetClientRect(foreground_, &client) || !ClientToScreen(foreground_, &origin)) return false;
        const int width = std::max<LONG>(1, client.right - client.left);
        const int height = std::max<LONG>(1, client.bottom - client.top);
        // The surface spans the whole overlay and only its visual changes per frame.
        // Moving or resizing a second HWND on every animation frame lags the card.
        const QRect frame(origin.x, origin.y, width, height);
        if (frame != frame_) {
            if (!SetWindowPos(window_, foreground_, frame.x(), frame.y(), width, height, SWP_NOACTIVATE)) return false;
            frame_ = frame;
            root_.Size({static_cast<float>(width), static_cast<float>(height)});
        }
        const double inset = std::min({BackdropInset, card.width() / 2, card.height() / 2});
        const QRectF shape = card.adjusted(inset, inset, -inset, -inset);
        const winrt::Windows::Foundation::Numerics::float2 size{static_cast<float>(shape.width() * ratio),
                                                               static_cast<float>(shape.height() * ratio)};
        if (sprite_.Size() != size) { sprite_.Size(size); rounded_.Size(size); }
        sprite_.Offset({static_cast<float>(shape.left() * ratio), static_cast<float>(shape.top() * ratio), 0});
        const float corner = static_cast<float>(std::max(0.0, radius - inset) * ratio);
        rounded_.CornerRadius({corner, corner});
        const QRectF visible = clip.isValid() ? card.intersected(clip) : card;
        clippedOut_ = visible.isEmpty();
        // The Composition geometry owns rounded-edge coverage; a binary HWND region
        // would cut its antialiased boundary. Screen limits use an inset clip instead.
        screenClip_.LeftInset(clip.isValid() ? static_cast<float>(std::max(0.0, std::ceil(clip.left() * ratio))) : 0);
        screenClip_.TopInset(clip.isValid() ? static_cast<float>(std::max(0.0, std::ceil(clip.top() * ratio))) : 0);
        screenClip_.RightInset(clip.isValid() ? static_cast<float>(std::max(0.0, width - std::floor(clip.right() * ratio))) : 0);
        screenClip_.BottomInset(clip.isValid() ? static_cast<float>(std::max(0.0, height - std::floor(clip.bottom() * ratio))) : 0);
        syncVisibility();
        return true;
    }

    void setOpacity(double opacity) { root_.Opacity(static_cast<float>(opacity)); }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (event->type() == QEvent::Hide) ShowWindow(window_, SW_HIDE);
        if (event->type() == QEvent::WindowStateChange) syncVisibility();
        if ((event->type() == QEvent::Move || event->type() == QEvent::Resize) && bounds_.isValid()) {
            try { update(bounds_, radius_, ratio_, clip_); }
            catch (const winrt::hresult_error& error) {
                qWarning() << "Cannot move overlay backdrop:" << QString::fromWCharArray(error.message().c_str());
            }
        }
        if (event->type() == QEvent::ZOrderChange || event->type() == QEvent::WindowActivate
            || event->type() == QEvent::WindowDeactivate)
            SetWindowPos(window_, foreground_, 0, 0, 0, 0, SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE);
        return QObject::eventFilter(watched, event);
    }

private:
    void syncVisibility() {
        const bool shown = enabled_ && !clippedOut_ && IsWindowVisible(foreground_) && !IsIconic(foreground_);
        if (shown == static_cast<bool>(IsWindowVisible(window_))) return;
        ShowWindow(window_, shown ? SW_SHOWNOACTIVATE : SW_HIDE);
        if (shown) SetWindowPos(window_, foreground_, 0, 0, 0, 0, SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE);
    }

    HWND foreground_ = nullptr;
    HWND window_ = nullptr;
    bool enabled_ = false;
    bool clippedOut_ = false;
    QRect frame_;
    QRectF bounds_;
    QRectF clip_;
    double radius_ = 0;
    double ratio_ = 1;
    std::shared_ptr<CompositionContext> context_;
    winrt::Windows::UI::Composition::Desktop::DesktopWindowTarget target_{nullptr};
    composition::ContainerVisual root_{nullptr};
    composition::SpriteVisual sprite_{nullptr};
    composition::CompositionRoundedRectangleGeometry rounded_{nullptr};
    composition::InsetClip screenClip_{nullptr};
};

bool validOverlayGeometry(const QRectF& bounds, double radius, double ratio, const QRectF& clip) {
    if (!bounds.isValid() || !std::isfinite(radius) || radius < 0 || !std::isfinite(ratio) || ratio <= 0) return false;
    const auto valid = [ratio](const QRectF& rectangle) {
        return std::isfinite(rectangle.left()) && std::isfinite(rectangle.top())
            && std::isfinite(rectangle.right()) && std::isfinite(rectangle.bottom())
            && std::abs(rectangle.left() * ratio) < RegionCoordinateLimit
            && std::abs(rectangle.top() * ratio) < RegionCoordinateLimit
            && std::abs(rectangle.right() * ratio) < RegionCoordinateLimit
            && std::abs(rectangle.bottom() * ratio) < RegionCoordinateLimit;
    };
    return valid(bounds) && bounds.width() * ratio < RegionCoordinateLimit
        && bounds.height() * ratio < RegionCoordinateLimit
        && (clip.isNull() || (clip.isValid() && valid(clip)));
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

bool WindowsIntegration::updateOverlayRegion(WId window, const QRectF& cardBounds, double radius,
                                              double devicePixelRatio, const QRectF& clipBounds,
                                              const QRectF& hitBounds) {
    const HWND hwnd = reinterpret_cast<HWND>(window);
    if (!hwnd || GetWindowThreadProcessId(hwnd, nullptr) != GetCurrentThreadId()
        || !validOverlayGeometry(cardBounds, radius, devicePixelRatio, clipBounds)
        || (!hitBounds.isNull() && !validOverlayGeometry(hitBounds, 0, devicePixelRatio, {}))) return false;
    radius = std::min(radius, std::min(cardBounds.width(), cardBounds.height()) / 2.0);
    const QRectF regionBounds = hitBounds.isValid() ? hitBounds : cardBounds;
    RECT frame{};
    POINT origin{};
    if (!GetWindowRect(hwnd, &frame) || !ClientToScreen(hwnd, &origin)) return false;
    const int offsetX = origin.x - frame.left;
    const int offsetY = origin.y - frame.top;
    const auto x = [devicePixelRatio, offsetX](double value) { return static_cast<int>(std::floor(value * devicePixelRatio)) + offsetX; };
    const auto y = [devicePixelRatio, offsetY](double value) { return static_cast<int>(std::floor(value * devicePixelRatio)) + offsetY; };
    // The layered Qt window paints the rounded contour with per-pixel alpha.
    // SetWindowRgn is binary and must not trim that antialiasing. Transparent
    // pixels still pass hit tests through to the window underneath on Windows.
    HRGN desired = CreateRectRgn(x(regionBounds.left()), y(regionBounds.top()),
        static_cast<int>(std::ceil(regionBounds.right() * devicePixelRatio)) + offsetX,
        static_cast<int>(std::ceil(regionBounds.bottom() * devicePixelRatio)) + offsetY);
    if (!desired) return false;
    if (clipBounds.isValid()) {
        HRGN clip = CreateRectRgn(static_cast<int>(std::ceil(clipBounds.left() * devicePixelRatio)) + offsetX,
            static_cast<int>(std::ceil(clipBounds.top() * devicePixelRatio)) + offsetY, x(clipBounds.right()), y(clipBounds.bottom()));
        if (!clip || CombineRgn(desired, desired, clip, RGN_AND) == ERROR) {
            if (clip) DeleteObject(clip);
            DeleteObject(desired);
            return false;
        }
        DeleteObject(clip);
    }
    HRGN current = CreateRectRgn(0, 0, 0, 0);
    const bool same = current && GetWindowRgn(hwnd, current) != ERROR && EqualRgn(current, desired);
    if (current) DeleteObject(current);
    if (same) DeleteObject(desired);
    else if (!SetWindowRgn(hwnd, desired, TRUE)) { DeleteObject(desired); return false; }
    const auto surface = overlaySurfaces.value(hwnd, nullptr);
    if (!surface) return true;
    try { return surface->update(cardBounds, radius, devicePixelRatio, clipBounds); }
    catch (const winrt::hresult_error& error) {
        qWarning() << "Cannot update overlay backdrop:" << QString::fromWCharArray(error.message().c_str());
        return false;
    }
}

bool WindowsIntegration::applyOverlayBackdrop(WId window, bool enabled, const QRectF& cardBounds,
                                               double radius, double devicePixelRatio, const QString& tint,
                                               double opacity, const QRectF& clipBounds, const QRectF& hitBounds) {
    const QColor color(tint);
    if (!color.isValid() || !std::isfinite(opacity) || opacity < 0 || opacity > 1
        || !updateOverlayRegion(window, cardBounds, radius, devicePixelRatio, clipBounds, hitBounds)) return false;
    const HWND hwnd = reinterpret_cast<HWND>(window);
    // HWND-wide Acrylic ignores SetWindowRgn; only the clipped Composition visual supplies blur.
    applyBackdrop(window, false, tint, opacity);
    auto surface = overlaySurfaces.value(hwnd, nullptr);
    if (!surface && !enabled) return true;
    try {
        if (!surface) {
            auto* widget = QWidget::find(window);
            if (!widget) return false;
            surface = new OverlaySurface(hwnd, widget);
            overlaySurfaces.insert(hwnd, surface);
        }
        radius = std::min(radius, std::min(cardBounds.width(), cardBounds.height()) / 2.0);
        if (!surface->update(cardBounds, radius, devicePixelRatio, clipBounds)) return false;
        return surface->configure(enabled, color, opacity);
    } catch (const winrt::hresult_error& error) {
        qWarning() << "Overlay blur is unavailable:" << QString::fromWCharArray(error.message().c_str());
        releaseOverlayBackdrop(window);
        return false;
    }
}

void WindowsIntegration::setOverlayOpacity(WId window, double opacity) {
    if (!std::isfinite(opacity) || opacity < 0 || opacity > 1) return;
    if (auto* surface = overlaySurfaces.value(reinterpret_cast<HWND>(window), nullptr)) {
        try { surface->setOpacity(opacity); }
        catch (const winrt::hresult_error& error) {
            qWarning() << "Cannot fade overlay backdrop:" << QString::fromWCharArray(error.message().c_str());
        }
    }
}

void WindowsIntegration::releaseOverlayBackdrop(WId window) {
    delete overlaySurfaces.take(reinterpret_cast<HWND>(window));
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

bool WindowsIntegration::cursorVisible() {
    CURSORINFO info{};
    info.cbSize = sizeof(info);
    // An unreadable cursor state must not lock hover out on unusual desktops.
    if (!GetCursorInfo(&info)) return true;
    return (info.flags & CURSOR_SHOWING) && !(info.flags & CursorSuppressed);
}

bool WindowsIntegration::mouseButtonsDown() {
    for (const int button : {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2}) {
        if (GetAsyncKeyState(button) & 0x8000) return true;
    }
    return false;
}

bool WindowsIntegration::foregroundIsFullscreen(WId reference) {
    QUERY_USER_NOTIFICATION_STATE notification{};
    if (SUCCEEDED(SHQueryUserNotificationState(&notification)) && notification == QUNS_RUNNING_D3D_FULL_SCREEN)
        return true;
    const HWND window = GetForegroundWindow();
    if (!window || IsIconic(window)) return false;
    DWORD process = 0;
    GetWindowThreadProcessId(window, &process);
    if (process == GetCurrentProcessId()) return false;
    wchar_t name[64]{};
    GetClassNameW(window, name, static_cast<int>(std::size(name)));
    for (const wchar_t* shell : {L"Progman", L"WorkerW", L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd"}) {
        if (wcscmp(name, shell) == 0) return false;
    }
    const HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONULL);
    if (!monitor) return false;
    if (reference && MonitorFromWindow(reinterpret_cast<HWND>(reference), MONITOR_DEFAULTTONEAREST) != monitor)
        return false;
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    RECT bounds{};
    if (!GetMonitorInfoW(monitor, &info) || !GetWindowRect(window, &bounds)) return false;
    const bool covers = bounds.left <= info.rcMonitor.left && bounds.top <= info.rcMonitor.top
        && bounds.right >= info.rcMonitor.right && bounds.bottom >= info.rcMonitor.bottom;
    // A maximized captioned window also spans the monitor when the taskbar auto-hides.
    const LONG_PTR style = GetWindowLongPtrW(window, GWL_STYLE);
    return covers && !(IsZoomed(window) && (style & WS_CAPTION) == WS_CAPTION);
}

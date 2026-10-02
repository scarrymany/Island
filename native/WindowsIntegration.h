#pragma once

#include <QAbstractNativeEventFilter>
#include <QObject>
#include <QRectF>
#include <QString>
#include <QtGui/qwindowdefs.h>

#include <optional>

class WindowsIntegration final : public QObject, public QAbstractNativeEventFilter {
    Q_OBJECT

public:
    explicit WindowsIntegration(QObject* parent = nullptr);
    ~WindowsIntegration() override;

    bool setHotkey(const QString& sequence, QString* error = nullptr);
    bool nativeEventFilter(const QByteArray& eventType, void* message, qintptr* result) override;

    static bool setStartup(bool enabled, QString* error = nullptr);
    static bool isStartupEnabled();
    static bool applyBackdrop(WId hwnd, bool enabled, const QString& tint = QStringLiteral("#10121b"),
                              double opacity = 0.9);
    // hitBounds, when valid, replaces the card as the native window region. Animated
    // overlays pass their whole frame so a moving card is never clipped mid-frame.
    static bool applyOverlayBackdrop(WId hwnd, bool enabled, const QRectF& cardBounds, double radius,
                                     double devicePixelRatio, const QString& tint = QStringLiteral("#10121b"),
                                     double opacity = 0.9, const QRectF& clipBounds = {},
                                     const QRectF& hitBounds = {});
    static bool updateOverlayRegion(WId hwnd, const QRectF& cardBounds, double radius,
                                    double devicePixelRatio, const QRectF& clipBounds = {},
                                    const QRectF& hitBounds = {});
    static void setOverlayOpacity(WId hwnd, double opacity);
    static void releaseOverlayBackdrop(WId hwnd);
    static void setClickThrough(WId hwnd, bool enabled);
    static void ensureTopmost(WId hwnd);

    // Pointer context for hover decisions. A hidden or suppressed cursor means the
    // user cannot see what they would be hovering (games, video players, touch).
    static bool cursorVisible();
    static bool mouseButtonsDown();
    // True when another process owns a borderless window covering the whole monitor
    // that contains reference (a game or fullscreen video), or a Direct3D exclusive app runs.
    static bool foregroundIsFullscreen(WId reference);

    double volume(QString* error = nullptr);
    bool setVolume(double value, QString* error = nullptr);

signals:
    void activated();

private:
    friend class PlatformTest;
    struct Hotkey {
        unsigned int modifiers;
        unsigned int key;
        bool operator==(const Hotkey&) const = default;
    };

    static std::optional<Hotkey> parseHotkey(const QString& sequence, QString* error);
    bool releaseHotkey(QString* error = nullptr);
    bool onOwnerThread(QString* error) const;

    int hotkeyId_ = -1;
    std::optional<Hotkey> hotkey_;
    unsigned long nativeThreadId_ = 0;
    long comResult_ = 0;
    bool ownsCom_ = false;
};

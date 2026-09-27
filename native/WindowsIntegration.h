#pragma once

#include <QAbstractNativeEventFilter>
#include <QObject>
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
    static void setClickThrough(WId hwnd, bool enabled);
    static void ensureTopmost(WId hwnd);

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

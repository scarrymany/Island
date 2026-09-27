#pragma once

#include "ConfigStore.h"
#include "HudWindow.h"
#include "MediaBridge.h"
#include "SettingsWindow.h"
#include "UpdateService.h"
#include "WindowsIntegration.h"
#include <QSystemTrayIcon>
#include <QTimer>
#include <memory>

class QMenu;

class Application final : public QObject {
    Q_OBJECT
public:
    explicit Application(bool demo, bool background, QString configPath = {}, QObject* parent = nullptr);
    ~Application() override;
    void showSettings();
    void capture(const QString& directory);
    void writeDiagnostics(const QString& path) const;
    void shutdown();
private:
    void applyConfig(const QJsonObject& config);
    void setEditing(bool enabled);
    void setStatus(const QString& message);
    void setupTray();
    void refreshProfiles();
    void playDemo(const QString& action, double value);
    void updateSettingsBackdrop();
    ConfigStore store_;
    WindowsIntegration windows_;
    MediaBridge media_;
    UpdateService updates_;
    SettingsWindow settings_;
    HudWindow hud_;
    QSystemTrayIcon tray_;
    std::unique_ptr<QMenu> trayMenu_;
    QMenu* profilesMenu_ = nullptr;
    QTimer volumeTimer_;
    MediaSnapshot latest_;
    MediaSources sources_;
    QJsonObject applied_;
    QStringList errors_;
    bool demo_ = false;
    bool stopping_ = false;
};

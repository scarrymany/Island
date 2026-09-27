#include "Application.h"

#include <QApplication>
#include <QBuffer>
#include <QDateTime>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QIcon>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QSaveFile>
#include <QScreen>
#include <QTimer>

namespace {
QPixmap makeMark(int size) {
    QPixmap image(size, size); image.fill(Qt::transparent);
    QPainter p(&image); p.setRenderHint(QPainter::Antialiasing);
    QLinearGradient gradient(0, 0, size, size);
    gradient.setColorAt(0, QColor("#BDACFF")); gradient.setColorAt(1, QColor("#516CEE"));
    p.setPen(Qt::NoPen); p.setBrush(gradient);
    p.drawRoundedRect(QRectF(0, 0, size, size), size * .28, size * .28);
    p.setPen(QPen(Qt::white, size * .055, Qt::SolidLine, Qt::RoundCap));
    const double heights[]{.20, .38, .57, .31};
    for (int i = 0; i < 4; ++i) {
        const double x = size * (.28 + .15 * i), h = size * heights[i];
        p.drawLine(QPointF(x, (size - h) / 2), QPointF(x, (size + h) / 2));
    }
    return image;
}
QByteArray demoCover() {
    QPixmap image(400, 400); image.fill(QColor("#151726"));
    QPainter p(&image); p.setRenderHint(QPainter::Antialiasing);
    QLinearGradient background(0, 0, 400, 400);
    background.setColorAt(0, QColor("#152454")); background.setColorAt(1, QColor("#71419D"));
    p.fillRect(image.rect(), background);
    p.setBrush(QColor("#B9AEFF")); p.setPen(Qt::NoPen); p.drawEllipse(QPointF(258, 140), 92, 92);
    p.setBrush(QColor("#171C3E")); p.drawEllipse(QPointF(222, 114), 93, 93);
    p.setPen(QPen(QColor("#787FF0"), 2));
    for (int i = 0; i < 14; ++i) p.drawEllipse(QRectF(-100, 245 + i * 12, 590, 180));
    p.end();
    QByteArray bytes; QBuffer buffer(&bytes); buffer.open(QIODevice::WriteOnly); image.save(&buffer, "PNG");
    return bytes;
}
}

Application::Application(bool demo, bool background, QString configPath, QObject* parent)
    : QObject(parent), store_(std::move(configPath)), windows_(), media_(), updates_(),
      settings_(&store_), hud_(store_.config()), demo_(demo) {
    const QIcon appIcon(makeMark(128));
    qApp->setWindowIcon(appIcon); settings_.setWindowIcon(appIcon); tray_.setIcon(appIcon);
    settings_.installEventFilter(this);
    setupTray();
    connect(&store_, &ConfigStore::configChanged, this, &Application::applyConfig);
    connect(&hud_, &HudWindow::configChanged, this, [this](const QJsonObject& c) {
        QString error;
        if (!store_.update(c, &error)) { setStatus(error); hud_.applyConfig(store_.config()); }
        settings_.refresh();
    });
    connect(&settings_, &SettingsWindow::toggleHud, &hud_, &HudWindow::toggle);
    connect(&settings_, &SettingsWindow::editLayoutChanged, this, &Application::setEditing);
    connect(&hud_, &HudWindow::editingChanged, this, &Application::setEditing);
    connect(&hud_, &HudWindow::settingsRequested, this, &Application::showSettings);
    connect(&windows_, &WindowsIntegration::activated, &hud_, &HudWindow::toggle);
    connect(&media_, &MediaBridge::snapshotChanged, this, [this](const MediaSnapshot& snapshot) {
        if (stopping_) return;
        latest_ = snapshot; hud_.setSnapshot(snapshot);
        tray_.setToolTip(snapshot.active ? (snapshot.title + " - " + snapshot.artist).left(120) : QStringLiteral("Island - музыка рядом"));
    });
    connect(&media_, &MediaBridge::sourcesChanged, this, [this](const MediaSources& sources) {
        if (stopping_) return;
        sources_ = sources; settings_.setSources(sources);
    });
    connect(&media_, &MediaBridge::error, this, &Application::setStatus);
    connect(&hud_, &HudWindow::command, this, [this](const QString& action, double value) {
        if (demo_) playDemo(action, value); else media_.execute(action, value);
    });
    connect(&hud_, &HudWindow::volumeChanged, this, [this](double value) {
        QString error;
        if (demo_ || windows_.setVolume(value, &error)) hud_.setVolume(value); else setStatus(error);
    });
    connect(&settings_, &SettingsWindow::checkUpdates, this, [this] {
        settings_.setUpdateState(QStringLiteral("Проверяем GitHub Releases..."), false, true);
        updates_.check(store_.config()["update_repository"].toString());
    });
    connect(&settings_, &SettingsWindow::updateInstallRequested, this, [this] {
        if (QMessageBox::question(&settings_, QStringLiteral("Обновление Island"),
                QStringLiteral("Скачать проверенный установщик и закрыть Island для обновления?")) == QMessageBox::Yes)
            updates_.downloadAndInstall();
    });
    connect(&updates_, &UpdateService::statusChanged, this, [this](const QString& message) {
        if (stopping_) return;
        settings_.setUpdateState(message, updates_.hasUpdate(), updates_.busy());
    });
    connect(&updates_, &UpdateService::updateAvailable, this, [this](const QString& version, const QString& notes) {
        if (stopping_) return;
        settings_.setUpdateState(QStringLiteral("Доступна версия %1\n%2").arg(version, notes.left(3000)), true, false);
        tray_.showMessage("Island", QStringLiteral("Доступна версия %1. Откройте раздел обновлений.").arg(version));
    });
    connect(&updates_, &UpdateService::readyToQuit, this, [this] { shutdown(); QCoreApplication::exit(0); });
    volumeTimer_.setInterval(2500);
    connect(&volumeTimer_, &QTimer::timeout, this, [this] {
        if (!hud_.isVisible() || demo_) return;
        const auto value = windows_.volume(); if (value >= 0) hud_.setVolume(value);
    });
    applyConfig(store_.config());
    if (demo_) {
        latest_.active = true; latest_.title = QStringLiteral("After Hours"); latest_.artist = QStringLiteral("Island Sessions");
        latest_.album = QStringLiteral("Midnight Collection"); latest_.source = "DEMO"; latest_.sourceId = "demo";
        latest_.position = 93; latest_.duration = 247; latest_.playing = true; latest_.canSeek = true;
        latest_.updatedAt = QDateTime::currentMSecsSinceEpoch(); latest_.cover = demoCover();
        hud_.setSnapshot(latest_);
        settings_.setStatus(QStringLiteral("Демонстрация интерфейса. Системный плеер и громкость не изменяются."));
    } else {
        const double volume = windows_.volume();
        if (volume >= 0) hud_.setVolume(volume);
        media_.start(); volumeTimer_.start();
        if (store_.config()["check_updates"].toBool(true))
            QTimer::singleShot(5000, this, [this] {
                if (!stopping_) updates_.check(store_.config()["update_repository"].toString());
            });
    }
    hud_.reveal();
    if (!background) showSettings();
    if (!store_.loadError().isEmpty()) setStatus(store_.loadError());
    connect(qApp, &QCoreApplication::aboutToQuit, this, &Application::shutdown);
}

Application::~Application() {
    shutdown();
    settings_.removeEventFilter(this);
    if (QGuiApplication::platformName() == "windows") {
        WindowsIntegration::releaseOverlayBackdrop(settings_.winId());
        WindowsIntegration::releaseOverlayBackdrop(hud_.winId());
    }
}
void Application::shutdown() {
    if (stopping_) return;
    stopping_ = true;
    updates_.cancel();
    volumeTimer_.stop(); media_.stop(); tray_.hide(); hud_.hide(); settings_.hide();
}

void Application::setupTray() {
    trayMenu_ = std::make_unique<QMenu>();
    trayMenu_->addAction(QStringLiteral("Открыть настройки"), this, &Application::showSettings);
    trayMenu_->addAction(QStringLiteral("Показать / скрыть островок"), &hud_, &HudWindow::toggle);
    auto* editing = trayMenu_->addAction(QStringLiteral("Редактировать расположение")); editing->setCheckable(true);
    connect(editing, &QAction::triggered, this, &Application::setEditing);
    connect(trayMenu_.get(), &QMenu::aboutToShow, this, [this, editing] { editing->setChecked(hud_.editing()); refreshProfiles(); });
    profilesMenu_ = trayMenu_->addMenu(QStringLiteral("Профили"));
    trayMenu_->addSeparator();
    trayMenu_->addAction(QStringLiteral("Выход"), this, [this] { shutdown(); QCoreApplication::exit(0); });
    tray_.setContextMenu(trayMenu_.get());
    tray_.setToolTip(QStringLiteral("Island - музыка рядом"));
    connect(&tray_, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick) showSettings();
        if (reason == QSystemTrayIcon::MiddleClick) hud_.toggle();
    });
    tray_.show();
}
void Application::refreshProfiles() {
    profilesMenu_->clear();
    for (const auto& name : store_.profiles()) {
        auto* action = profilesMenu_->addAction(name); action->setCheckable(true); action->setChecked(store_.activeProfile() == name);
        connect(action, &QAction::triggered, this, [this, name] {
            QString error; if (!store_.loadProfile(name, &error)) setStatus(error); settings_.refresh();
        });
    }
    profilesMenu_->setEnabled(!store_.profiles().isEmpty());
}

void Application::applyConfig(const QJsonObject& config) {
    if (stopping_) return;
    QJsonObject effective = config;
    if (applied_.contains("update_repository") && applied_["update_repository"] != config["update_repository"]) {
        updates_.cancel();
        settings_.setUpdateState(QStringLiteral("Репозиторий изменён. Проверьте наличие обновлений."));
    }
    if (!demo_) {
        QString error;
        if (applied_["hotkey"] != config["hotkey"] && !windows_.setHotkey(config["hotkey"].toString(), &error)) {
            effective["hotkey"] = applied_["hotkey"].toString();
            setStatus(error);
        }
        if (applied_["source_id"] != config["source_id"]) media_.setSource(config["source_id"].toString());
        if (applied_.contains("startup") && applied_["startup"] != config["startup"]) {
            if (!WindowsIntegration::setStartup(config["startup"].toBool(), &error)) {
                effective["startup"] = WindowsIntegration::isStartupEnabled();
                setStatus(error);
            }
        }
    }
    applied_ = effective;
    hud_.applyConfig(effective);
    if (effective != config) {
        QString error;
        if (!store_.update(effective, &error)) setStatus(error);
        settings_.refresh();
    }
    updateSettingsBackdrop();
}
void Application::updateSettingsBackdrop() {
    if (stopping_) return;
    const auto c = store_.config();
    if (settings_.isVisible() && !settings_.isMinimized() && QGuiApplication::platformName() == "windows")
        WindowsIntegration::applyOverlayBackdrop(settings_.winId(), c["settings_blur"].toBool(true), settings_.rect(),
            13, settings_.devicePixelRatioF(), c["settings_background"].toString(), c["settings_opacity"].toDouble(.94));
}
bool Application::eventFilter(QObject* watched, QEvent* event) {
    if (watched == &settings_ && (event->type() == QEvent::Show
        || event->type() == QEvent::Resize || event->type() == QEvent::WindowStateChange
        || event->type() == QEvent::DevicePixelRatioChange)) updateSettingsBackdrop();
    return QObject::eventFilter(watched, event);
}
void Application::showSettings() {
    if (stopping_) return;
    settings_.showNormal(); settings_.raise(); settings_.activateWindow(); updateSettingsBackdrop();
}
void Application::setEditing(bool enabled) { hud_.setEditing(enabled); settings_.setEditing(enabled); }
void Application::setStatus(const QString& message) {
    if (stopping_ || message.isEmpty()) return;
    qWarning().noquote() << message;
    errors_.append(message); if (errors_.size() > 40) errors_.removeFirst();
    settings_.setStatus(message);
}
void Application::playDemo(const QString& action, double value) {
    latest_.position = latest_.estimatedPosition();
    latest_.updatedAt = QDateTime::currentMSecsSinceEpoch();
    if (action == "play_pause") latest_.playing = !latest_.playing;
    if (action == "seek") latest_.position = value;
    if (action == "previous" || action == "next") { latest_.position = 0; latest_.title = latest_.title == "After Hours" ? "Soft Landing" : "After Hours"; }
    hud_.setSnapshot(latest_);
}
void Application::capture(const QString& directory) {
    QDir().mkpath(directory);
    hud_.grab().save(QDir(directory).filePath("hud.png"));
    settings_.grab().save(QDir(directory).filePath("settings.png"));
}
void Application::writeDiagnostics(const QString& path) const {
    QJsonArray sources;
    for (const auto& source : sources_) sources.append(QJsonObject{{"id", source.first}, {"name", source.second}});
    QJsonArray errors; for (const auto& error : errors_) errors.append(error);
    const QJsonObject result{{"active", latest_.active}, {"title", latest_.title}, {"artist", latest_.artist},
        {"source", latest_.source}, {"position", latest_.estimatedPosition()}, {"duration", latest_.duration},
        {"playing", latest_.playing}, {"cover_bytes", latest_.cover.size()}, {"sources", sources},
        {"errors", errors}, {"demo", demo_}, {"hud_visible", hud_.isVisible()}, {"settings_visible", settings_.isVisible()}};
    QSaveFile file(path);
    if (file.open(QIODevice::WriteOnly)) { file.write(QJsonDocument(result).toJson()); file.commit(); }
}

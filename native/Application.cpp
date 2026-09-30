#include "Application.h"
#include "AppAssets.h"
#include "AppInfo.h"

#include <QApplication>
#include <QBuffer>
#include <QDateTime>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QListWidget>
#include <QIcon>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QSaveFile>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QStackedWidget>
#include <QTimer>

namespace {
constexpr int VolumeRefreshIntervalMs = 750;
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
    const QIcon appIcon = AppAssets::icon();
    qApp->setWindowIcon(appIcon); settings_.setWindowIcon(appIcon); tray_.setIcon(appIcon);
    settings_.installEventFilter(this);
    setupTray();
    connect(&store_, &ConfigStore::configChanged, this, &Application::applyConfig);
    connect(&hud_, &HudWindow::configChanged, this, [this](const QJsonObject& c) {
        QString error;
        if (!store_.update(c, &error)) { setStatus(error); hud_.applyConfig(store_.config()); }
        settings_.refresh();
    });
    connect(&settings_, &SettingsWindow::editLayoutChanged, this, &Application::setEditing);
    connect(&hud_, &HudWindow::editingChanged, this, &Application::setEditing);
    connect(&hud_, &HudWindow::settingsRequested, this, &Application::showSettings);
    connect(&settings_, &SettingsWindow::resetPositionRequested, &hud_, &HudWindow::resetPosition);
    connect(&settings_, &SettingsWindow::releaseNotesRequested, this, &Application::showReleaseNotes);
    connect(&windows_, &WindowsIntegration::activated, &hud_, &HudWindow::toggle);
    connect(&media_, &MediaBridge::snapshotChanged, this, [this](const MediaSnapshot& snapshot) {
        if (stopping_) return;
        const QString preferred = store_.config()["source_id"].toString();
        if (snapshot.active && !preferred.isEmpty() && snapshot.sourceId != preferred) return;
        latest_ = snapshot; hud_.setSnapshot(snapshot);
        sessionVolume_.setSource(snapshot.active ? snapshot.sourceId : QString{});
        tray_.setToolTip(snapshot.active ? (snapshot.title + " - " + snapshot.artist).left(120) : QStringLiteral("SCARP ISLAND - музыка рядом"));
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
        hud_.setVolume(value);
        if (!demo_) sessionVolume_.setVolume(value);
    });
    connect(&sessionVolume_, &SessionVolume::changed, this, [this](const SessionVolumeState& state) {
        if (stopping_ || demo_ || state.sourceId != (latest_.active ? latest_.sourceId : QString{})) return;
        hud_.setVolumeAvailable(state.available);
        if (state.available) hud_.setVolume(state.muted ? 0.0 : state.volume);
    });
    connect(&sessionVolume_, &SessionVolume::error, this, &Application::setStatus);
    connect(&settings_, &SettingsWindow::checkUpdates, this, [this] {
        settings_.setUpdateState(QStringLiteral("Проверяем GitHub Releases..."), false, true);
        updates_.check();
    });
    connect(&settings_, &SettingsWindow::updateInstallRequested, this, [this] {
        if (QMessageBox::question(&settings_, QStringLiteral("Обновление SCARP ISLAND"),
                QStringLiteral("Скачать проверенный установщик и закрыть SCARP ISLAND для обновления?")) == QMessageBox::Yes)
            updates_.downloadAndInstall();
    });
    connect(&updates_, &UpdateService::statusChanged, this, [this](const QString& message) {
        if (stopping_) return;
        settings_.setUpdateState(message, updates_.hasUpdate(), updates_.busy());
    });
    connect(&updates_, &UpdateService::updateAvailable, this, [this](const QString& version, const QString& notes) {
        if (stopping_) return;
        settings_.setUpdateState(QStringLiteral("Доступна версия %1\n%2").arg(version, notes.left(3000)), true, false);
        tray_.showMessage("SCARP ISLAND", QStringLiteral("Доступна версия %1. Откройте раздел обновлений.").arg(version));
    });
    connect(&updates_, &UpdateService::readyToQuit, this, [this] { shutdown(); QCoreApplication::exit(0); });
    volumeTimer_.setInterval(VolumeRefreshIntervalMs);
    connect(&volumeTimer_, &QTimer::timeout, this, [this] {
        if (!hud_.isVisible() || demo_) return;
        sessionVolume_.refresh();
    });
    applyConfig(store_.config());
    if (demo_) {
        hud_.setVolumeAvailable(true);
        latest_.active = true; latest_.title = QStringLiteral("After Hours"); latest_.artist = QStringLiteral("Island Sessions");
        latest_.album = QStringLiteral("Midnight Collection"); latest_.source = "DEMO"; latest_.sourceId = "demo";
        latest_.position = 93; latest_.duration = 247; latest_.playing = true; latest_.canSeek = true;
        latest_.updatedAt = QDateTime::currentMSecsSinceEpoch(); latest_.cover = demoCover();
        hud_.setSnapshot(latest_);
        settings_.setStatus(QStringLiteral("Демонстрация интерфейса. Системный плеер и громкость не изменяются."));
    } else {
        hud_.setVolumeAvailable(false);
        sessionVolume_.start();
        media_.start(); volumeTimer_.start();
        if (store_.config()["check_updates"].toBool(true))
            QTimer::singleShot(5000, this, [this] {
                if (!stopping_ && store_.config()["check_updates"].toBool(true)) updates_.check();
            });
    }
    hud_.reveal();
    if (!background) showSettings();
    if (!store_.loadError().isEmpty()) setStatus(store_.loadError());
    if (!demo_) QTimer::singleShot(800, this, [this] {
        const ReleaseNotesState state(QFileInfo(store_.path()).dir().filePath("release-state.json"));
        if (!stopping_ && state.shouldShow(QString::fromLatin1(AppInfo::Version))) showReleaseNotes();
    });
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
    if (releaseNotes_) releaseNotes_->close();
    updates_.cancel();
    volumeTimer_.stop(); sessionVolume_.stop(); media_.stop(); tray_.hide(); hud_.hide(); settings_.hide();
}

void Application::setupTray() {
    trayMenu_ = std::make_unique<QMenu>();
    trayMenu_->addAction(QStringLiteral("Открыть настройки"), this, &Application::showSettings);
    trayMenu_->addAction(QStringLiteral("Показать / скрыть островок"), &hud_, &HudWindow::toggle);
    trayMenu_->addAction(QStringLiteral("Вернуть островок в исходное положение"), &hud_, &HudWindow::resetPosition);
    trayMenu_->addAction(QStringLiteral("Что нового в %1").arg(AppInfo::Version), this, &Application::showReleaseNotes);
    auto* editing = trayMenu_->addAction(QStringLiteral("Редактировать расположение")); editing->setCheckable(true);
    connect(editing, &QAction::triggered, this, &Application::setEditing);
    connect(trayMenu_.get(), &QMenu::aboutToShow, this, [this, editing] { editing->setChecked(hud_.editing()); refreshProfiles(); });
    profilesMenu_ = trayMenu_->addMenu(QStringLiteral("Профили"));
    trayMenu_->addSeparator();
    trayMenu_->addAction(QStringLiteral("Выход"), this, [this] { shutdown(); QCoreApplication::exit(0); });
    tray_.setContextMenu(trayMenu_.get());
    tray_.setToolTip(QStringLiteral("SCARP ISLAND - музыка рядом"));
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
    if (!demo_) {
        QString error;
        if (applied_["hotkey"] != config["hotkey"] && !windows_.setHotkey(config["hotkey"].toString(), &error)) {
            effective["hotkey"] = applied_["hotkey"].toString();
            setStatus(error);
        }
        if (applied_["source_id"] != config["source_id"]) {
            sessionVolume_.setSource({});
            hud_.setVolumeAvailable(false);
            media_.setSource(config["source_id"].toString());
        }
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
            14, settings_.devicePixelRatioF(), c["settings_background"].toString(), c["settings_opacity"].toDouble(.94));
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
void Application::showReleaseNotes() {
    if (stopping_) return;
    if (!releaseNotes_) {
        releaseNotes_ = new ReleaseNotesDialog(&settings_);
        releaseNotes_->setAttribute(Qt::WA_DeleteOnClose);
        connect(releaseNotes_, &QDialog::finished, this, [this] {
            if (demo_) return;
            const ReleaseNotesState state(QFileInfo(store_.path()).dir().filePath("release-state.json"));
            QString error;
            if (!state.markRead(QString::fromLatin1(AppInfo::Version), &error)) setStatus(error);
        });
    }
    releaseNotes_->showNormal(); releaseNotes_->raise(); releaseNotes_->activateWindow();
}

bool Application::capture(const QString& directory) {
    if (!QDir().mkpath(directory)) return false;
    // Demo capture renders real application widgets with deterministic state.
    // Never modify the user's settings when capturing a live media session.
    if (demo_) {
        auto config = store_.config();
        config["idle_collapse"] = false;
        config["auto_hide_seconds"] = 0;
        config["settings_animations"] = false;
        auto animations = config["animations"].toObject();
        for (auto it = animations.begin(); it != animations.end(); ++it) it.value() = false;
        config["animations"] = animations;
        if (!store_.update(config)) return false;
        hud_.reveal(true);
        settings_.resize(1080, 800);
        settings_.showNormal();
    }
    QCoreApplication::processEvents();
    const QDir output(directory);
    bool ok = hud_.grab().save(output.filePath("hud.png"));
    if (demo_) {
        const auto captureConfig = store_.config();
        for (const auto& preset : ConfigStore::presets()) {
            if (!store_.applyPreset(preset.id)) return false;
            hud_.reveal(true);
            QCoreApplication::processEvents();
            ok = hud_.grab().save(output.filePath("hud-" + preset.id + ".png")) && ok;
        }
        if (!store_.update(captureConfig)) return false;
    }
    auto* navigation = settings_.findChild<QListWidget*>("navigation");
    const int originalPage = navigation ? navigation->currentRow() : 0;
    const QList<QPair<int, QString>> captures = {{0, "settings.png"}, {1, "position.png"}, {4, "profiles.png"}, {5, "system.png"}, {6, "updates.png"}};
    for (const auto& capture : captures) {
        if (navigation) navigation->setCurrentRow(capture.first);
        QCoreApplication::processEvents();
        if (capture.first == 1) {
            auto* pages = settings_.findChild<QStackedWidget*>("settingsPages");
            if (auto* page = pages ? qobject_cast<QScrollArea*>(pages->currentWidget()) : nullptr)
                page->verticalScrollBar()->setValue(page->verticalScrollBar()->maximum());
            QCoreApplication::processEvents();
        }
        ok = settings_.grab().save(output.filePath(capture.second)) && ok;
    }
    ReleaseNotesDialog notes;
    notes.show();
    QCoreApplication::processEvents();
    ok = notes.grab().save(output.filePath("whats-new.png")) && ok;
    notes.close();
    if (navigation) navigation->setCurrentRow(originalPage);
    QFile description(output.filePath("capture-info.txt"));
    if (!description.open(QIODevice::WriteOnly | QIODevice::Text)) return false;
    description.write(QString("SCARP ISLAND %1\nPlatform: %2\nDemo: %3\n"
        "Actual Qt application widget captures. Demo media is synthetic.\n"
        "Widget captures do not verify Windows desktop compositor blur.\n")
        .arg(AppInfo::Version, QGuiApplication::platformName(), demo_ ? "yes" : "no").toUtf8());
    return ok;
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

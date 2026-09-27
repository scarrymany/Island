#pragma once

#include "AnimationClock.h"
#include <QHash>
#include <QJsonValue>
#include <QList>
#include <QPair>
#include <QWidget>

class ConfigStore;
class QCheckBox;
class QCloseEvent;
class QComboBox;
class QDoubleSpinBox;
class QFormLayout;
class QGraphicsOpacityEffect;
class QLabel;
class QListWidget;
class QPushButton;
class QSpinBox;
class QSpacerItem;
class QStackedWidget;
class QSizeGrip;
class QVBoxLayout;

class SettingsWindow final : public QWidget {
    Q_OBJECT

public:
    explicit SettingsWindow(ConfigStore* store, QWidget* parent = nullptr);
    ~SettingsWindow() override;
    void setSources(const QList<QPair<QString, QString>>& sources);
    void setStatus(const QString& message);
    void setUpdateState(const QString& message, bool available = false, bool busy = false);
    void setEditing(bool enabled);
    void refresh();

signals:
    void toggleHud();
    void editLayoutChanged(bool enabled);
    void sourceChanged(QString source);
    void checkUpdates();
    void updateInstallRequested();

protected:
    void closeEvent(QCloseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    QVBoxLayout* addPage(const QString& title, const QString& description);
    QFormLayout* addGroup(QVBoxLayout* page, const QString& title);
    void addNumber(QFormLayout* form, const QString& label, const QString& key,
                   const QString& suffix = {}, bool decimal = false);
    void addToggle(QFormLayout* form, const QString& label, const QString& key);
    void addColor(QFormLayout* form, const QString& label, const QString& key);
    QComboBox* addChoice(QFormLayout* form, const QString& label, const QString& key,
                         const QList<QPair<QString, QString>>& choices);
    void buildAppearance();
    void buildLayout();
    void buildAnimations();
    void buildSources();
    void buildProfiles();
    void buildSystem();
    void buildUpdates();
    bool put(const QString& key, const QJsonValue& value);
    void updateStyle();
    void updateMotion();
    void animatePage();
    void finishPageAnimation();
    void refreshHideDelay();
    void refreshMonitors();
    void refreshCoordinates();
    void refreshCollections();
    void updateMonitorPosition();
    void updateElementPosition();
    QString currentMonitor() const;
    void saveProfile();
    void saveTheme();
    void reportError(const QString& error);
    void applyTheme(const QString& name);

    ConfigStore* store_;
    bool refreshing_ = false;
    bool updateBusy_ = false;
    bool updateAvailable_ = false;
    bool compactSidebar_ = false;
    bool customHideDelay_ = false;
    AnimationClock motion_;
    QMetaObject::Connection refreshRateConnection_;
    QMetaObject::Connection screenConnection_;
    QGraphicsOpacityEffect* pageOpacity_ = nullptr;
    QHash<QString, QWidget*> controls_;
    QList<QPair<QString, QString>> sources_;
    QListWidget* navigation_ = nullptr;
    QStackedWidget* pages_ = nullptr;
    QLabel* heading_ = nullptr;
    QLabel* status_ = nullptr;
    QLabel* sidebarSubtitle_ = nullptr;
    QLabel* sidebarHint_ = nullptr;
    QVBoxLayout* sidebarLayout_ = nullptr;
    QSpacerItem* sidebarSpacer_ = nullptr;
    QLabel* sourceStatus_ = nullptr;
    QLabel* profileStatus_ = nullptr;
    QLabel* updateStatus_ = nullptr;
    QWidget* titleBar_ = nullptr;
    QSizeGrip* resizeGrip_ = nullptr;
    QComboBox* sourceChoice_ = nullptr;
    QComboBox* monitorChoice_ = nullptr;
    QComboBox* elementChoice_ = nullptr;
    QComboBox* profileChoice_ = nullptr;
    QComboBox* themeChoice_ = nullptr;
    QComboBox* hideDelayChoice_ = nullptr;
    QSpinBox* hideDelayValue_ = nullptr;
    QSpinBox* monitorX_ = nullptr;
    QSpinBox* monitorY_ = nullptr;
    QDoubleSpinBox* elementX_ = nullptr;
    QDoubleSpinBox* elementY_ = nullptr;
    QPushButton* editLayout_ = nullptr;
    QPushButton* updateCheck_ = nullptr;
    QPushButton* updateInstall_ = nullptr;
};

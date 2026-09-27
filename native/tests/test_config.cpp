#include "ConfigStore.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QDebug>

#include <limits>

namespace {
int failures = 0;

void check(bool condition, const char* description)
{
    if (!condition) {
        qCritical().noquote() << "FAIL:" << description;
        ++failures;
    }
}

void writeJson(const QString& path, const QJsonObject& object)
{
    QFile file(path);
    check(file.open(QIODevice::WriteOnly), "open fixture");
    const auto bytes = QJsonDocument(object).toJson();
    check(file.write(bytes) == bytes.size(), "write fixture");
}

QByteArray readFile(const QString& path)
{
    QFile file(path);
    check(file.open(QIODevice::ReadOnly), "open saved file");
    return file.readAll();
}

void invalidValues()
{
    const QList<QJsonObject> cases = {
        {{"width", true}}, {{"width", "560"}}, {{"width", 0}}, {{"width", 2000000}},
        {{"width", 600.5}}, {{"opacity", QJsonValue(std::numeric_limits<double>::quiet_NaN())}},
        {{"scale", QJsonValue(std::numeric_limits<double>::infinity())}},
        {{"background", "#12345"}}, {{"background", "red; color: transparent"}},
        {{"layout", "invalid"}}, {{"startup", 1}}, {{"unknown", 1}},
        {{"visible", QJsonObject{{"cover", "yes"}}}},
        {{"visible", QJsonObject{{"unknown", false}}}},
        {{"monitor_positions", QJsonObject{{"screen", QJsonArray{true, 4}}}}},
        {{"monitor_positions", QJsonObject{{"screen", QJsonArray{1.5, 4}}}}},
        {{"element_positions", QJsonObject{{"cover", QJsonArray{1, 2, 3}}}}},
        {{"animations", QJsonObject{{"appear", 1}}}}, {{"hotkey", "a\nexec"}},
        {{"update_repository", "https://github.com/owner/repo"}},
        {{"update_repository", "owner/../../file"}}, {{"settings_opacity", 0.01}}
    };
    for (auto config : cases) {
        QString error;
        check(!ConfigStore::validate(config, &error), "invalid config rejected");
        check(!error.isEmpty(), "validation explains failure");
    }
    QJsonObject valid{{"update_repository", "owner/my-project"}, {"visible", QJsonObject{{"cover", false}}}};
    check(ConfigStore::validate(valid), "valid partial config accepted");
    check(valid.value("visible").toObject().value("title").toBool(), "missing toggles receive defaults");
    check(valid.value("width").toInt() == 560, "missing fields receive defaults");
}
}

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    QTemporaryDir temporary;
    check(temporary.isValid(), "temporary directory available");
    const QString path = temporary.filePath("config.json");
    ConfigStore store(path);
    int changed = 0;
    QObject::connect(&store, &ConfigStore::configChanged, &store, [&changed] { ++changed; });
    auto config = store.config();
    config.insert("width", 720);
    config.insert("opacity", 0.75);
    config.insert("monitor_positions", QJsonObject{{"DISPLAY1", QJsonArray{30, 12}}});
    config.insert("element_positions", QJsonObject{{"cover", QJsonArray{12.5, 8.0}}});
    QString error;
    check(store.update(config, &error), "settings saved");
    check(changed == 1, "successful commit emits change");
    ConfigStore loaded(path);
    check(loaded.config() == config, "settings roundtrip");
    check(store.saveProfile(QStringLiteral("Рабочий стол"), &error), "profile saved");
    config.insert("width", 800);
    check(store.update(config), "active settings updated");
    check(store.loadProfile(QStringLiteral("Рабочий стол")), "profile loaded");
    check(store.config().value("width").toInt() == 720, "profile preserves snapshot");
    ConfigStore profileLoaded(path);
    check(profileLoaded.activeProfile() == QStringLiteral("Рабочий стол"), "active profile roundtrip");

    config = store.config();
    config.insert("accent_color", "#123456");
    config.insert("hotkey", "Ctrl+Alt+P");
    check(store.update(config), "theme setup");
    check(store.saveTheme("Custom"), "theme saved");
    config.insert("accent_color", "#ABCDEF");
    config.insert("width", 880);
    check(store.update(config), "theme changes setup");
    check(store.applyTheme("Custom"), "theme applied");
    check(store.config().value("accent_color").toString() == "#123456", "theme restores visual values");
    check(store.config().value("width").toInt() == 880, "theme preserves dimensions");
    check(store.config().value("hotkey").toString() == "Ctrl+Alt+P", "theme preserves system settings");
    check(!store.saveTheme("Lunar"), "built-in theme protected");
    check(!store.deleteTheme("Lunar"), "built-in theme cannot be deleted");

    const QString exported = temporary.filePath("export.json");
    check(store.exportFile(exported), "configuration exported");
    ConfigStore imported(temporary.filePath("imported.json"));
    check(imported.importFile(exported), "configuration imported");
    check(imported.config() == store.config(), "imported config matches");
    check(imported.profiles() == store.profiles(), "imported profiles match");
    check(imported.themes() == store.themes(), "imported themes match");

    const auto before = readFile(path);
    const auto beforeConfig = store.config();
    const QString malicious = temporary.filePath("malicious.json");
    writeJson(malicious, {{"schema", 1}, {"config", QJsonObject{{"width", 900}}},
                         {"profiles", QJsonObject{{"bad", QJsonObject{{"background", "red"}}}}}});
    const int beforeSignal = changed;
    check(!store.importFile(malicious, &error), "entire import validated before application");
    check(store.config() == beforeConfig, "invalid import preserves memory");
    check(readFile(path) == before, "invalid import preserves disk");
    check(changed == beforeSignal, "invalid import does not emit change");

    const QString oversized = temporary.filePath("large.json");
    {
        QFile file(oversized);
        check(file.open(QIODevice::WriteOnly), "oversized fixture opened");
        file.write(QByteArray(1024 * 1024 + 1, ' '));
    }
    check(!store.importFile(oversized), "oversized import rejected");

    const QString corrupt = temporary.filePath("broken.json");
    {
        QFile file(corrupt);
        check(file.open(QIODevice::WriteOnly), "corrupt fixture opened");
        file.write("{broken");
    }
    ConfigStore recovered(corrupt);
    check(recovered.config() == ConfigStore::defaults(), "corrupt config falls back to defaults");
    check(!recovered.loadError().isEmpty(), "corruption reported");
    check(QDir(temporary.path()).entryList({"broken.corrupt-*.json"}, QDir::Files).size() == 1, "corrupt file backed up");

    const QString legacy = temporary.filePath("legacy.json");
    writeJson(legacy, {{"width", 680}, {"background", "#112233"}});
    ConfigStore migrated(legacy);
    check(migrated.config().value("width").toInt() == 680, "legacy flat format loaded");
    check(migrated.update(migrated.config()), "legacy config persisted");
    check(QJsonDocument::fromJson(readFile(legacy)).object().value("schema").toInt() == 1, "legacy schema migrated");

    const QString unavailable = temporary.filePath("directory");
    check(QDir().mkpath(unavailable), "unwritable target fixture created");
    ConfigStore failed(unavailable);
    config = failed.config();
    config.insert("width", 900);
    check(!failed.update(config, &error), "failed write reported");
    check(failed.config() == ConfigStore::defaults(), "failed write preserves memory");
    check(QFileInfo(unavailable).isDir(), "failed write preserves target");

    check(store.deleteProfile(QStringLiteral("Рабочий стол")), "profile removed");
    check(ConfigStore(path).profiles().isEmpty(), "profile deletion persisted");
    check(store.deleteTheme("Custom"), "custom theme removed");
    check(!ConfigStore(path).themes().contains("Custom"), "theme deletion persisted");
    invalidValues();
    qInfo() << "Configuration checks completed. Failures:" << failures;
    return failures == 0 ? 0 : 1;
}

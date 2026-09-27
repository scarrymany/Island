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
        {{"font_weight", 99}}, {{"font_weight", 901}}, {{"font_weight", 650.5}}, {{"font_weight", "700"}},
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

void compactConfiguration(const QString& directory)
{
    const QJsonObject expectedDefaults{
        {"idle_collapse", true}, {"idle_collapse_seconds", 3},
        {"compact_width", 156}, {"compact_height", 32}, {"compact_visible_height", 8},
        {"compact_radius", 16}, {"compact_opacity", 0.94}, {"compact_background", "#10121B"},
        {"border_width", 0.0}, {"border_opacity", 0.22}, {"border_color", "#9B8CFF"}
    };
    const auto defaults = ConfigStore::defaults();
    for (auto field = expectedDefaults.constBegin(); field != expectedDefaults.constEnd(); ++field)
        check(defaults.value(field.key()) == field.value(), qPrintable("compact default: " + field.key()));
    check(defaults.value("animations").toObject().value("dock").toBool(), "dock animation enabled by default");

    struct NumericField {
        QString name;
        double minimum;
        double maximum;
        bool integer;
    };
    const QList<NumericField> numericFields = {
        {"idle_collapse_seconds", 1, 120, true}, {"compact_width", 48, 500, true},
        {"compact_height", 8, 96, true}, {"compact_visible_height", 2, 96, true},
        {"compact_radius", 0, 48, true}, {"compact_opacity", 0.1, 1, false},
        {"border_width", 0, 4, false}, {"border_opacity", 0, 1, false}
    };
    for (const auto& field : numericFields) {
        for (double accepted : {field.minimum, field.maximum}) {
            QJsonObject config{{field.name, accepted}};
            check(ConfigStore::validate(config), qPrintable("compact boundary accepted: " + field.name));
        }
        for (double rejected : {field.minimum - 0.01, field.maximum + 0.01}) {
            QJsonObject config{{field.name, rejected}};
            check(!ConfigStore::validate(config), qPrintable("compact boundary rejected: " + field.name));
        }
        QJsonObject wrongType{{field.name, true}};
        check(!ConfigStore::validate(wrongType), qPrintable("compact boolean rejected: " + field.name));
        if (field.integer) {
            QJsonObject fractional{{field.name, field.minimum + 0.5}};
            check(!ConfigStore::validate(fractional), qPrintable("compact fraction rejected: " + field.name));
        }
    }
    QJsonObject fractionalBorder{{"border_width", 0.75}};
    check(ConfigStore::validate(fractionalBorder), "fractional border width supported");
    QJsonObject tallStrip{{"compact_height", 8}, {"compact_visible_height", 96}};
    check(ConfigStore::validate(tallStrip), "strip height can be clamped by renderer");
    for (QJsonObject rejected : {
            QJsonObject{{"idle_collapse", 1}}, QJsonObject{{"animations", QJsonObject{{"dock", "yes"}}}},
            QJsonObject{{"border_color", "red"}}, QJsonObject{{"compact_background", "#12345"}}})
        check(!ConfigStore::validate(rejected), "compact invalid type or color rejected");

    const QString legacyPath = QDir(directory).filePath("legacy-compact.json");
    const QJsonObject oldConfig{
        {"width", 680}, {"background", "#112233"}, {"auto_hide_seconds", 12},
        {"animations", QJsonObject{{"appear", false}}}
    };
    writeJson(legacyPath, {{"schema", 1}, {"config", oldConfig},
        {"profiles", QJsonObject{{"Legacy", oldConfig}}},
        {"themes", QJsonObject{{"Legacy theme", QJsonObject{{"background", "#223344"}}}}},
        {"active_profile", "Legacy"}});
    ConfigStore migrated(legacyPath);
    check(migrated.loadError().isEmpty(), "old schema remains readable");
    for (auto field = expectedDefaults.constBegin(); field != expectedDefaults.constEnd(); ++field)
        check(migrated.config().value(field.key()) == field.value(), qPrintable("legacy receives compact default: " + field.key()));
    check(migrated.config().value("auto_hide_seconds").toInt() == 12, "existing hide timeout preserved");
    check(!migrated.config().value("animations").toObject().value("appear").toBool(), "existing animation choice preserved");
    check(migrated.config().value("animations").toObject().value("dock").toBool(), "old animations receive dock default");
    check(migrated.loadProfile("Legacy"), "legacy profile accepted");
    check(migrated.config().value("compact_visible_height").toInt() == 8, "legacy profile receives compact defaults");
    check(migrated.applyTheme("Legacy theme"), "legacy partial theme accepted");

    const QString path = QDir(directory).filePath("compact-roundtrip.json");
    ConfigStore store(path);
    auto styled = store.config();
    const QJsonObject style{
        {"compact_radius", 12}, {"compact_opacity", 0.73}, {"compact_background", "#334455"},
        {"border_width", 0.75}, {"border_opacity", 0.38}, {"border_color", "#ABCDEF"}
    };
    for (auto field = style.constBegin(); field != style.constEnd(); ++field)
        styled.insert(field.key(), field.value());
    styled.insert("idle_collapse_seconds", 11);
    styled.insert("compact_width", 184);
    styled.insert("compact_height", 40);
    styled.insert("compact_visible_height", 12);
    auto animations = styled.value("animations").toObject();
    animations.insert("dock", false);
    styled.insert("animations", animations);
    check(store.update(styled), "compact custom config saved");
    check(ConfigStore(path).config() == styled, "compact settings roundtrip");
    check(store.saveProfile("Compact profile"), "compact profile saved");
    check(store.saveTheme("Compact theme"), "compact theme saved");
    auto altered = ConfigStore::defaults();
    altered.insert("idle_collapse", false);
    altered.insert("idle_collapse_seconds", 7);
    altered.insert("compact_width", 220);
    check(store.update(altered), "compact theme application setup");
    check(store.applyTheme("Compact theme"), "compact theme applied");
    for (auto field = style.constBegin(); field != style.constEnd(); ++field)
        check(store.config().value(field.key()) == field.value(), qPrintable("compact theme restores style: " + field.key()));
    check(!store.config().value("idle_collapse").toBool(), "theme preserves collapse toggle");
    check(store.config().value("idle_collapse_seconds").toInt() == 7, "theme preserves collapse delay");
    check(store.config().value("compact_width").toInt() == 220, "theme preserves compact dimensions");
    check(store.config().value("animations").toObject().value("dock").toBool(), "theme preserves dock animation preference");
    ConfigStore reloaded(path);
    check(reloaded.loadProfile("Compact profile"), "compact saved profile loaded");
    check(reloaded.config() == styled, "profile restores all compact behavior and styling");
}

void artworkConfiguration(const QString& directory)
{
    const auto defaults = ConfigStore::defaults();
    check(defaults.value("artwork_background") == QJsonValue(true), "artwork background enabled by default");
    check(defaults.value("artwork_background_strength") == QJsonValue(0.75), "artwork background default strength");
    check(ConfigStore::numericRange("artwork_background_strength") == qMakePair(0.0, 1.0), "artwork strength control range");
    for (double strength : {0.0, 0.375, 1.0}) {
        QJsonObject config{{"artwork_background_strength", strength}};
        check(ConfigStore::validate(config), "artwork strength accepts endpoints and fractions");
        check(config.value("artwork_background_strength").toDouble() == strength, "artwork strength retained exactly");
    }
    const QList<QJsonObject> invalid = {
        {{"artwork_background", 1}}, {{"artwork_background", "true"}}, {{"artwork_background", QJsonValue::Null}},
        {{"artwork_background_strength", true}}, {{"artwork_background_strength", "0.75"}},
        {{"artwork_background_strength", -0.001}}, {{"artwork_background_strength", 1.001}},
        {{"artwork_background_strength", QJsonValue(std::numeric_limits<double>::infinity())}},
        {{"artwork_background_strength", QJsonValue(std::numeric_limits<double>::quiet_NaN())}}
    };
    for (auto config : invalid) {
        QString error;
        check(!ConfigStore::validate(config, &error), "invalid artwork option rejected");
        check(!error.isEmpty(), "invalid artwork option reports the failure");
    }

    const QString legacyPath = QDir(directory).filePath("legacy-artwork.json");
    const QJsonObject legacyConfig{{"background", "#112233"}, {"compact_width", 208},
        {"compact_visible_height", 12}, {"idle_collapse_seconds", 9}};
    writeJson(legacyPath, {{"schema", 1}, {"config", legacyConfig},
        {"profiles", QJsonObject{{"Existing profile", legacyConfig}}},
        {"themes", QJsonObject{{"Existing theme", QJsonObject{{"background", "#334455"}}}}}});
    ConfigStore legacy(legacyPath);
    check(legacy.loadError().isEmpty(), "existing configuration loads with artwork defaults");
    check(legacy.config().value("artwork_background") == QJsonValue(true), "existing config receives artwork toggle");
    check(legacy.config().value("artwork_background_strength") == QJsonValue(0.75), "existing config receives artwork strength");
    check(legacy.loadProfile("Existing profile"), "existing profile remains loadable");
    check(legacy.config().value("artwork_background_strength") == QJsonValue(0.75), "existing profile receives artwork strength");
    for (auto field = legacyConfig.constBegin(); field != legacyConfig.constEnd(); ++field)
        check(legacy.config().value(field.key()) == field.value(), "artwork migration preserves prior settings");

    auto customized = legacy.config();
    customized.insert("artwork_background", false);
    customized.insert("artwork_background_strength", 0.375);
    check(legacy.update(customized), "disabled artwork background saved with custom strength");
    check(legacy.applyTheme("Existing theme"), "old partial theme remains applicable");
    check(legacy.config().value("artwork_background") == QJsonValue(false), "old theme preserves artwork preference");
    check(legacy.config().value("artwork_background_strength") == QJsonValue(0.375), "old theme preserves artwork strength");
    customized = legacy.config();
    check(legacy.saveProfile("Artwork profile"), "artwork profile saved");
    check(legacy.saveTheme("Artwork theme"), "artwork theme saved");
    const QString exported = QDir(directory).filePath("artwork-export.json");
    check(legacy.exportFile(exported), "artwork settings exported");
    ConfigStore imported(QDir(directory).filePath("artwork-import.json"));
    check(imported.importFile(exported), "artwork settings imported");
    check(imported.config() == customized, "artwork configuration import roundtrip");
    check(imported.loadProfile("Artwork profile"), "imported artwork profile loaded");
    check(imported.config() == customized, "artwork profile import roundtrip");
    auto altered = ConfigStore::defaults();
    altered.insert("compact_width", 224);
    check(imported.update(altered), "artwork theme setup");
    check(imported.applyTheme("Artwork theme"), "imported artwork theme applied");
    check(imported.config().value("artwork_background") == QJsonValue(false), "theme restores disabled artwork background");
    check(imported.config().value("artwork_background_strength") == QJsonValue(0.375), "theme restores artwork strength");
    check(imported.config().value("compact_width").toInt() == 224, "artwork theme preserves docking dimensions");
    ConfigStore reloaded(imported.path());
    check(reloaded.config() == imported.config(), "artwork options persist after reload");
    check(reloaded.applyTheme("Lunar"), "built-in theme applies with artwork fields");
    check(reloaded.config().value("artwork_background") == QJsonValue(true), "Lunar restores default artwork toggle");
    check(reloaded.config().value("artwork_background_strength") == QJsonValue(0.75), "Lunar restores default artwork strength");
}

void settingsAppearanceConfiguration(const QString& directory)
{
    const QJsonObject expected{{"settings_animations", true}, {"settings_animation_duration", 200},
        {"settings_font_family", "Inter"}};
    const auto defaults = ConfigStore::defaults();
    for (auto field = expected.constBegin(); field != expected.constEnd(); ++field)
        check(defaults.value(field.key()) == field.value(), qPrintable("settings appearance default: " + field.key()));
    check(defaults.value("font_family").toString() == "Inter", "HUD uses the bundled Inter font");
    check(defaults.value("font_weight").toInt() == 600, "HUD uses semibold text by default");
    check(ConfigStore::numericRange("settings_animation_duration") == qMakePair(80.0, 600.0), "settings animation duration control range");
    for (int duration : {80, 600}) {
        QJsonObject config{{"settings_animation_duration", duration}};
        check(ConfigStore::validate(config), "settings animation duration accepts boundaries");
        check(config.value("settings_animation_duration").toInt() == duration, "settings duration boundary retained");
    }
    QJsonObject longestFont{{"settings_font_family", QString(120, 'A')}};
    check(ConfigStore::validate(longestFont), "settings font accepts maximum length");
    const QList<QJsonObject> invalid = {
        {{"settings_animations", 1}}, {{"settings_animations", "false"}}, {{"settings_animations", QJsonValue::Null}},
        {{"settings_animation_duration", 79}}, {{"settings_animation_duration", 601}},
        {{"settings_animation_duration", 80.5}}, {{"settings_animation_duration", true}},
        {{"settings_animation_duration", "200"}}, {{"settings_animation_duration", QJsonValue::Null}},
        {{"settings_font_family", 600}}, {{"settings_font_family", QJsonValue::Null}},
        {{"settings_font_family", ""}}, {{"settings_font_family", "   "}},
        {{"settings_font_family", "Manrope\nSegoe UI"}}, {{"settings_font_family", "Manrope\t"}},
        {{"settings_font_family", QString(121, 'A')}}
    };
    for (auto config : invalid) {
        QString error;
        check(!ConfigStore::validate(config, &error), "invalid settings appearance rejected");
        check(!error.isEmpty(), "invalid settings appearance explains failure");
    }

    const QString path = QDir(directory).filePath("legacy-settings-appearance.json");
    const QJsonObject oldConfig{{"font_family", "Consolas"}, {"settings_font_size", 12},
        {"artwork_background_strength", 0.5}, {"compact_width", 220}};
    writeJson(path, {{"schema", 1}, {"config", oldConfig},
        {"profiles", QJsonObject{{"Old profile", oldConfig}}},
        {"themes", QJsonObject{{"Old theme", QJsonObject{{"settings_background", "#112233"}}}}}});
    ConfigStore store(path);
    check(store.loadError().isEmpty(), "old settings appearance config remains readable");
    for (auto field = expected.constBegin(); field != expected.constEnd(); ++field)
        check(store.config().value(field.key()) == field.value(), qPrintable("old config receives settings default: " + field.key()));
    check(store.loadProfile("Old profile"), "old settings appearance profile remains readable");
    for (auto field = expected.constBegin(); field != expected.constEnd(); ++field)
        check(store.config().value(field.key()) == field.value(), qPrintable("old profile receives settings default: " + field.key()));
    for (auto field = oldConfig.constBegin(); field != oldConfig.constEnd(); ++field)
        check(store.config().value(field.key()) == field.value(), "settings appearance migration preserves existing options");

    const QJsonObject custom{{"settings_animations", false}, {"settings_animation_duration", 360},
        {"settings_font_family", "Noto Sans"}};
    auto configured = store.config();
    for (auto field = custom.constBegin(); field != custom.constEnd(); ++field)
        configured.insert(field.key(), field.value());
    check(store.update(configured), "custom settings animation and font saved");
    check(store.applyTheme("Old theme"), "old partial settings theme remains applicable");
    for (auto field = custom.constBegin(); field != custom.constEnd(); ++field)
        check(store.config().value(field.key()) == field.value(), "old theme preserves settings animation and font choices");
    configured = store.config();
    check(store.saveProfile("Settings profile"), "settings appearance profile saved");
    check(store.saveTheme("Settings theme"), "settings appearance theme saved");
    const QString exported = QDir(directory).filePath("settings-appearance-export.json");
    check(store.exportFile(exported), "settings appearance exported");
    ConfigStore imported(QDir(directory).filePath("settings-appearance-import.json"));
    check(imported.importFile(exported), "settings appearance imported");
    check(imported.config() == configured, "settings appearance import roundtrip");
    check(imported.update(defaults), "settings appearance profile restore setup");
    check(imported.loadProfile("Settings profile"), "imported settings appearance profile loaded");
    check(imported.config() == configured, "settings appearance profile restores all options");
    check(imported.update(defaults), "settings appearance theme restore setup");
    check(imported.applyTheme("Settings theme"), "imported settings appearance theme applied");
    for (auto field = custom.constBegin(); field != custom.constEnd(); ++field)
        check(imported.config().value(field.key()) == field.value(), qPrintable("theme restores settings appearance: " + field.key()));
    ConfigStore reloaded(imported.path());
    check(reloaded.config() == imported.config(), "settings appearance persists across reload");
    check(reloaded.applyTheme("Lunar"), "Lunar theme restores settings appearance defaults");
    for (auto field = expected.constBegin(); field != expected.constEnd(); ++field)
        check(reloaded.config().value(field.key()) == field.value(), qPrintable("Lunar restores settings appearance: " + field.key()));
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
    compactConfiguration(temporary.path());
    artworkConfiguration(temporary.path());
    settingsAppearanceConfiguration(temporary.path());
    qInfo() << "Configuration checks completed. Failures:" << failures;
    return failures == 0 ? 0 : 1;
}

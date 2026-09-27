#include "ConfigStore.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QMap>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>

#include <cmath>
#include <utility>

namespace {
constexpr qint64 MaxFileBytes = 1024 * 1024;
constexpr int MaxSavedItems = 64;
constexpr int SchemaVersion = 1;
constexpr double MaxCoordinate = 32768.0;

const QStringList Elements = {
    "cover", "title", "artist", "album", "source", "progress", "time", "previous", "play", "next", "volume"
};
const QStringList Animations = {"appear", "disappear", "cover", "title", "progress", "hover", "play"};
const QSet<QString> ColorFields = {
    "background", "gradient_color", "text_color", "secondary_color", "accent_color",
    "progress_color", "icon_color", "settings_background", "settings_accent", "settings_text"
};
const QSet<QString> ThemeFields = ColorFields | QSet<QString>{
    "opacity", "blur", "gradient_enabled", "radius", "font_family", "font_size",
    "icon_size", "progress_height", "settings_font_size", "settings_opacity", "settings_blur"
};
const QMap<QString, QPair<double, double>> NumericRanges = {
    {"width", {260, 2000}}, {"height", {64, 600}}, {"scale", {0.5, 2.5}},
    {"cover_size", {24, 240}}, {"radius", {0, 160}}, {"opacity", {0.1, 1}},
    {"progress_height", {1, 20}}, {"icon_size", {10, 48}}, {"font_size", {8, 36}},
    {"spacing", {0, 80}}, {"animation_duration", {0, 3000}}, {"offset_y", {0, 4000}},
    {"auto_hide_seconds", {0, 3600}}, {"settings_font_size", {8, 18}}, {"settings_opacity", {0.3, 1}}
};
const QSet<QString> DecimalFields = {"scale", "opacity", "settings_opacity"};
const QMap<QString, int> StringLimits = {
    {"font_family", 120}, {"hotkey", 80}, {"monitor", 256}, {"source_id", 512}, {"update_repository", 140}
};

bool fail(QString* error, const QString& message)
{
    if (error)
        *error = message;
    return false;
}

bool validName(const QString& name, int limit = 80, bool allowEmpty = false)
{
    if (name.size() > limit || (!allowEmpty && name.trimmed().isEmpty()))
        return false;
    for (const QChar character : name) {
        if (character.unicode() < 32 || character.unicode() == 127)
            return false;
    }
    return true;
}

QJsonObject builtinThemes()
{
    QJsonObject lunar;
    const auto defaults = ConfigStore::defaults();
    for (const auto& key : ThemeFields)
        lunar.insert(key, defaults.value(key));
    return {
        {"Lunar", lunar},
        {"Midnight", QJsonObject{
            {"background", "#09121D"}, {"gradient_color", "#142D45"}, {"accent_color", "#70CDFF"},
            {"progress_color", "#70CDFF"}, {"text_color", "#F1F8FF"}, {"secondary_color", "#88A7BD"},
            {"icon_color", "#F1F8FF"}, {"settings_accent", "#70CDFF"}}},
        {"Ember", QJsonObject{
            {"background", "#1B1212"}, {"gradient_color", "#3C2324"}, {"accent_color", "#FFAD87"},
            {"progress_color", "#FFAD87"}, {"text_color", "#FFF4EF"}, {"secondary_color", "#B89A91"},
            {"icon_color", "#FFF4EF"}, {"settings_accent", "#FFAD87"}}},
        {"Mono", QJsonObject{
            {"background", "#151515"}, {"gradient_color", "#242424"}, {"accent_color", "#E5E5E5"},
            {"progress_color", "#E5E5E5"}, {"text_color", "#FFFFFF"}, {"secondary_color", "#A0A0A0"},
            {"icon_color", "#FFFFFF"}, {"settings_accent", "#E5E5E5"}, {"gradient_enabled", false}}}
    };
}

bool readDocument(const QString& path, QJsonObject& object, QString* error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return fail(error, QStringLiteral("Не удалось прочитать файл: %1").arg(file.errorString()));
    const QByteArray bytes = file.read(MaxFileBytes + 1);
    if (file.error() != QFileDevice::NoError)
        return fail(error, file.errorString());
    if (bytes.size() > MaxFileBytes)
        return fail(error, QStringLiteral("Размер файла настроек превышает 1 МБ"));
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        return fail(error, QStringLiteral("Файл не содержит корректный JSON-объект: %1").arg(parseError.errorString()));
    object = document.object();
    return true;
}

bool writeDocument(const QString& path, const QJsonObject& object, QString* error)
{
    const QByteArray bytes = QJsonDocument(object).toJson(QJsonDocument::Indented);
    if (bytes.size() > MaxFileBytes)
        return fail(error, QStringLiteral("Настройки превышают допустимый размер 1 МБ"));
    const QFileInfo info(path);
    if (!QDir().mkpath(info.absolutePath()))
        return fail(error, QStringLiteral("Не удалось создать папку настроек"));
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly))
        return fail(error, file.errorString());
    if (file.write(bytes) != bytes.size()) {
        file.cancelWriting();
        return fail(error, file.errorString());
    }
    if (!file.commit())
        return fail(error, file.errorString());
    return true;
}
}

ConfigStore::ConfigStore(QString path, QObject* parent)
    : QObject(parent), path_(std::move(path)), config_(defaults())
{
    if (path_.isEmpty()) {
        QString base = qEnvironmentVariable("LOCALAPPDATA");
        if (base.isEmpty())
            base = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
        path_ = QDir(base).filePath("Island/config.json");
    }
    if (!QFileInfo::exists(path_))
        return;
    QJsonObject loaded;
    QFile readable(path_);
    if (!readable.open(QIODevice::ReadOnly)) {
        loadError_ = QStringLiteral("Не удалось прочитать настройки: %1").arg(readable.errorString());
        return;
    }
    readable.close();
    if (readDocument(path_, loaded, &loadError_) && validateDocument(loaded, &loadError_)) {
        accept(loaded);
        return;
    }
    const QFileInfo info(path_);
    const QString backup = info.dir().filePath(info.completeBaseName() + ".corrupt-"
        + QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss-zzz") + ".json");
    if (QFile::rename(path_, backup))
        loadError_ += QStringLiteral(". Исходный файл сохранён: %1").arg(backup);
    else
        loadError_ += QStringLiteral(". Не удалось создать резервную копию");
}

QJsonObject ConfigStore::defaults()
{
    QJsonObject visible;
    for (const auto& key : Elements)
        visible.insert(key, key != "album");
    QJsonObject animations;
    for (const auto& key : Animations)
        animations.insert(key, true);
    return {
        {"width", 560}, {"height", 132}, {"scale", 1.0}, {"cover_size", 76}, {"radius", 30},
        {"opacity", 0.94}, {"blur", true}, {"background", "#10121B"}, {"gradient_enabled", true},
        {"gradient_color", "#242344"}, {"text_color", "#F5F5FA"}, {"secondary_color", "#9394AB"},
        {"accent_color", "#9B8CFF"}, {"progress_color", "#9B8CFF"}, {"progress_height", 3},
        {"icon_color", "#F5F5FA"}, {"icon_size", 18}, {"font_family", "Segoe UI"}, {"font_size", 13},
        {"spacing", 16}, {"layout", "island"}, {"visible", visible}, {"element_positions", QJsonObject{}},
        {"animations", animations}, {"animation_duration", 260}, {"monitor", ""},
        {"monitor_positions", QJsonObject{}}, {"anchor", "top_center"}, {"offset_y", 12},
        {"auto_hide_seconds", 0}, {"hotkey", "Ctrl+Alt+M"}, {"click_through", false}, {"startup", false},
        {"source_id", ""}, {"settings_background", "#0D0F17"}, {"settings_accent", "#9B8CFF"},
        {"settings_text", "#F5F5FA"}, {"settings_font_size", 10}, {"settings_opacity", 0.94},
        {"settings_blur", true}, {"update_repository", "scarrymany/Island"}, {"check_updates", true}
    };
}

QPair<double, double> ConfigStore::numericRange(const QString& field)
{
    return NumericRanges.value(field);
}

bool ConfigStore::validate(QJsonObject& config, QString* error)
{
    if (error)
        error->clear();
    QJsonObject merged = defaults();
    static const QRegularExpression colorPattern("^#[0-9a-fA-F]{6}$");
    static const QRegularExpression repositoryPattern("^[A-Za-z0-9](?:[A-Za-z0-9-]{0,37}[A-Za-z0-9])?/[A-Za-z0-9][A-Za-z0-9_.-]{0,99}$");
    for (auto it = config.constBegin(); it != config.constEnd(); ++it) {
        const QString key = it.key();
        QJsonValue value = it.value();
        if (!merged.contains(key))
            return fail(error, QStringLiteral("Неизвестная настройка: %1").arg(key));
        if (NumericRanges.contains(key)) {
            const auto range = NumericRanges.value(key);
            const double number = value.toDouble();
            if (!value.isDouble() || !std::isfinite(number) || number < range.first || number > range.second
                || (!DecimalFields.contains(key) && std::trunc(number) != number))
                return fail(error, QStringLiteral("%1: требуется число от %2 до %3").arg(key).arg(range.first).arg(range.second));
        } else if (ColorFields.contains(key)) {
            if (!value.isString() || !colorPattern.match(value.toString()).hasMatch())
                return fail(error, QStringLiteral("%1: цвет должен иметь формат #RRGGBB").arg(key));
        } else if (StringLimits.contains(key)) {
            if (!value.isString() || !validName(value.toString(), StringLimits.value(key), key != "font_family"))
                return fail(error, QStringLiteral("%1: недопустимая строка").arg(key));
            if (key == "update_repository" && !value.toString().isEmpty()
                && !repositoryPattern.match(value.toString()).hasMatch())
                return fail(error, QStringLiteral("Репозиторий указывается в формате owner/repository"));
        } else if (key == "layout" || key == "anchor") {
            const QStringList choices = key == "layout" ? QStringList{"island", "stacked", "custom"}
                                                        : QStringList{"top_center", "free"};
            if (!value.isString() || !choices.contains(value.toString()))
                return fail(error, QStringLiteral("%1: недопустимый вариант").arg(key));
        } else if (key == "visible" || key == "animations") {
            if (!value.isObject())
                return fail(error, QStringLiteral("%1: требуется объект переключателей").arg(key));
            QJsonObject flags = merged.value(key).toObject();
            const auto input = value.toObject();
            for (auto flag = input.constBegin(); flag != input.constEnd(); ++flag) {
                if (!flags.contains(flag.key()) || !flag.value().isBool())
                    return fail(error, QStringLiteral("%1: неизвестный или некорректный переключатель").arg(key));
                flags.insert(flag.key(), flag.value());
            }
            value = flags;
        } else if (key == "element_positions" || key == "monitor_positions") {
            if (!value.isObject() || value.toObject().size() > MaxSavedItems)
                return fail(error, QStringLiteral("%1: недопустимая карта координат").arg(key));
            const auto positions = value.toObject();
            for (auto position = positions.constBegin(); position != positions.constEnd(); ++position) {
                if (!validName(position.key(), 256) || (key == "element_positions" && !Elements.contains(position.key()))
                    || !position.value().isArray() || position.value().toArray().size() != 2)
                    return fail(error, QStringLiteral("%1: недопустимый элемент или координаты").arg(key));
                for (const auto& point : position.value().toArray()) {
                    const double coordinate = point.toDouble();
                    if (!point.isDouble() || !std::isfinite(coordinate) || std::abs(coordinate) > MaxCoordinate
                        || (key == "monitor_positions" && std::trunc(coordinate) != coordinate))
                        return fail(error, QStringLiteral("%1: координаты выходят за допустимые границы").arg(key));
                }
            }
        } else if (merged.value(key).isBool() && !value.isBool()) {
            return fail(error, QStringLiteral("%1: требуется логическое значение").arg(key));
        }
        merged.insert(key, value);
    }
    config = merged;
    return true;
}

bool ConfigStore::validateDocument(QJsonObject& document, QString* error)
{
    if (error)
        error->clear();
    if (!document.contains("schema") && !document.contains("config"))
        document = {{"schema", SchemaVersion}, {"config", document}};
    const auto schema = document.value("schema");
    if (!schema.isUndefined() && (!schema.isDouble() || (schema.toDouble() != 0 && schema.toDouble() != SchemaVersion)))
        return fail(error, QStringLiteral("Версия файла настроек не поддерживается"));
    const QSet<QString> allowed = {"schema", "config", "profiles", "themes", "active_profile"};
    for (const auto& key : document.keys()) {
        if (!allowed.contains(key))
            return fail(error, QStringLiteral("Неизвестный раздел файла: %1").arg(key));
    }
    if (!document.contains("config") || !document.value("config").isObject())
        return fail(error, QStringLiteral("В файле отсутствует объект config"));
    QJsonObject config = document.value("config").toObject();
    if (!validate(config, error))
        return false;
    document.insert("config", config);
    for (const QString& section : {QStringLiteral("profiles"), QStringLiteral("themes")}) {
        const auto value = document.value(section);
        if (!value.isUndefined() && (!value.isObject() || value.toObject().size() > MaxSavedItems))
            return fail(error, QStringLiteral("%1: недопустимый раздел").arg(section));
        QJsonObject entries = value.toObject();
        for (auto it = entries.begin(); it != entries.end(); ++it) {
            if (!validName(it.key()) || !it.value().isObject())
                return fail(error, QStringLiteral("%1: недопустимое название или значение").arg(section));
            QJsonObject entry = it.value().toObject();
            if (section == "themes") {
                if (builtinThemes().contains(it.key()) || entry.isEmpty())
                    return fail(error, QStringLiteral("Нельзя заменить встроенную тему или сохранить пустую тему"));
                for (const auto& key : entry.keys()) {
                    if (!ThemeFields.contains(key))
                        return fail(error, QStringLiteral("Недопустимое поле темы: %1").arg(key));
                }
                auto validated = entry;
                if (!validate(validated, error))
                    return false;
            } else if (!validate(entry, error)) {
                return false;
            }
            it.value() = entry;
        }
        document.insert(section, entries);
    }
    const auto active = document.value("active_profile");
    if (!active.isUndefined() && (!active.isString() || !validName(active.toString(), 80, true)))
        return fail(error, QStringLiteral("Некорректное название активного профиля"));
    if (!active.toString().isEmpty() && !document.value("profiles").toObject().contains(active.toString()))
        return fail(error, QStringLiteral("Активный профиль отсутствует в файле"));
    document.insert("active_profile", active.toString());
    document.insert("schema", SchemaVersion);
    return true;
}

QJsonObject ConfigStore::config() const { return config_; }
QStringList ConfigStore::profiles() const { return profiles_.keys(); }
QString ConfigStore::activeProfile() const { return activeProfile_; }
QString ConfigStore::loadError() const { return loadError_; }
QString ConfigStore::path() const { return path_; }

QStringList ConfigStore::themes() const
{
    auto names = builtinThemes().keys();
    names.append(themes_.keys());
    return names;
}

QJsonObject ConfigStore::document() const
{
    return {{"schema", SchemaVersion}, {"config", config_}, {"profiles", profiles_},
            {"themes", themes_}, {"active_profile", activeProfile_}};
}

void ConfigStore::accept(const QJsonObject& document)
{
    config_ = document.value("config").toObject();
    profiles_ = document.value("profiles").toObject();
    themes_ = document.value("themes").toObject();
    activeProfile_ = document.value("active_profile").toString();
}

bool ConfigStore::commit(QJsonObject document, QString* error)
{
    if (!validateDocument(document, error) || !writeDocument(path_, document, error))
        return false;
    accept(document);
    emit configChanged(config_);
    return true;
}

bool ConfigStore::update(const QJsonObject& config, QString* error)
{
    auto next = document();
    next.insert("config", config);
    return commit(next, error);
}

bool ConfigStore::saveProfile(const QString& name, QString* error)
{
    const QString normalized = name.trimmed();
    if (!validName(normalized))
        return fail(error, QStringLiteral("Введите название профиля длиной до 80 символов"));
    auto next = document();
    auto profiles = profiles_;
    profiles.insert(normalized, config_);
    next.insert("profiles", profiles);
    next.insert("active_profile", normalized);
    return commit(next, error);
}

bool ConfigStore::loadProfile(const QString& name, QString* error)
{
    if (!profiles_.contains(name))
        return fail(error, QStringLiteral("Профиль не найден"));
    auto next = document();
    next.insert("config", profiles_.value(name));
    next.insert("active_profile", name);
    return commit(next, error);
}

bool ConfigStore::deleteProfile(const QString& name, QString* error)
{
    if (!profiles_.contains(name))
        return fail(error, QStringLiteral("Профиль не найден"));
    auto next = document();
    auto profiles = profiles_;
    profiles.remove(name);
    next.insert("profiles", profiles);
    if (activeProfile_ == name)
        next.insert("active_profile", "");
    return commit(next, error);
}

bool ConfigStore::saveTheme(const QString& name, QString* error)
{
    const QString normalized = name.trimmed();
    if (!validName(normalized) || builtinThemes().contains(normalized))
        return fail(error, QStringLiteral("Укажите название своей темы, отличающееся от встроенных"));
    QJsonObject theme;
    for (const auto& key : ThemeFields)
        theme.insert(key, config_.value(key));
    auto next = document();
    auto themes = themes_;
    themes.insert(normalized, theme);
    next.insert("themes", themes);
    return commit(next, error);
}

bool ConfigStore::applyTheme(const QString& name, QString* error)
{
    const auto builtin = builtinThemes();
    if (!builtin.contains(name) && !themes_.contains(name))
        return fail(error, QStringLiteral("Тема не найдена"));
    const auto theme = (builtin.contains(name) ? builtin.value(name) : themes_.value(name)).toObject();
    auto config = config_;
    for (auto it = theme.constBegin(); it != theme.constEnd(); ++it)
        config.insert(it.key(), it.value());
    return update(config, error);
}

bool ConfigStore::deleteTheme(const QString& name, QString* error)
{
    if (!themes_.contains(name))
        return fail(error, QStringLiteral("Встроенные темы нельзя удалить"));
    auto next = document();
    auto themes = themes_;
    themes.remove(name);
    next.insert("themes", themes);
    return commit(next, error);
}

bool ConfigStore::exportFile(const QString& path, QString* error) const
{
    if (error)
        error->clear();
    return writeDocument(path, document(), error);
}

bool ConfigStore::importFile(const QString& path, QString* error)
{
    QJsonObject incoming;
    if (!readDocument(path, incoming, error))
        return false;
    return commit(incoming, error);
}

#pragma once

#include <QJsonObject>
#include <QObject>
#include <QPair>
#include <QStringList>

class ConfigStore final : public QObject {
    Q_OBJECT

public:
    explicit ConfigStore(QString path = {}, QObject* parent = nullptr);

    [[nodiscard]] QJsonObject config() const;
    [[nodiscard]] QStringList profiles() const;
    [[nodiscard]] QStringList themes() const;
    [[nodiscard]] QString activeProfile() const;
    [[nodiscard]] QString loadError() const;
    [[nodiscard]] QString path() const;

    bool update(const QJsonObject& config, QString* error = nullptr);
    bool saveProfile(const QString& name, QString* error = nullptr);
    bool loadProfile(const QString& name, QString* error = nullptr);
    bool deleteProfile(const QString& name, QString* error = nullptr);
    bool saveTheme(const QString& name, QString* error = nullptr);
    bool applyTheme(const QString& name, QString* error = nullptr);
    bool deleteTheme(const QString& name, QString* error = nullptr);
    bool exportFile(const QString& path, QString* error = nullptr) const;
    bool importFile(const QString& path, QString* error = nullptr);

    [[nodiscard]] static QJsonObject defaults();
    [[nodiscard]] static QPair<double, double> numericRange(const QString& field);
    static bool validate(QJsonObject& config, QString* error = nullptr);

signals:
    void configChanged(QJsonObject config);

private:
    [[nodiscard]] QJsonObject document() const;
    bool commit(QJsonObject document, QString* error);
    void accept(const QJsonObject& document);
    static bool validateDocument(QJsonObject& document, QString* error);

    QString path_;
    QString loadError_;
    QJsonObject config_;
    QJsonObject profiles_;
    QJsonObject themes_;
    QString activeProfile_;
};

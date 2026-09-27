#pragma once

#include <QList>
#include <QMetaType>
#include <QObject>
#include <QString>

#include <memory>

struct SessionVolumeState {
    QString sourceId;
    bool available = false;
    double volume = 0.0;
    bool muted = false;
    int sessionCount = 0;
    bool operator==(const SessionVolumeState&) const = default;
};

Q_DECLARE_METATYPE(SessionVolumeState)

namespace SessionVolumeMatching {
struct ProcessIdentity {
    QString executablePath;
    QString appUserModelId;
    QString packageFamily;
};

struct Reading {
    double volume = 0.0;
    bool muted = false;
    bool active = false;
};

[[nodiscard]] bool matches(const QString& sourceId, const ProcessIdentity& process);
[[nodiscard]] SessionVolumeState summarize(const QString& sourceId, const QList<Reading>& readings);
}

class SessionVolume final : public QObject {
    Q_OBJECT

public:
    explicit SessionVolume(QObject* parent = nullptr);
    ~SessionVolume() override;

    void start();
    void stop();
    void setSource(const QString& sourceId);
    void refresh();
    void setVolume(double value);

signals:
    void changed(SessionVolumeState state);
    void error(QString message);

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

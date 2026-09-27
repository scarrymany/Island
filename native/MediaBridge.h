#pragma once

#include <QByteArray>
#include <QList>
#include <QMetaType>
#include <QObject>
#include <QPair>
#include <QString>

#include <memory>

struct MediaSnapshot {
    bool active = false;
    QString title;
    QString artist;
    QString album;
    QString source;
    QString sourceId;
    double position = 0.0;
    double duration = 0.0;
    double playbackRate = 1.0;
    bool playing = false;
    bool canSeek = false;
    bool canPrevious = true;
    bool canNext = true;
    bool canPlayPause = true;
    QByteArray cover;
    qint64 updatedAt = 0;

    [[nodiscard]] double estimatedPosition() const;
};

Q_DECLARE_METATYPE(MediaSnapshot)

using MediaSources = QList<QPair<QString, QString>>;

namespace MediaSelection {
struct Source {
    QString id;
    bool playing = false;
};

[[nodiscard]] QString selectSource(const QList<Source>& sources, const QString& current,
                                   const QString& preferred, const QString& previous);
}

class MediaBridge final : public QObject {
    Q_OBJECT

public:
    explicit MediaBridge(QObject* parent = nullptr);
    ~MediaBridge() override;

    void start();
    void stop();
    void setSource(const QString& sourceId);
    void execute(const QString& action, double value = 0.0);

signals:
    void snapshotChanged(MediaSnapshot snapshot);
    void sourcesChanged(MediaSources sources);
    void error(QString message);

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

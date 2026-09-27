#pragma once

#include <QCryptographicHash>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QUrl>

#include <memory>

class QNetworkReply;
class QSaveFile;
class QTemporaryDir;

class UpdateService final : public QObject {
    Q_OBJECT

public:
    explicit UpdateService(QObject* parent = nullptr);
    ~UpdateService() override;

    void check(const QString& repository);
    void downloadAndInstall();
    void cancel();
    bool busy() const;
    bool hasUpdate() const;

signals:
    void statusChanged(const QString& status);
    void updateAvailable(const QString& version, const QString& notes);
    void readyToQuit();

private:
    friend class PlatformTest;
    enum class Operation { None, Metadata, Download };

    static bool isNewerVersion(const QString& remote, const QString& current);
    static bool trustedDownloadUrl(const QUrl& url);
    static bool validRepository(const QString& repository);
    void startRequest(const QUrl& url);
    void readAvailable();
    void requestFinished();
    void fail(const QString& reason);
    void parseRelease(const QByteArray& payload);
    void launchInstaller();
    void clearDownload();

    QNetworkAccessManager network_;
    QPointer<QNetworkReply> reply_;
    QTimer deadline_;
    Operation operation_ = Operation::None;
    quint64 requestGeneration_ = 0;
    QByteArray metadata_;
    QString failure_;
    QString repository_;
    QString version_;
    QString assetName_;
    QUrl assetUrl_;
    QByteArray digest_;
    qint64 expectedSize_ = 0;
    qint64 received_ = 0;
    int progressPercent_ = -1;
    std::unique_ptr<QTemporaryDir> staging_;
    std::unique_ptr<QSaveFile> download_;
    QCryptographicHash hash_{QCryptographicHash::Sha256};
};

#include "UpdateService.h"
#include "WindowsIntegration.h"

#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QSaveFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

namespace {
class DeferredReply final : public QNetworkReply {
public:
    explicit DeferredReply(QObject* parent) : QNetworkReply(parent) {
        open(QIODevice::ReadOnly);
    }
    void abort() override {
        aborted = true;
        setFinished(true);
        emit finished();
    }
    void finishLater() { emit finished(); }
    bool aborted = false;
protected:
    qint64 readData(char*, qint64) override { return -1; }
};

class BufferedReply final : public QNetworkReply {
public:
    BufferedReply(QByteArray body, int status, NetworkError error, QObject* parent)
        : QNetworkReply(parent), body_(std::move(body)) {
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, status);
        open(QIODevice::ReadOnly);
        if (error != NoError) setError(error, QStringLiteral("Connection timed out"));
    }
    void abort() override {
        setFinished(true);
        emit finished();
    }
    qint64 bytesAvailable() const override { return body_.size() - offset_ + QNetworkReply::bytesAvailable(); }
protected:
    qint64 readData(char* destination, qint64 maximum) override {
        const qint64 count = std::min(maximum, static_cast<qint64>(body_.size()) - offset_);
        if (count <= 0) return -1;
        std::memcpy(destination, body_.constData() + offset_, static_cast<size_t>(count));
        offset_ += count;
        return count;
    }
private:
    QByteArray body_;
    qint64 offset_ = 0;
};

QJsonObject releaseWithInstaller() {
    return {
        {QStringLiteral("tag_name"), QStringLiteral("v9.0.0")},
        {QStringLiteral("body"), QStringLiteral("Playback fixes")},
        {QStringLiteral("draft"), false},
        {QStringLiteral("prerelease"), false},
        {QStringLiteral("assets"), QJsonArray{QJsonObject{
            {QStringLiteral("name"), QStringLiteral("Island-Setup.exe")},
            {QStringLiteral("browser_download_url"), QStringLiteral("https://github.com/scarrymany/Island/releases/download/v9.0.0/Island-Setup.exe")},
            {QStringLiteral("digest"), QStringLiteral("sha256:") + QString(64, QLatin1Char('a'))},
            {QStringLiteral("size"), 4096},
            {QStringLiteral("state"), QStringLiteral("uploaded")}
        }}}
    };
}
}

class PlatformTest final : public QObject {
    Q_OBJECT

private slots:
    void hotkeyParsing_data() {
        QTest::addColumn<QString>("sequence");
        QTest::addColumn<unsigned int>("modifiers");
        QTest::addColumn<unsigned int>("key");
        QTest::newRow("default") << QStringLiteral("Ctrl+Alt+M") << 3U << 0x4DU;
        QTest::newRow("function") << QStringLiteral("Ctrl+Shift+F8") << 6U << 0x77U;
        QTest::newRow("space") << QStringLiteral("Alt+Space") << 1U << 0x20U;
        QTest::newRow("arrow") << QStringLiteral("Ctrl+Left") << 2U << 0x25U;
    }

    void hotkeyParsing() {
        QFETCH(QString, sequence);
        QFETCH(unsigned int, modifiers);
        QFETCH(unsigned int, key);
        QString error;
        const auto parsed = WindowsIntegration::parseHotkey(sequence, &error);
        QVERIFY2(parsed.has_value(), qPrintable(error));
        QCOMPARE(parsed->modifiers, modifiers);
        QCOMPARE(parsed->key, key);
    }

    void invalidHotkey_data() {
        QTest::addColumn<QString>("sequence");
        QTest::newRow("empty") << QString();
        QTest::newRow("plain-letter") << QStringLiteral("M");
        QTest::newRow("missing-key") << QStringLiteral("Ctrl+");
        QTest::newRow("reserved") << QStringLiteral("Ctrl+F12");
        QTest::newRow("sequence") << QStringLiteral("Ctrl+M, Ctrl+N");
        QTest::newRow("invalid") << QStringLiteral("invalid");
    }

    void invalidHotkey() {
        QFETCH(QString, sequence);
        QString error;
        QVERIFY(!WindowsIntegration::parseHotkey(sequence, &error));
        QVERIFY(!error.isEmpty());
    }

    void volumeBounds() {
        WindowsIntegration integration;
        QString error;
        QVERIFY(!integration.setVolume(-0.1, &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(!integration.setVolume(1.1, &error));
        QVERIFY(!integration.setVolume(std::numeric_limits<double>::quiet_NaN(), &error));
        QVERIFY(!integration.setVolume(std::numeric_limits<double>::infinity(), &error));
    }

    void versionComparison_data() {
        QTest::addColumn<QString>("remote");
        QTest::addColumn<QString>("current");
        QTest::addColumn<bool>("newer");
        QTest::newRow("patch") << QStringLiteral("v1.0.1") << QStringLiteral("1.0.0") << true;
        QTest::newRow("numeric") << QStringLiteral("1.10.0") << QStringLiteral("1.9.9") << true;
        QTest::newRow("same") << QStringLiteral("v1.0.0") << QStringLiteral("1.0.0") << false;
        QTest::newRow("older") << QStringLiteral("0.9.0") << QStringLiteral("1.0.0") << false;
        QTest::newRow("prerelease") << QStringLiteral("2.0.0-beta") << QStringLiteral("1.0.0") << false;
        QTest::newRow("junk") << QStringLiteral("2.0.0garbage") << QStringLiteral("1.0.0") << false;
        QTest::newRow("leading-zero") << QStringLiteral("02.0.0") << QStringLiteral("1.0.0") << false;
        QTest::newRow("overflow") << QStringLiteral("999999999999999999999.0.0") << QStringLiteral("1.0.0") << false;
        QTest::newRow("build-metadata") << QStringLiteral("2.0.0+build.3") << QStringLiteral("1.0.0") << true;
    }

    void versionComparison() {
        QFETCH(QString, remote);
        QFETCH(QString, current);
        QFETCH(bool, newer);
        QCOMPARE(UpdateService::isNewerVersion(remote, current), newer);
    }

    void downloadUrlPolicy_data() {
        QTest::addColumn<QString>("url");
        QTest::addColumn<bool>("allowed");
        QTest::newRow("github") << QStringLiteral("https://github.com/scarrymany/Island/releases/download/v9.0.0/Island-Setup.exe") << true;
        QTest::newRow("cdn") << QStringLiteral("https://release-assets.githubusercontent.com/path?signature=123") << true;
        QTest::newRow("plain-http") << QStringLiteral("http://github.com/file.exe") << false;
        QTest::newRow("host-suffix") << QStringLiteral("https://github.com.evil.test/file.exe") << false;
        QTest::newRow("credentials") << QStringLiteral("https://user:pass@github.com/file.exe") << false;
        QTest::newRow("alternate-port") << QStringLiteral("https://github.com:8080/file.exe") << false;
        QTest::newRow("fragment") << QStringLiteral("https://github.com/file.exe#section") << false;
        QTest::newRow("file-url") << QStringLiteral("file:///C:/file.exe") << false;
    }

    void downloadUrlPolicy() {
        QFETCH(QString, url);
        QFETCH(bool, allowed);
        QCOMPARE(UpdateService::trustedDownloadUrl(QUrl(url)), allowed);
    }

    void releaseAcceptsVerifiedInstaller() {
        UpdateService service;
        QSignalSpy available(&service, &UpdateService::updateAvailable);
        service.parseRelease(QJsonDocument(releaseWithInstaller()).toJson());
        QCOMPARE(available.count(), 1);
        QCOMPARE(service.expectedSize_, 4096);
        QCOMPARE(service.digest_.size(), 32);
        QCOMPARE(service.assetName_, QStringLiteral("Island-Setup.exe"));
    }

    void releaseRejectsMissingDigest() {
        UpdateService service;
        auto release = releaseWithInstaller();
        auto installer = release.value(QStringLiteral("assets")).toArray().first().toObject();
        installer.remove(QStringLiteral("digest"));
        release.insert(QStringLiteral("assets"), QJsonArray{installer});
        QSignalSpy available(&service, &UpdateService::updateAvailable);
        service.parseRelease(QJsonDocument(release).toJson());
        QCOMPARE(available.count(), 0);
        QVERIFY(service.assetUrl_.isEmpty());
    }

    void releaseRejectsDifferentRepository() {
        UpdateService service;
        auto release = releaseWithInstaller();
        auto installer = release.value(QStringLiteral("assets")).toArray().first().toObject();
        installer.insert(QStringLiteral("browser_download_url"), QStringLiteral("https://github.com/other/Island/releases/download/v9.0.0/Island-Setup.exe"));
        release.insert(QStringLiteral("assets"), QJsonArray{installer});
        QSignalSpy available(&service, &UpdateService::updateAvailable);
        service.parseRelease(QJsonDocument(release).toJson());
        QCOMPARE(available.count(), 0);
        QVERIFY(service.assetUrl_.isEmpty());
    }

    void releaseRejectsInvalidMetadata_data() {
        QTest::addColumn<QByteArray>("payload");
        QTest::newRow("invalid-json") << QByteArrayLiteral("{broken");
        QTest::newRow("json-array") << QByteArrayLiteral("[]");
        for (const QString& flag : {QStringLiteral("draft"), QStringLiteral("prerelease")}) {
            auto release = releaseWithInstaller();
            release.insert(flag, true);
            QTest::newRow(qPrintable(flag)) << QJsonDocument(release).toJson();
        }
        auto release = releaseWithInstaller();
        release.insert(QStringLiteral("tag_name"), QStringLiteral("v999.0.0-beta"));
        QTest::newRow("unstable-tag") << QJsonDocument(release).toJson();
        release = releaseWithInstaller();
        release.insert(QStringLiteral("tag_name"), QStringLiteral("v0.1.0"));
        QTest::newRow("older-release") << QJsonDocument(release).toJson();
        release = releaseWithInstaller();
        release.insert(QStringLiteral("assets"), QJsonArray{});
        QTest::newRow("missing-installer") << QJsonDocument(release).toJson();
        const QList<QPair<QString, QJsonValue>> invalidAssetFields{
            {QStringLiteral("size"), 0}, {QStringLiteral("size"), 1.5},
            {QStringLiteral("size"), 512LL * 1024 * 1024 + 1},
            {QStringLiteral("state"), QStringLiteral("new")},
            {QStringLiteral("digest"), QStringLiteral("sha256:invalid")}
        };
        int row = 0;
        for (const auto& [key, value] : invalidAssetFields) {
            release = releaseWithInstaller();
            auto installer = release.value(QStringLiteral("assets")).toArray().first().toObject();
            installer.insert(key, value);
            release.insert(QStringLiteral("assets"), QJsonArray{installer});
            QTest::newRow(qPrintable(QStringLiteral("invalid-asset-%1").arg(++row))) << QJsonDocument(release).toJson();
        }
    }

    void releaseRejectsInvalidMetadata() {
        QFETCH(QByteArray, payload);
        UpdateService service;
        QSignalSpy available(&service, &UpdateService::updateAvailable);
        QSignalSpy status(&service, &UpdateService::statusChanged);
        service.parseRelease(payload);
        QCOMPARE(available.count(), 0);
        QCOMPARE(status.count(), 1);
        QVERIFY(!status.first().first().toString().isEmpty());
        QVERIFY(!service.hasUpdate());
    }

    void releaseRejectsMismatchedAssetPath_data() {
        QTest::addColumn<QString>("path");
        QTest::newRow("tag-case") << QStringLiteral("/scarrymany/Island/releases/download/V9.0.0/Island-Setup.exe");
        QTest::newRow("asset-case") << QStringLiteral("/scarrymany/Island/releases/download/v9.0.0/island-setup.exe");
        QTest::newRow("different-tag") << QStringLiteral("/scarrymany/Island/releases/download/v1.3.0/Island-Setup.exe");
    }

    void releaseRejectsMismatchedAssetPath() {
        QFETCH(QString, path);
        UpdateService service;
        auto release = releaseWithInstaller();
        auto installer = release.value(QStringLiteral("assets")).toArray().first().toObject();
        installer.insert(QStringLiteral("browser_download_url"), QStringLiteral("https://github.com") + path);
        release.insert(QStringLiteral("assets"), QJsonArray{installer});
        QSignalSpy available(&service, &UpdateService::updateAvailable);
        service.parseRelease(QJsonDocument(release).toJson());
        QCOMPARE(available.count(), 0);
        QVERIFY(!service.hasUpdate());
    }

    void networkFailures_data() {
        QTest::addColumn<int>("httpStatus");
        QTest::addColumn<int>("networkError");
        QTest::addColumn<QString>("message");
        QTest::newRow("missing-release") << 404 << int(QNetworkReply::ContentNotFoundError) << QStringLiteral("не найден");
        QTest::newRow("rate-limit") << 429 << int(QNetworkReply::UnknownContentError) << QStringLiteral("ограничил запросы");
        QTest::newRow("server-error") << 500 << int(QNetworkReply::InternalServerError) << QStringLiteral("HTTP 500");
        QTest::newRow("timeout") << 0 << int(QNetworkReply::TimeoutError) << QStringLiteral("Connection timed out");
    }

    void networkFailures() {
        QFETCH(int, httpStatus);
        QFETCH(int, networkError);
        QFETCH(QString, message);
        UpdateService service;
        service.operation_ = UpdateService::Operation::Metadata;
        service.reply_ = new BufferedReply(QByteArrayLiteral("error"), httpStatus,
            static_cast<QNetworkReply::NetworkError>(networkError), &service);
        service.deadline_.start(1000);
        QSignalSpy status(&service, &UpdateService::statusChanged);
        QSignalSpy available(&service, &UpdateService::updateAvailable);
        service.requestFinished();
        QVERIFY(!service.busy());
        QVERIFY(!service.reply_);
        QVERIFY(!service.deadline_.isActive());
        QCOMPARE(available.count(), 0);
        QCOMPARE(status.count(), 1);
        QVERIFY2(status.first().first().toString().contains(message), qPrintable(status.first().first().toString()));
    }

    void damagedDownloadsAreRemoved_data() {
        QTest::addColumn<QByteArray>("body");
        QTest::newRow("wrong-digest") << QByteArrayLiteral("tamperdata");
        QTest::newRow("truncated") << QByteArrayLiteral("valid");
        QTest::newRow("oversized") << QByteArrayLiteral("valid data with excess bytes");
    }

    void damagedDownloadsAreRemoved() {
        QFETCH(QByteArray, body);
        const QByteArray expected = QByteArrayLiteral("valid data");
        UpdateService service;
        service.operation_ = UpdateService::Operation::Download;
        service.expectedSize_ = expected.size();
        service.digest_ = QCryptographicHash::hash(expected, QCryptographicHash::Sha256);
        service.staging_ = std::make_unique<QTemporaryDir>();
        QVERIFY(service.staging_->isValid());
        const QString directory = service.staging_->path();
        service.download_ = std::make_unique<QSaveFile>(service.staging_->filePath(QStringLiteral("installer.exe")));
        QVERIFY(service.download_->open(QIODevice::WriteOnly));
        service.reply_ = new BufferedReply(body, 200, QNetworkReply::NoError, &service);
        connect(service.reply_, &QNetworkReply::finished, &service, &UpdateService::requestFinished);
        QSignalSpy quit(&service, &UpdateService::readyToQuit);
        QSignalSpy status(&service, &UpdateService::statusChanged);
        service.requestFinished();
        QVERIFY(!service.busy());
        QVERIFY(!service.reply_);
        QVERIFY(!service.download_);
        QVERIFY(!service.staging_);
        QVERIFY(!QFileInfo::exists(directory));
        QCOMPARE(quit.count(), 0);
        QVERIFY(!status.isEmpty());
        const QString message = status.last().first().toString();
        QVERIFY2(message.contains(QStringLiteral("SHA-256")) || message.contains(QStringLiteral("размер")), qPrintable(message));
    }

    void installWithoutReleaseDoesNotLaunch() {
        UpdateService service;
        QSignalSpy quit(&service, &UpdateService::readyToQuit);
        QSignalSpy status(&service, &UpdateService::statusChanged);
        service.downloadAndInstall();
        QCOMPARE(quit.count(), 0);
        QCOMPARE(status.count(), 1);
        QVERIFY(!service.reply_);
    }

    void checkingWhileBusyPreservesPendingRequest() {
        UpdateService service;
        service.operation_ = UpdateService::Operation::Metadata;
        auto* pending = new DeferredReply(&service);
        service.reply_ = pending;
        service.deadline_.start(1000);
        QSignalSpy status(&service, &UpdateService::statusChanged);
        service.check();
        QVERIFY(service.busy());
        QVERIFY(!pending->aborted);
        QCOMPARE(service.reply_.data(), pending);
        QVERIFY(service.deadline_.isActive());
        QCOMPARE(status.count(), 1);
    }

    void cancelClearsDownloadAndCachedRelease() {
        UpdateService service;
        service.parseRelease(QJsonDocument(releaseWithInstaller()).toJson());
        QVERIFY(service.hasUpdate());
        service.operation_ = UpdateService::Operation::Download;
        service.metadata_ = QByteArrayLiteral("cached");
        service.staging_ = std::make_unique<QTemporaryDir>();
        QVERIFY(service.staging_->isValid());
        service.download_ = std::make_unique<QSaveFile>(service.staging_->filePath(QStringLiteral("installer.exe")));
        QVERIFY(service.download_->open(QIODevice::WriteOnly));
        service.deadline_.start(1000);
        auto* oldReply = new DeferredReply(&service);
        service.reply_ = oldReply;
        connect(oldReply, &QNetworkReply::finished, &service, &UpdateService::requestFinished);
        QSignalSpy available(&service, &UpdateService::updateAvailable);
        QSignalSpy quit(&service, &UpdateService::readyToQuit);
        QVERIFY(service.busy());
        service.cancel();
        QVERIFY(oldReply->aborted);
        oldReply->finishLater();
        QCoreApplication::processEvents();
        QVERIFY(!service.busy());
        QVERIFY(!service.hasUpdate());
        QVERIFY(!service.reply_);
        QVERIFY(!service.deadline_.isActive());
        QVERIFY(service.metadata_.isEmpty());
        QVERIFY(service.version_.isEmpty());
        QVERIFY(service.assetName_.isEmpty());
        QVERIFY(service.digest_.isEmpty());
        QVERIFY(!service.download_);
        QVERIFY(!service.staging_);
        QCOMPARE(service.expectedSize_, 0);
        QCOMPARE(available.count(), 0);
        service.downloadAndInstall();
        QCOMPARE(quit.count(), 0);
        QVERIFY(!service.reply_);
    }

    void cancellationDuringStatusSuppressesLateAvailability() {
        UpdateService service;
        QSignalSpy available(&service, &UpdateService::updateAvailable);
        connect(&service, &UpdateService::statusChanged, &service, &UpdateService::cancel);
        service.parseRelease(QJsonDocument(releaseWithInstaller()).toJson());
        QCOMPARE(available.count(), 0);
        QVERIFY(!service.hasUpdate());
    }
};

QTEST_MAIN(PlatformTest)
#include "test_platform.moc"

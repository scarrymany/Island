#include "UpdateService.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

#include <QCoreApplication>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QVersionNumber>

namespace {
constexpr qint64 MaxMetadataSize = 2 * 1024 * 1024;
constexpr qint64 MaxInstallerSize = 512LL * 1024 * 1024;
constexpr qint64 ReadChunkSize = 64 * 1024;
constexpr int TransferTimeoutMs = 30000;
constexpr int MetadataDeadlineMs = 60000;
constexpr int DownloadDeadlineMs = 15 * 60 * 1000;
constexpr int MaxRedirects = 5;
constexpr auto CurrentVersion = "1.0.2";
constexpr auto InstallerExe = "Island-Setup.exe";
constexpr auto InstallerMsi = "Island-Setup.msi";

QVersionNumber parsedVersion(const QString& version) {
    static const QRegularExpression pattern(QStringLiteral("^[vV]?(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)(?:\\+[0-9A-Za-z.-]+)?$"));
    const auto match = pattern.match(version.trimmed());
    if (!match.hasMatch()) return {};
    QList<int> parts;
    for (int part = 1; part <= 3; ++part) {
        bool valid = false;
        const int value = match.captured(part).toInt(&valid);
        if (!valid) return {};
        parts.append(value);
    }
    return QVersionNumber(parts);
}

bool secureUrl(const QUrl& url) {
    return url.isValid() && url.scheme() == QStringLiteral("https")
        && (url.port() == -1 || url.port() == 443) && url.userInfo().isEmpty()
        && !url.hasFragment();
}
}

UpdateService::UpdateService(QObject* parent) : QObject(parent), network_(this) {
    deadline_.setSingleShot(true);
    connect(&deadline_, &QTimer::timeout, this, [this] {
        fail(QStringLiteral("Истекло время ожидания сервера обновлений"));
    });
}

UpdateService::~UpdateService() {
    cancel();
}

void UpdateService::cancel() {
    ++requestGeneration_;
    deadline_.stop();
    if (reply_) {
        QNetworkReply* cancelled = reply_;
        reply_.clear();
        cancelled->disconnect(this);
        cancelled->abort();
        cancelled->deleteLater();
    }
    operation_ = Operation::None;
    metadata_.clear();
    failure_.clear();
    repository_.clear();
    version_.clear();
    assetName_.clear();
    assetUrl_.clear();
    digest_.clear();
    expectedSize_ = 0;
    received_ = 0;
    progressPercent_ = -1;
    hash_.reset();
    clearDownload();
}

bool UpdateService::busy() const {
    return operation_ != Operation::None;
}

bool UpdateService::hasUpdate() const {
    return !assetUrl_.isEmpty() && !version_.isEmpty() && digest_.size() == 32 && expectedSize_ > 0;
}

bool UpdateService::validRepository(const QString& repository) {
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9-]{0,38}/[A-Za-z0-9_.-]{1,100}$"));
    return pattern.match(repository).hasMatch();
}

bool UpdateService::isNewerVersion(const QString& remote, const QString& current) {
    const auto candidate = parsedVersion(remote);
    const auto installed = parsedVersion(current);
    return !candidate.isNull() && !installed.isNull() && QVersionNumber::compare(candidate, installed) > 0;
}

bool UpdateService::trustedDownloadUrl(const QUrl& url) {
    if (!secureUrl(url)) return false;
    const QString host = url.host().toLower();
    return host == QStringLiteral("github.com") || host == QStringLiteral("release-assets.githubusercontent.com")
        || host == QStringLiteral("objects.githubusercontent.com") || host == QStringLiteral("github-releases.githubusercontent.com");
}

void UpdateService::check(const QString& repository) {
    const QString normalized = repository.trimmed();
    if (busy() && normalized == repository_) {
        emit statusChanged(QStringLiteral("Дождитесь завершения текущего запроса"));
        return;
    }
    cancel();
    repository_ = normalized;
    if (repository_.isEmpty()) {
        emit statusChanged(QStringLiteral("Для обновлений укажите GitHub-репозиторий в формате владелец/проект"));
        return;
    }
    if (!validRepository(repository_)) {
        emit statusChanged(QStringLiteral("Неверный репозиторий. Используйте формат владелец/проект"));
        return;
    }
    operation_ = Operation::Metadata;
    metadata_.clear();
    failure_.clear();
    received_ = 0;
    deadline_.start(MetadataDeadlineMs);
    startRequest(QUrl(QStringLiteral("https://api.github.com/repos/%1/releases/latest").arg(repository_)));
    emit statusChanged(QStringLiteral("Проверка обновлений..."));
}

void UpdateService::startRequest(const QUrl& url) {
    QNetworkRequest request(url);
    request.setRawHeader("User-Agent", "Island-Updater/1.0");
    request.setRawHeader("Accept", operation_ == Operation::Metadata ? "application/vnd.github+json" : "application/octet-stream");
    request.setRawHeader("X-GitHub-Api-Version", "2022-11-28");
    request.setRawHeader("Accept-Encoding", "identity");
    request.setTransferTimeout(TransferTimeoutMs);
    request.setMaximumRedirectsAllowed(MaxRedirects);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::UserVerifiedRedirectPolicy);
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    reply_ = network_.get(request);
    const quint64 generation = ++requestGeneration_;
    reply_->setReadBufferSize(ReadChunkSize);
    connect(reply_, &QNetworkReply::redirected, this, [this, generation](const QUrl& target) {
        if (generation != requestGeneration_ || !reply_) return;
        const auto resolved = reply_->url().resolved(target);
        const bool allowed = operation_ == Operation::Metadata
            ? secureUrl(resolved) && resolved.host() == QStringLiteral("api.github.com")
            : trustedDownloadUrl(resolved);
        if (!allowed) {
            fail(QStringLiteral("Сервер перенаправил обновление на недоверенный адрес"));
            return;
        }
        reply_->redirectAllowed();
    });
    connect(reply_, &QIODevice::readyRead, this, [this, generation] {
        if (generation == requestGeneration_) readAvailable();
    });
    connect(reply_, &QNetworkReply::finished, this, [this, generation] {
        if (generation == requestGeneration_) requestFinished();
    });
    connect(reply_, &QNetworkReply::metaDataChanged, this, [this, generation] {
        if (generation != requestGeneration_ || !reply_) return;
        const qint64 size = reply_->header(QNetworkRequest::ContentLengthHeader).toLongLong();
        const qint64 limit = operation_ == Operation::Metadata ? MaxMetadataSize : MaxInstallerSize;
        if (size > limit) fail(QStringLiteral("Файл обновления превышает допустимый размер"));
    });
}

void UpdateService::readAvailable() {
    if (!reply_ || !failure_.isEmpty()) return;
    if (reply_->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200) {
        reply_->readAll();
        return;
    }
    const qint64 limit = operation_ == Operation::Metadata ? MaxMetadataSize : expectedSize_;
    while (reply_ && reply_->bytesAvailable() > 0) {
        const QByteArray chunk = reply_->read(ReadChunkSize);
        if (chunk.isEmpty()) break;
        if (received_ > limit - chunk.size()) {
            fail(QStringLiteral("Ответ сервера превышает заявленный размер"));
            return;
        }
        received_ += chunk.size();
        if (operation_ == Operation::Metadata) {
            metadata_.append(chunk);
        } else {
            if (!download_ || download_->write(chunk) != chunk.size()) {
                fail(QStringLiteral("Не удалось сохранить обновление на диск"));
                return;
            }
            hash_.addData(chunk);
            const int percentage = static_cast<int>(received_ * 100 / expectedSize_);
            if (percentage != progressPercent_) {
                progressPercent_ = percentage;
                emit statusChanged(QStringLiteral("Загрузка обновления: %1%").arg(percentage));
            }
        }
    }
}

void UpdateService::requestFinished() {
    if (!reply_) return;
    readAvailable();
    if (!reply_) return;
    QNetworkReply* completed = reply_;
    reply_.clear();
    deadline_.stop();
    const auto operation = operation_;
    operation_ = Operation::None;
    const int status = completed->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const auto networkError = completed->error();
    const QString networkMessage = completed->errorString();
    completed->deleteLater();
    if (!failure_.isEmpty()) {
        clearDownload();
        emit statusChanged(failure_);
        return;
    }
    if (networkError != QNetworkReply::NoError || status != 200) {
        clearDownload();
        if (status == 404) {
            emit statusChanged(QStringLiteral("Репозиторий или опубликованный релиз не найден"));
        } else if (status == 403 || status == 429) {
            emit statusChanged(QStringLiteral("GitHub временно ограничил запросы. Повторите позже"));
        } else {
            emit statusChanged(QStringLiteral("Не удалось получить обновление: %1").arg(networkMessage));
        }
        return;
    }
    if (operation == Operation::Metadata) {
        parseRelease(metadata_);
        metadata_.clear();
        return;
    }
    if (received_ != expectedSize_ || hash_.result() != digest_) {
        clearDownload();
        emit statusChanged(QStringLiteral("Проверка SHA-256 или размера не пройдена. Обновление удалено"));
        return;
    }
    if (!download_ || !download_->commit()) {
        clearDownload();
        emit statusChanged(QStringLiteral("Не удалось завершить запись установщика"));
        return;
    }
    download_.reset();
    launchInstaller();
}

void UpdateService::fail(const QString& reason) {
    failure_ = reason;
    if (reply_ && !reply_->isFinished()) {
        reply_->abort();
    }
}

void UpdateService::parseRelease(const QByteArray& payload) {
    const quint64 generation = requestGeneration_;
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(payload, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        emit statusChanged(QStringLiteral("GitHub вернул некорректное описание релиза"));
        return;
    }
    const auto release = document.object();
    const QString tag = release.value(QStringLiteral("tag_name")).toString();
    if (release.value(QStringLiteral("draft")).toBool() || release.value(QStringLiteral("prerelease")).toBool()
        || parsedVersion(tag).isNull()) {
        emit statusChanged(QStringLiteral("Релиз должен иметь стабильную версию вида v1.2.3"));
        return;
    }
    QString current = QCoreApplication::applicationVersion();
    if (parsedVersion(current).isNull()) current = QString::fromLatin1(CurrentVersion);
    if (!isNewerVersion(tag, current)) {
        emit statusChanged(QStringLiteral("Установлена актуальная версия %1").arg(current));
        return;
    }
    const QJsonArray assets = release.value(QStringLiteral("assets")).toArray();
    QJsonObject installer;
    for (const auto& value : assets) {
        const auto asset = value.toObject();
        const QString name = asset.value(QStringLiteral("name")).toString();
        if (name == QLatin1String(InstallerExe)) {
            installer = asset;
            break;
        }
        if (name == QLatin1String(InstallerMsi)) installer = asset;
    }
    if (installer.isEmpty()) {
        emit statusChanged(QStringLiteral("Версия %1 найдена, но в релизе нет Island-Setup.exe или Island-Setup.msi").arg(tag));
        return;
    }
    const QString name = installer.value(QStringLiteral("name")).toString();
    const QUrl url(installer.value(QStringLiteral("browser_download_url")).toString());
    const QString expectedPath = QStringLiteral("/%1/releases/download/%2/%3").arg(repository_, tag, name);
    if (!trustedDownloadUrl(url) || url.host() != QStringLiteral("github.com") || url.hasQuery()
        || url.path().compare(expectedPath, Qt::CaseInsensitive) != 0) {
        emit statusChanged(QStringLiteral("Установщик ссылается за пределы выбранного GitHub-релиза"));
        return;
    }
    const QString digest = installer.value(QStringLiteral("digest")).toString();
    static const QRegularExpression digestPattern(QStringLiteral("^sha256:([0-9a-fA-F]{64})$"));
    const auto match = digestPattern.match(digest);
    if (!match.hasMatch()) {
        emit statusChanged(QStringLiteral("В релизе нет SHA-256 установщика. Перезагрузите файл в GitHub Releases"));
        return;
    }
    const qint64 size = installer.value(QStringLiteral("size")).toInteger();
    if (size <= 0 || size > MaxInstallerSize || installer.value(QStringLiteral("state")).toString() != QStringLiteral("uploaded")) {
        emit statusChanged(QStringLiteral("Установщик не загружен полностью или имеет недопустимый размер"));
        return;
    }
    version_ = tag;
    assetName_ = name;
    assetUrl_ = url;
    digest_ = QByteArray::fromHex(match.captured(1).toLatin1());
    expectedSize_ = size;
    emit statusChanged(QStringLiteral("Доступна версия %1. Установка начнется по вашей кнопке").arg(tag));
    if (generation == requestGeneration_ && hasUpdate()) {
        emit updateAvailable(tag, release.value(QStringLiteral("body")).toString());
    }
}

void UpdateService::downloadAndInstall() {
    if (operation_ != Operation::None) {
        emit statusChanged(QStringLiteral("Дождитесь завершения текущего запроса"));
        return;
    }
    if (!hasUpdate()) {
        emit statusChanged(QStringLiteral("Сначала проверьте наличие обновления"));
        return;
    }
    clearDownload();
    staging_ = std::make_unique<QTemporaryDir>(QDir::tempPath() + QStringLiteral("/Island-update-XXXXXX"));
    if (!staging_->isValid()) {
        clearDownload();
        emit statusChanged(QStringLiteral("Не удалось создать папку для обновления"));
        return;
    }
    download_ = std::make_unique<QSaveFile>(staging_->filePath(assetName_));
    if (!download_->open(QIODevice::WriteOnly)) {
        clearDownload();
        emit statusChanged(QStringLiteral("Не удалось создать файл установщика"));
        return;
    }
    operation_ = Operation::Download;
    received_ = 0;
    progressPercent_ = -1;
    failure_.clear();
    hash_.reset();
    deadline_.start(DownloadDeadlineMs);
    startRequest(assetUrl_);
    emit statusChanged(QStringLiteral("Загрузка версии %1...").arg(version_));
}

void UpdateService::clearDownload() {
    if (download_) download_->cancelWriting();
    download_.reset();
    staging_.reset();
}

void UpdateService::launchInstaller() {
    const QString installer = QDir::toNativeSeparators(staging_->filePath(assetName_));
    QString executable = installer;
    QString parameters;
    if (assetName_.endsWith(QStringLiteral(".msi"), Qt::CaseInsensitive)) {
        wchar_t systemDirectory[MAX_PATH]{};
        const UINT length = GetSystemDirectoryW(systemDirectory, MAX_PATH);
        if (!length || length >= MAX_PATH) {
            clearDownload();
            emit statusChanged(QStringLiteral("Не удалось найти установщик Windows"));
            return;
        }
        executable = QString::fromWCharArray(systemDirectory) + QStringLiteral("\\msiexec.exe");
        parameters = QStringLiteral("/i \"%1\" /norestart").arg(installer);
    }
    const std::wstring file = executable.toStdWString();
    const std::wstring arguments = parameters.toStdWString();
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOASYNC;
    info.lpVerb = L"open";
    info.lpFile = file.c_str();
    info.lpParameters = arguments.empty() ? nullptr : arguments.c_str();
    info.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&info)) {
        const DWORD code = GetLastError();
        clearDownload();
        emit statusChanged(code == ERROR_CANCELLED ? QStringLiteral("Запуск установщика отменен")
                                                    : QStringLiteral("Не удалось запустить установщик, код Windows: %1").arg(code));
        return;
    }
    staging_->setAutoRemove(false);
    emit statusChanged(QStringLiteral("Установщик запущен. SCARP ISLAND завершает работу для обновления"));
    emit readyToQuit();
}

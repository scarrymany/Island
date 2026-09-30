#include "ReleaseNotes.h"
#include "AppAssets.h"
#include "AppInfo.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScreen>
#include <QTextBrowser>
#include <QTextDocument>
#include <QVersionNumber>
#include <QVBoxLayout>
#include <utility>

static void initializeReleaseNotes() {
    static const bool initialized = [] {
        Q_INIT_RESOURCE(release_notes);
        return true;
    }();
    Q_UNUSED(initialized);
}

namespace {
QVersionNumber versionNumber(const QString& version) {
    static const QRegularExpression pattern(QStringLiteral("^[0-9]+\\.[0-9]+\\.[0-9]+$"));
    return pattern.match(version).hasMatch() ? QVersionNumber::fromString(version) : QVersionNumber{};
}
}

ReleaseNotesState::ReleaseNotesState(QString path) : path_(std::move(path)) {}

bool ReleaseNotesState::shouldShow(const QString& version) const {
    const auto current = versionNumber(version);
    if (current.isNull()) return false;
    QFile file(path_);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 4096) return true;
    const auto object = QJsonDocument::fromJson(file.readAll()).object();
    const auto seen = versionNumber(object["last_read_version"].toString());
    return seen.isNull() || QVersionNumber::compare(current, seen) > 0;
}

bool ReleaseNotesState::markRead(const QString& version, QString* error) const {
    if (error) error->clear();
    if (versionNumber(version).isNull()) {
        if (error) *error = QStringLiteral("Некорректная версия списка изменений");
        return false;
    }
    // Opening old notes after a downgrade must not forget a newer read version.
    if (!shouldShow(version)) return true;
    if (!QDir().mkpath(QFileInfo(path_).absolutePath())) {
        if (error) *error = QStringLiteral("Не удалось создать папку истории обновлений");
        return false;
    }
    QSaveFile file(path_);
    file.setDirectWriteFallback(false);
    const auto bytes = QJsonDocument(QJsonObject{{"last_read_version", version}}).toJson();
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        if (error) *error = QStringLiteral("Не удалось сохранить прочтение списка изменений: %1").arg(file.errorString());
        return false;
    }
    return true;
}

QString ReleaseNotesDialog::markdown() {
    initializeReleaseNotes();
    QFile file(QStringLiteral(":/island/whats-new.md"));
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString{};
}

ReleaseNotesDialog::ReleaseNotesDialog(QWidget* parent) : QDialog(parent) {
    setObjectName("releaseNotesDialog");
    setWindowTitle(QStringLiteral("Что нового в SCARP ISLAND %1").arg(AppInfo::Version));
    setWindowIcon(AppAssets::icon());
    setModal(false);
    resize(700, 690);
    setMinimumSize(420, 360);
    if (screen()) resize(size().boundedTo(screen()->availableGeometry().size() - QSize(32, 32)));
    QFont font(AppAssets::settingsFontFamily());
    font.setPointSize(10);
    setFont(font);
    setStyleSheet(QStringLiteral(
        "QDialog { background: #101114; color: #F3F3F6; }"
        "QLabel { color: #F3F3F6; background: transparent; }"
        "QLabel#releaseEyebrow { color: #B5A9FF; font-size: 11px; font-weight: 700; }"
        "QLabel#releaseTitle { font-size: 28px; font-weight: 700; }"
        "QLabel#releaseSubtitle { color: #999AA7; }"
        "QTextBrowser { background: #191A20; color: #E2E2E8; border: 1px solid #2C2D36; border-radius: 14px; }"
        "QPushButton { background: #B5A9FF; color: #181423; border: none; border-radius: 9px; padding: 11px 25px; font-weight: 600; }"
        "QPushButton:hover { background: #CBC2FF; }"
        "QPushButton:focus { border: 2px solid #F3F3F6; padding: 9px 23px; }"
        "QScrollBar:vertical { background: #191A20; width: 10px; margin: 12px 2px; }"
        "QScrollBar::handle:vertical { background: #494855; min-height: 24px; border-radius: 4px; }"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(26, 24, 26, 22);
    layout->setSpacing(10);
    auto* eyebrow = new QLabel(QStringLiteral("SCARP ISLAND / %1").arg(AppInfo::Version), this);
    eyebrow->setObjectName("releaseEyebrow");
    layout->addWidget(eyebrow);
    auto* title = new QLabel(QStringLiteral("Больше удобства. Меньше суеты."), this);
    title->setObjectName("releaseTitle");
    title->setWordWrap(true);
    layout->addWidget(title);
    auto* subtitle = new QLabel(QStringLiteral("Ваши настройки и профили сохранены"), this);
    subtitle->setObjectName("releaseSubtitle");
    subtitle->setWordWrap(true);
    layout->addWidget(subtitle);
    layout->addSpacing(8);
    auto* content = new QTextBrowser(this);
    content->setObjectName("releaseNotesContent");
    content->setAccessibleName(QStringLiteral("Изменения в версии %1").arg(AppInfo::Version));
    content->setOpenExternalLinks(true);
    content->document()->setDocumentMargin(20);
    content->document()->setDefaultStyleSheet(QStringLiteral(
        "h2 { color: #F3F3F6; font-size: 16px; margin-top: 20px; margin-bottom: 8px; }"
        "p, li { line-height: 135%; } a { color: #B5A9FF; }"));
    content->setMarkdown(markdown());
    layout->addWidget(content, 1);
    auto* footer = new QHBoxLayout;
    auto* hint = new QLabel(QStringLiteral("Можно открыть снова: Настройки → Обновления"), this);
    hint->setObjectName("releaseSubtitle"); hint->setWordWrap(true);
    footer->addWidget(hint, 1);
    auto* done = new QPushButton(QStringLiteral("Отлично"), this);
    done->setObjectName("releaseNotesDone");
    done->setDefault(true);
    connect(done, &QPushButton::clicked, this, &QDialog::accept);
    footer->addWidget(done);
    layout->addLayout(footer);
}

#pragma once

#include <QDialog>
#include <QString>

// Kept beside config.json, outside profiles/imports: applying a profile must not
// make an already-read release announcement appear again.
class ReleaseNotesState final {
public:
    explicit ReleaseNotesState(QString path);
    [[nodiscard]] bool shouldShow(const QString& version) const;
    bool markRead(const QString& version, QString* error = nullptr) const;
private:
    QString path_;
};

class ReleaseNotesDialog final : public QDialog {
    Q_OBJECT
public:
    explicit ReleaseNotesDialog(QWidget* parent = nullptr);
    [[nodiscard]] static QString markdown();
};

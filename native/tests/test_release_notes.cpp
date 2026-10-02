#include "ReleaseNotes.h"
#include "AppInfo.h"

#include <QFile>
#include <QPushButton>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTextBrowser>
#include <QtTest>

class ReleaseNotesTest final : public QObject {
    Q_OBJECT
private slots:
    void newVersionShowsOnceAndSurvivesRestart() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto path = directory.filePath("release-state.json");
        ReleaseNotesState state(path);
        QVERIFY(state.shouldShow("1.1.0"));
        QString error;
        QVERIFY2(state.markRead("1.1.0", &error), qPrintable(error));
        QVERIFY(!ReleaseNotesState(path).shouldShow("1.1.0"));
        QVERIFY(!state.shouldShow("1.0.6"));
        QVERIFY(state.shouldShow("1.2.0"));
        QVERIFY(state.markRead("1.0.6"));
        QVERIFY(!state.shouldShow("1.1.0"));
        QVERIFY(state.markRead("1.2.0"));
        QVERIFY(!state.shouldShow("1.2.0"));
    }

    void corruptOrMissingStateIsRecoverable() {
        QTemporaryDir directory;
        const auto path = directory.filePath("nested/release-state.json");
        ReleaseNotesState state(path);
        QVERIFY(state.markRead("1.0.6"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("{broken"); file.close();
        QVERIFY(state.shouldShow("1.1.0"));
        QVERIFY(state.markRead("1.1.0"));
        QVERIFY(!state.shouldShow("1.1.0"));
        QVERIFY(!state.shouldShow("not-a-version"));
        QString error;
        QVERIFY(!state.markRead("not-a-version", &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(!state.shouldShow("1.1.0"));
    }

    void failedSaveDoesNotMarkAsRead() {
        QTemporaryDir directory;
        QFile blocker(directory.filePath("blocker"));
        QVERIFY(blocker.open(QIODevice::WriteOnly)); blocker.close();
        ReleaseNotesState state(blocker.fileName() + "/release-state.json");
        QString error;
        QVERIFY(!state.markRead("1.1.0", &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(state.shouldShow("1.1.0"));
    }

    void readableBundledNotesCanBeDismissedAndReopened() {
        QVERIFY(ReleaseNotesDialog::markdown().contains(QStringLiteral("Новый островок")));
        ReleaseNotesDialog dialog;
        QVERIFY(dialog.windowTitle().contains(AppInfo::Version));
        auto* content = dialog.findChild<QTextBrowser*>("releaseNotesContent");
        auto* done = dialog.findChild<QPushButton*>("releaseNotesDone");
        QVERIFY(content && done);
        QVERIFY(content->toPlainText().contains(QStringLiteral("Не закрывается сам")));
        QSignalSpy finished(&dialog, &QDialog::finished);
        dialog.show();
        QCoreApplication::processEvents();
        QVERIFY(!dialog.grab().isNull());
        QTest::mouseClick(done, Qt::LeftButton);
        QCOMPARE(finished.size(), 1);
        QVERIFY(!dialog.isVisible());
        dialog.show();
        QTest::keyClick(&dialog, Qt::Key_Escape);
        QCOMPARE(finished.size(), 2);
        QVERIFY(!dialog.isVisible());
    }
};
QTEST_MAIN(ReleaseNotesTest)
#include "test_release_notes.moc"

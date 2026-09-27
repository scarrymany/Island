#include "Application.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QLibraryInfo>
#include <QMessageBox>
#include <QMutex>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QTranslator>
#include <memory>

namespace {
QMutex logMutex;
QString logPath;
void logMessage(QtMsgType type, const QMessageLogContext&, const QString& message) {
    QMutexLocker lock(&logMutex);
    QFile log(logPath);
    if (log.size() > 2 * 1024 * 1024) { QFile::remove(logPath + ".1"); log.rename(logPath + ".1"); log.setFileName(logPath); }
    if (log.open(QIODevice::Append | QIODevice::Text))
        log.write(QString("%1 [%2] %3\n").arg(QDateTime::currentDateTime().toString(Qt::ISODateWithMs)).arg(type).arg(message).toUtf8());
}
}

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    app.setApplicationName("Island"); app.setOrganizationName("Island"); app.setApplicationVersion("1.0.2");
    app.setApplicationDisplayName("SCARP ISLAND");
    app.setQuitOnLastWindowClosed(false);
    QTranslator translator;
    if (translator.load(QLocale("ru"), "qt", "_", QLibraryInfo::path(QLibraryInfo::TranslationsPath)))
        app.installTranslator(&translator);
    const auto dataDirectory = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    QDir().mkpath(dataDirectory);
    logPath = QDir(dataDirectory).filePath("island.log");
    qInstallMessageHandler(logMessage);
    QCommandLineParser parser;
    parser.setApplicationDescription("SCARP ISLAND - native Windows music overlay"); parser.addHelpOption(); parser.addVersionOption();
    parser.addOption({"background", "Start in the system tray."});
    parser.addOption({"quit", "Quit the running SCARP ISLAND instance."});
    parser.addOption({"demo", "Use isolated demonstration data without controlling system media."});
    parser.addOption({"smoke-test", "Capture and exit after the interface has initialized."});
    parser.addOption({"capture", "Save HUD and settings screenshots to this directory.", "directory"});
    parser.addOption({"diagnostics", "Write a media diagnostic JSON file before exiting the smoke test.", "path"});
    parser.process(app);

    const bool isolated = parser.isSet("demo") || parser.isSet("smoke-test");
    std::unique_ptr<QTemporaryDir> temporary;
    if (isolated) temporary = std::make_unique<QTemporaryDir>();
    QLocalServer server;
    std::unique_ptr<QLockFile> lock;
    if (!isolated) {
        const QString serverName = "Island-" + QString::fromLatin1(QCryptographicHash::hash(dataDirectory.toUtf8(), QCryptographicHash::Sha256).toHex().left(20));
        lock = std::make_unique<QLockFile>(QDir(dataDirectory).filePath("instance.lock"));
        lock->setStaleLockTime(0);
        if (parser.isSet("quit")) {
            QLocalSocket socket;
            socket.connectToServer(serverName);
            if (socket.waitForConnected(1000)) { socket.write("quit\n"); socket.waitForBytesWritten(500); }
            return 0;
        }
        if (!lock->tryLock(0)) {
            QLocalSocket socket;
            for (int attempt = 0; attempt < 4; ++attempt) {
                socket.connectToServer(serverName);
                if (socket.waitForConnected(750)) { socket.write("show\n"); socket.waitForBytesWritten(500); return 0; }
                socket.abort();
            }
            QMessageBox::information(nullptr, "SCARP ISLAND", QStringLiteral("SCARP ISLAND уже работает. Откройте настройки через значок в трее."));
            return 0;
        }
        QLocalServer::removeServer(serverName);
        server.setSocketOptions(QLocalServer::UserAccessOption);
        if (!server.listen(serverName)) {
            QMessageBox::critical(nullptr, "SCARP ISLAND", QStringLiteral("Не удалось создать канал экземпляра: %1").arg(server.errorString()));
            return 1;
        }
    }
    Application controller(parser.isSet("demo"), parser.isSet("background"), temporary ? temporary->filePath("config.json") : QString{});
    QObject::connect(&server, &QLocalServer::newConnection, &controller, [&] {
        while (auto* connection = server.nextPendingConnection()) {
            connection->setReadBufferSize(256);
            QObject::connect(connection, &QLocalSocket::disconnected, connection, &QObject::deleteLater);
            QObject::connect(connection, &QLocalSocket::readyRead, &controller, [&, connection] {
                if (!connection->canReadLine()) return;
                const QByteArray command = connection->readLine(256).trimmed();
                if (command == "show") controller.showSettings();
                if (command == "quit") { controller.shutdown(); app.exit(0); }
                connection->disconnectFromServer();
            });
            QTimer::singleShot(2000, connection, [connection] { connection->disconnectFromServer(); });
        }
    });
    if (parser.isSet("capture") || parser.isSet("smoke-test")) {
        QTimer::singleShot(3500, &controller, [&] {
            if (parser.isSet("capture")) controller.capture(parser.value("capture"));
            if (parser.isSet("diagnostics")) controller.writeDiagnostics(parser.value("diagnostics"));
            if (parser.isSet("smoke-test")) { controller.shutdown(); app.exit(0); }
        });
    }
    return app.exec();
}

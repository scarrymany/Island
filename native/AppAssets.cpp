#include "AppAssets.h"

#include <QFontDatabase>
#include <QGuiApplication>
#include <QResource>
#include <QDebug>

// Keep the resource initializer in the global namespace so static linking resolves it.
static void initializeResources() {
    static const bool initialized = [] {
        Q_INIT_RESOURCE(island_assets);
        return true;
    }();
    Q_UNUSED(initialized);
}

QString AppAssets::settingsFontFamily() {
    if (!qobject_cast<QGuiApplication*>(QCoreApplication::instance())) return QStringLiteral("Segoe UI");
    static const QString family = [] {
        initializeResources();
        if (QFontDatabase::addApplicationFont(QStringLiteral(":/island/fonts/Manrope.ttf")) < 0)
            qWarning() << "Cannot load the bundled Manrope font";
        const int id = QFontDatabase::addApplicationFont(QStringLiteral(":/island/fonts/Inter.ttf"));
        const auto families = QFontDatabase::applicationFontFamilies(id);
        if (!families.isEmpty()) return families.first();
        qWarning() << "Cannot load the bundled Inter font; using Segoe UI";
        return QStringLiteral("Segoe UI");
    }();
    return family;
}

QIcon AppAssets::icon() {
    initializeResources();
    return QIcon(QStringLiteral(":/island/icon.ico"));
}

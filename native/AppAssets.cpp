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

QStringList AppAssets::bundledFontFamilies() {
    if (!qobject_cast<QGuiApplication*>(QCoreApplication::instance())) return {};
    static const QStringList loaded = [] {
        initializeResources();
        QStringList families;
        for (const auto* file : {"Inter", "Manrope", "GolosText", "Rubik", "IBMPlexSans", "JetBrainsMono"}) {
            const int id = QFontDatabase::addApplicationFont(QStringLiteral(":/island/fonts/%1.ttf").arg(QString::fromLatin1(file)));
            const auto registered = QFontDatabase::applicationFontFamilies(id);
            if (registered.isEmpty())
                qWarning() << "Cannot load the bundled font:" << file;
            for (const auto& family : registered) {
                if (!families.contains(family)) families.append(family);
            }
        }
        return families;
    }();
    return loaded;
}

QString AppAssets::settingsFontFamily() {
    const auto families = bundledFontFamilies();
    return families.contains(QStringLiteral("Inter")) ? QStringLiteral("Inter") : QStringLiteral("Segoe UI");
}

QIcon AppAssets::icon() {
    initializeResources();
    return QIcon(QStringLiteral(":/island/icon.ico"));
}

QIcon AppAssets::githubIcon(bool darkBackground) {
    initializeResources();
    return QIcon(darkBackground ? QStringLiteral(":/island/github/white.png")
                               : QStringLiteral(":/island/github/black.png"));
}

#include "AppAssets.h"
#include "ConfigStore.h"
#include <QFontInfo>

#include <QFile>
#include <QFontDatabase>
#include <QRawFont>
#include <QtTest>

class AssetsTest final : public QObject {
    Q_OBJECT
private slots:
    void githubMarksRenderForBothBackgrounds() {
        for (const bool dark : {false, true}) {
            const auto image = AppAssets::githubIcon(dark).pixmap(20, 20).toImage();
            QVERIFY(!image.isNull());
            QVERIFY(image.width() <= 20 && image.height() <= 20);
            bool hasOpaquePixel = false;
            for (int y = 0; y < image.height(); ++y) {
                for (int x = 0; x < image.width(); ++x) {
                    const auto color = image.pixelColor(x, y);
                    if (color.alpha() < 200) continue;
                    hasOpaquePixel = true;
                    QCOMPARE(color.red(), dark ? 255 : 0);
                    QCOMPARE(color.green(), color.red());
                    QCOMPARE(color.blue(), color.red());
                }
            }
            QVERIFY(hasOpaquePixel);
        }
    }

    void embeddedFontLoadsWithCyrillic() {
        const auto family = AppAssets::settingsFontFamily();
        QVERIFY(QFontDatabase::hasFamily(family));
        QCOMPARE(AppAssets::settingsFontFamily(), family);
        const QString letters = QStringLiteral("АБВГДЕЁЖЗИЙКЛМНОПРСТУФХЦЧШЩЪЫЬЭЮЯ"
                                                "абвгдеёжзийклмнопрстуфхцчшщъыьэюяІіЇїЄєҐґ");
        const QList<QPair<QString, QString>> fonts = {
            {"Inter", "Inter"}, {"Manrope", "Manrope"}, {"Golos Text", "GolosText"},
            {"Rubik", "Rubik"}, {"IBM Plex Sans", "IBMPlexSans"}, {"JetBrains Mono", "JetBrainsMono"}
        };
        for (const auto& entry : fonts) {
            const auto& name = entry.first;
            QVERIFY2(AppAssets::bundledFontFamilies().contains(name), qPrintable(name));
            QVERIFY(QFontDatabase::families(QFontDatabase::Cyrillic).contains(name));
            QFile file(QStringLiteral(":/island/fonts/%1.ttf").arg(entry.second));
            QVERIFY(file.open(QIODevice::ReadOnly));
            const QRawFont raw(file.readAll(), 24, QFont::PreferNoHinting);
            QVERIFY(raw.isValid());
            // Variable Rubik has a legacy "Rubik Light" face but the preferred
            // application family is "Rubik" on both supported Windows backends.
            QVERIFY(raw.familyName() == name || (name == "Rubik" && raw.familyName() == "Rubik Light"));
            for (const QChar letter : letters)
                QVERIFY2(raw.supportsCharacter(letter),
                    qPrintable(QStringLiteral("%1 is missing Cyrillic glyph: %2").arg(name).arg(letter)));
            QFont requested(name);
            requested.setPixelSize(24);
            requested.setWeight(QFont::Normal);
            const auto resolved = QRawFont::fromFont(requested);
            QVERIFY(resolved.isValid());
            // Compare glyph mapping from the real resource: an installed font
            // or Windows fallback must not silently satisfy this check.
            QCOMPARE(resolved.fontTable("cmap"), raw.fontTable("cmap"));
            const auto glyphs = resolved.glyphIndexesForString(letters);
            QCOMPARE(glyphs.size(), letters.size());
            for (const auto glyph : glyphs) QVERIFY(glyph != 0);
            qInfo() << "Bundled font:" << name << "; checked Cyrillic glyphs:" << letters.size();
        }
        QCOMPARE(AppAssets::bundledFontFamilies(), AppAssets::bundledFontFamilies());
    }

    void settingsFontResolvesWithCyrillic() {
        // The saved default names Windows 11's face; elsewhere it must still resolve to a real font.
        QFont font(ConfigStore::defaults()["settings_font_family"].toString());
        font.setPixelSize(16);
        font.setWeight(QFont::DemiBold);
        const auto raw = QRawFont::fromFont(font);
        QVERIFY(raw.isValid());
        QVERIFY(raw.supportsCharacter(QChar(u'Я')));
        QCOMPARE(QFontInfo(font).family(), AppAssets::settingsFontFamily());
    }

    void applicationIconRendersFromResource() {
        const QIcon icon = AppAssets::icon();
        QVERIFY(!icon.isNull());
        for (const int size : {16, 20, 24, 32, 48, 64, 128, 256}) {
            const QPixmap pixmap = icon.pixmap(size, size);
            QVERIFY(!pixmap.isNull());
            QCOMPARE(pixmap.size(), QSize(size, size));
            const auto image = pixmap.toImage();
            QVERIFY(image.pixelColor(size / 2, size / 2).alpha() > 0);
            QCOMPARE(image.pixelColor(0, 0).alpha(), 0);
        }
    }
};

QTEST_MAIN(AssetsTest)
#include "test_assets.moc"

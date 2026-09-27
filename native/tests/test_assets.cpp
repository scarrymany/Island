#include "AppAssets.h"

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
        QCOMPARE(family, QStringLiteral("Inter"));
        QCOMPARE(AppAssets::settingsFontFamily(), family);
        const QString letters = QStringLiteral("АБВГДЕЁЖЗИЙКЛМНОПРСТУФХЦЧШЩЪЫЬЭЮЯ"
                                                "абвгдеёжзийклмнопрстуфхцчшщъыьэюяІіЇїЄєҐґ");
        for (const auto& name : {QStringLiteral("Inter"), QStringLiteral("Manrope")}) {
            QVERIFY(QFontDatabase::families(QFontDatabase::Cyrillic).contains(name));
            QFile file(QStringLiteral(":/island/fonts/%1.ttf").arg(name));
            QVERIFY(file.open(QIODevice::ReadOnly));
            const QRawFont raw(file.readAll(), 24, QFont::PreferNoHinting);
            QVERIFY(raw.isValid());
            QCOMPARE(raw.familyName(), name);
            for (const QChar letter : letters)
                QVERIFY2(raw.supportsCharacter(letter),
                    qPrintable(QStringLiteral("%1 is missing Cyrillic glyph: %2").arg(name).arg(letter)));
            qInfo() << "Bundled font:" << name << "; checked Cyrillic glyphs:" << letters.size();
        }
    }

    void semiboldFontUsesInter() {
        QFont font(AppAssets::settingsFontFamily());
        font.setPixelSize(16);
        font.setWeight(QFont::DemiBold);
        const auto raw = QRawFont::fromFont(font);
        QVERIFY(raw.isValid());
        QCOMPARE(raw.familyName(), QStringLiteral("Inter"));
        QVERIFY(raw.supportsCharacter(QChar(u'Я')));
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

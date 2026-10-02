#include "ConfigStore.h"
#include "HudWindow.h"
#include "WindowsIntegration.h"

#include <QApplication>
#include <QBuffer>
#include <QCursor>
#include <QDir>
#include <QEnterEvent>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QPainterPathStroker>
#include <QPalette>
#include <QScopeGuard>
#include <QScreen>
#include <QtTest>

#include <Windows.h>

#include <algorithm>

namespace {
constexpr int CaptureWidth = 760;
constexpr int CaptureHeight = 360;
constexpr int ColorTolerance = 2;
constexpr int AntialiasMargin = 2;
constexpr int VisibleHeight = 8;
constexpr int CompactWidth = 156;
constexpr int CompactHeight = 32;
constexpr int AnimationDuration = 260;
constexpr int CheckerSize = 16;

QJsonObject desktopConfiguration(QScreen* screen, bool blur = true) {
    auto config = ConfigStore::defaults();
    config["monitor"] = screen->name();
    config["anchor"] = "top_center";
    config["scale"] = 1.0;
    config["width"] = 560;
    config["height"] = 132;
    config["offset_y"] = 12;
    config["idle_collapse"] = true;
    config["idle_collapse_seconds"] = 1;
    config["compact_width"] = CompactWidth;
    config["compact_height"] = CompactHeight;
    config["compact_visible_height"] = VisibleHeight;
    config["animation_duration"] = AnimationDuration;
    config["opacity"] = 0.3;
    config["blur"] = blur;
    config["click_through"] = false;
    auto animations = config["animations"].toObject();
    animations["dock"] = true;
    config["animations"] = animations;
    return config;
}

// The overlay window is a stable transparent frame; compare the card with its margin.
QRect cardWindow(const HudWindow& hud, int inset) {
    return hud.cardGeometry().toAlignedRect().adjusted(-inset, -inset, inset, inset);
}

bool docked(const HudWindow& hud) { return hud.isCollapsed() && hud.dockProgress() == 1.0; }

QString cursorUnavailable(const QPoint& requested) {
    POINT actual{};
    const BOOL readable = GetCursorPos(&actual);
    return QStringLiteral("Interactive cursor movement is unavailable: requested Qt (%1,%2), observed Qt (%3,%4), "
                          "GetCursorPos success=%5 physical=(%6,%7). Native hover was not exercised.")
        .arg(requested.x()).arg(requested.y()).arg(QCursor::pos().x()).arg(QCursor::pos().y())
        .arg(readable).arg(actual.x).arg(actual.y);
}

struct PixelComparison {
    qsizetype outside = 0;
    qsizetype changedOutside = 0;
    qsizetype changedInside = 0;
    QPoint firstOutside{-1, -1};
    QRect changedBounds;
    int maximumDifference = 0;

    QString description() const {
        return QStringLiteral("%1 of %2 outside pixels changed; first=(%3,%4), maximum channel difference=%5")
            .arg(changedOutside).arg(outside).arg(firstOutside.x()).arg(firstOutside.y()).arg(maximumDifference);
    }
};

QImage captureDesktop(QScreen* screen, const QRect& area, const QString& path) {
    const QPoint offset = area.topLeft() - screen->geometry().topLeft();
    const QPixmap pixels = screen->grabWindow(0, offset.x(), offset.y(), area.width(), area.height());
    const QImage image = pixels.toImage().convertToFormat(QImage::Format_RGB32);
    return !image.isNull() && image.save(path) ? image : QImage{};
}

QPainterPath cardPath(const QRect& geometry, int inset, double radius, const QRect& area, double ratio) {
    const QRectF card = QRectF(geometry).adjusted(inset, inset, -inset, -inset)
        .translated(-area.topLeft());
    QPainterPath path;
    path.addRoundedRect(QRectF(card.x() * ratio, card.y() * ratio,
                               card.width() * ratio, card.height() * ratio), radius * ratio, radius * ratio);
    return path;
}

PixelComparison comparePixels(const QImage& baseline, const QImage& actual, const QPainterPath& card = {}) {
    PixelComparison result;
    QPainterPath allowed = card;
    if (!card.isEmpty()) {
        QPainterPathStroker edge;
        edge.setWidth(AntialiasMargin * 2);
        allowed = allowed.united(edge.createStroke(card));
    }
    for (int y = 0; y < baseline.height(); ++y) {
        const auto* before = reinterpret_cast<const QRgb*>(baseline.constScanLine(y));
        const auto* after = reinterpret_cast<const QRgb*>(actual.constScanLine(y));
        for (int x = 0; x < baseline.width(); ++x) {
            const int difference = std::max({std::abs(qRed(before[x]) - qRed(after[x])),
                std::abs(qGreen(before[x]) - qGreen(after[x])), std::abs(qBlue(before[x]) - qBlue(after[x]))});
            const QPointF center(x + 0.5, y + 0.5);
            const bool outside = !allowed.contains(center);
            if (outside) ++result.outside;
            if (difference <= ColorTolerance) continue;
            result.changedBounds = result.changedBounds.united(QRect(x, y, 1, 1));
            if (outside) {
                if (result.changedOutside == 0) result.firstOutside = QPoint(x, y);
                ++result.changedOutside;
                result.maximumDifference = std::max(result.maximumDifference, difference);
            } else if (card.contains(center)) {
                ++result.changedInside;
            }
        }
    }
    return result;
}
}

class TestDesktop final : public QObject {
    Q_OBJECT

private slots:
    void desktopComposition() {
        if (QGuiApplication::platformName() != QStringLiteral("windows"))
            QSKIP("Requires an interactive Windows desktop and the windows QPA plugin");
        QScreen* screen = QGuiApplication::primaryScreen();
        QVERIFY(screen);
        const QRect screenRect = screen->geometry();
        if (screenRect.width() < CaptureWidth || screenRect.height() < CaptureHeight)
            QSKIP("Primary screen is too small for the controlled desktop capture area");

        const QPoint originalCursor = QCursor::pos();
        const auto restoreCursor = qScopeGuard([originalCursor] { QCursor::setPos(originalCursor); });
        const HWND originalFocus = GetForegroundWindow();
        QVERIFY2(originalFocus, "No foreground window exists on the interactive desktop");
        QCursor::setPos(screenRect.bottomRight() - QPoint(8, 8));

        const QString output = qEnvironmentVariable("ISLAND_DESKTOP_CAPTURE_DIR",
            QDir::current().filePath(QStringLiteral("build/desktop-captures")));
        QVERIFY2(QDir().mkpath(output), qPrintable(QStringLiteral("Cannot create capture directory: %1").arg(output)));
        const QDir captures(output);
        qInfo().noquote() << "Desktop captures:" << captures.absolutePath();

        const QRect captureArea(screenRect.x() + (screenRect.width() - CaptureWidth) / 2,
                                screenRect.y(), CaptureWidth, CaptureHeight);
        QWidget background(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint
            | Qt::WindowDoesNotAcceptFocus);
        background.setAttribute(Qt::WA_ShowWithoutActivating);
        background.setWindowTitle(QStringLiteral("Island desktop verification background"));
        QPixmap texture(CheckerSize * 2, CheckerSize * 2);
        texture.fill(QColor("#246D8D"));
        {
            QPainter painter(&texture);
            painter.fillRect(CheckerSize, 0, CheckerSize, CheckerSize, QColor("#CAE6F2"));
            painter.fillRect(0, CheckerSize, CheckerSize, CheckerSize, QColor("#CAE6F2"));
        }
        QPalette palette;
        palette.setBrush(QPalette::Window, QBrush(texture));
        background.setPalette(palette);
        background.setAutoFillBackground(true);
        background.setGeometry(captureArea);
        background.show();
        WindowsIntegration::ensureTopmost(background.winId());
        QTest::qWait(250);
        QVERIFY2(GetForegroundWindow() == originalFocus, "The verification background stole keyboard focus");

        const QImage baseline = captureDesktop(screen, captureArea, captures.filePath("baseline.png"));
        QVERIFY2(!baseline.isNull(), "Desktop capture failed or baseline.png could not be saved");
        const double ratio = static_cast<double>(baseline.width()) / captureArea.width();
        QVERIFY(ratio > 0.0);
        QCOMPARE(baseline.height(), qRound(captureArea.height() * ratio));
        const QImage expectedBackground = background.grab().toImage().convertToFormat(QImage::Format_RGB32);
        QCOMPARE(expectedBackground.size(), baseline.size());
        for (int y = 3; y < baseline.height() - 3; y += 11) {
            for (int x = 3; x < baseline.width() - 3; x += 11) {
                const QColor color = baseline.pixelColor(x, y);
                const QColor expected = expectedBackground.pixelColor(x, y);
                QVERIFY2(std::abs(color.red() - expected.red()) <= ColorTolerance
                    && std::abs(color.green() - expected.green()) <= ColorTolerance
                    && std::abs(color.blue() - expected.blue()) <= ColorTolerance,
                    "Another window covers the controlled desktop background");
            }
        }

        QImage blurredExpanded;
        for (const bool blur : {true, false}) {
            const QString prefix = blur ? QStringLiteral("blur-on-") : QStringLiteral("blur-off-");
            const auto config = desktopConfiguration(screen, blur);

            HudWindow hud(config);
            MediaSnapshot track;
            track.active = true;
            track.title = QStringLiteral("Desktop composition check");
            track.artist = QStringLiteral("Local test data");
            track.source = QStringLiteral("TEST");
            track.sourceId = QStringLiteral("desktop-test");
            track.position = 30;
            track.duration = 180;
            QImage cover(96, 96, QImage::Format_RGB32);
            {
                QPainter painter(&cover);
                QLinearGradient colors(0, 0, 96, 96);
                colors.setColorAt(0, QColor("#DFA857"));
                colors.setColorAt(1, QColor("#654BD0"));
                painter.fillRect(cover.rect(), colors);
            }
            QBuffer coverBytes(&track.cover);
            QVERIFY(coverBytes.open(QIODevice::WriteOnly));
            QVERIFY(cover.save(&coverBytes, "PNG"));
            track.canNext = track.canPrevious = track.canPlayPause = false;
            hud.setSnapshot(track);
            hud.reveal(true);
            const int inset = 14;
            const QRect expandedGeometry = cardWindow(hud, inset);
            QVERIFY(captureArea.contains(expandedGeometry));
            QTest::qWait(700);
            QCOMPARE(cardWindow(hud, inset), expandedGeometry);
            const QImage expanded = captureDesktop(screen, captureArea, captures.filePath(prefix + "expanded.png"));
            QVERIFY(!expanded.isNull());
            QCOMPARE(expanded.size(), baseline.size());
            const auto expandedShape = cardPath(expandedGeometry, inset, config["radius"].toDouble(), captureArea, ratio);
            const auto expandedPixels = comparePixels(baseline, expanded, expandedShape);
            QVERIFY(expandedPixels.outside > 10'000);
            QVERIFY2(expandedPixels.changedOutside == 0, qPrintable(prefix + expandedPixels.description()));
            QVERIFY2(expandedPixels.changedInside > 1000, "The expanded HUD is missing from the desktop composition");
            QVERIFY2(GetForegroundWindow() == originalFocus, "Showing the HUD stole keyboard focus");
            if (blur) {
                blurredExpanded = expanded;
            } else {
                const auto blurDifference = comparePixels(blurredExpanded, expanded, expandedShape);
                const double cardArea = expandedShape.boundingRect().width() * expandedShape.boundingRect().height();
                QVERIFY2(blurDifference.changedInside > cardArea * 0.05,
                    qPrintable(QStringLiteral("Enabling blur changed only %1 interior pixels on a checkerboard background")
                        .arg(blurDifference.changedInside)));
            }

            const QSize compactSize(CompactWidth + inset * 2, CompactHeight + inset * 2);
            QTRY_VERIFY_WITH_TIMEOUT(docked(hud), 2500);
            QTest::qWait(80);
            const QRect compactGeometry = cardWindow(hud, inset);
            QCOMPARE(compactGeometry.size(), compactSize);
            QCOMPARE(compactGeometry.bottom() + 1 - inset, screenRect.top() + VisibleHeight);
            const QImage compact = captureDesktop(screen, captureArea, captures.filePath(prefix + "collapsed.png"));
            QVERIFY(!compact.isNull());
            QCOMPARE(compact.size(), baseline.size());
            const auto compactShape = cardPath(compactGeometry, inset, config["compact_radius"].toDouble(), captureArea, ratio);
            const auto compactPixels = comparePixels(baseline, compact, compactShape);
            QVERIFY2(compactPixels.changedOutside == 0, qPrintable(prefix + compactPixels.description()));
            QVERIFY2(compactPixels.changedInside > 50, "The compact edge handle is not visible");
            QVERIFY(compactPixels.changedBounds.bottom() < qCeil(VisibleHeight * ratio) + 1);
            QVERIFY(compactPixels.changedBounds.height() >= qFloor(VisibleHeight * ratio) - 1);
            QVERIFY(compactPixels.changedBounds.width() <= qCeil(CompactWidth * ratio) + 2);

            const QPoint hoverTarget(compactGeometry.center().x(), screenRect.top() + VisibleHeight / 2);
            // Over a fullscreen foreground app the strip deliberately passes the pointer through.
            const bool fullscreenForeground = WindowsIntegration::foregroundIsFullscreen(hud.winId());
            if (fullscreenForeground)
                qInfo() << "A fullscreen application is in the foreground; hit-test and hover checks are skipped";
            if (!fullscreenForeground) {
                const QPoint local = hud.mapFromGlobal(hoverTarget);
                POINT nativeTarget{qRound(local.x() * hud.devicePixelRatioF()),
                                   qRound(local.y() * hud.devicePixelRatioF())};
                QVERIFY(ClientToScreen(reinterpret_cast<HWND>(hud.winId()), &nativeTarget));
                QVERIFY2(WindowFromPoint(nativeTarget) == reinterpret_cast<HWND>(hud.winId()),
                    "The native compact handle does not receive hit tests at its visible center");
                // Composition coverage is independent of access to the interactive input desktop.
                QEnterEvent enter(local, local, hoverTarget);
                QCoreApplication::sendEvent(&hud, &enter);
                QTRY_COMPARE_WITH_TIMEOUT(cardWindow(hud, inset), expandedGeometry, 1500);
                QTest::qWait(80);
                QVERIFY2(GetForegroundWindow() == originalFocus, "Enter-event expansion stole keyboard focus");
                const QImage hovered = captureDesktop(screen, captureArea, captures.filePath(prefix + "event-expanded.png"));
                QVERIFY(!hovered.isNull());
                QCOMPARE(hovered.size(), baseline.size());
                const auto hoveredPixels = comparePixels(baseline, hovered, expandedShape);
                QVERIFY2(hoveredPixels.changedOutside == 0, qPrintable(prefix + hoveredPixels.description()));
                QVERIFY(hoveredPixels.changedInside > 1000);
            }

            hud.conceal(true);
            QTRY_VERIFY_WITH_TIMEOUT(!hud.isVisible(), 1500);
            QCursor::setPos(screenRect.bottomRight() - QPoint(8, 8));
            QTest::qWait(100);
            const QImage hidden = captureDesktop(screen, captureArea, captures.filePath(prefix + "hidden.png"));
            QVERIFY(!hidden.isNull());
            QCOMPARE(hidden.size(), baseline.size());
            const auto hiddenPixels = comparePixels(baseline, hidden);
            QVERIFY2(hiddenPixels.changedOutside == 0, qPrintable(prefix + hiddenPixels.description()));
            QVERIFY2(GetForegroundWindow() == originalFocus, "The HUD changed keyboard focus during the test");
        }
    }

    void antialiasedCornersMatchQtPainting_data() {
        QTest::addColumn<double>("scale");
        QTest::newRow("normal") << 1.0;
        QTest::newRow("fractional") << 1.25;
        QTest::newRow("large") << 1.75;
    }
    void antialiasedCornersMatchQtPainting() {
        QFETCH(double, scale);
        if (QGuiApplication::platformName() != QStringLiteral("windows"))
            QSKIP("Requires an interactive Windows desktop and the windows QPA plugin");
        auto* screen = QGuiApplication::primaryScreen();
        QVERIFY(screen);
        auto c = desktopConfiguration(screen, false);
        c["scale"] = scale; c["width"] = 360; c["height"] = 100;
        c["idle_collapse"] = false; c["auto_hide_seconds"] = 0;
        c["background"] = "#FFFFFF"; c["gradient_enabled"] = false;
        c["artwork_background"] = false; c["opacity"] = 1; c["border_width"] = 0;
        auto visible = c["visible"].toObject();
        for (auto it = visible.begin(); it != visible.end(); ++it) it.value() = false;
        c["visible"] = visible;
        auto effects = c["animations"].toObject();
        for (auto it = effects.begin(); it != effects.end(); ++it) it.value() = false;
        c["animations"] = effects;
        HudWindow hud(c);
        if (!screen->geometry().contains(hud.geometry())) QSKIP("Screen is too small for the edge comparison");
        const QPoint oldCursor = QCursor::pos();
        const auto restoreCursor = qScopeGuard([oldCursor] { QCursor::setPos(oldCursor); });
        QCursor::setPos(screen->geometry().bottomRight() - QPoint(5, 5));
        const HWND originalFocus = GetForegroundWindow();
        QWidget background(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint
            | Qt::WindowDoesNotAcceptFocus);
        background.setAttribute(Qt::WA_ShowWithoutActivating);
        QPalette palette; palette.setColor(QPalette::Window, Qt::black);
        background.setPalette(palette); background.setAutoFillBackground(true);
        background.setGeometry(hud.geometry()); background.show();
        WindowsIntegration::ensureTopmost(background.winId());
        hud.reveal(); QTest::qWait(180);
        const QImage reference = hud.grab().toImage();
        const QString output = qEnvironmentVariable("ISLAND_DESKTOP_CAPTURE_DIR",
            QDir::current().filePath(QStringLiteral("build/desktop-captures")));
        QVERIFY(QDir().mkpath(output));
        const QImage actual = captureDesktop(screen, hud.geometry(),
            QDir(output).filePath(QStringLiteral("antialiased-edges-%1.png").arg(scale)));
        QVERIFY(!actual.isNull()); QCOMPARE(actual.size(), reference.size());
        const auto hwnd = reinterpret_cast<HWND>(hud.winId());
        HRGN region = CreateRectRgn(0, 0, 0, 0);
        QVERIFY(region);
        const auto freeRegion = qScopeGuard([region] { DeleteObject(region); });
        QVERIFY(GetWindowRgn(hwnd, region) != ERROR);
        RECT frame{}; POINT origin{};
        QVERIFY(GetWindowRect(hwnd, &frame)); QVERIFY(ClientToScreen(hwnd, &origin));
        int partial = 0;
        for (int y = 0; y < reference.height(); ++y) {
            for (int x = 0; x < reference.width(); ++x) {
                const QColor expected = reference.pixelColor(x, y);
                if (expected.alpha() <= 0 || expected.alpha() >= 255) continue;
                ++partial;
                QVERIFY2(PtInRegion(region, x + origin.x - frame.left, y + origin.y - frame.top),
                    "A binary native window region clipped a painted antialiased edge pixel");
                const QColor composed = actual.pixelColor(x, y);
                // Over a black desktop the compositor must show exactly the premultiplied edge colour.
                const int red = qRound(expected.red() * expected.alphaF());
                const int green = qRound(expected.green() * expected.alphaF());
                const int blue = qRound(expected.blue() * expected.alphaF());
                QVERIFY2(std::abs(composed.red() - red) <= 4 && std::abs(composed.green() - green) <= 4
                    && std::abs(composed.blue() - blue) <= 4,
                    qPrintable(QStringLiteral("Edge (%1,%2): alpha %3, expected RGB (%4,%5,%6), desktop RGB (%7,%8,%9)")
                        .arg(x).arg(y).arg(expected.alpha()).arg(red).arg(green).arg(blue)
                        .arg(composed.red()).arg(composed.green()).arg(composed.blue())));
            }
        }
        QVERIFY2(partial > 40, "No smoothly antialiased corner pixels were rendered");
        QVERIFY2(GetForegroundWindow() == originalFocus, "Edge rendering test stole keyboard focus");
    }

    void nativePointerHover() {
        if (QGuiApplication::platformName() != QStringLiteral("windows"))
            QSKIP("Requires an interactive Windows desktop and the windows QPA plugin");
        QScreen* screen = QGuiApplication::primaryScreen();
        QVERIFY(screen);
        const QPoint originalCursor = QCursor::pos();
        const auto restoreCursor = qScopeGuard([originalCursor] { QCursor::setPos(originalCursor); });
        const QPoint away = screen->geometry().bottomRight() - QPoint(8, 8);
        QCursor::setPos(away);
        if (QCursor::pos() != away) QSKIP(qPrintable(cursorUnavailable(away)));

        HudWindow hud(desktopConfiguration(screen));
        hud.reveal(true);
        const int inset = 14;
        const QRect expandedGeometry = cardWindow(hud, inset);
        QTRY_VERIFY_WITH_TIMEOUT(docked(hud), 2500);
        QCOMPARE(cardWindow(hud, inset).size(), QSize(CompactWidth + inset * 2, CompactHeight + inset * 2));
        QTest::qWait(80);
        const HWND originalFocus = GetForegroundWindow();
        if (WindowsIntegration::foregroundIsFullscreen(hud.winId()))
            QSKIP("A fullscreen application is in the foreground; the strip intentionally ignores hover there");
        const QPoint target(cardWindow(hud, inset).center().x(), screen->geometry().top() + VisibleHeight / 2);
        QCursor::setPos(target);
        if (QCursor::pos() != target) QSKIP(qPrintable(cursorUnavailable(target)));
        QTRY_COMPARE_WITH_TIMEOUT(cardWindow(hud, inset), expandedGeometry, 1500);
        QVERIFY2(GetForegroundWindow() == originalFocus, "Native hover expansion stole keyboard focus");
    }
};

QTEST_MAIN(TestDesktop)
#include "test_desktop.moc"

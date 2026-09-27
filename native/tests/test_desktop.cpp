#include "ConfigStore.h"
#include "HudWindow.h"
#include "WindowsIntegration.h"

#include <QApplication>
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
            track.canNext = track.canPrevious = track.canPlayPause = false;
            hud.setSnapshot(track);
            hud.reveal(true);
            const QRect expandedGeometry = hud.geometry();
            const int inset = (expandedGeometry.width() - config["width"].toInt()) / 2;
            QVERIFY(captureArea.contains(expandedGeometry));
            QTest::qWait(700);
            QCOMPARE(hud.geometry(), expandedGeometry);
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
            QTRY_COMPARE_WITH_TIMEOUT(hud.size(), compactSize, 2500);
            QTest::qWait(80);
            QCOMPARE(hud.geometry().bottom() + 1 - inset, screenRect.top() + VisibleHeight);
            const QImage compact = captureDesktop(screen, captureArea, captures.filePath(prefix + "collapsed.png"));
            QVERIFY(!compact.isNull());
            QCOMPARE(compact.size(), baseline.size());
            const auto compactShape = cardPath(hud.geometry(), inset, config["compact_radius"].toDouble(), captureArea, ratio);
            const auto compactPixels = comparePixels(baseline, compact, compactShape);
            QVERIFY2(compactPixels.changedOutside == 0, qPrintable(prefix + compactPixels.description()));
            QVERIFY2(compactPixels.changedInside > 50, "The compact edge handle is not visible");
            QVERIFY(compactPixels.changedBounds.bottom() < qCeil(VisibleHeight * ratio) + 1);
            QVERIFY(compactPixels.changedBounds.height() >= qFloor(VisibleHeight * ratio) - 1);
            QVERIFY(compactPixels.changedBounds.width() <= qCeil(CompactWidth * ratio) + 2);

            const QPoint hoverTarget(hud.geometry().center().x(), screenRect.top() + VisibleHeight / 2);
            const QPoint local = hud.mapFromGlobal(hoverTarget);
            POINT nativeTarget{qRound(local.x() * hud.devicePixelRatioF()),
                               qRound(local.y() * hud.devicePixelRatioF())};
            QVERIFY(ClientToScreen(reinterpret_cast<HWND>(hud.winId()), &nativeTarget));
            QVERIFY2(WindowFromPoint(nativeTarget) == reinterpret_cast<HWND>(hud.winId()),
                "The native compact handle does not receive hit tests at its visible center");
            // Composition coverage is independent of access to the interactive input desktop.
            QEnterEvent enter(local, local, hoverTarget);
            QCoreApplication::sendEvent(&hud, &enter);
            QTRY_COMPARE_WITH_TIMEOUT(hud.geometry(), expandedGeometry, 1500);
            QTest::qWait(80);
            QVERIFY2(GetForegroundWindow() == originalFocus, "Enter-event expansion stole keyboard focus");
            const QImage hovered = captureDesktop(screen, captureArea, captures.filePath(prefix + "event-expanded.png"));
            QVERIFY(!hovered.isNull());
            QCOMPARE(hovered.size(), baseline.size());
            const auto hoveredPixels = comparePixels(baseline, hovered, expandedShape);
            QVERIFY2(hoveredPixels.changedOutside == 0, qPrintable(prefix + hoveredPixels.description()));
            QVERIFY(hoveredPixels.changedInside > 1000);

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
        const QRect expandedGeometry = hud.geometry();
        const int inset = (expandedGeometry.width() - 560) / 2;
        QTRY_COMPARE_WITH_TIMEOUT(hud.size(), QSize(CompactWidth + inset * 2, CompactHeight + inset * 2), 2500);
        QTest::qWait(80);
        const HWND originalFocus = GetForegroundWindow();
        const QPoint target(hud.geometry().center().x(), screen->geometry().top() + VisibleHeight / 2);
        QCursor::setPos(target);
        if (QCursor::pos() != target) QSKIP(qPrintable(cursorUnavailable(target)));
        QTRY_COMPARE_WITH_TIMEOUT(hud.geometry(), expandedGeometry, 1500);
        QVERIFY2(GetForegroundWindow() == originalFocus, "Native hover expansion stole keyboard focus");
    }
};

QTEST_MAIN(TestDesktop)
#include "test_desktop.moc"

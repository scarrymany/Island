#include "ConfigStore.h"
#include "Layout.h"
#include <QJsonArray>
#include <QtTest>

class LayoutTest : public QObject {
    Q_OBJECT
private slots:
    void defaultElementsFit() {
        const auto c = ConfigStore::defaults();
        const QRectF bounds(0, 0, c["width"].toDouble(), c["height"].toDouble());
        for (const auto& rect : Layout::elements(c)) QVERIFY(bounds.contains(rect));
    }
    void customPositionClampsAfterResize() {
        auto c = ConfigStore::defaults(); c["width"] = 300; c["height"] = 110; c["layout"] = "custom";
        c["element_positions"] = QJsonObject{{"cover", QJsonArray{900, -100}}};
        const auto rect = Layout::elements(c)["cover"];
        QCOMPARE(rect.x(), 300 - rect.width()); QCOMPARE(rect.y(), 0.0);
    }
    void invisibleElementsHaveNoHitArea() {
        auto c = ConfigStore::defaults(); auto visible = c["visible"].toObject(); visible["play"] = false; c["visible"] = visible;
        QVERIFY(!Layout::elements(c).contains("play"));
    }
    void secondaryMonitorNegativeOrigin() {
        QCOMPARE(Layout::screenPosition({-1920, 0, 1920, 1080}, {600, 160}, "top_center", 12), QPoint(-1260, 12));
        const QPoint saved(1800, -20);
        QCOMPARE(Layout::screenPosition({-1920, 0, 1920, 1080}, {600, 160}, "free", 12, &saved), QPoint(-600, 0));
    }
    void formatsTime() {
        QCOMPARE(Layout::formatTime(-3), QString("0:00"));
        QCOMPARE(Layout::formatTime(3661), QString("1:01:01"));
    }
    void collapsedCardMeetsPhysicalMonitorEdge() {
        QCOMPARE(Layout::dockGeometry({-1920, -1080, 1920, 1080}, {184, 60}, 14, 8),
                 QRect(-1052, -1118, 184, 60));
        QCOMPARE(Layout::dockGeometry({0, 0, 3840, 2160}, {368, 120}, 28, 16),
                 QRect(1736, -76, 368, 120));
        QCOMPARE(Layout::dockGeometry({0, 0, 1920, 1080}, {184, 60}, 14, 100).bottom() + 1 - 14, 32);
    }
};
QTEST_APPLESS_MAIN(LayoutTest)
#include "test_layout.moc"

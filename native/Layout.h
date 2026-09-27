#pragma once

#include <QJsonObject>
#include <QMap>
#include <QRectF>
#include <QSize>
#include <QString>

namespace Layout {
QMap<QString, QRectF> elements(const QJsonObject& config);
QPoint screenPosition(const QRect& screen, const QSize& window, const QString& anchor,
                      int offsetY, const QPoint* saved = nullptr);
QString formatTime(double seconds);
}

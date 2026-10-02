#pragma once

#include <QImage>
#include <QPainterPath>
#include <QRectF>
#include <QSize>

namespace Squircle {
// Continuous-curvature rounded rectangle (the "smoothed corner" used by modern
// UI kits). The circular part of each corner keeps the requested radius; the
// tangent transition starts earlier, so the shape always fits inside the plain
// rounded rectangle of the same radius.
QPainterPath path(const QRectF& rect, double radius, double smoothing = 0.6);
// Soft black shadow of a squircle, cached. size, radius and blur are device pixels;
// the image is padded by blur * 2 on every side.
QImage shadow(const QSize& size, double radius, int blur);
}

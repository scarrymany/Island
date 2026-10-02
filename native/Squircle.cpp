#include "Squircle.h"

#include <QHash>
#include <QPainter>
#include <QPointF>
#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

namespace {
double radians(double degrees) { return degrees * std::numbers::pi / 180.0; }

void blurAlpha(QImage& image, int radius) {
    if (radius <= 0) return;
    const int width = image.width(), height = image.height();
    std::vector<int> alpha(static_cast<size_t>(width) * height), scratch(alpha.size());
    for (int y = 0; y < height; ++y) {
        const auto* row = reinterpret_cast<const QRgb*>(image.constScanLine(y));
        for (int x = 0; x < width; ++x) alpha[static_cast<size_t>(y) * width + x] = qAlpha(row[x]);
    }
    const int samples = radius * 2 + 1;
    // Three box passes per axis approximate a Gaussian closely enough for shadows.
    for (int pass = 0; pass < 6; ++pass) {
        const bool horizontal = pass % 2 == 0;
        const int lines = horizontal ? height : width, length = horizontal ? width : height;
        const auto index = [&](int line, int position) {
            return horizontal ? static_cast<size_t>(line) * width + position : static_cast<size_t>(position) * width + line;
        };
        for (int line = 0; line < lines; ++line) {
            const auto at = [&](int position) { return alpha[index(line, std::clamp(position, 0, length - 1))]; };
            int sum = 0;
            for (int position = -radius; position <= radius; ++position) sum += at(position);
            for (int position = 0; position < length; ++position) {
                scratch[index(line, position)] = sum / samples;
                sum += at(position + radius + 1) - at(position - radius);
            }
        }
        alpha.swap(scratch);
    }
    for (int y = 0; y < height; ++y) {
        auto* row = reinterpret_cast<QRgb*>(image.scanLine(y));
        for (int x = 0; x < width; ++x) row[x] = qRgba(0, 0, 0, alpha[static_cast<size_t>(y) * width + x]);
    }
}

struct Corner {
    QPointF apex;
    QPointF along;
    QPointF inward;
};
}

QPainterPath Squircle::path(const QRectF& rect, double radius, double smoothing) {
    QPainterPath result;
    if (!rect.isValid()) return result;
    const double budget = std::min(rect.width(), rect.height()) / 2;
    radius = std::clamp(radius, 0.0, budget);
    if (radius < 0.01) {
        result.addRect(rect);
        return result;
    }
    smoothing = std::clamp(std::min(smoothing, budget / radius - 1), 0.0, 1.0);
    const double extent = std::min((1 + smoothing) * radius, budget);
    const double arcMeasure = 90 * (1 - smoothing);
    const double arcLength = std::sin(radians(arcMeasure / 2)) * radius * std::sqrt(2.0);
    const double alpha = (90 - arcMeasure) / 2;
    const double tangent = radius * std::tan(radians(alpha / 2));
    const double beta = 45 * smoothing;
    const double c = tangent * std::cos(radians(beta));
    const double d = c * std::tan(radians(beta));
    const double b = std::max(0.0, (extent - arcLength - c - d) / 3);
    const double a = 2 * b;
    const double theta = radians(arcMeasure);
    const double handle = 4.0 / 3.0 * std::tan(theta / 4) * radius;

    const Corner corners[] = {
        {rect.topRight(), {1, 0}, {0, 1}},
        {rect.bottomRight(), {0, 1}, {-1, 0}},
        {rect.bottomLeft(), {-1, 0}, {0, -1}},
        {rect.topLeft(), {0, -1}, {1, 0}},
    };
    // Offsets are expressed along the incoming edge (u) and towards the outgoing edge (v).
    const auto map = [](const Corner& corner, double u, double v) {
        return corner.apex + corner.along * u + corner.inward * v;
    };
    for (int index = 0; index < 4; ++index) {
        const Corner& corner = corners[index];
        const QPointF start = map(corner, -extent, 0);
        if (index == 0) result.moveTo(start);
        else result.lineTo(start);
        const QPointF arcStart = map(corner, -arcLength - d, d);
        const QPointF arcEnd = map(corner, -d, d + arcLength);
        result.cubicTo(map(corner, -extent + a, 0), map(corner, -extent + a + b, 0), arcStart);
        const QPointF center = map(corner, -radius, radius);
        const auto tangentAt = [&](const QPointF& point) {
            const QPointF r = point - center;
            QPointF t(-r.y(), r.x());
            const double length = std::hypot(t.x(), t.y());
            if (length > 0) t /= length;
            const QPointF chord = arcEnd - arcStart;
            return QPointF::dotProduct(t, chord) >= 0 ? t : -t;
        };
        if (theta > 1e-6) result.cubicTo(arcStart + tangentAt(arcStart) * handle, arcEnd - tangentAt(arcEnd) * handle, arcEnd);
        result.cubicTo(map(corner, 0, d + arcLength + c), map(corner, 0, d + arcLength + b + c), map(corner, 0, extent));
    }
    result.closeSubpath();
    return result;
}

QImage Squircle::shadow(const QSize& size, double radius, int blur) {
    static QHash<QString, QImage> cache;
    blur = std::max(1, blur);
    const QString key = QStringLiteral("%1x%2:%3:%4").arg(size.width()).arg(size.height()).arg(qRound(radius)).arg(blur);
    if (const auto found = cache.constFind(key); found != cache.cend()) return *found;
    QImage image(size + QSize(blur * 4, blur * 4), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    {
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.fillPath(path(QRectF(blur * 2, blur * 2, size.width(), size.height()), radius), Qt::black);
    }
    blurAlpha(image, std::max(1, blur / 2));
    if (cache.size() > 24) cache.clear();
    cache.insert(key, image);
    return image;
}

#include "HudIcons.h"

#include <QPainter>
#include <QPainterPath>
#include <algorithm>
#include <array>

namespace {
double lerp(double from, double to, double progress) { return from + (to - from) * progress; }

double smoothstep(double edge0, double edge1, double value) {
    const double t = std::clamp((value - edge0) / (edge1 - edge0), 0.0, 1.0);
    return t * t * (3 - 2 * t);
}

QColor withAlpha(QColor color, double alpha) {
    color.setAlphaF(std::clamp(color.alphaF() * alpha, 0.0, 1.0));
    return color;
}
}

namespace HudIcons {
void skipIcon(QPainter& p, const QPointF& center, double size, const QColor& color, bool next) {
    p.save();
    p.translate(center);
    p.scale(size / 24, size / 24);
    if (!next) p.scale(-1, 1);
    p.setPen(QPen(color, 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(color);
    QPainterPath triangle;
    triangle.moveTo(-6.6, -6.8); triangle.lineTo(4.4, 0); triangle.lineTo(-6.6, 6.8); triangle.closeSubpath();
    p.drawPath(triangle);
    p.setPen(Qt::NoPen);
    p.drawRoundedRect(QRectF(6.2, -7.8, 2.8, 15.6), 1.4, 1.4);
    p.restore();
}

// Morphs a play triangle (0) into two pause bars (1) by moving matching quad vertices.
void playPauseIcon(QPainter& p, const QPointF& center, double size, const QColor& color, double morph) {
    static const std::array<QPointF, 8> play{QPointF(-5, -8), QPointF(2, -4), QPointF(2, 4), QPointF(-5, 8),
                                             QPointF(2, -4), QPointF(9, 0), QPointF(9, 0), QPointF(2, 4)};
    static const std::array<QPointF, 8> pause{QPointF(-6.6, -7.6), QPointF(-2.2, -7.6), QPointF(-2.2, 7.6), QPointF(-6.6, 7.6),
                                              QPointF(2.2, -7.6), QPointF(6.6, -7.6), QPointF(6.6, 7.6), QPointF(2.2, 7.6)};
    morph = std::clamp(morph, -0.15, 1.15);
    p.save();
    p.translate(center);
    p.scale(size / 24, size / 24);
    p.setPen(QPen(color, 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(color);
    for (int quad = 0; quad < 2; ++quad) {
        QPainterPath path;
        for (int corner = 0; corner < 4; ++corner) {
            const QPointF a = play[quad * 4 + corner], b = pause[quad * 4 + corner];
            const QPointF point(lerp(a.x(), b.x(), morph), lerp(a.y(), b.y(), morph));
            if (corner == 0) path.moveTo(point); else path.lineTo(point);
        }
        path.closeSubpath();
        p.drawPath(path);
    }
    p.restore();
}

void volumeIcon(QPainter& p, const QRectF& rect, const QColor& color, double size, double level) {
    p.save();
    p.translate(rect.center());
    p.scale(size / 24, size / 24);
    p.setPen(QPen(color, 1.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(color);
    QPainterPath body;
    body.moveTo(-10, -3.4); body.lineTo(-6, -3.4); body.lineTo(-0.6, -8.2);
    body.lineTo(-0.6, 8.2); body.lineTo(-6, 3.4); body.lineTo(-10, 3.4); body.closeSubpath();
    p.drawPath(body);
    p.setBrush(Qt::NoBrush);
    if (level <= 0.002) {
        p.setPen(QPen(color, 1.9, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(3.4, -3.4), QPointF(9.4, 3.4));
        p.drawLine(QPointF(9.4, -3.4), QPointF(3.4, 3.4));
    } else {
        for (int wave = 0; wave < 3; ++wave) {
            const double visible = wave == 0 ? 1.0 : smoothstep(wave / 3.0 - 0.1, wave / 3.0 + 0.06, level);
            if (visible <= 0.01) continue;
            const double radius = 4.2 + wave * 3.6;
            p.setPen(QPen(withAlpha(color, visible), 1.8, Qt::SolidLine, Qt::RoundCap));
            p.drawArc(QRectF(-radius + 0.6, -radius, radius * 2, radius * 2), -52 * 16, 104 * 16);
        }
    }
    p.restore();
}

void noteIcon(QPainter& p, const QRectF& rect, const QColor& color, double size) {
    p.save();
    p.translate(rect.center());
    p.scale(size / 24, size / 24);
    p.setPen(QPen(color, 2.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.drawLine(QPointF(5, -9), QPointF(5, 5));
    p.drawLine(QPointF(5, -9), QPointF(-5, -6));
    p.drawLine(QPointF(-5, -6), QPointF(-5, 8));
    p.setPen(Qt::NoPen);
    p.setBrush(color);
    p.drawEllipse(QRectF(-11, 4, 7, 5));
    p.drawEllipse(QRectF(-1, 1, 7, 5));
    p.restore();
}
}

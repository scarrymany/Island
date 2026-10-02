#pragma once

#include <QColor>
#include <QPointF>
#include <QRectF>

class QPainter;

// Vector glyphs shared by the island and its previews. Sizes follow a 24-unit grid.
namespace HudIcons {
void skipIcon(QPainter& p, const QPointF& center, double size, const QColor& color, bool next);
// morph: 0 shows play, 1 shows pause; values in between interpolate the outline.
void playPauseIcon(QPainter& p, const QPointF& center, double size, const QColor& color, double morph);
void volumeIcon(QPainter& p, const QRectF& rect, const QColor& color, double size, double level);
void noteIcon(QPainter& p, const QRectF& rect, const QColor& color, double size);
}

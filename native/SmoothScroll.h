#pragma once

#include <QColor>

class QAbstractScrollArea;

namespace SmoothScroll {
// Replaces the native vertical scroll bar of area with a slim overlay thumb that
// grows under the pointer, and turns mouse-wheel notches into short glides.
// Touchpad pixel scrolling and programmatic QScrollBar access keep working as before.
void install(QAbstractScrollArea* area);
void setColor(QAbstractScrollArea* area, const QColor& color);
void setMotion(QAbstractScrollArea* area, bool enabled);
}

#include "Layout.h"

#include <QJsonArray>
#include <algorithm>
#include <cmath>

namespace Layout {
QString formatTime(double seconds) {
    const auto value = std::isfinite(seconds) ? std::max(0, static_cast<int>(seconds)) : 0;
    const int hours = value / 3600;
    const int minutes = (value % 3600) / 60;
    const int remainder = value % 60;
    if (hours > 0)
        return QStringLiteral("%1:%2:%3").arg(hours).arg(minutes, 2, 10, QLatin1Char('0')).arg(remainder, 2, 10, QLatin1Char('0'));
    return QStringLiteral("%1:%2").arg(minutes).arg(remainder, 2, 10, QLatin1Char('0'));
}

QMap<QString, QRectF> elements(const QJsonObject& c) {
    const double width = c["width"].toDouble(560), height = c["height"].toDouble(132);
    const double gap = c["spacing"].toDouble(16), pad = std::max(12.0, gap);
    const double cover = std::max(16.0, std::min({c["cover_size"].toDouble(76), height - pad * 2, width * .3}));
    const double button = std::max(28.0, c["icon_size"].toDouble(18) + 14);
    const double line = c["font_size"].toDouble(13) + 9;
    const auto visible = c["visible"].toObject();
    const double left = visible["cover"].toBool(true) ? pad + cover + gap : pad;
    const double controls = width - pad - button * 3 - 8;
    const double textWidth = std::max(32.0, controls - left - gap);
    const double trackWidth = std::max(36.0, width - left - pad - 106);
    const double progressY = std::max(pad + line * 2, height - pad - 27);
    QMap<QString, QRectF> result{
        {"cover", {pad, pad, cover, cover}},
        {"title", {left, pad + 1, textWidth, line}},
        {"artist", {left, pad + line + 3, textWidth, line - 2}},
        {"album", {left, pad + line * 2 + 2, textWidth, line - 4}},
        {"source", {pad, std::min(height - line - 5, pad + cover + 6), cover, line - 3}},
        {"progress", {left, progressY, trackWidth, 14}},
        {"time", {left, progressY + 15, trackWidth, 17}},
        {"previous", {controls, pad + 9, button, button}},
        {"play", {controls + button + 4, pad + 9, button, button}},
        {"next", {controls + button * 2 + 8, pad + 9, button, button}},
        {"volume", {width - pad - 88, progressY - 1, 88, 24}}
    };
    const auto mode = c["layout"].toString("island");
    if (mode == "stacked") {
        const double controlsY = height - pad - button;
        const double progress = controlsY - 37;
        const double stackedTextWidth = std::max(16.0, width - left - pad);
        result["title"] = {left, pad, stackedTextWidth, line};
        result["artist"] = {left, pad + line + 3, stackedTextWidth, line - 2};
        result["album"] = {left, pad + line * 2 + 4, stackedTextWidth, line - 4};
        result["source"] = {left, pad + line * 3 + 2, stackedTextWidth, line - 3};
        result["progress"] = {pad, progress, width - pad * 2, 14};
        result["time"] = {pad, progress + 14, width - pad * 2, 17};
        result["previous"] = {width / 2 - button * 1.5 - 4, controlsY, button, button};
        result["play"] = {width / 2 - button / 2, controlsY, button, button};
        result["next"] = {width / 2 + button / 2 + 4, controlsY, button, button};
        result["volume"] = {width - pad - 88, controlsY + 4, 88, 24};
    }
    const auto positions = c["element_positions"].toObject();
    for (auto it = result.begin(); it != result.end();) {
        if (!visible[it.key()].toBool(true)) { it = result.erase(it); continue; }
        auto& rect = it.value();
        rect.setSize({std::min(rect.width(), width), std::min(rect.height(), height)});
        if (mode == "custom" && positions.contains(it.key())) {
            const auto point = positions[it.key()].toArray();
            if (point.size() == 2) rect.moveTopLeft({point[0].toDouble(), point[1].toDouble()});
        }
        rect.moveTopLeft({std::clamp(rect.x(), 0.0, width - rect.width()),
                          std::clamp(rect.y(), 0.0, height - rect.height())});
        ++it;
    }
    return result;
}

QPoint screenPosition(const QRect& screen, const QSize& window, const QString& anchor,
                      int offsetY, const QPoint* saved) {
    const QPoint local = anchor == "free" && saved ? *saved : QPoint((screen.width() - window.width()) / 2, offsetY);
    return screen.topLeft() + QPoint(std::clamp(local.x(), 0, std::max(0, screen.width() - window.width())),
                                     std::clamp(local.y(), 0, std::max(0, screen.height() - window.height())));
}

QRect dockGeometry(const QRect& screen, const QSize& window, int inset, int visibleHeight) {
    const int visible = std::clamp(visibleHeight, 1, std::max(1, window.height() - inset * 2));
    return {QPoint(screen.x() + (screen.width() - window.width()) / 2,
                   screen.y() + visible - window.height() + inset), window};
}
}

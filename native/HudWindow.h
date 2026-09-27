#pragma once

#include "AnimationClock.h"
#include "MediaBridge.h"
#include <QJsonObject>
#include <QImage>
#include <QMap>
#include <QPixmap>
#include <QTimer>
#include <QWidget>
#include <functional>
#include <optional>

class HudWindow final : public QWidget {
    Q_OBJECT
public:
    explicit HudWindow(const QJsonObject& config);
    ~HudWindow() override;
    void applyConfig(const QJsonObject& config);
    void setSnapshot(const MediaSnapshot& snapshot);
    void setVolume(double value);
    void setVolumeAvailable(bool available);
    void setEditing(bool enabled);
    bool editing() const { return editing_; }
    void reveal(bool manual = false);
    void conceal(bool manual = false);
    void toggle();
    void placeOnScreen();
    QMap<QString, QRectF> elementRects() const;
    const QJsonObject& config() const { return config_; }
signals:
    void command(const QString& action, double value);
    void volumeChanged(double value);
    void configChanged(const QJsonObject& config);
    void settingsRequested();
    void editingChanged(bool enabled);
protected:
    bool event(QEvent*) override;
    void paintEvent(QPaintEvent*) override;
    void showEvent(QShowEvent*) override;
    void hideEvent(QHideEvent*) override;
    void closeEvent(QCloseEvent*) override;
    void enterEvent(QEnterEvent*) override;
    void leaveEvent(QEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void contextMenuEvent(QContextMenuEvent*) override;
private:
    void applyNative();
    void updateNativeRegion();
    void setHudOpacity(double opacity);
    void applyDockGeometry(double progress);
    void setCollapsed(bool collapsed);
    bool pointerInDockArea() const;
    QScreen* targetScreen() const;
    void syncFrameTimer();
    void syncRefreshRate();
    void restartHideTimer();
    void watchScreen(QScreen* screen);
    void animate(const QString& name, double from, double to, std::function<void(double)> callback,
                 std::function<void()> finished = {});
    void stopAnimation(const QString& name);
    void paintElement(QPainter& painter, const QString& name, QRectF rect);
    void paintCover(QPainter& painter, const QRectF& rect, const QPixmap& image, double opacity);
    QPointF localPoint(const QPointF& point) const;
    QString hitTest(const QPointF& point) const;
    void volumeAt(const QPointF& point);
    bool seekEnabled() const;
    void previewSeekAt(const QPointF& point);
    void cancelSeek();
    double displayedPosition() const;
    QJsonObject config_;
    MediaSnapshot snapshot_;
    double volume_ = .5;
    bool volumeAvailable_ = true;
    bool editing_ = false, manualHidden_ = false, fadingOut_ = false, moved_ = false;
    bool collapsed_ = false, menuOpen_ = false, hoverFromDock_ = false;
    double dockProgress_ = 0;
    QRect expandedGeometry_, compactGeometry_;
    QString hover_, dragElement_;
    std::optional<QPointF> dragOrigin_;
    std::optional<QPoint> dragWindow_;
    std::optional<double> seekPreview_;
    QPixmap cover_, oldCover_;
    QImage artwork_, oldArtwork_, displayedArtwork_;
    double coverAlpha_ = 1, titleAlpha_ = 1, playAlpha_ = 1, hoverAlpha_ = 1;
    QChronoTimer frameTimer_;
    QTimer hideTimer_;
    AnimationClock animations_;
};

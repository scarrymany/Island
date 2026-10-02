#pragma once

#include "AnimationClock.h"
#include "MediaBridge.h"
#include <QChronoTimer>
#include <QColor>
#include <QElapsedTimer>
#include <QHash>
#include <QJsonObject>
#include <QImage>
#include <QMap>
#include <QPixmap>
#include <QThreadPool>
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
    void resetPosition();
    QMap<QString, QRectF> elementRects() const;
    const QJsonObject& config() const { return config_; }
    // Docking state and the visible card in global logical coordinates. The window
    // itself is a stable transparent frame, so its geometry no longer equals the card.
    bool isCollapsed() const { return collapsed_; }
    double dockProgress() const { return dockProgress_; }
    QRectF cardGeometry() const { return card_; }
    // Local position of the expanded card inside the window, used to map layout units.
    QPointF cardOrigin() const;
    // Local rectangle of the visible card with its transparent margin, for screenshots.
    QRect captureRect() const;
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
    enum class FrameMode { Union, Moving };

    void applyNative();
    void updateNativeRegion();
    void applyHitTesting();
    void setHudOpacity(double opacity);
    void applyDockGeometry(double progress);
    void updateCard();
    QRectF cardAt(double progress) const;
    QRect frameFor(const QRectF& card) const;
    double cardRadius() const;
    void setCollapsed(bool collapsed);
    bool pointerInDockArea() const;
    bool pointerKeepsOpen() const;
    QRectF dockHandle() const;
    QScreen* targetScreen() const;
    void syncFrameTimer();
    void syncTicker();
    void tick();
    bool marqueeWanted() const;
    void syncRefreshRate();
    void restartHideTimer();
    void watchScreen(QScreen* screen);
    void scheduleScreenPlacement();
    void sampleSentry();
    void beginRevealIntent(bool fromEnter);
    void finishRevealIntent();
    void cancelRevealIntent();
    bool revealGestureAllowed() const;
    void setPeek(bool engaged);
    void present(const MediaSnapshot& snapshot);
    struct Artwork {
        QImage image;
        QImage blurred;
        QColor accent;
    };
    static Artwork processArtwork(const QByteArray& bytes);
    void applyArtwork(const Artwork& artwork);
    void applyPendingDrag();
    void animate(const QString& name, double from, double to, std::function<void(double)> callback,
                 std::function<void()> finished = {});
    void stopAnimation(const QString& name);
    bool motionEnabled(const QString& name) const;
    std::chrono::milliseconds duration(double factor = 1) const;
    void setHover(const QString& name);
    void updateExpansion();
    void updateElement(const QString& name);
    void paintSurface(QPainter& painter, const QRectF& card, double radius);
    void paintElement(QPainter& painter, const QString& name, QRectF rect);
    void paintCover(QPainter& painter, const QRectF& rect, const QPixmap& image, double opacity, double scale);
    void paintTransport(QPainter& painter, const QString& name, const QRectF& rect);
    void paintProgress(QPainter& painter, const QRectF& rect);
    void paintVolume(QPainter& painter, const QRectF& rect);
    void paintText(QPainter& painter, const QString& name, const QRectF& rect);
    QColor accentColor() const;
    QColor progressColor() const;
    QPointF localPoint(const QPointF& point) const;
    QString hitTest(const QPointF& point) const;
    void volumeAt(const QPointF& point);
    bool seekEnabled() const;
    void previewSeekAt(const QPointF& point);
    void cancelScrub();
    void cancelSeek();
    double displayedPosition() const;
    QString displayText(const MediaSnapshot& snapshot, const QString& name) const;
    double titleWidth() const;
    void invalidateLayout();

    QJsonObject config_;
    MediaSnapshot snapshot_;
    MediaSnapshot presented_;
    MediaSnapshot previous_;
    double volume_ = .5;
    double shownVolume_ = .5;
    bool volumeAvailable_ = true;
    bool editing_ = false, manualHidden_ = false, autoHidden_ = false, fadingOut_ = false, moved_ = false;
    bool collapsed_ = false, menuOpen_ = false, hoverFromDock_ = false;
    bool intentFromEnter_ = false, cursorShownOutside_ = true, pointerHidden_ = false, fullscreenForeground_ = false;
    bool passThrough_ = false, cardHovered_ = false, presentedOnce_ = false;
    double dockProgress_ = 0;
    FrameMode frameMode_ = FrameMode::Moving;
    QRect expandedGeometry_, compactGeometry_, unionFrame_;
    QRectF expandedCard_, compactCard_, card_;
    QSize frameSize_;
    QString hover_, pressed_, dragElement_;
    std::optional<QPointF> dragOrigin_;
    std::optional<QPoint> dragWindow_;
    std::optional<QPoint> pendingDrag_;
    std::optional<QRect> pendingFrame_;
    std::optional<double> seekPreview_;
    std::optional<MediaSnapshot> pendingSeek_;
    QPixmap cover_, oldCover_;
    QImage artwork_, oldArtwork_, displayedArtwork_;
    QColor artworkAccent_, accentFrom_, progressFrom_;
    QHash<QString, double> hoverLevels_, pressLevels_;
    double coverAlpha_ = 1, textAlpha_ = 1, playMorph_ = 0, appear_ = 1;
    double progressExpand_ = 0, volumeExpand_ = 0, peek_ = 0, equalizer_ = 0;
    double marqueeOffset_ = 0, marqueePause_ = 0, equalizerPhase_ = 0;
    int marqueeBudget_ = 0;
    qint64 tickerLast_ = 0;
    QElapsedTimer clock_;
    mutable std::optional<QMap<QString, QRectF>> rects_;
    mutable double titleWidth_ = -1;
    quint64 artworkGeneration_ = 0;
    QThreadPool artworkPool_;
    QChronoTimer progressTimer_;
    QChronoTimer ticker_;
    QTimer hideTimer_;
    QTimer screenPlacementTimer_;
    QTimer seekTimer_;
    QTimer sentryTimer_;
    QTimer intentTimer_;
    QTimer inactiveTimer_;
    AnimationClock animations_;
};

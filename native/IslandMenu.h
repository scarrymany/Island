#pragma once

#include "AnimationClock.h"

#include <QColor>
#include <QMenu>

// Translucent context menu with continuous corners, a soft shadow, a sliding
// highlight and a short fade-in. Used for the island and the tray.
class IslandMenu final : public QMenu {
    Q_OBJECT

public:
    struct Theme {
        QColor background{"#141418"};
        QColor text{"#ECECF1"};
        QColor accent{"#FFFFFF"};
        QString fontFamily{QStringLiteral("Inter")};
        bool motion = true;
    };

    explicit IslandMenu(QWidget* parent = nullptr);
    explicit IslandMenu(const QString& title, QWidget* parent = nullptr);
    ~IslandMenu() override;
    IslandMenu* addIslandMenu(const QString& title);

    static void setTheme(const Theme& theme);
    static Theme theme();
    static constexpr int ShadowMargin = 12;

protected:
    void paintEvent(QPaintEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    void initialize();
    void trackHighlight();
    QRectF panelRect() const;

    AnimationClock motion_;
    QRectF highlight_;
    QRectF target_;
    double highlightAlpha_ = 0;
};

#pragma once

#include <QObject>
#include <QPointer>

class QWidget;
class ToolTipBubble;

// Replaces Qt's square system tooltips application-wide with a themed bubble
// that fades in, follows text changes (the island retargets its hint per control)
// and never takes focus or input.
class IslandToolTip final : public QObject {
public:
    static void install();
    static void hideText();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    explicit IslandToolTip(QObject* parent);
    void show(QWidget* target, const QPoint& globalPosition);
    void hide();

    QPointer<QWidget> target_;
    ToolTipBubble* bubble_ = nullptr;
};

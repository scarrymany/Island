#include "SettingsWindow.h"
#include "ConfigStore.h"
#include "AppAssets.h"
#include "AppInfo.h"
#include "SettingsControls.h"
#include "Layout.h"

#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QColorDialog>
#include <QCursor>
#include <QFocusEvent>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QDesktopServices>
#include <QFileDialog>
#include <QFontComboBox>
#include <QFontMetrics>
#include <QFormLayout>
#include <QFrame>
#include <QGuiApplication>
#include <QGraphicsOpacityEffect>
#include <QShowEvent>
#include <QHideEvent>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QKeyEvent>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QLinearGradient>
#include <QListWidget>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSizeGrip>
#include <QSpinBox>
#include <QSlider>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStyle>
#include <QVBoxLayout>
#include <QWindow>

#include <algorithm>

namespace {
class PresetPreview final : public QWidget {
public:
    PresetPreview() {
        setObjectName("presetPreview");
        setMinimumHeight(180);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }

    void setPreset(const ConfigStore::Preset& preset) {
        config_ = preset.settings;
        setAccessibleName(QStringLiteral("Предпросмотр пресета %1").arg(preset.name));
        setAccessibleDescription(preset.description);
        update();
    }

    QSize sizeHint() const override { return {360, 180}; }
    QSize minimumSizeHint() const override { return {0, 180}; }

protected:
    void paintEvent(QPaintEvent*) override {
        if (config_.isEmpty()) return;
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setRenderHint(QPainter::TextAntialiasing);
        const double panelWidth = config_["width"].toDouble();
        const double panelHeight = config_["height"].toDouble();
        const double scale = std::min({1.0, std::max(1, width() - 12) / panelWidth, (height() - 12) / panelHeight});
        painter.translate((width() - panelWidth * scale) / 2, (height() - panelHeight * scale) / 2);
        painter.scale(scale, scale);
        QLinearGradient surface(0, 0, panelWidth, panelHeight);
        surface.setColorAt(0, QColor(config_["background"].toString()));
        surface.setColorAt(1, QColor(config_[config_["gradient_enabled"].toBool() ? "gradient_color" : "background"].toString()));
        painter.setBrush(surface);
        QColor border(config_["border_color"].toString());
        border.setAlphaF(config_["border_opacity"].toDouble());
        const double borderWidth = config_["border_width"].toDouble();
        painter.setPen(borderWidth > 0 ? QPen(border, borderWidth) : QPen(Qt::NoPen));
        const double radius = config_["radius"].toDouble();
        painter.drawRoundedRect(QRectF(0, 0, panelWidth, panelHeight), radius, radius);
        const auto elements = Layout::elements(config_);
        const QColor accent(config_["accent_color"].toString());
        const QColor primary(config_["text_color"].toString());
        const QColor secondary(config_["secondary_color"].toString());
        for (auto it = elements.constBegin(); it != elements.constEnd(); ++it) {
            const QRectF rect = it.value();
            painter.save();
            painter.setClipRect(rect);
            if (it.key() == "cover") {
                QLinearGradient artwork(rect.topLeft(), rect.bottomRight());
                artwork.setColorAt(0, accent.darker(180));
                artwork.setColorAt(1, accent.lighter(120));
                painter.setPen(Qt::NoPen);
                painter.setBrush(artwork);
                painter.drawRoundedRect(rect, 10, 10);
                painter.setBrush(QColor(255, 255, 255, 65));
                painter.drawEllipse(rect.adjusted(rect.width() * 0.22, rect.height() * 0.22,
                    -rect.width() * 0.22, -rect.height() * 0.22));
            } else if (it.key() == "progress" || it.key() == "volume") {
                const double thickness = it.key() == "progress" ? config_["progress_height"].toDouble() : 3;
                const QRectF track(rect.x(), rect.center().y() - thickness / 2, rect.width(), thickness);
                painter.setPen(Qt::NoPen);
                painter.setBrush(QColor(secondary.red(), secondary.green(), secondary.blue(), 55));
                painter.drawRoundedRect(track, thickness / 2, thickness / 2);
                painter.setBrush(it.key() == "progress" ? QColor(config_["progress_color"].toString()) : accent);
                painter.drawRoundedRect(QRectF(track.topLeft(), QSizeF(track.width() * 0.42, track.height())), thickness / 2, thickness / 2);
            } else if (it.key() == "previous" || it.key() == "play" || it.key() == "next") {
                const QPointF center = rect.center();
                const double direction = it.key() == "previous" ? -1.0 : 1.0;
                QPainterPath triangle;
                triangle.moveTo(center + QPointF(-4 * direction, -6));
                triangle.lineTo(center + QPointF(6 * direction, 0));
                triangle.lineTo(center + QPointF(-4 * direction, 6));
                triangle.closeSubpath();
                painter.setPen(Qt::NoPen);
                painter.setBrush(QColor(config_["icon_color"].toString()));
                painter.drawPath(triangle);
                if (it.key() != "play")
                    painter.drawRect(QRectF(center.x() + (direction > 0 ? 7 : -9), center.y() - 6, 2, 12));
            } else {
                QString text;
                if (it.key() == "title") text = QStringLiteral("Музыка рядом");
                else if (it.key() == "artist") text = QStringLiteral("Любимый исполнитель");
                else if (it.key() == "album") text = QStringLiteral("Новый альбом");
                else if (it.key() == "source") text = QStringLiteral("Плеер");
                else if (it.key() == "time") text = "1:24 / 3:32";
                QFont font(config_["font_family"].toString());
                font.setPixelSize(it.key() == "time" || it.key() == "source" ? 11 : config_["font_size"].toInt());
                font.setWeight(it.key() == "title" ? static_cast<QFont::Weight>(config_["font_weight"].toInt()) : QFont::Normal);
                painter.setFont(font);
                painter.setPen(it.key() == "title" ? primary : secondary);
                painter.drawText(rect, Qt::AlignVCenter | Qt::AlignLeft,
                    QFontMetrics(font).elidedText(text, Qt::ElideRight, qRound(rect.width())));
            }
            painter.restore();
        }
    }

private:
    QJsonObject config_;
};

class StatusLabel final : public QLabel {
public:
    StatusLabel() {
        setObjectName("description");
        setTextFormat(Qt::PlainText);
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    }

    QSize sizeHint() const override { return {0, fontMetrics().height()}; }
    QSize minimumSizeHint() const override { return sizeHint(); }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setFont(font());
        painter.setPen(palette().color(foregroundRole()));
        painter.drawText(contentsRect(), Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine,
            fontMetrics().elidedText(text(), Qt::ElideRight, contentsRect().width()));
    }
};

class ButtonRow final : public QWidget {
public:
    explicit ButtonRow(const QList<QPushButton*>& buttons) : buttons_(buttons) {
        layout_ = new QBoxLayout(QBoxLayout::TopToBottom, this);
        layout_->setContentsMargins(0, 4, 0, 0);
        layout_->setSpacing(8);
        layout_->setSizeConstraint(QLayout::SetNoConstraint);
        for (auto* button : buttons_) layout_->addWidget(button);
        layout_->addStretch();
    }

    QSize minimumSizeHint() const override {
        auto result = QWidget::minimumSizeHint();
        int width = 0;
        for (auto* button : buttons_) width = std::max(width, button->minimumSizeHint().width());
        result.setWidth(width);
        return result;
    }

protected:
    void resizeEvent(QResizeEvent* event) override {
        updateDirection();
        QWidget::resizeEvent(event);
    }

    bool event(QEvent* event) override {
        if (layout_ && (event->type() == QEvent::LayoutRequest || event->type() == QEvent::StyleChange))
            updateDirection();
        return QWidget::event(event);
    }

private:
    void updateDirection() {
        int requiredWidth = std::max(0, static_cast<int>(buttons_.size()) - 1) * layout_->spacing();
        for (auto* button : buttons_) requiredWidth += button->sizeHint().width();
        layout_->setDirection(width() >= requiredWidth ? QBoxLayout::LeftToRight : QBoxLayout::TopToBottom);
    }

    QList<QPushButton*> buttons_;
    QBoxLayout* layout_ = nullptr;
};

class CaptionButton final : public QPushButton {
public:
    CaptionButton() { setFocusPolicy(Qt::TabFocus); }

protected:
    bool event(QEvent* event) override {
        const bool result = QPushButton::event(event);
        if (event->type() == QEvent::Enter || event->type() == QEvent::Leave
            || event->type() == QEvent::WindowActivate || event->type() == QEvent::WindowDeactivate)
            update();
        return result;
    }

    void focusInEvent(QFocusEvent* event) override {
        keyboardFocus_ = event->reason() == Qt::TabFocusReason || event->reason() == Qt::BacktabFocusReason
            || event->reason() == Qt::ShortcutFocusReason;
        QPushButton::focusInEvent(event);
    }

    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const bool hovered = window()->isActiveWindow() && underMouse()
            && rect().contains(mapFromGlobal(QCursor::pos()));
        if (hovered || isDown() || (hasFocus() && keyboardFocus_)) {
            QColor color = palette().color(QPalette::WindowText);
            color.setAlphaF(isDown() ? 0.14 : 0.08);
            if (hovered && objectName() == "windowClose") color = QColor("#C42B1C");
            painter.setPen(Qt::NoPen);
            painter.setBrush(color);
            painter.drawRoundedRect(rect(), 5, 5);
        }
        icon().paint(&painter, QRect(QPoint((width() - iconSize().width()) / 2,
            (height() - iconSize().height()) / 2), iconSize()));
    }

private:
    bool keyboardFocus_ = false;
};

const QList<QPair<QString, QString>> ElementNames = {
    {"cover", QStringLiteral("Обложка")}, {"title", QStringLiteral("Название трека")},
    {"artist", QStringLiteral("Исполнитель")}, {"album", QStringLiteral("Альбом")},
    {"source", QStringLiteral("Источник")}, {"progress", QStringLiteral("Прогресс")},
    {"time", QStringLiteral("Время")}, {"previous", QStringLiteral("Предыдущий трек")},
    {"play", QStringLiteral("Воспроизведение")}, {"next", QStringLiteral("Следующий трек")},
    {"volume", QStringLiteral("Громкость")}
};

QLabel* description(const QString& text)
{
    auto* label = new QLabel(text);
    label->setObjectName("description");
    label->setWordWrap(true);
    label->setTextFormat(Qt::PlainText);
    return label;
}

QWidget* buttonRow(const QList<QPushButton*>& buttons)
{
    return new ButtonRow(buttons);
}

QString blended(const QColor& foreground, const QColor& background, double amount)
{
    return QColor::fromRgbF(
        foreground.redF() * amount + background.redF() * (1.0 - amount),
        foreground.greenF() * amount + background.greenF() * (1.0 - amount),
        foreground.blueF() * amount + background.blueF() * (1.0 - amount)).name();
}

QIcon navigationIcon(char16_t glyph, const QColor& color)
{
    QPixmap pixmap(40, 40);
    pixmap.fill(Qt::transparent);
    pixmap.setDevicePixelRatio(2);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::TextAntialiasing);
    QFont font("Segoe MDL2 Assets");
    font.setPixelSize(17);
    painter.setFont(font);
    painter.setPen(color);
    painter.drawText(QRect(0, 0, 20, 20), Qt::AlignCenter, QString(QChar(glyph)));
    QIcon icon;
    for (const auto mode : {QIcon::Normal, QIcon::Active, QIcon::Selected})
        icon.addPixmap(pixmap, mode);
    return icon;
}

QJsonValue settingValue(const QJsonObject& config, const QString& key)
{
    const auto parts = key.split('/');
    return parts.size() == 2 ? config.value(parts[0]).toObject().value(parts[1]) : config.value(key);
}

void styleColorButton(QPushButton* button, const QString& value)
{
    const QColor color(value);
    const bool light = color.lightnessF() > 0.6;
    button->setText(value.toUpper());
    button->setStyleSheet(QStringLiteral(
        "QPushButton { background: %1; color: %2; border: 1px solid rgba(255,255,255,42);"
        "padding: 7px 16px; border-radius: 7px; font-family: 'Cascadia Mono', 'Consolas'; }"
        "QPushButton:hover { border: 1px solid %2; }").arg(value, light ? "#16171C" : "#FFFFFF"));
}
}

SettingsWindow::SettingsWindow(ConfigStore* store, QWidget* parent)
    : QWidget(parent), store_(store)
{
    Q_ASSERT(store_);
    AppAssets::settingsFontFamily();
    setObjectName("SettingsWindow");
    setWindowTitle(QStringLiteral("SCARP ISLAND - настройки"));
    const auto* initialScreen = QGuiApplication::primaryScreen();
    const QSize available = initialScreen ? initialScreen->availableGeometry().size() - QSize(32, 32) : QSize(1080, 800);
    setMinimumSize(QSize(820, 520).boundedTo(available));
    resize(QSize(1080, 800).boundedTo(available));
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_TranslucentBackground);

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    auto* frame = new QFrame;
    frame->setObjectName("windowFrame");
    outer->addWidget(frame);
    auto* shell = new QVBoxLayout(frame);
    shell->setContentsMargins(0, 0, 0, 0);
    shell->setSpacing(0);
    titleBar_ = new QWidget;
    titleBar_->setObjectName("titleBar");
    titleBar_->setAttribute(Qt::WA_StyledBackground);
    titleBar_->setFixedHeight(42);
    titleBar_->installEventFilter(this);
    auto* titleLayout = new QHBoxLayout(titleBar_);
    titleLayout->setContentsMargins(20, 0, 6, 0);
    titleLayout->setSpacing(0);
    auto* caption = new QLabel(QStringLiteral("SCARP ISLAND - настройки"));
    caption->setObjectName("windowCaption");
    caption->setAttribute(Qt::WA_TransparentForMouseEvents);
    titleLayout->addWidget(caption, 1);
    auto* minimize = new CaptionButton;
    auto* maximize = new CaptionButton;
    auto* close = new CaptionButton;
    const QList<QPair<QPushButton*, char16_t>> titleButtons = {{minimize, u'\uE921'}, {maximize, u'\uE922'}, {close, u'\uE8BB'}};
    for (const auto& button : titleButtons) {
        button.first->setProperty("captionButton", true);
        button.first->setFixedSize(42, 32);
        button.first->setIcon(navigationIcon(button.second, QColor("#CACBD7")));
        button.first->setIconSize(QSize(12, 12));
        titleLayout->addWidget(button.first);
    }
    minimize->setObjectName("windowMinimize");
    maximize->setObjectName("windowMaximize");
    close->setObjectName("windowClose");
    minimize->setToolTip(QStringLiteral("Свернуть"));
    maximize->setToolTip(QStringLiteral("Развернуть / восстановить"));
    close->setToolTip(QStringLiteral("Скрыть в системный трей"));
    minimize->setAccessibleName(minimize->toolTip());
    maximize->setAccessibleName(maximize->toolTip());
    close->setAccessibleName(close->toolTip());
    connect(minimize, &QPushButton::clicked, this, &QWidget::showMinimized);
    connect(maximize, &QPushButton::clicked, this, [this] {
        isMaximized() ? showNormal() : showMaximized();
        resizeGrip_->setVisible(!isMaximized());
    });
    connect(close, &QPushButton::clicked, this, &QWidget::close);
    shell->addWidget(titleBar_);
    auto* body = new QWidget;
    auto* bodyLayout = new QHBoxLayout(body);
    bodyLayout->setContentsMargins(0, 0, 0, 0);
    bodyLayout->setSpacing(0);
    shell->addWidget(body, 1);

    auto* sidebar = new QFrame;
    sidebar->setObjectName("sidebar");
    sidebar->setFixedWidth(224);
    sidebarLayout_ = new QVBoxLayout(sidebar);
    sidebarLayout_->setContentsMargins(16, 28, 16, 20);
    sidebarLayout_->setSpacing(8);
    auto* brandRow = new QWidget;
    auto* brandLayout = new QHBoxLayout(brandRow);
    brandLayout->setContentsMargins(8, 0, 0, 0);
    brandLayout->setSpacing(10);
    auto* logo = new QLabel;
    logo->setObjectName("brandIcon");
    logo->setPixmap(AppAssets::icon().pixmap(QSize(38, 38), devicePixelRatioF()));
    logo->setFixedSize(38, 38);
    brandLayout->addWidget(logo);
    auto* brand = new QLabel("SCARP\nISLAND");
    brand->setObjectName("brand");
    brandLayout->addWidget(brand, 1);
    sidebarLayout_->addWidget(brandRow);
    setWindowIcon(AppAssets::icon());
    sidebarSubtitle_ = description(QStringLiteral("Музыка всегда рядом"));
    sidebarLayout_->addWidget(sidebarSubtitle_);
    sidebarSpacer_ = new QSpacerItem(0, 28, QSizePolicy::Minimum, QSizePolicy::Fixed);
    sidebarLayout_->addItem(sidebarSpacer_);
    navigation_ = new SettingsNavigation;
    navigation_->setObjectName("navigation");
    navigation_->setFrameShape(QFrame::NoFrame);
    navigation_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    navigation_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    navigation_->setSpacing(4);
    navigation_->setIconSize(QSize(20, 20));
    const QStringList titles = {
        QStringLiteral("Внешний вид"), QStringLiteral("Компоновка"), QStringLiteral("Анимации"),
        QStringLiteral("Источники"), QStringLiteral("Профили"), QStringLiteral("Система"), QStringLiteral("Обновления")
    };
    const QList<char16_t> glyphs = {u'\uE790', u'\uE8A9', u'\uE768', u'\uE8D6', u'\uE8B7', u'\uE713', u'\uE895'};
    for (int index = 0; index < titles.size(); ++index) {
        auto* item = new QListWidgetItem(navigationIcon(glyphs[index], QColor("#B7B8C8")), titles[index]);
        item->setSizeHint(QSize(180, 43));
        navigation_->addItem(item);
    }
    sidebarLayout_->addWidget(navigation_, 1);
    sidebarHint_ = description(QStringLiteral("Все изменения сохраняются автоматически"));
    sidebarLayout_->addWidget(sidebarHint_);
    bodyLayout->addWidget(sidebar);

    auto* content = new QWidget;
    content->setObjectName("content");
    auto* contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(30, 27, 26, 20);
    contentLayout->setSpacing(18);
    heading_ = new QLabel;
    heading_->setObjectName("heading");
    contentLayout->addWidget(heading_);
    pages_ = new QStackedWidget;
    pages_->setObjectName("settingsPages");
    pageOpacity_ = new QGraphicsOpacityEffect(pages_);
    pageOpacity_->setOpacity(1.0);
    pages_->setGraphicsEffect(pageOpacity_);
    contentLayout->addWidget(pages_, 1);
    bodyLayout->addWidget(content, 1);

    auto* footer = new QFrame;
    footer->setObjectName("footer");
    auto* footerLayout = new QHBoxLayout(footer);
    footerLayout->setContentsMargins(22, 12, 22, 12);
    status_ = new StatusLabel;
    status_->setText(QStringLiteral("Готово к воспроизведению"));
    status_->setMinimumHeight(20);
    footerLayout->addWidget(status_, 1);
    auto* live = new QLabel(QStringLiteral("Изменения сразу в HUD"));
    live->setObjectName("liveLabel");
    footerLayout->addWidget(live);
    resizeGrip_ = new QSizeGrip(this);
    resizeGrip_->setFixedSize(16, 16);
    resizeGrip_->setToolTip(QStringLiteral("Изменить размер окна"));
    footerLayout->addWidget(resizeGrip_, 0, Qt::AlignBottom);
    shell->addWidget(footer);

    buildAppearance();
    buildLayout();
    buildAnimations();
    buildSources();
    buildProfiles();
    buildSystem();
    buildUpdates();
    for (auto* button : findChildren<QPushButton*>()) {
        if (button->property("captionButton").toBool()) continue;
        button->setProperty("mouseFocus", true);
        button->installEventFilter(this);
    }
    // AlignTop caps wrapped layouts to sizeHint(); a stretch preserves their full height-for-width.
    for (auto* scroll : pages_->findChildren<QScrollArea*>())
        qobject_cast<QVBoxLayout*>(scroll->widget()->layout())->addStretch();
    connect(navigation_, &QListWidget::currentRowChanged, this, [this, titles](int index) {
        if (index < 0 || index >= titles.size())
            return;
        heading_->setText(titles[index]);
        pages_->setCurrentIndex(index);
        animatePage();
    });
    connect(store_, &ConfigStore::configChanged, this, [this] { refresh(); });
    connect(qApp, &QGuiApplication::screenAdded, this, [this] { refresh(); });
    connect(qApp, &QGuiApplication::screenRemoved, this, [this] { refresh(); });
    navigation_->setCurrentRow(0);
    refresh();
    if (!store_->loadError().isEmpty())
        setStatus(store_->loadError());
}

QVBoxLayout* SettingsWindow::addPage(const QString& title, const QString& text)
{
    auto* page = new QWidget;
    page->setObjectName("page");
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 12, 16);
    layout->setSpacing(18);
    layout->addWidget(description(text));
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setWidget(page);
    scroll->setAccessibleName(title);
    pages_->addWidget(scroll);
    return layout;
}

QFormLayout* SettingsWindow::addGroup(QVBoxLayout* page, const QString& title)
{
    auto* group = new QFrame;
    group->setObjectName("settingsGroup");
    auto* layout = new QVBoxLayout(group);
    layout->setContentsMargins(20, 17, 20, 19);
    layout->setSpacing(15);
    auto* heading = new QLabel(title);
    heading->setObjectName("groupHeading");
    layout->addWidget(heading);
    auto* form = new QFormLayout;
    form->setHorizontalSpacing(24);
    form->setVerticalSpacing(12);
    form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    form->setFormAlignment(Qt::AlignTop);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    layout->addLayout(form);
    page->addWidget(group);
    return form;
}

void SettingsWindow::addNumber(QFormLayout* form, const QString& label, const QString& key,
                               const QString& suffix, bool decimal)
{
    const auto range = ConfigStore::numericRange(key);
    auto* slider = new SettingsSlider;
    slider->setObjectName(key);
    slider->setAccessibleName(label);
    slider->setDecimals(decimal ? 2 : 0);
    slider->setRange(range.first, range.second);
    slider->setSingleStep(decimal ? (key == "border_width" ? 0.25 : 0.01) : 1);
    slider->setSuffix(suffix);
    slider->slider()->setAccessibleName(label);
    slider->editor()->setAccessibleName(label + QStringLiteral(" - точное значение"));
    controls_.insert(key, slider);
    connect(slider, &SettingsSlider::valueChanged, this, [this, key, decimal](double value) {
        put(key, decimal ? QJsonValue(value) : QJsonValue(qRound(value)));
    });
    form->addRow(label, slider);
}

void SettingsWindow::addToggle(QFormLayout* form, const QString& label, const QString& key)
{
    auto* toggle = new SettingsToggle;
    toggle->setObjectName(key);
    toggle->setAccessibleName(label);
    controls_.insert(key, toggle);
    connect(toggle, &QCheckBox::toggled, this, [this, key](bool value) { put(key, value); });
    form->addRow(label, toggle);
}

void SettingsWindow::addColor(QFormLayout* form, const QString& label, const QString& key)
{
    auto* button = new QPushButton;
    button->setObjectName(key);
    button->setMinimumWidth(156);
    button->setAccessibleName(label);
    controls_.insert(key, button);
    connect(button, &QPushButton::clicked, this, [this, key, label] {
        const QColor color = QColorDialog::getColor(QColor(store_->config().value(key).toString()), this, label);
        if (color.isValid())
            put(key, color.name().toUpper());
    });
    form->addRow(label, button);
}

QComboBox* SettingsWindow::addChoice(QFormLayout* form, const QString& label, const QString& key,
                                    const QList<QPair<QString, QString>>& choices)
{
    auto* combo = new SettingsChoice;
    combo->setObjectName(key);
    combo->setMinimumWidth(170);
    combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    for (const auto& choice : choices)
        combo->addItem(choice.second, choice.first);
    controls_.insert(key, combo);
    connect(combo, &QComboBox::currentIndexChanged, this, [this, key, combo](int index) {
        if (index >= 0)
            put(key, key == "font_weight" ? QJsonValue(combo->itemData(index).toInt())
                                          : QJsonValue(combo->itemData(index).toString()));
    });
    form->addRow(label, combo);
    return combo;
}

void SettingsWindow::buildAppearance()
{
    auto* page = addPage(QStringLiteral("Внешний вид"), QStringLiteral("Настройте музыкальный островок под свой рабочий стол. Результат виден сразу."));
    auto* presets = addGroup(page, QStringLiteral("Готовые пресеты"));
    auto* presetChoice = new SettingsChoice;
    presetChoice->setObjectName("presetChoice");
    presetChoice->setAccessibleName(QStringLiteral("Готовый пресет оформления"));
    const auto availablePresets = ConfigStore::presets();
    for (const auto& preset : availablePresets)
        presetChoice->addItem(preset.name, preset.id);
    presets->addRow(QStringLiteral("Пресет"), presetChoice);
    auto* preview = new PresetPreview;
    presets->addRow(preview);
    auto* presetDescription = description({});
    presetDescription->setObjectName("presetDescription");
    presets->addRow(presetDescription);
    const auto previewSelection = [presetChoice, preview, presetDescription, availablePresets] {
        const int index = presetChoice->currentIndex();
        if (index < 0 || index >= availablePresets.size()) return;
        preview->setPreset(availablePresets[index]);
        presetDescription->setText(availablePresets[index].description);
    };
    connect(presetChoice, &QComboBox::currentIndexChanged, this, previewSelection);
    previewSelection();
    presets->addRow(description(QStringLiteral("Выбор показывает образец. Кнопка применяет только оформление, размер и компоновку HUD. Источник, монитор, сохранённые координаты, горячие клавиши и ваши профили сохраняются.")));
    auto* applyPreset = new QPushButton(QStringLiteral("Применить пресет"));
    applyPreset->setObjectName("applyPreset");
    connect(applyPreset, &QPushButton::clicked, this, [this, presetChoice] {
        QString error;
        if (!store_->applyPreset(presetChoice->currentData().toString(), &error))
            reportError(error);
        else
            setStatus(QStringLiteral("Применён пресет: %1").arg(presetChoice->currentText()));
    });
    presets->addRow(applyPreset);
    auto* themes = addGroup(page, QStringLiteral("Быстрый выбор темы"));
    QList<QPushButton*> buttons;
    for (const auto& name : {"Lunar", "Midnight", "Ember", "Mono"}) {
        auto* button = new QPushButton(QString::fromLatin1(name));
        connect(button, &QPushButton::clicked, this, [this, name] { applyTheme(QString::fromLatin1(name)); });
        buttons.append(button);
    }
    themes->addRow(buttonRow(buttons));
    auto* size = addGroup(page, QStringLiteral("Форма и размер"));
    addNumber(size, QStringLiteral("Ширина"), "width", " px");
    addNumber(size, QStringLiteral("Высота"), "height", " px");
    addNumber(size, QStringLiteral("Масштаб"), "scale", " x", true);
    addNumber(size, QStringLiteral("Размер обложки"), "cover_size", " px");
    addNumber(size, QStringLiteral("Скругление"), "radius", " px");
    addNumber(size, QStringLiteral("Отступы"), "spacing", " px");
    auto* surface = addGroup(page, QStringLiteral("Фон и цвета"));
    addNumber(surface, QStringLiteral("Непрозрачность"), "opacity", {}, true);
    addToggle(surface, QStringLiteral("Размывать фон за островком"), "blur");
    addToggle(surface, QStringLiteral("Фон из обложки"), "artwork_background");
    addNumber(surface, QStringLiteral("Выраженность обложки"), "artwork_background_strength", {}, true);
    surface->addRow(description(QStringLiteral("Цвета сильно размытой обложки окрашивают фон островка. Если обложки нет, используются выбранные ниже цвета.")));
    addColor(surface, QStringLiteral("Основной фон"), "background");
    addToggle(surface, QStringLiteral("Градиент"), "gradient_enabled");
    addColor(surface, QStringLiteral("Второй цвет градиента"), "gradient_color");
    addColor(surface, QStringLiteral("Акцент"), "accent_color");
    auto* border = addGroup(page, QStringLiteral("Контур островка"));
    addNumber(border, QStringLiteral("Толщина контура"), "border_width", " px", true);
    addColor(border, QStringLiteral("Цвет контура"), "border_color");
    addNumber(border, QStringLiteral("Непрозрачность контура"), "border_opacity", {}, true);
    border->addRow(description(QStringLiteral("Толщина 0 полностью отключает контур.")));
    auto* compact = addGroup(page, QStringLiteral("Компактная полоска"));
    compact->addRow(description(QStringLiteral("Когда островок свёрнут, большая часть панели находится за верхним краем монитора. Наведите мышь на видимую полоску, чтобы раскрыть HUD.")));
    addNumber(compact, QStringLiteral("Ширина"), "compact_width", " px");
    addNumber(compact, QStringLiteral("Полная высота"), "compact_height", " px");
    addNumber(compact, QStringLiteral("Высота видимой полоски"), "compact_visible_height", " px");
    addNumber(compact, QStringLiteral("Скругление"), "compact_radius", " px");
    addColor(compact, QStringLiteral("Фон полоски"), "compact_background");
    addNumber(compact, QStringLiteral("Непрозрачность"), "compact_opacity", {}, true);
    compact->addRow(description(QStringLiteral("Размеры задаются до масштабирования. Видимая полоска не может быть выше полной панели. Таймер сворачивания находится в разделе «Система».")));
    auto* text = addGroup(page, QStringLiteral("Текст и элементы"));
    auto* fonts = new SettingsFontChoice;
    fonts->setObjectName("font_family");
    controls_.insert("font_family", fonts);
    connect(fonts, &QFontComboBox::currentFontChanged, this, [this](const QFont& font) { put("font_family", font.family()); });
    text->addRow(QStringLiteral("Шрифт"), fonts);
    text->addRow(description(QStringLiteral("Встроены шесть шрифтов с кириллицей: Inter, Manrope, Golos Text, Rubik, IBM Plex Sans и JetBrains Mono. Устанавливать их в Windows не нужно.")));
    addNumber(text, QStringLiteral("Размер текста"), "font_size", " px");
    addChoice(text, QStringLiteral("Насыщенность шрифта"), "font_weight", {
        {"400", QStringLiteral("Обычный")}, {"500", QStringLiteral("Средний")},
        {"600", QStringLiteral("Полужирный")}, {"700", QStringLiteral("Жирный")},
        {"800", QStringLiteral("Очень жирный")}, {"900", QStringLiteral("Максимальный")}
    });
    addColor(text, QStringLiteral("Основной текст"), "text_color");
    addColor(text, QStringLiteral("Вторичный текст"), "secondary_color");
    addColor(text, QStringLiteral("Иконки"), "icon_color");
    addNumber(text, QStringLiteral("Размер иконок"), "icon_size", " px");
    addColor(text, QStringLiteral("Прогресс воспроизведения"), "progress_color");
    addNumber(text, QStringLiteral("Толщина прогресса"), "progress_height", " px");
}

void SettingsWindow::buildLayout()
{
    auto* page = addPage(QStringLiteral("Компоновка"), QStringLiteral("Выберите расположение элементов и монитор. Для точной настройки включите перетаскивание внутри HUD."));
    auto* layout = addGroup(page, QStringLiteral("Расположение элементов"));
    addChoice(layout, QStringLiteral("Компоновка"), "layout", {
        {"island", QStringLiteral("Островок")}, {"stacked", QStringLiteral("Вертикальная")}, {"custom", QStringLiteral("Своя компоновка")}
    });
    editLayout_ = new QPushButton(QStringLiteral("Перетаскивать элементы HUD"));
    editLayout_->setCheckable(true);
    editLayout_->setObjectName("accentButton");
    connect(editLayout_, &QPushButton::toggled, this, [this](bool enabled) {
        if (refreshing_)
            return;
        if (enabled && !put("layout", "custom")) {
            const QSignalBlocker blocker(editLayout_);
            editLayout_->setChecked(false);
            return;
        }
        editLayout_->setText(enabled ? QStringLiteral("Завершить редактирование") : QStringLiteral("Перетаскивать элементы HUD"));
        emit editLayoutChanged(enabled);
    });
    layout->addRow(editLayout_);
    layout->addRow(description(QStringLiteral("Во время редактирования элементы можно перемещать мышью. Координаты сохраняются автоматически.")));
    elementChoice_ = new SettingsChoice;
    for (const auto& element : ElementNames)
        elementChoice_->addItem(element.second, element.first);
    layout->addRow(QStringLiteral("Элемент"), elementChoice_);
    auto* coordinates = new QWidget;
    auto* coordinateLayout = new QHBoxLayout(coordinates);
    coordinateLayout->setContentsMargins(0, 0, 0, 0);
    elementX_ = new QDoubleSpinBox;
    elementY_ = new QDoubleSpinBox;
    for (auto* spin : {elementX_, elementY_}) {
        spin->setRange(0, 32768);
        spin->setDecimals(1);
        spin->setSuffix(" px");
        spin->setKeyboardTracking(false);
        spin->setButtonSymbols(QAbstractSpinBox::NoButtons);
        coordinateLayout->addWidget(spin);
        connect(spin, &QDoubleSpinBox::valueChanged, this, [this] { updateElementPosition(); });
    }
    elementX_->setPrefix("X: ");
    elementY_->setPrefix("Y: ");
    layout->addRow(QStringLiteral("Координаты в HUD"), coordinates);
    connect(elementChoice_, &QComboBox::currentIndexChanged, this, [this] { refreshCoordinates(); });
    auto* reset = new QPushButton(QStringLiteral("Сбросить позиции элементов"));
    connect(reset, &QPushButton::clicked, this, [this] { put("element_positions", QJsonObject{}); });
    layout->addRow(reset);

    auto* visible = addGroup(page, QStringLiteral("Видимые элементы"));
    for (const auto& entry : ElementNames)
        addToggle(visible, entry.second, "visible/" + entry.first);
    auto* display = addGroup(page, QStringLiteral("Монитор и положение"));
    addToggle(display, QStringLiteral("Закрепить островок"), "position_locked");
    display->addRow(description(QStringLiteral("Закрепление отключает перетаскивание всего островка мышью. Управление музыкой и редактирование отдельных элементов остаются доступны.")));
    auto* resetPosition = new QPushButton(QStringLiteral("Вернуть на начальное место"));
    resetPosition->setObjectName("resetPosition");
    resetPosition->setAccessibleName(QStringLiteral("Вернуть островок на начальное место"));
    resetPosition->setToolTip(QStringLiteral("Сверху по центру выбранного монитора, с исходным отступом. Работает и при закреплении."));
    connect(resetPosition, &QPushButton::clicked, this, &SettingsWindow::resetPositionRequested);
    display->addRow(resetPosition);
    monitorChoice_ = addChoice(display, QStringLiteral("Монитор"), "monitor", {});
    addChoice(display, QStringLiteral("Привязка"), "anchor", {
        {"top_center", QStringLiteral("Сверху по центру")}, {"free", QStringLiteral("Свободное положение")}
    });
    addNumber(display, QStringLiteral("Отступ от верхнего края"), "offset_y", " px");
    auto* monitorCoordinates = new QWidget;
    auto* monitorLayout = new QHBoxLayout(monitorCoordinates);
    monitorLayout->setContentsMargins(0, 0, 0, 0);
    monitorX_ = new QSpinBox;
    monitorY_ = new QSpinBox;
    for (auto* spin : {monitorX_, monitorY_}) {
        spin->setRange(-32768, 32768);
        spin->setSuffix(" px");
        spin->setKeyboardTracking(false);
        spin->setButtonSymbols(QAbstractSpinBox::NoButtons);
        monitorLayout->addWidget(spin);
        connect(spin, &QSpinBox::valueChanged, this, [this] { updateMonitorPosition(); });
    }
    monitorX_->setPrefix("X: ");
    monitorY_->setPrefix("Y: ");
    display->addRow(QStringLiteral("Позиция на мониторе"), monitorCoordinates);
    display->addRow(description(QStringLiteral("Координаты считаются от левого верхнего угла выбранного монитора и применяются в свободном режиме. Для каждого монитора сохраняется отдельная позиция.")));
}

void SettingsWindow::buildAnimations()
{
    auto* page = addPage(QStringLiteral("Анимации"), QStringLiteral("Настройте движение островка. Каждый эффект можно отключить отдельно."));
    auto* timing = addGroup(page, QStringLiteral("Плавность"));
    addNumber(timing, QStringLiteral("Длительность переходов"), "animation_duration", QStringLiteral(" мс"));
    timing->addRow(description(QStringLiteral("Эта длительность применяется и к сворачиванию островка у верхнего края.")));
    auto* panel = addGroup(page, QStringLiteral("Панель настроек"));
    addToggle(panel, QStringLiteral("Плавные переходы"), "settings_animations");
    addNumber(panel, QStringLiteral("Длительность"), "settings_animation_duration", QStringLiteral(" мс"));
    panel->addRow(description(QStringLiteral("Переходы страниц и переключатели обновляются с частотой монитора, на котором открыты настройки.")));
    auto* effects = addGroup(page, QStringLiteral("Эффекты островка"));
    const QList<QPair<QString, QString>> names = {
        {"appear", QStringLiteral("Появление HUD")}, {"disappear", QStringLiteral("Исчезновение HUD")},
        {"cover", QStringLiteral("Смена обложки")}, {"title", QStringLiteral("Смена названия трека")},
        {"progress", QStringLiteral("Плавный прогресс")}, {"hover", QStringLiteral("Подсветка при наведении")},
        {"play", QStringLiteral("Нажатие Play / Pause")},
        {"dock", QStringLiteral("Сворачивание и раскрытие у края")}
    };
    for (const auto& effect : names)
        addToggle(effects, effect.second, "animations/" + effect.first);
}

void SettingsWindow::buildSources()
{
    auto* page = addPage(QStringLiteral("Источники"), QStringLiteral("SCARP ISLAND получает музыку из системных медиасессий Windows. Поддерживаются Spotify, браузеры с SoundCloud и другие совместимые приложения."));
    auto* source = addGroup(page, QStringLiteral("Воспроизведение"));
    sourceChoice_ = addChoice(source, QStringLiteral("Источник музыки"), "source_id", {{"", QStringLiteral("Автоматически")}});
    source->addRow(description(QStringLiteral("В автоматическом режиме выбирается активное музыкальное приложение. Чтобы закрепить конкретный проигрыватель, выберите его в списке.")));
    sourceStatus_ = description(QStringLiteral("Ожидание медиасессии. Запустите музыку в приложении или браузере."));
    auto* state = addGroup(page, QStringLiteral("Состояние подключения"));
    state->addRow(sourceStatus_);
    state->addRow(description(QStringLiteral("Список источников обновляется автоматически. Если вкладка браузера не отображается, начните воспроизведение и проверьте поддержку системного управления медиа в браузере.")));
    auto* volume = addGroup(page, QStringLiteral("Громкость источника"));
    volume->addRow(description(QStringLiteral("Ползунок на островке меняет громкость выбранного приложения в микшере Windows. Для YouTube и SoundCloud изменяется громкость всего браузера, включая другие вкладки. Общая громкость устройства остаётся прежней.")));
    volume->addRow(description(QStringLiteral("Если приложение не создало звуковую сессию или его не удалось определить, ползунок временно недоступен. Запустите воспроизведение в нужном источнике.")));
}

void SettingsWindow::buildProfiles()
{
    auto* page = addPage(QStringLiteral("Профили"), QStringLiteral("Сохраняйте разные варианты HUD для работы и игр. Тема меняет оформление, а профиль хранит все настройки."));
    auto* profiles = addGroup(page, QStringLiteral("Профили HUD"));
    profileChoice_ = new SettingsChoice;
    profiles->addRow(QStringLiteral("Сохранённый профиль"), profileChoice_);
    auto* load = new QPushButton(QStringLiteral("Применить"));
    auto* save = new QPushButton(QStringLiteral("Сохранить как..."));
    save->setObjectName("accentButton");
    auto* remove = new QPushButton(QStringLiteral("Удалить"));
    connect(load, &QPushButton::clicked, this, [this] {
        if (profileChoice_->currentText().isEmpty())
            return;
        QString error;
        if (!store_->loadProfile(profileChoice_->currentText(), &error))
            reportError(error);
    });
    connect(save, &QPushButton::clicked, this, &SettingsWindow::saveProfile);
    connect(remove, &QPushButton::clicked, this, [this] {
        const QString name = profileChoice_->currentText();
        if (name.isEmpty() || QMessageBox::question(this, QStringLiteral("Удаление профиля"),
            QStringLiteral("Удалить профиль «%1»?").arg(name)) != QMessageBox::Yes)
            return;
        QString error;
        if (!store_->deleteProfile(name, &error))
            reportError(error);
    });
    profiles->addRow(buttonRow({load, save, remove}));
    profileStatus_ = description({});
    profiles->addRow(profileStatus_);
    auto* themes = addGroup(page, QStringLiteral("Темы оформления"));
    themeChoice_ = new SettingsChoice;
    themes->addRow(QStringLiteral("Тема"), themeChoice_);
    auto* apply = new QPushButton(QStringLiteral("Применить"));
    auto* saveThemeButton = new QPushButton(QStringLiteral("Сохранить тему..."));
    auto* removeTheme = new QPushButton(QStringLiteral("Удалить"));
    connect(apply, &QPushButton::clicked, this, [this] { applyTheme(themeChoice_->currentText()); });
    connect(saveThemeButton, &QPushButton::clicked, this, &SettingsWindow::saveTheme);
    connect(removeTheme, &QPushButton::clicked, this, [this] {
        const QString name = themeChoice_->currentText();
        if (name.isEmpty())
            return;
        if (name == "Lunar" || name == "Midnight" || name == "Ember" || name == "Mono") {
            reportError(QStringLiteral("Встроенные темы нельзя удалить"));
            return;
        }
        if (QMessageBox::question(this, QStringLiteral("Удаление темы"), QStringLiteral("Удалить тему «%1»?").arg(name)) != QMessageBox::Yes)
            return;
        QString error;
        if (!store_->deleteTheme(name, &error))
            reportError(error);
    });
    themes->addRow(buttonRow({apply, saveThemeButton, removeTheme}));
    auto* files = addGroup(page, QStringLiteral("Перенос настроек"));
    files->addRow(description(QStringLiteral("JSON-файл включает текущее оформление, профили и собственные темы. При импорте весь набор настроек заменяется содержимым файла.")));
    auto* importButton = new QPushButton(QStringLiteral("Импорт JSON"));
    auto* exportButton = new QPushButton(QStringLiteral("Экспорт JSON"));
    connect(importButton, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Импорт настроек"), {}, "JSON (*.json)");
        if (path.isEmpty())
            return;
        QString error;
        if (!store_->importFile(path, &error))
            reportError(error);
        else
            setStatus(QStringLiteral("Настройки импортированы"));
    });
    connect(exportButton, &QPushButton::clicked, this, [this] {
        const QString initial = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + "/island-settings.json";
        QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Экспорт настроек"), initial, "JSON (*.json)");
        if (path.isEmpty())
            return;
        if (!path.endsWith(".json", Qt::CaseInsensitive))
            path += ".json";
        QString error;
        if (!store_->exportFile(path, &error))
            reportError(error);
        else
            setStatus(QStringLiteral("Настройки экспортированы: %1").arg(path));
    });
    files->addRow(buttonRow({importButton, exportButton}));
}

void SettingsWindow::buildSystem()
{
    auto* page = addPage(QStringLiteral("Система"), QStringLiteral("Поведение HUD, горячие клавиши и оформление панели настроек."));
    auto* idle = addGroup(page, QStringLiteral("Сворачивание у верхнего края"));
    addToggle(idle, QStringLiteral("Сворачивать при бездействии"), "idle_collapse");
    addNumber(idle, QStringLiteral("Сворачивать через"), "idle_collapse_seconds", QStringLiteral(" сек"));
    idle->addRow(description(QStringLiteral("При бездействии островок уменьшается и уходит за верхний край выбранного монитора. На экране остаётся узкая полоска, которая раскрывает HUD при наведении мыши.")));
    auto* behavior = addGroup(page, QStringLiteral("Поведение островка"));
    auto* hotkey = new QKeySequenceEdit;
    hotkey->setAttribute(Qt::WA_StyledBackground);
    hotkey->setObjectName("hotkey");
    hotkey->setAccessibleName(QStringLiteral("Показать или скрыть HUD"));
    hotkey->setMaximumSequenceLength(1);
    hotkey->setClearButtonEnabled(false);
    if (auto* editor = hotkey->findChild<QLineEdit*>())
        editor->setTextMargins(12, 0, 12, 0);
    controls_.insert("hotkey", hotkey);
    connect(hotkey, &QKeySequenceEdit::editingFinished, this, [this, hotkey] {
        put("hotkey", hotkey->keySequence().toString(QKeySequence::PortableText));
    });
    auto* hotkeyRow = new QWidget;
    auto* hotkeyLayout = new QHBoxLayout(hotkeyRow);
    hotkeyLayout->setContentsMargins(0, 0, 0, 0);
    hotkeyLayout->setSpacing(8);
    hotkeyLayout->addWidget(hotkey, 1);
    auto* restoreHotkey = new QPushButton(QStringLiteral("Вернуть"));
    restoreHotkey->setObjectName("restoreHotkey");
    restoreHotkey->setToolTip(QStringLiteral("Вернуть Ctrl+Alt+M"));
    restoreHotkey->setAccessibleName(restoreHotkey->toolTip());
    connect(restoreHotkey, &QPushButton::clicked, this, [this] {
        put("hotkey", ConfigStore::defaults().value("hotkey"));
    });
    hotkeyLayout->addWidget(restoreHotkey);
    behavior->addRow(QStringLiteral("Показать / скрыть HUD"), hotkeyRow);
    behavior->addRow(description(QStringLiteral("Нажмите поле и новое сочетание клавиш. Кнопка «Вернуть» восстанавливает Ctrl+Alt+M.")));
    hideDelayChoice_ = new SettingsChoice;
    hideDelayChoice_->setObjectName("auto_hide_seconds");
    hideDelayChoice_->setAccessibleName(QStringLiteral("Полностью скрыть через"));
    hideDelayChoice_->addItem(QStringLiteral("Не скрывать"), 0);
    for (int seconds : {5, 10, 15, 30, 60, 120, 300})
        hideDelayChoice_->addItem(seconds < 60 ? QStringLiteral("Через %1 сек").arg(seconds)
                                              : QStringLiteral("Через %1 мин").arg(seconds / 60), seconds);
    hideDelayChoice_->addItem(QStringLiteral("Свой интервал"), -1);
    hideDelayValue_ = new QSpinBox;
    hideDelayValue_->setObjectName("customHideDelay");
    hideDelayValue_->setAccessibleName(QStringLiteral("Свой интервал скрытия в секундах"));
    hideDelayValue_->setRange(1, 3600);
    hideDelayValue_->setSuffix(QStringLiteral(" сек"));
    hideDelayValue_->setButtonSymbols(QAbstractSpinBox::NoButtons);
    hideDelayValue_->setKeyboardTracking(false);
    hideDelayValue_->hide();
    auto* delayRow = new QWidget;
    auto* delayLayout = new QHBoxLayout(delayRow);
    delayLayout->setContentsMargins(0, 0, 0, 0);
    delayLayout->addWidget(hideDelayChoice_, 1);
    delayLayout->addWidget(hideDelayValue_);
    connect(hideDelayChoice_, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (refreshing_ || index < 0) return;
        const int seconds = hideDelayChoice_->itemData(index).toInt();
        customHideDelay_ = seconds < 0;
        hideDelayValue_->setVisible(customHideDelay_);
        if (customHideDelay_) {
            hideDelayValue_->setFocus();
            hideDelayValue_->selectAll();
            put("auto_hide_seconds", hideDelayValue_->value());
        } else {
            put("auto_hide_seconds", seconds);
        }
    });
    connect(hideDelayValue_, &QSpinBox::valueChanged, this, [this](int seconds) {
        put("auto_hide_seconds", seconds);
    });
    behavior->addRow(QStringLiteral("Полностью скрыть через"), delayRow);
    behavior->addRow(description(QStringLiteral("Если включено сворачивание при бездействии, островок остаётся полоской у края. Для полного скрытия отключите сворачивание выше.")));
    addToggle(behavior, QStringLiteral("Пропускать клики сквозь HUD"), "click_through");
    addToggle(behavior, QStringLiteral("Запускать вместе с Windows"), "startup");
    behavior->addRow(description(QStringLiteral("При пропуске кликов управление доступно через исходный проигрыватель. Свёрнутая полоска остаётся доступной для наведения. Настройки открываются через значок SCARP ISLAND в трее.")));
    auto* panel = addGroup(page, QStringLiteral("Оформление настроек"));
    addColor(panel, QStringLiteral("Фон окна"), "settings_background");
    addColor(panel, QStringLiteral("Акцент интерфейса"), "settings_accent");
    addColor(panel, QStringLiteral("Цвет текста"), "settings_text");
    auto* panelFonts = new SettingsFontChoice;
    panelFonts->setObjectName("settings_font_family");
    controls_.insert("settings_font_family", panelFonts);
    connect(panelFonts, &QFontComboBox::currentFontChanged, this, [this](const QFont& font) {
        put("settings_font_family", font.family());
    });
    panel->addRow(QStringLiteral("Шрифт интерфейса"), panelFonts);
    addNumber(panel, QStringLiteral("Размер шрифта"), "settings_font_size", " pt");
    addNumber(panel, QStringLiteral("Непрозрачность окна"), "settings_opacity", {}, true);
    addToggle(panel, QStringLiteral("Размытие системного фона"), "settings_blur");
    auto* windows = addGroup(page, QStringLiteral("Поверх других окон"));
    windows->addRow(description(QStringLiteral("HUD закрепляется поверх обычных окон и игр в оконном режиме без рамки. Игры с эксклюзивным полноэкранным режимом могут скрывать внешние оверлеи.")));
}

void SettingsWindow::buildUpdates()
{
    auto* page = addPage(QStringLiteral("Обновления"), QStringLiteral("Проверка новых версий SCARP ISLAND через GitHub Releases."));
    auto* settings = addGroup(page, QStringLiteral("Источник обновлений"));
    auto* repository = new QPushButton(QString::fromLatin1(AppInfo::Repository));
    repository->setObjectName("projectRepository");
    repository->setIconSize(QSize(20, 20));
    repository->setCursor(Qt::PointingHandCursor);
    repository->setToolTip(QStringLiteral("Открыть SCARP ISLAND на GitHub"));
    repository->setAccessibleName(repository->toolTip());
    connect(repository, &QPushButton::clicked, this, [this] {
        if (!QDesktopServices::openUrl(QUrl(QString::fromLatin1(AppInfo::ProjectUrl))))
            reportError(QStringLiteral("Не удалось открыть браузер. Адрес проекта: %1").arg(QString::fromLatin1(AppInfo::ProjectUrl)));
    });
    settings->addRow(QStringLiteral("Проект на GitHub"), repository);
    addToggle(settings, QStringLiteral("Проверять при запуске"), "check_updates");
    settings->addRow(description(QStringLiteral("Обновления загружаются из официальных релизов SCARP ISLAND. На GitHub доступны история версий и исходный код.")));
    auto* current = addGroup(page, QStringLiteral("Доступная версия"));
    current->addRow(description(QStringLiteral("Установлена: SCARP ISLAND %1").arg(QApplication::applicationVersion())));
    updateStatus_ = description(QStringLiteral("Проверка ещё не выполнялась"));
    current->addRow(updateStatus_);
    updateCheck_ = new QPushButton(QStringLiteral("Проверить обновления"));
    updateInstall_ = new QPushButton(QStringLiteral("Установить обновление"));
    updateInstall_->setObjectName("accentButton");
    updateInstall_->setEnabled(false);
    connect(updateCheck_, &QPushButton::clicked, this, &SettingsWindow::checkUpdates);
    connect(updateInstall_, &QPushButton::clicked, this, &SettingsWindow::updateInstallRequested);
    current->addRow(buttonRow({updateCheck_, updateInstall_}));
    auto* releaseNotes = new QPushButton(QStringLiteral("Что нового в %1").arg(QString::fromLatin1(AppInfo::Version)));
    releaseNotes->setObjectName("releaseNotes");
    connect(releaseNotes, &QPushButton::clicked, this, &SettingsWindow::releaseNotesRequested);
    current->addRow(releaseNotes);
}

bool SettingsWindow::put(const QString& key, const QJsonValue& value)
{
    if (refreshing_)
        return true;
    auto config = store_->config();
    if (settingValue(config, key) == value)
        return true;
    const auto parts = key.split('/');
    if (parts.size() == 2) {
        auto object = config.value(parts[0]).toObject();
        object.insert(parts[1], value);
        config.insert(parts[0], object);
    } else {
        config.insert(key, value);
    }
    if (key == "layout" && value.toString() == "stacked") {
        config.insert("height", std::max(240, config.value("height").toInt()));
        config.insert("width", std::max(380, config.value("width").toInt()));
    }
    QString error;
    if (!store_->update(config, &error)) {
        reportError(error);
        refresh();
        return false;
    }
    if (key == "source_id")
        emit sourceChanged(value.toString());
    return true;
}

void SettingsWindow::setSources(const QList<QPair<QString, QString>>& sources)
{
    sources_ = sources;
    const QSignalBlocker blocker(sourceChoice_);
    sourceChoice_->clear();
    sourceChoice_->addItem(QStringLiteral("Автоматически"), "");
    for (const auto& source : sources_) {
        if (!source.first.isEmpty() && sourceChoice_->findData(source.first) < 0)
            sourceChoice_->addItem(source.second.isEmpty() ? source.first : source.second, source.first);
    }
    const QString selected = store_->config().value("source_id").toString();
    if (!selected.isEmpty() && sourceChoice_->findData(selected) < 0)
        sourceChoice_->addItem(QStringLiteral("Недоступен: %1").arg(selected), selected);
    sourceChoice_->setCurrentIndex(sourceChoice_->findData(selected));
}

void SettingsWindow::setStatus(const QString& message)
{
    status_->setText(message);
    status_->setToolTip(message);
    sourceStatus_->setText(message);
}

void SettingsWindow::setUpdateState(const QString& message, bool available, bool busy)
{
    updateBusy_ = busy;
    updateAvailable_ = available;
    updateStatus_->setText(message);
    updateCheck_->setEnabled(!busy);
    updateInstall_->setEnabled(available && !busy);
}

void SettingsWindow::setEditing(bool enabled)
{
    const QSignalBlocker blocker(editLayout_);
    editLayout_->setChecked(enabled);
    editLayout_->setText(enabled ? QStringLiteral("Завершить редактирование") : QStringLiteral("Перетаскивать элементы HUD"));
}

void SettingsWindow::refresh()
{
    if (refreshing_)
        return;
    refreshing_ = true;
    const auto config = store_->config();
    refreshMonitors();
    setSources(sources_);
    for (auto it = controls_.constBegin(); it != controls_.constEnd(); ++it) {
        const auto value = settingValue(config, it.key());
        QWidget* widget = it.value();
        const QSignalBlocker blocker(widget);
        if (auto* slider = qobject_cast<SettingsSlider*>(widget))
            slider->setValue(value.toDouble());
        else if (auto* toggle = qobject_cast<QCheckBox*>(widget))
            toggle->setChecked(value.toBool());
        else if (auto* decimal = qobject_cast<QDoubleSpinBox*>(widget))
            decimal->setValue(value.toDouble());
        else if (auto* integer = qobject_cast<QSpinBox*>(widget))
            integer->setValue(value.toInt());
        else if (auto* font = qobject_cast<QFontComboBox*>(widget))
            font->setCurrentFont(QFont(value.toString()));
        else if (auto* combo = qobject_cast<QComboBox*>(widget)) {
            const QString selected = value.isDouble() ? QString::number(value.toInt()) : value.toString();
            if (it.key() == "font_weight" && combo->findData(selected) < 0)
                combo->addItem(QStringLiteral("Своя (%1)").arg(selected), selected);
            combo->setCurrentIndex(combo->findData(selected));
        }
        else if (auto* hotkey = qobject_cast<QKeySequenceEdit*>(widget))
            hotkey->setKeySequence(QKeySequence::fromString(value.toString(), QKeySequence::PortableText));
        else if (auto* line = qobject_cast<QLineEdit*>(widget))
            line->setText(value.toString());
        else if (auto* button = qobject_cast<QPushButton*>(widget))
            styleColorButton(button, value.toString());
    }
    refreshCoordinates();
    refreshCollections();
    refreshHideDelay();
    updateStyle();
    const bool custom = config.value("layout").toString() == "custom";
    elementX_->setEnabled(custom);
    elementY_->setEnabled(custom);
    elementChoice_->setEnabled(custom);
    const bool free = config.value("anchor").toString() == "free";
    monitorX_->setEnabled(free);
    monitorY_->setEnabled(free);
    controls_.value("offset_y")->setEnabled(!free);
    controls_.value("gradient_color")->setEnabled(config.value("gradient_enabled").toBool());
    controls_.value("artwork_background_strength")->setEnabled(config.value("artwork_background").toBool());
    controls_.value("idle_collapse_seconds")->setEnabled(config.value("idle_collapse").toBool());
    const bool borderEnabled = config.value("border_width").toDouble() > 0;
    controls_.value("border_color")->setEnabled(borderEnabled);
    controls_.value("border_opacity")->setEnabled(borderEnabled);
    updateCheck_->setEnabled(!updateBusy_);
    updateInstall_->setEnabled(updateAvailable_ && !updateBusy_);
    controls_.value("settings_animation_duration")->setEnabled(config.value("settings_animations").toBool());
    refreshing_ = false;
    if (!custom && editLayout_->isChecked())
        editLayout_->setChecked(false);
}

void SettingsWindow::refreshMonitors()
{
    const QSignalBlocker blocker(monitorChoice_);
    monitorChoice_->clear();
    monitorChoice_->addItem(QStringLiteral("Основной монитор"), "");
    for (QScreen* screen : QGuiApplication::screens()) {
        const auto size = screen->geometry().size();
        monitorChoice_->addItem(QStringLiteral("%1 (%2 x %3)").arg(screen->name()).arg(size.width()).arg(size.height()), screen->name());
    }
    const QString selected = store_->config().value("monitor").toString();
    if (!selected.isEmpty() && monitorChoice_->findData(selected) < 0)
        monitorChoice_->addItem(QStringLiteral("Недоступен: %1").arg(selected), selected);
    monitorChoice_->setCurrentIndex(monitorChoice_->findData(selected));
}

QString SettingsWindow::currentMonitor() const
{
    const QString selected = store_->config().value("monitor").toString();
    if (!selected.isEmpty())
        return selected;
    const QScreen* primary = QGuiApplication::primaryScreen();
    return primary ? primary->name() : QString();
}

void SettingsWindow::refreshCoordinates()
{
    const auto config = store_->config();
    auto elementConfig = config;
    const QString elementKey = elementChoice_->currentData().toString();
    auto visible = config.value("visible").toObject();
    visible.insert(elementKey, true);
    elementConfig.insert("visible", visible);
    const auto element = Layout::elements(elementConfig).value(elementKey);
    const QSignalBlocker elementXBlocker(elementX_);
    const QSignalBlocker elementYBlocker(elementY_);
    elementX_->setValue(element.x());
    elementY_->setValue(element.y());
    const auto monitor = config.value("monitor_positions").toObject().value(currentMonitor()).toArray();
    const QSignalBlocker monitorXBlocker(monitorX_);
    const QSignalBlocker monitorYBlocker(monitorY_);
    QScreen* screen = QGuiApplication::primaryScreen();
    for (auto* candidate : QGuiApplication::screens()) {
        if (candidate->name() == currentMonitor()) {
            screen = candidate;
            break;
        }
    }
    const int defaultX = screen ? std::max(0, qRound((screen->geometry().width()
        - config.value("width").toDouble() * config.value("scale").toDouble()) / 2.0)) : 0;
    monitorX_->setValue(monitor.size() == 2 ? monitor[0].toInt() : defaultX);
    monitorY_->setValue(monitor.size() == 2 ? monitor[1].toInt() : config.value("offset_y").toInt());
}

void SettingsWindow::updateMonitorPosition()
{
    if (refreshing_ || currentMonitor().isEmpty())
        return;
    auto positions = store_->config().value("monitor_positions").toObject();
    positions.insert(currentMonitor(), QJsonArray{monitorX_->value(), monitorY_->value()});
    put("monitor_positions", positions);
}

void SettingsWindow::updateElementPosition()
{
    if (refreshing_)
        return;
    auto positions = store_->config().value("element_positions").toObject();
    positions.insert(elementChoice_->currentData().toString(), QJsonArray{elementX_->value(), elementY_->value()});
    put("element_positions", positions);
}

void SettingsWindow::refreshCollections()
{
    const QSignalBlocker profileBlocker(profileChoice_);
    const QString selectedProfile = profileChoice_->currentText();
    profileChoice_->clear();
    profileChoice_->addItems(store_->profiles());
    const QString active = store_->activeProfile();
    profileChoice_->setCurrentText(store_->profiles().contains(selectedProfile) ? selectedProfile : active);
    profileStatus_->setText(active.isEmpty() ? QStringLiteral("Профиль ещё не сохранён")
        : QStringLiteral("Загружен: %1. Кнопка «Сохранить как...» обновляет сохранённый снимок.").arg(active));
    const QSignalBlocker themeBlocker(themeChoice_);
    const QString selectedTheme = themeChoice_->currentText();
    themeChoice_->clear();
    themeChoice_->addItems(store_->themes());
    themeChoice_->setCurrentText(store_->themes().contains(selectedTheme) ? selectedTheme : "Lunar");
}

void SettingsWindow::saveProfile()
{
    bool accepted = false;
    const QString name = QInputDialog::getText(this, QStringLiteral("Сохранить профиль"),
        QStringLiteral("Название профиля"), QLineEdit::Normal, store_->activeProfile(), &accepted).trimmed();
    if (!accepted || name.isEmpty())
        return;
    if (store_->profiles().contains(name) && QMessageBox::question(this, QStringLiteral("Заменить профиль"),
        QStringLiteral("Перезаписать профиль «%1» текущими настройками?").arg(name)) != QMessageBox::Yes)
        return;
    QString error;
    if (!store_->saveProfile(name, &error))
        reportError(error);
}

void SettingsWindow::saveTheme()
{
    bool accepted = false;
    const QString name = QInputDialog::getText(this, QStringLiteral("Сохранить тему"),
        QStringLiteral("Название темы"), QLineEdit::Normal, {}, &accepted).trimmed();
    if (!accepted || name.isEmpty())
        return;
    if (store_->themes().contains(name) && QMessageBox::question(this, QStringLiteral("Заменить тему"),
        QStringLiteral("Перезаписать тему «%1» текущим оформлением?").arg(name)) != QMessageBox::Yes)
        return;
    QString error;
    if (!store_->saveTheme(name, &error))
        reportError(error);
}

void SettingsWindow::applyTheme(const QString& name)
{
    QString error;
    if (!store_->applyTheme(name, &error))
        reportError(error);
}

void SettingsWindow::reportError(const QString& error)
{
    QMessageBox::warning(this, QStringLiteral("Не удалось применить настройки"), error);
}

void SettingsWindow::updateStyle()
{
    const auto config = store_->config();
    const QColor background(config.value("settings_background").toString());
    const QColor foreground(config.value("settings_text").toString());
    const QString accent = config.value("settings_accent").toString();
    const QString muted = blended(foreground, background, 0.60);
    const QString surface = blended(foreground, background, 0.045);
    const QString border = blended(foreground, background, 0.09);
    const QString hover = blended(foreground, background, 0.07);
    const QString selected = blended(QColor(accent), background, 0.07);
    const int fontSize = config.value("settings_font_size").toInt();
    const int alpha = qRound(config.value("settings_opacity").toDouble() * 255);
    const QString panelBackground = QStringLiteral("rgba(%1,%2,%3,%4)").arg(background.red()).arg(background.green()).arg(background.blue()).arg(alpha);
    const QString accentText = QColor(accent).lightnessF() > 0.5 ? "#11121A" : "#FFFFFF";
    AppAssets::settingsFontFamily();
    QFont panelFont(config.value("settings_font_family").toString());
    panelFont.setPointSize(fontSize);
    panelFont.setWeight(QFont::DemiBold);
    QString family = panelFont.family();
    family.replace('\\', QStringLiteral("\\\\")).replace('"', QStringLiteral("\\\""));
    family.replace('\n', ' ').replace('\r', ' ');
    if (font() != panelFont) setFont(panelFont);
    if (auto* repository = findChild<QPushButton*>("projectRepository"))
        repository->setIcon(AppAssets::githubIcon(background.lightnessF() < 0.5));
    updateMotion();
    for (auto* toggle : findChildren<SettingsToggle*>())
        toggle->setColors(QColor(accent), QColor(hover), foreground);
    for (auto* slider : findChildren<SettingsSlider*>())
        slider->setColors(QColor(accent), QColor(hover), foreground);
    for (auto* choice : findChildren<SettingsChoice*>())
        choice->setColors(QColor(accent), QColor(hover), foreground);
    for (auto* choice : findChildren<SettingsFontChoice*>())
        choice->setColors(QColor(accent), QColor(hover), foreground);
    qobject_cast<SettingsNavigation*>(navigation_)->setColors(QColor(accent), QColor(selected), foreground);
    const QString sheet = QStringLiteral(R"(
        QWidget { color: %1; font-size: %2pt; font-weight: 600; font-family: "%13"; }
        QWidget#SettingsWindow { background: transparent; }
        QFrame#windowFrame { background: %3; border: 1px solid %4; border-radius: 14px; }
        QWidget#titleBar { background: transparent; border-bottom: 1px solid %4; }
        QLabel#windowCaption { color: %7; font-size: 10pt; }
        QPushButton[captionButton="true"] { background: transparent; border: none; border-radius: 5px; padding: 0; }
        QPushButton#projectRepository { text-align: left; padding: 10px 16px; }
        QWidget#content, QWidget#page, QScrollArea, QScrollArea > QWidget > QWidget { background: transparent; }
        QFrame#sidebar { background: %12; border-right: 1px solid %4; }
        QFrame#footer { background: transparent; border-top: 1px solid %4; }
        QFrame#settingsGroup { background: %5; border: 1px solid %4; border-radius: 10px; }
        QLabel { background: transparent; }
        QLabel#brand { font-size: 14pt; font-weight: 750; }
        QLabel#heading { font-size: 23pt; font-weight: 700; }
        QLabel#groupHeading { font-size: %6pt; font-weight: 600; }
        QLabel#description { color: %7; font-weight: 400; }
        QLabel#liveLabel { color: %8; font-size: 9pt; }
        QListWidget#navigation { background: transparent; border: none; outline: none; font-size: 10pt; font-weight: 500; }
        QPushButton { background: %9; border: 1px solid %4; border-radius: 8px; padding: 9px 14px; outline: none; }
        QPushButton:focus { border-color: %7; }
        QPushButton[mouseFocus="true"]:focus:!hover:!pressed:!checked { border-color: %4; }
        QPushButton:hover { background: %10; border-color: %8; }
        QPushButton:pressed, QPushButton:checked { background: %10; border-color: %8; }
        QPushButton#accentButton { background: %8; color: %11; font-weight: 600; border-color: %8; }
        QPushButton#accentButton:hover { background: %1; border-color: %1; color: %3; }
        QPushButton:disabled { background: transparent; color: %7; border-color: %4; }
        QPushButton#accentButton:disabled { background: %9; color: %7; border-color: %4; }
        QSpinBox, QDoubleSpinBox, QComboBox, QLineEdit, QKeySequenceEdit { background: %3; border: 1px solid %4;
            border-radius: 8px; padding: 9px 12px; min-height: 20px; selection-background-color: %10; }
        QSpinBox:focus, QDoubleSpinBox:focus, QComboBox:focus, QLineEdit:focus, QKeySequenceEdit:focus { border: 1px solid %7; }
        QSpinBox:disabled, QDoubleSpinBox:disabled, QComboBox:disabled { color: %7; background: transparent; }
        QComboBox { padding-right: 32px; }
        QComboBox::drop-down { border: none; width: 26px; }
        QComboBox::down-arrow { image: none; width: 0; height: 0; }
        QComboBox QAbstractItemView { background: %5; color: %1; selection-background-color: %10;
            padding: 6px; border: 1px solid %4; outline: none; }
        QComboBox QAbstractItemView::item { min-height: 30px; padding: 3px 8px; border-radius: 5px; }
        QKeySequenceEdit QLineEdit { background: transparent; border: none; padding: 0; }
        QSlider::groove:horizontal { height: 4px; background: %9; border-radius: 2px; }
        QSlider::sub-page:horizontal { background: %8; border-radius: 2px; }
        QSlider::handle:horizontal { background: %8; border: 2px solid %5; width: 14px;
            margin: -7px 0; border-radius: 9px; }
        QSlider::handle:horizontal:hover, QSlider::handle:horizontal:focus { border-color: %7; }
        QSlider::handle:horizontal:disabled, QSlider::sub-page:horizontal:disabled { background: %4; }
        QCheckBox { background: transparent; }
        QScrollBar:vertical { background: transparent; width: 8px; margin: 0; }
        QScrollBar::handle:vertical { background: %4; border-radius: 4px; min-height: 35px; }
        QScrollBar::handle:vertical:hover { background: %7; }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
        QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: transparent; }
        QToolTip { color: %1; background: %5; border: 1px solid %4; padding: 7px; }
        QDialog, QMessageBox, QInputDialog, QColorDialog { background: %3; }
    )").arg(foreground.name()).arg(fontSize).arg(panelBackground, border, surface).arg(fontSize + 1)
        .arg(muted, accent, hover, selected, accentText).arg(blended(foreground, background, 0.02)).arg(family);
    if (styleSheet() != sheet) setStyleSheet(sheet);
}

SettingsWindow::~SettingsWindow()
{
    motion_.stopAll();
}

void SettingsWindow::refreshHideDelay()
{
    const int seconds = store_->config().value("auto_hide_seconds").toInt();
    const QSignalBlocker choiceBlocker(hideDelayChoice_);
    const QSignalBlocker valueBlocker(hideDelayValue_);
    const int preset = hideDelayChoice_->findData(seconds);
    if (seconds == 0) customHideDelay_ = false;
    hideDelayChoice_->setCurrentIndex(customHideDelay_ || preset < 0 ? hideDelayChoice_->count() - 1 : preset);
    hideDelayValue_->setVisible(customHideDelay_ || preset < 0);
    hideDelayValue_->setValue(seconds > 0 ? seconds : 10);
}

void SettingsWindow::updateMotion()
{
    const auto config = store_->config();
    const bool enabled = config.value("settings_animations").toBool();
    const int duration = config.value("settings_animation_duration").toInt();
    const auto* display = screen();
    const double refreshRate = display ? display->refreshRate() : 60.0;
    motion_.setRefreshRate(refreshRate);
    for (auto* toggle : findChildren<SettingsToggle*>())
        toggle->setMotion(enabled, duration, refreshRate);
    for (auto* choice : findChildren<SettingsChoice*>())
        choice->setMotion(enabled, duration, refreshRate);
    for (auto* choice : findChildren<SettingsFontChoice*>())
        choice->setMotion(enabled, duration, refreshRate);
    qobject_cast<SettingsNavigation*>(navigation_)->setMotion(enabled, duration, refreshRate);
    if (!enabled) finishPageAnimation();
}

void SettingsWindow::animatePage()
{
    if (!pageOpacity_) return;
    finishPageAnimation();
    if (!isVisible() || !store_->config().value("settings_animations").toBool()) return;
    const auto duration = std::chrono::milliseconds(store_->config().value("settings_animation_duration").toInt());
    motion_.start("page", 0.15, 1.0, duration, [this](double opacity) {
        pageOpacity_->setOpacity(opacity);
    });
}

void SettingsWindow::finishPageAnimation()
{
    motion_.stop("page");
    if (pageOpacity_) pageOpacity_->setOpacity(1.0);
}

void SettingsWindow::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    disconnect(screenConnection_);
    const auto bindScreen = [this](QScreen* display) {
        disconnect(refreshRateConnection_);
        if (display)
            refreshRateConnection_ = connect(display, &QScreen::refreshRateChanged, this, [this] { updateMotion(); });
        updateMotion();
    };
    if (windowHandle())
        screenConnection_ = connect(windowHandle(), &QWindow::screenChanged, this, bindScreen);
    bindScreen(screen());
    animatePage();
}

void SettingsWindow::hideEvent(QHideEvent* event)
{
    finishPageAnimation();
    QWidget::hideEvent(event);
}

void SettingsWindow::closeEvent(QCloseEvent* event)
{
    if (editLayout_->isChecked())
        editLayout_->setChecked(false);
    hide();
    event->ignore();
}

void SettingsWindow::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    finishPageAnimation();
    const bool compact = height() < 680;
    if (!navigation_ || compactSidebar_ == compact)
        return;
    compactSidebar_ = compact;
    sidebarSubtitle_->setVisible(!compact);
    sidebarHint_->setVisible(!compact);
    sidebarLayout_->setContentsMargins(16, compact ? 12 : 28, 16, compact ? 12 : 20);
    sidebarSpacer_->changeSize(0, compact ? 6 : 28, QSizePolicy::Minimum, QSizePolicy::Fixed);
    navigation_->setSpacing(compact ? 1 : 4);
    const int rowHeight = compact ? std::max(31, navigation_->fontMetrics().height() + 8) : 43;
    for (int index = 0; index < navigation_->count(); ++index)
        navigation_->item(index)->setSizeHint(QSize(180, rowHeight));
    sidebarLayout_->invalidate();
    navigation_->scrollToItem(navigation_->currentItem());
}

bool SettingsWindow::eventFilter(QObject* watched, QEvent* event)
{
    if (auto* button = qobject_cast<QPushButton*>(watched)) {
        bool mouseFocus = button->property("mouseFocus").toBool();
        if (event->type() == QEvent::FocusIn) {
            const auto reason = static_cast<QFocusEvent*>(event)->reason();
            if (reason != Qt::PopupFocusReason && reason != Qt::ActiveWindowFocusReason)
                mouseFocus = reason != Qt::TabFocusReason && reason != Qt::BacktabFocusReason
                    && reason != Qt::ShortcutFocusReason;
        } else if (event->type() == QEvent::MouseButtonPress) {
            mouseFocus = true;
        } else if (event->type() == QEvent::KeyPress) {
            const auto key = static_cast<QKeyEvent*>(event)->key();
            if (key == Qt::Key_Space || key == Qt::Key_Return || key == Qt::Key_Enter)
                mouseFocus = false;
        }
        if (mouseFocus != button->property("mouseFocus").toBool()) {
            button->setProperty("mouseFocus", mouseFocus);
            button->style()->unpolish(button);
            button->style()->polish(button);
            button->update();
        }
    }
    if (watched == titleBar_) {
        if (event->type() == QEvent::MouseButtonPress) {
            const auto* mouse = static_cast<QMouseEvent*>(event);
            if (mouse->button() == Qt::LeftButton && windowHandle()) {
                windowHandle()->startSystemMove();
                return true;
            }
        }
        if (event->type() == QEvent::MouseButtonDblClick) {
            const auto* mouse = static_cast<QMouseEvent*>(event);
            if (mouse->button() == Qt::LeftButton) {
                isMaximized() ? showNormal() : showMaximized();
                resizeGrip_->setVisible(!isMaximized());
                return true;
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

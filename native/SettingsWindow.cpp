#include "SettingsWindow.h"
#include "ConfigStore.h"
#include "Layout.h"

#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QColorDialog>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFontComboBox>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSizeGrip>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStyleOptionButton>
#include <QVBoxLayout>
#include <QWindow>

#include <algorithm>

namespace {
void paintSteps(QAbstractSpinBox* spin)
{
    QPainter painter(spin);
    painter.setRenderHint(QPainter::Antialiasing);
    const QColor color = spin->palette().color(spin->isEnabled() ? QPalette::Text : QPalette::PlaceholderText);
    painter.setPen(QPen(color, 1.2, Qt::SolidLine, Qt::RoundCap));
    const double x = spin->width() - 12.0;
    const double upper = spin->height() * 0.25;
    const double lower = spin->height() * 0.75;
    painter.drawLine(QPointF(x - 3, upper), QPointF(x + 3, upper));
    painter.drawLine(QPointF(x, upper - 3), QPointF(x, upper + 3));
    painter.drawLine(QPointF(x - 3, lower), QPointF(x + 3, lower));
}

class NumberSpinBox final : public QSpinBox {
protected:
    void paintEvent(QPaintEvent* event) override
    {
        QSpinBox::paintEvent(event);
        paintSteps(this);
    }
};

class DecimalSpinBox final : public QDoubleSpinBox {
protected:
    void paintEvent(QPaintEvent* event) override
    {
        QDoubleSpinBox::paintEvent(event);
        paintSteps(this);
    }
};

class ToggleCheckBox final : public QCheckBox {
public:
    using QCheckBox::QCheckBox;

protected:
    void paintEvent(QPaintEvent* event) override
    {
        QCheckBox::paintEvent(event);
        if (!isChecked())
            return;
        QStyleOptionButton option;
        initStyleOption(&option);
        const QRect rect = style()->subElementRect(QStyle::SE_CheckBoxIndicator, &option, this);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(QColor(property("markColor").toString()), 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        QPolygonF mark;
        mark << QPointF(rect.left() + rect.width() * 0.25, rect.top() + rect.height() * 0.52)
             << QPointF(rect.left() + rect.width() * 0.44, rect.top() + rect.height() * 0.70)
             << QPointF(rect.left() + rect.width() * 0.76, rect.top() + rect.height() * 0.32);
        painter.drawPolyline(mark);
    }
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
    auto* widget = new QWidget;
    auto* layout = new QHBoxLayout(widget);
    layout->setContentsMargins(0, 4, 0, 0);
    layout->setSpacing(8);
    for (auto* button : buttons)
        layout->addWidget(button);
    layout->addStretch();
    return widget;
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
    return QIcon(pixmap);
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
    setObjectName("SettingsWindow");
    setWindowTitle(QStringLiteral("Island - настройки"));
    setMinimumSize(920, 680);
    resize(1080, 800);
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
    titleBar_->setFixedHeight(42);
    titleBar_->installEventFilter(this);
    auto* titleLayout = new QHBoxLayout(titleBar_);
    titleLayout->setContentsMargins(20, 0, 6, 0);
    titleLayout->setSpacing(0);
    auto* caption = new QLabel(QStringLiteral("Island - настройки"));
    caption->setObjectName("windowCaption");
    caption->setAttribute(Qt::WA_TransparentForMouseEvents);
    titleLayout->addWidget(caption, 1);
    auto* minimize = new QPushButton;
    auto* maximize = new QPushButton;
    auto* close = new QPushButton;
    const QList<QPair<QPushButton*, char16_t>> titleButtons = {{minimize, u'\uE921'}, {maximize, u'\uE922'}, {close, u'\uE8BB'}};
    for (const auto& button : titleButtons) {
        button.first->setObjectName("captionButton");
        button.first->setFixedSize(42, 32);
        button.first->setIcon(navigationIcon(button.second, QColor("#CACBD7")));
        button.first->setIconSize(QSize(12, 12));
        titleLayout->addWidget(button.first);
    }
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
    auto* sidebarLayout = new QVBoxLayout(sidebar);
    sidebarLayout->setContentsMargins(16, 28, 16, 20);
    sidebarLayout->setSpacing(8);
    auto* brand = new QLabel("island");
    brand->setObjectName("brand");
    sidebarLayout->addWidget(brand);
    auto* subtitle = description(QStringLiteral("Музыка всегда рядом"));
    sidebarLayout->addWidget(subtitle);
    sidebarLayout->addSpacing(28);
    navigation_ = new QListWidget;
    navigation_->setObjectName("navigation");
    navigation_->setFrameShape(QFrame::NoFrame);
    navigation_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    navigation_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
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
    sidebarLayout->addWidget(navigation_, 1);
    sidebarLayout->addWidget(description(QStringLiteral("Все изменения сохраняются автоматически")));
    bodyLayout->addWidget(sidebar);

    auto* content = new QWidget;
    content->setObjectName("content");
    auto* contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(30, 27, 26, 20);
    contentLayout->setSpacing(18);
    auto* header = new QHBoxLayout;
    heading_ = new QLabel;
    heading_->setObjectName("heading");
    header->addWidget(heading_, 1);
    auto* toggle = new QPushButton(QStringLiteral("Показать / скрыть HUD"));
    toggle->setToolTip(QStringLiteral("Переключить видимость музыкального островка"));
    connect(toggle, &QPushButton::clicked, this, &SettingsWindow::toggleHud);
    header->addWidget(toggle);
    contentLayout->addLayout(header);
    pages_ = new QStackedWidget;
    contentLayout->addWidget(pages_, 1);
    bodyLayout->addWidget(content, 1);

    auto* footer = new QFrame;
    footer->setObjectName("footer");
    auto* footerLayout = new QHBoxLayout(footer);
    footerLayout->setContentsMargins(22, 12, 22, 12);
    status_ = description(QStringLiteral("Готово к воспроизведению"));
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
    connect(navigation_, &QListWidget::currentRowChanged, this, [this, titles](int index) {
        if (index < 0 || index >= titles.size())
            return;
        heading_->setText(titles[index]);
        pages_->setCurrentIndex(index);
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
    layout->setAlignment(Qt::AlignTop);
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
    if (decimal) {
        auto* spin = new DecimalSpinBox;
        spin->setObjectName(key);
        spin->setRange(range.first, range.second);
        spin->setDecimals(2);
        spin->setSingleStep(0.05);
        spin->setSuffix(suffix);
        spin->setKeyboardTracking(false);
        spin->setButtonSymbols(QAbstractSpinBox::PlusMinus);
        spin->setMinimumWidth(156);
        controls_.insert(key, spin);
        connect(spin, &QDoubleSpinBox::valueChanged, this, [this, key](double value) { put(key, value); });
        form->addRow(label, spin);
    } else {
        auto* spin = new NumberSpinBox;
        spin->setObjectName(key);
        spin->setRange(static_cast<int>(range.first), static_cast<int>(range.second));
        spin->setSuffix(suffix);
        spin->setKeyboardTracking(false);
        spin->setButtonSymbols(QAbstractSpinBox::PlusMinus);
        spin->setMinimumWidth(156);
        if (key == "auto_hide_seconds")
            spin->setSpecialValueText(QStringLiteral("Не скрывать"));
        controls_.insert(key, spin);
        connect(spin, &QSpinBox::valueChanged, this, [this, key](int value) { put(key, value); });
        form->addRow(label, spin);
    }
}

void SettingsWindow::addToggle(QFormLayout* form, const QString& label, const QString& key)
{
    auto* toggle = new ToggleCheckBox;
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
    auto* combo = new QComboBox;
    combo->setObjectName(key);
    combo->setMinimumWidth(170);
    combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    for (const auto& choice : choices)
        combo->addItem(choice.second, choice.first);
    controls_.insert(key, combo);
    connect(combo, &QComboBox::currentIndexChanged, this, [this, key, combo](int index) {
        if (index >= 0)
            put(key, combo->itemData(index).toString());
    });
    form->addRow(label, combo);
    return combo;
}

void SettingsWindow::buildAppearance()
{
    auto* page = addPage(QStringLiteral("Внешний вид"), QStringLiteral("Настройте музыкальный островок под свой рабочий стол. Результат виден сразу."));
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
    auto* surface = addGroup(page, QStringLiteral("Поверхность и свет"));
    addNumber(surface, QStringLiteral("Непрозрачность"), "opacity", {}, true);
    addToggle(surface, QStringLiteral("Размывать фон за островком"), "blur");
    addColor(surface, QStringLiteral("Основной фон"), "background");
    addToggle(surface, QStringLiteral("Градиент"), "gradient_enabled");
    addColor(surface, QStringLiteral("Второй цвет градиента"), "gradient_color");
    addColor(surface, QStringLiteral("Акцент и подсветка"), "accent_color");
    auto* text = addGroup(page, QStringLiteral("Текст и элементы"));
    auto* fonts = new QFontComboBox;
    fonts->setObjectName("font_family");
    controls_.insert("font_family", fonts);
    connect(fonts, &QFontComboBox::currentFontChanged, this, [this](const QFont& font) { put("font_family", font.family()); });
    text->addRow(QStringLiteral("Шрифт"), fonts);
    addNumber(text, QStringLiteral("Размер текста"), "font_size", " pt");
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
    elementChoice_ = new QComboBox;
    for (const auto& element : ElementNames)
        elementChoice_->addItem(element.second, element.first);
    layout->addRow(QStringLiteral("Элемент"), elementChoice_);
    auto* coordinates = new QWidget;
    auto* coordinateLayout = new QHBoxLayout(coordinates);
    coordinateLayout->setContentsMargins(0, 0, 0, 0);
    elementX_ = new DecimalSpinBox;
    elementY_ = new DecimalSpinBox;
    for (auto* spin : {elementX_, elementY_}) {
        spin->setRange(0, 32768);
        spin->setDecimals(1);
        spin->setSuffix(" px");
        spin->setKeyboardTracking(false);
        spin->setButtonSymbols(QAbstractSpinBox::PlusMinus);
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
    auto* toggles = new QWidget;
    auto* grid = new QGridLayout(toggles);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setSpacing(14);
    for (int index = 0; index < ElementNames.size(); ++index) {
        const auto& entry = ElementNames[index];
        const QString key = "visible/" + entry.first;
        auto* checkbox = new ToggleCheckBox(entry.second);
        checkbox->setObjectName(key);
        controls_.insert(key, checkbox);
        connect(checkbox, &QCheckBox::toggled, this, [this, key](bool value) { put(key, value); });
        grid->addWidget(checkbox, index / 2, index % 2);
    }
    visible->addRow(toggles);
    auto* display = addGroup(page, QStringLiteral("Монитор и положение"));
    monitorChoice_ = addChoice(display, QStringLiteral("Монитор"), "monitor", {});
    addChoice(display, QStringLiteral("Привязка"), "anchor", {
        {"top_center", QStringLiteral("Сверху по центру")}, {"free", QStringLiteral("Свободное положение")}
    });
    addNumber(display, QStringLiteral("Отступ от верхнего края"), "offset_y", " px");
    auto* monitorCoordinates = new QWidget;
    auto* monitorLayout = new QHBoxLayout(monitorCoordinates);
    monitorLayout->setContentsMargins(0, 0, 0, 0);
    monitorX_ = new NumberSpinBox;
    monitorY_ = new NumberSpinBox;
    for (auto* spin : {monitorX_, monitorY_}) {
        spin->setRange(-32768, 32768);
        spin->setSuffix(" px");
        spin->setKeyboardTracking(false);
        spin->setButtonSymbols(QAbstractSpinBox::PlusMinus);
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
    auto* effects = addGroup(page, QStringLiteral("Эффекты"));
    const QList<QPair<QString, QString>> names = {
        {"appear", QStringLiteral("Появление HUD")}, {"disappear", QStringLiteral("Исчезновение HUD")},
        {"cover", QStringLiteral("Смена обложки")}, {"title", QStringLiteral("Смена названия трека")},
        {"progress", QStringLiteral("Плавный прогресс")}, {"hover", QStringLiteral("Подсветка при наведении")},
        {"play", QStringLiteral("Нажатие Play / Pause")}
    };
    for (const auto& effect : names)
        addToggle(effects, effect.second, "animations/" + effect.first);
}

void SettingsWindow::buildSources()
{
    auto* page = addPage(QStringLiteral("Источники"), QStringLiteral("Island получает музыку из системных медиасессий Windows. Поддерживаются Spotify, браузеры с SoundCloud и другие совместимые приложения."));
    auto* source = addGroup(page, QStringLiteral("Воспроизведение"));
    sourceChoice_ = addChoice(source, QStringLiteral("Источник музыки"), "source_id", {{"", QStringLiteral("Автоматически")}});
    source->addRow(description(QStringLiteral("В автоматическом режиме выбирается активное музыкальное приложение. Чтобы закрепить конкретный проигрыватель, выберите его в списке.")));
    sourceStatus_ = description(QStringLiteral("Ожидание медиасессии. Запустите музыку в приложении или браузере."));
    auto* state = addGroup(page, QStringLiteral("Состояние подключения"));
    state->addRow(sourceStatus_);
    state->addRow(description(QStringLiteral("Список источников обновляется автоматически. Если вкладка браузера не отображается, начните воспроизведение и проверьте поддержку системного управления медиа в браузере.")));
}

void SettingsWindow::buildProfiles()
{
    auto* page = addPage(QStringLiteral("Профили"), QStringLiteral("Сохраняйте разные варианты HUD для работы и игр. Тема меняет оформление, а профиль хранит все настройки."));
    auto* profiles = addGroup(page, QStringLiteral("Профили HUD"));
    profileChoice_ = new QComboBox;
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
    themeChoice_ = new QComboBox;
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
    auto* behavior = addGroup(page, QStringLiteral("Поведение островка"));
    auto* hotkey = new QKeySequenceEdit;
    hotkey->setObjectName("hotkey");
    hotkey->setMaximumSequenceLength(1);
    hotkey->setClearButtonEnabled(true);
    controls_.insert("hotkey", hotkey);
    connect(hotkey, &QKeySequenceEdit::editingFinished, this, [this, hotkey] {
        put("hotkey", hotkey->keySequence().toString(QKeySequence::PortableText));
    });
    behavior->addRow(QStringLiteral("Показать / скрыть HUD"), hotkey);
    addNumber(behavior, QStringLiteral("Автоскрытие после бездействия"), "auto_hide_seconds", QStringLiteral(" сек"));
    addToggle(behavior, QStringLiteral("Пропускать клики сквозь HUD"), "click_through");
    addToggle(behavior, QStringLiteral("Запускать вместе с Windows"), "startup");
    behavior->addRow(description(QStringLiteral("Если включён пропуск кликов, управление доступно через исходный проигрыватель. Панель настроек можно открыть через значок Island в системном трее.")));
    auto* panel = addGroup(page, QStringLiteral("Оформление настроек"));
    addColor(panel, QStringLiteral("Фон окна"), "settings_background");
    addColor(panel, QStringLiteral("Акцент интерфейса"), "settings_accent");
    addColor(panel, QStringLiteral("Цвет текста"), "settings_text");
    addNumber(panel, QStringLiteral("Размер шрифта"), "settings_font_size", " pt");
    addNumber(panel, QStringLiteral("Непрозрачность окна"), "settings_opacity", {}, true);
    addToggle(panel, QStringLiteral("Размытие системного фона"), "settings_blur");
    auto* windows = addGroup(page, QStringLiteral("Поверх других окон"));
    windows->addRow(description(QStringLiteral("HUD закрепляется поверх обычных окон и игр в оконном режиме без рамки. Игры с эксклюзивным полноэкранным режимом могут скрывать внешние оверлеи.")));
}

void SettingsWindow::buildUpdates()
{
    auto* page = addPage(QStringLiteral("Обновления"), QStringLiteral("Проверка новых версий Island через GitHub Releases."));
    auto* settings = addGroup(page, QStringLiteral("Источник обновлений"));
    auto* repository = new QLineEdit;
    repository->setObjectName("update_repository");
    repository->setMaxLength(140);
    repository->setPlaceholderText("owner/repository");
    controls_.insert("update_repository", repository);
    connect(repository, &QLineEdit::editingFinished, this, [this, repository] {
        put("update_repository", repository->text().trimmed());
    });
    settings->addRow(QStringLiteral("Репозиторий GitHub"), repository);
    addToggle(settings, QStringLiteral("Проверять при запуске"), "check_updates");
    settings->addRow(description(QStringLiteral("Укажите репозиторий, в котором публикуются релизы вашей сборки Island. Пустое поле отключает проверку обновлений.")));
    auto* current = addGroup(page, QStringLiteral("Доступная версия"));
    current->addRow(description(QStringLiteral("Установлена: Island %1").arg(QApplication::applicationVersion())));
    updateStatus_ = description(QStringLiteral("Проверка ещё не выполнялась"));
    current->addRow(updateStatus_);
    updateCheck_ = new QPushButton(QStringLiteral("Проверить обновления"));
    updateInstall_ = new QPushButton(QStringLiteral("Установить обновление"));
    updateInstall_->setObjectName("accentButton");
    updateInstall_->setEnabled(false);
    connect(updateCheck_, &QPushButton::clicked, this, &SettingsWindow::checkUpdates);
    connect(updateInstall_, &QPushButton::clicked, this, &SettingsWindow::updateInstallRequested);
    current->addRow(buttonRow({updateCheck_, updateInstall_}));
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
    if (key == "update_repository")
        setUpdateState(QStringLiteral("Источник изменён. Проверьте наличие новой версии."));
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
    updateCheck_->setEnabled(!busy && !store_->config().value("update_repository").toString().isEmpty());
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
        if (auto* toggle = qobject_cast<QCheckBox*>(widget))
            toggle->setChecked(value.toBool());
        else if (auto* decimal = qobject_cast<QDoubleSpinBox*>(widget))
            decimal->setValue(value.toDouble());
        else if (auto* integer = qobject_cast<QSpinBox*>(widget))
            integer->setValue(value.toInt());
        else if (auto* font = qobject_cast<QFontComboBox*>(widget))
            font->setCurrentFont(QFont(value.toString()));
        else if (auto* combo = qobject_cast<QComboBox*>(widget))
            combo->setCurrentIndex(combo->findData(value.toString()));
        else if (auto* hotkey = qobject_cast<QKeySequenceEdit*>(widget))
            hotkey->setKeySequence(QKeySequence::fromString(value.toString(), QKeySequence::PortableText));
        else if (auto* line = qobject_cast<QLineEdit*>(widget))
            line->setText(value.toString());
        else if (auto* button = qobject_cast<QPushButton*>(widget))
            styleColorButton(button, value.toString());
    }
    refreshCoordinates();
    refreshCollections();
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
    updateCheck_->setEnabled(!updateBusy_ && !config.value("update_repository").toString().isEmpty());
    updateInstall_->setEnabled(updateAvailable_ && !updateBusy_);
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
    const QString border = blended(foreground, background, 0.10);
    const QString hover = blended(foreground, background, 0.10);
    const QString selected = blended(QColor(accent), background, 0.18);
    const int fontSize = config.value("settings_font_size").toInt();
    const int alpha = qRound(config.value("settings_opacity").toDouble() * 255);
    const QString panelBackground = QStringLiteral("rgba(%1,%2,%3,%4)").arg(background.red()).arg(background.green()).arg(background.blue()).arg(alpha);
    const QString accentText = QColor(accent).lightnessF() > 0.5 ? "#11121A" : "#FFFFFF";
    for (auto* control : controls_) {
        if (auto* checkbox = qobject_cast<QCheckBox*>(control))
            checkbox->setProperty("markColor", accentText);
    }
    setStyleSheet(QStringLiteral(R"(
        QWidget { color: %1; font-family: 'Segoe UI'; font-size: %2pt; }
        QWidget#SettingsWindow { background: transparent; }
        QFrame#windowFrame { background: %3; border: 1px solid %4; border-radius: 13px; }
        QWidget#titleBar { background: transparent; }
        QLabel#windowCaption { color: %7; font-size: 10pt; }
        QPushButton#captionButton, QPushButton#windowClose { background: transparent; border: none; border-radius: 5px; padding: 0; }
        QPushButton#captionButton:hover { background: %9; }
        QPushButton#windowClose:hover { background: #C42B1C; }
        QWidget#content, QWidget#page, QScrollArea, QScrollArea > QWidget > QWidget { background: transparent; }
        QFrame#sidebar { background: transparent; border-right: 1px solid %4; }
        QFrame#footer { background: transparent; border-top: 1px solid %4; }
        QFrame#settingsGroup { background: %5; border: 1px solid %4; border-radius: 10px; }
        QLabel { background: transparent; }
        QLabel#brand { font-size: 30pt; font-weight: 650; padding-left: 10px; }
        QLabel#heading { font-size: 24pt; font-weight: 600; }
        QLabel#groupHeading { font-size: %6pt; font-weight: 600; }
        QLabel#description { color: %7; }
        QLabel#liveLabel { color: %8; font-size: 9pt; }
        QListWidget#navigation { background: transparent; border: none; outline: none; }
        QListWidget#navigation::item { padding: 0 11px; border-radius: 7px; color: %7; }
        QListWidget#navigation::item:hover { background: %9; color: %1; }
        QListWidget#navigation::item:selected { background: %10; color: %1; border-left: 3px solid %8; }
        QPushButton { background: %9; border: 1px solid %4; border-radius: 7px; padding: 8px 14px; }
        QPushButton:hover { background: %10; border-color: %8; }
        QPushButton:pressed, QPushButton:checked { background: %10; border-color: %8; }
        QPushButton#accentButton { background: %8; color: %11; font-weight: 600; border-color: %8; }
        QPushButton#accentButton:hover { background: %1; border-color: %1; color: %3; }
        QPushButton:disabled { background: transparent; color: %7; border-color: %4; }
        QPushButton#accentButton:disabled { background: %9; color: %7; border-color: %4; }
        QSpinBox, QDoubleSpinBox, QComboBox, QLineEdit, QKeySequenceEdit { background: %3; border: 1px solid %4;
            border-bottom: 1px solid %7; border-radius: 6px; padding: 7px 10px; min-height: 19px; }
        QSpinBox:focus, QDoubleSpinBox:focus, QComboBox:focus, QLineEdit:focus, QKeySequenceEdit:focus { border-bottom: 2px solid %8; }
        QSpinBox:disabled, QDoubleSpinBox:disabled, QComboBox:disabled { color: %7; background: transparent; }
        QSpinBox::up-button, QDoubleSpinBox::up-button { subcontrol-origin: border; subcontrol-position: top right;
            width: 24px; background: %9; border-left: 1px solid %4; border-bottom: 1px solid %4; border-top-right-radius: 5px; }
        QSpinBox::down-button, QDoubleSpinBox::down-button { subcontrol-origin: border; subcontrol-position: bottom right;
            width: 24px; background: %9; border-left: 1px solid %4; border-bottom-right-radius: 5px; }
        QSpinBox::up-button:hover, QSpinBox::down-button:hover, QDoubleSpinBox::up-button:hover, QDoubleSpinBox::down-button:hover { background: %10; }
        QComboBox::drop-down { border: none; width: 26px; }
        QComboBox QAbstractItemView { background: %5; color: %1; selection-background-color: %10; padding: 4px; border: 1px solid %4; }
        QCheckBox { spacing: 10px; background: transparent; min-height: 25px; }
        QCheckBox::indicator { width: 18px; height: 18px; background: %3; border: 1px solid %7; border-radius: 5px; }
        QCheckBox::indicator:checked { background: %8; border: 1px solid %8; image: none; }
        QCheckBox::indicator:hover { border-color: %8; }
        QScrollBar:vertical { background: transparent; width: 8px; margin: 0; }
        QScrollBar::handle:vertical { background: %4; border-radius: 4px; min-height: 35px; }
        QScrollBar::handle:vertical:hover { background: %7; }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
        QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: transparent; }
        QToolTip { color: %1; background: %5; border: 1px solid %4; padding: 7px; }
        QDialog, QMessageBox, QInputDialog, QColorDialog { background: %3; }
    )").arg(foreground.name()).arg(fontSize).arg(panelBackground, border, surface).arg(fontSize + 1)
        .arg(muted, accent, hover, selected, accentText));
}

void SettingsWindow::closeEvent(QCloseEvent* event)
{
    if (editLayout_->isChecked())
        editLayout_->setChecked(false);
    hide();
    event->ignore();
}

bool SettingsWindow::eventFilter(QObject* watched, QEvent* event)
{
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

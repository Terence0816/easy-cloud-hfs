#include "ui/MainWindow.h"

#include "app/Controller.h"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QColor>
#include <QComboBox>
#include <QCoreApplication>
#include <QCursor>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGraphicsDropShadowEffect>
#include <QFrame>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPixmap>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QSettings>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStyle>
#include <QStyleHints>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QToolTip>
#include <QUrl>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <cmath>
#include <functional>

namespace {
class ShareDropArea : public QFrame {
public:
    std::function<void(const QStringList &)> onDropPaths;

    explicit ShareDropArea(QWidget *parent = nullptr)
        : QFrame(parent)
    {
        setAcceptDrops(true);
        setFrameShape(QFrame::StyledPanel);
    }

protected:
    void dragEnterEvent(QDragEnterEvent *event) override
    {
        if (event->mimeData()->hasUrls()) {
            event->acceptProposedAction();
        }
    }

    void dropEvent(QDropEvent *event) override
    {
        QStringList paths;
        for (const QUrl &url : event->mimeData()->urls()) {
            if (url.isLocalFile()) {
                paths.append(url.toLocalFile());
            }
        }
        if (!paths.isEmpty() && onDropPaths) {
            onDropPaths(paths);
        }
        event->acceptProposedAction();
    }
};

class NoWheelSpinBox : public QSpinBox {
public:
    explicit NoWheelSpinBox(QWidget *parent = nullptr)
        : QSpinBox(parent)
    {
    }

protected:
    void wheelEvent(QWheelEvent *event) override
    {
        event->ignore();
    }
};

class NoWheelComboBox : public QComboBox {
public:
    explicit NoWheelComboBox(QWidget *parent = nullptr)
        : QComboBox(parent)
    {
    }

protected:
    void wheelEvent(QWheelEvent *event) override
    {
        event->ignore();
    }
};

enum class UiIcon {
    Dashboard,
    Shares,
    Settings,
    About,
    Download,
    Data,
    Connections,
    Speed
};

QPixmap drawUiIcon(UiIcon kind, const QColor &foreground, const QColor &background = Qt::transparent, int size = 48)
{
    QPixmap pixmap(size, size);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);

    if (background.alpha() > 0) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(background);
        painter.drawEllipse(QRectF(0.5, 0.5, size - 1.0, size - 1.0));
    }

    const qreal scale = size / 48.0;
    QPen pen(foreground, qMax<qreal>(1.6, 2.4 * scale), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);

    const auto P = [scale](qreal x, qreal y) { return QPointF(x * scale, y * scale); };
    const auto R = [scale](qreal x, qreal y, qreal w, qreal h) { return QRectF(x * scale, y * scale, w * scale, h * scale); };

    switch (kind) {
    case UiIcon::Dashboard:
        painter.drawRoundedRect(R(10, 10, 11, 11), 2.5 * scale, 2.5 * scale);
        painter.drawRoundedRect(R(27, 10, 11, 11), 2.5 * scale, 2.5 * scale);
        painter.drawRoundedRect(R(10, 27, 11, 11), 2.5 * scale, 2.5 * scale);
        painter.drawRoundedRect(R(27, 27, 11, 11), 2.5 * scale, 2.5 * scale);
        break;
    case UiIcon::Shares: {
        QPainterPath path;
        path.moveTo(P(8, 17));
        path.lineTo(P(19, 17));
        path.lineTo(P(23, 12));
        path.lineTo(P(39, 12));
        path.lineTo(P(41, 35));
        path.quadTo(P(41, 39), P(37, 39));
        path.lineTo(P(11, 39));
        path.quadTo(P(7, 39), P(7, 35));
        path.closeSubpath();
        painter.drawPath(path);
        break;
    }
    case UiIcon::Settings:
        painter.drawEllipse(R(18, 18, 12, 12));
        for (int i = 0; i < 8; ++i) {
            const qreal a = i * 3.14159265358979323846 / 4.0;
            const QPointF c = P(24, 24);
            const QPointF p1 = c + QPointF(std::cos(a) * 10 * scale, std::sin(a) * 10 * scale);
            const QPointF p2 = c + QPointF(std::cos(a) * 15 * scale, std::sin(a) * 15 * scale);
            painter.drawLine(p1, p2);
        }
        painter.drawEllipse(R(7, 7, 34, 34));
        break;
    case UiIcon::About:
        painter.drawEllipse(R(8, 8, 32, 32));
        painter.setFont(QFont(QStringLiteral("Microsoft JhengHei UI"), qMax(8, int(17 * scale)), QFont::Bold));
        painter.drawText(R(8, 8, 32, 32), Qt::AlignCenter, QStringLiteral("i"));
        break;
    case UiIcon::Download:
        painter.drawLine(P(24, 12), P(24, 31));
        painter.drawLine(P(16, 24), P(24, 32));
        painter.drawLine(P(32, 24), P(24, 32));
        painter.drawLine(P(14, 37), P(34, 37));
        break;
    case UiIcon::Data:
        painter.drawLine(P(24, 36), P(24, 17));
        painter.drawLine(P(16, 24), P(24, 16));
        painter.drawLine(P(32, 24), P(24, 16));
        painter.drawLine(P(14, 11), P(34, 11));
        break;
    case UiIcon::Connections:
        painter.drawEllipse(R(19, 12, 10, 10));
        painter.drawArc(R(14, 21, 20, 16), 20 * 16, 140 * 16);
        painter.drawEllipse(R(9, 17, 7, 7));
        painter.drawEllipse(R(32, 17, 7, 7));
        painter.drawArc(R(5, 23, 15, 12), 20 * 16, 120 * 16);
        painter.drawArc(R(28, 23, 15, 12), 40 * 16, 120 * 16);
        break;
    case UiIcon::Speed:
        painter.drawArc(R(10, 12, 28, 28), 20 * 16, 140 * 16);
        painter.drawLine(P(24, 27), P(32, 18));
        painter.drawEllipse(R(21.5, 24.5, 5, 5));
        break;
    }

    return pixmap;
}

QIcon buildSidebarIcon(UiIcon kind)
{
    QIcon icon;
    const QPixmap normal = drawUiIcon(kind, QColor(QStringLiteral("#64748b")), Qt::transparent, 24);
    const QPixmap active = drawUiIcon(kind, QColor(QStringLiteral("#6d5ce7")), Qt::transparent, 24);
    icon.addPixmap(normal, QIcon::Normal, QIcon::Off);
    icon.addPixmap(active, QIcon::Normal, QIcon::On);
    icon.addPixmap(active, QIcon::Active, QIcon::Off);
    icon.addPixmap(active, QIcon::Selected, QIcon::On);
    return icon;
}

void applySoftShadow(QWidget *widget, int blurRadius = 24, int yOffset = 7)
{
    auto *shadow = new QGraphicsDropShadowEffect(widget);
    shadow->setBlurRadius(blurRadius);
    shadow->setOffset(0, yOffset);
    shadow->setColor(QColor(26, 38, 64, 28));
    widget->setGraphicsEffect(shadow);
}

QFrame *makeCard(const QString &title,
                 QLabel **valueLabel,
                 QLabel **titleLabelOut,
                 UiIcon iconKind,
                 const QColor &iconColor,
                 const QColor &iconBackground,
                 QWidget *parent = nullptr)
{
    auto *card = new QFrame(parent);
    card->setObjectName(QStringLiteral("StatCard"));
    applySoftShadow(card, 22, 6);

    auto *layout = new QVBoxLayout(card);
    layout->setContentsMargins(22, 20, 22, 22);
    layout->setSpacing(9);

    auto *icon = new QLabel(card);
    icon->setObjectName(QStringLiteral("StatIcon"));
    icon->setFixedSize(48, 48);
    icon->setPixmap(drawUiIcon(iconKind, iconColor, iconBackground, 48));
    icon->setAlignment(Qt::AlignCenter);

    auto *titleLabel = new QLabel(title, card);
    titleLabel->setObjectName(QStringLiteral("CardTitle"));

    auto *value = new QLabel(QStringLiteral("0"), card);
    value->setObjectName(QStringLiteral("CardValue"));

    layout->addWidget(icon, 0, Qt::AlignLeft);
    layout->addWidget(titleLabel);
    layout->addWidget(value);
    layout->addStretch();

    if (valueLabel) {
        *valueLabel = value;
    }
    if (titleLabelOut) {
        *titleLabelOut = titleLabel;
    }
    return card;
}

QPushButton *makeSidebarButton(const QString &text, UiIcon iconKind, QWidget *parent = nullptr)
{
    auto *button = new QPushButton(text, parent);
    button->setCheckable(true);
    button->setCursor(Qt::PointingHandCursor);
    button->setMinimumHeight(50);
    button->setIcon(buildSidebarIcon(iconKind));
    button->setIconSize(QSize(22, 22));
    button->setObjectName(QStringLiteral("SidebarButton"));
    return button;
}

QLabel *makeFormLabel(const QString &text, QWidget *parent = nullptr)
{
    auto *label = new QLabel(text, parent);
    label->setObjectName(QStringLiteral("FieldLabel"));
    label->setAlignment(Qt::AlignCenter);
    label->setMinimumHeight(40);
    label->setMinimumWidth(118);
    return label;
}

QScrollArea *makeScrollablePage(QWidget *page, QWidget *parent = nullptr)
{
    auto *scrollArea = new QScrollArea(parent);
    scrollArea->setFrameShape(QFrame::NoFrame);
    scrollArea->setWidgetResizable(true);
    scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scrollArea->setWidget(page);
    return scrollArea;
}

QPixmap buildShareIcon(const ShareItem &share)
{
    const bool isFolder = share.type == ShareType::Directory || share.type == ShareType::VirtualDirectory;

    QPixmap pixmap(72, 72);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(isFolder ? QColor(QStringLiteral("#ffe8a3")) : QColor(QStringLiteral("#cfe9ff")));
    painter.drawRoundedRect(pixmap.rect(), 20, 20);

    painter.setPen(isFolder ? QColor(QStringLiteral("#cf9600")) : QColor(QStringLiteral("#148fe8")));
    painter.setFont(QFont(QStringLiteral("Microsoft JhengHei UI"), 24, QFont::Bold));
    painter.drawText(pixmap.rect(), Qt::AlignCenter, isFolder ? QStringLiteral("D") : QStringLiteral("F"));
    return pixmap;
}

bool isDirectoryShare(const ShareItem &share)
{
    return share.type == ShareType::Directory || share.type == ShareType::VirtualDirectory;
}

bool systemPrefersDarkTheme()
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    return QGuiApplication::styleHints()
           && QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
#else
    // Qt 5.15 has no QStyleHints::colorScheme(). Compare the active palette
    // so the same source builds on the Windows 7-compatible Qt baseline.
    const QPalette palette = QApplication::palette();
    const QColor windowColor = palette.color(QPalette::Window);
    const QColor textColor = palette.color(QPalette::WindowText);
    return windowColor.lightness() < textColor.lightness();
#endif
}

QString autoStartRegistryPath()
{
#ifdef Q_OS_WIN
    return QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run");
#else
    return QString();
#endif
}

QString autoStartValueName()
{
    return QStringLiteral("EasyCloudHfs");
}

QString autoStartCommand()
{
    const QString exePath = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
    return QStringLiteral("\"%1\" --startup-tray").arg(exePath);
}

QString externalInfoLabelText(const QString &text, const QString &link = QString())
{
    const QString safeText = text.toHtmlEscaped();
    if (link.isEmpty()) {
        return safeText;
    }
    return QStringLiteral("<a href=\"%1\" style=\"color:inherit;text-decoration:none;\">%2</a>")
        .arg(link.toHtmlEscaped(), safeText);
}

bool setLaunchOnStartupEnabled(bool enabled, QString *errorMessage)
{
#ifdef Q_OS_WIN
    QSettings settings(autoStartRegistryPath(), QSettings::NativeFormat);
    if (enabled) {
        settings.setValue(autoStartValueName(), autoStartCommand());
    } else {
        settings.remove(autoStartValueName());
    }
    settings.sync();
    if (settings.status() != QSettings::NoError) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("無法更新開機自動啟動設定。");
        }
        return false;
    }
    return true;
#else
    if (enabled && errorMessage) {
        *errorMessage = QStringLiteral("目前只支援 Windows 開機自動啟動。");
    }
    return !enabled;
#endif
}
}

MainWindow::MainWindow(Controller *controller, QWidget *parent)
    : QMainWindow(parent)
    , m_controller(controller)
{
    setWindowTitle(QStringLiteral("Easy Cloud HFS - 檔案分享伺服器"));
    resize(1180, 720);
    setMinimumSize(900, 600);

    buildUi();
    setupTrayIcon();
    restoreWindowSettings();
    refreshAll();
    retranslateUi();

    connect(m_controller, &Controller::sharesChanged, this, &MainWindow::refreshShares);
    connect(m_controller, &Controller::downloadsChanged, this, &MainWindow::refreshStats);
    connect(m_controller, &Controller::statsChanged, this, &MainWindow::refreshStats);
    connect(m_controller, &Controller::statsChanged, this, &MainWindow::refreshFooter);
    connect(m_controller, &Controller::activityLogChanged, this, &MainWindow::refreshActivityLog);
    connect(m_controller, &Controller::serverStateChanged, this, [this]() {
        refreshStartStopButton();
        refreshFooter();
        refreshStats();
        refreshActivityLog();
    });
    connect(m_controller, &Controller::statusMessageChanged, this, &MainWindow::refreshFooter);
    connect(m_controller, &Controller::settingsChanged, this, &MainWindow::retranslateUi);
    connect(m_controller, &Controller::settingsChanged, this, &MainWindow::applyTheme);
    connect(m_controller, &Controller::urlsChanged, this, &MainWindow::refreshFooter);
    connect(m_controller, &Controller::urlsChanged, this, &MainWindow::refreshShares);
    connect(m_controller, &Controller::tunnelStateChanged, this, &MainWindow::refreshFooter);
    connect(m_controller, &Controller::tunnelStateChanged, this, &MainWindow::refreshShares);
}

bool MainWindow::trayAvailable() const
{
    return m_trayIcon && m_trayIcon->isVisible();
}

void MainWindow::showMainWindow()
{
    show();
    setWindowState(windowState() & ~Qt::WindowMinimized);
    raise();
    activateWindow();
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    persistWindowSettings();
    if (!m_forceExit && shouldCloseToTray()) {
        hideToTray();
        event->ignore();
        return;
    }
    QMainWindow::closeEvent(event);
}

void MainWindow::restoreWindowSettings()
{
    const AppSettings settings = m_controller->settings();

    QSize targetSize(settings.windowWidth, settings.windowHeight);
    if (QScreen *screen = QGuiApplication::primaryScreen()) {
        const QSize available = screen->availableGeometry().size();
        const int maxWidth = qMax(760, available.width() - 24);
        const int maxHeight = qMax(500, available.height() - 48);
        targetSize.setWidth(qMin(qMax(minimumWidth(), targetSize.width()), maxWidth));
        targetSize.setHeight(qMin(qMax(minimumHeight(), targetSize.height()), maxHeight));
    } else {
        targetSize.setWidth(qMax(minimumWidth(), targetSize.width()));
        targetSize.setHeight(qMax(minimumHeight(), targetSize.height()));
    }

    resize(targetSize);
    if (settings.windowMaximized) {
        setWindowState(windowState() | Qt::WindowMaximized);
    }
}

void MainWindow::persistWindowSettings()
{
    AppSettings settings = m_controller->settings();
    const QSize storedSize = isMaximized() ? normalGeometry().size() : size();
    settings.windowWidth = qMax(minimumWidth(), storedSize.width());
    settings.windowHeight = qMax(minimumHeight(), storedSize.height());
    settings.windowMaximized = isMaximized();
    m_controller->saveSystemSettings(settings);
}

void MainWindow::setupTrayIcon()
{
    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        return;
    }

    m_trayMenu = new QMenu(this);
    m_trayShowAction = m_trayMenu->addAction(tx(QStringLiteral("顯示主頁"), QStringLiteral("Show Main Window")));
    m_trayExitAction = m_trayMenu->addAction(tx(QStringLiteral("退出"), QStringLiteral("Exit")));

    connect(m_trayShowAction, &QAction::triggered, this, &MainWindow::showMainWindow);
    connect(m_trayExitAction, &QAction::triggered, this, [this]() {
        m_forceExit = true;
        close();
    });

    m_trayIcon = new QSystemTrayIcon(windowIcon().isNull() ? QIcon(QStringLiteral(":/desktop_logo.png")) : windowIcon(), this);
    m_trayIcon->setContextMenu(m_trayMenu);
    m_trayIcon->setToolTip(QStringLiteral("Easy Cloud HFS"));
    connect(m_trayIcon, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick) {
            showMainWindow();
        }
    });
    m_trayIcon->show();
}

bool MainWindow::shouldCloseToTray() const
{
    return trayAvailable() && m_controller->settings().minimizeToTrayOnClose;
}

void MainWindow::hideToTray()
{
    hide();
    if (m_trayIcon && !m_trayNoticeShown) {
        m_trayIcon->showMessage(QStringLiteral("Easy Cloud HFS"),
                                QStringLiteral("程式仍在系統列執行中。"),
                                QSystemTrayIcon::Information,
                                1800);
        m_trayNoticeShown = true;
    }
}

void MainWindow::copyTextToClipboard(const QString &text)
{
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) {
        return;
    }

    if (QClipboard *clipboard = QGuiApplication::clipboard()) {
        clipboard->setText(trimmed);
    }

    QToolTip::showText(QCursor::pos(), QStringLiteral("複制網址完成"));
    if (m_trayIcon) {
        m_trayIcon->showMessage(QStringLiteral("Easy Cloud HFS"),
                                QStringLiteral("複制網址完成"),
                                QSystemTrayIcon::Information,
                                1200);
    }
}

void MainWindow::setStoppingUi(bool stopping)
{
    m_isStoppingServer = stopping;
    refreshStartStopButton();
    refreshFooter();
}

void MainWindow::buildUi()
{
    m_central = new QWidget(this);
    setCentralWidget(m_central);

    auto *rootLayout = new QHBoxLayout(m_central);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    buildSidebar(rootLayout);
    buildContent(rootLayout);
    applyTheme();
}

void MainWindow::buildSidebar(QHBoxLayout *rootLayout)
{
    auto *sidebar = new QFrame(m_central);
    sidebar->setObjectName(QStringLiteral("Sidebar"));
    sidebar->setFixedWidth(278);

    auto *layout = new QVBoxLayout(sidebar);
    layout->setContentsMargins(20, 22, 20, 20);
    layout->setSpacing(10);

    auto *brandBox = new QFrame(sidebar);
    brandBox->setObjectName(QStringLiteral("BrandBox"));
    auto *brandLayout = new QVBoxLayout(brandBox);
    brandLayout->setContentsMargins(8, 8, 8, 16);
    brandLayout->setSpacing(5);

    auto *logo = new QLabel(brandBox);
    logo->setPixmap(QPixmap(QStringLiteral(":/desktop_logo.png")).scaled(104, 104, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    logo->setFixedSize(104, 104);
    logo->setAlignment(Qt::AlignCenter);

    auto *brandTitle = new QLabel(QStringLiteral("Easy Cloud HFS"), brandBox);
    brandTitle->setObjectName(QStringLiteral("BrandTitle"));
    brandTitle->setAlignment(Qt::AlignCenter);

    m_brandSubtitle = new QLabel(tx(QStringLiteral("檔案分享伺服器"), QStringLiteral("File Sharing Server")), brandBox);
    m_brandSubtitle->setObjectName(QStringLiteral("BrandSub"));
    m_brandSubtitle->setAlignment(Qt::AlignCenter);

    auto *brandVersion = new QLabel(QStringLiteral("V1.2.0.0"), brandBox);
    brandVersion->setObjectName(QStringLiteral("BrandVersion"));
    brandVersion->setAlignment(Qt::AlignCenter);

    brandLayout->addWidget(logo, 0, Qt::AlignHCenter);
    brandLayout->addWidget(brandTitle);
    brandLayout->addWidget(m_brandSubtitle);
    brandLayout->addWidget(brandVersion);
    layout->addWidget(brandBox);
    layout->addSpacing(6);

    auto *buttonGroup = new QButtonGroup(this);
    buttonGroup->setExclusive(true);

    m_btnDashboard = makeSidebarButton(tx(QStringLiteral("儀表板"), QStringLiteral("Dashboard")), UiIcon::Dashboard, sidebar);
    m_btnShares = makeSidebarButton(tx(QStringLiteral("分享管理"), QStringLiteral("Shares")), UiIcon::Shares, sidebar);
    m_btnSettings = makeSidebarButton(tx(QStringLiteral("系統設定"), QStringLiteral("Settings")), UiIcon::Settings, sidebar);
    m_btnAbout = makeSidebarButton(tx(QStringLiteral("關於"), QStringLiteral("About")), UiIcon::About, sidebar);

    buttonGroup->addButton(m_btnDashboard, static_cast<int>(DashboardPage));
    buttonGroup->addButton(m_btnShares, static_cast<int>(SharesPage));
    buttonGroup->addButton(m_btnSettings, static_cast<int>(SystemSettingsPage));
    buttonGroup->addButton(m_btnAbout, static_cast<int>(AboutPage));

    layout->addWidget(m_btnDashboard);
    layout->addWidget(m_btnShares);
    layout->addWidget(m_btnSettings);
    layout->addWidget(m_btnAbout);

    connect(buttonGroup, &QButtonGroup::idClicked, this, [this](int id) {
        switchPage(static_cast<PageIndex>(id), QString());
    });

    if (QAbstractButton *first = buttonGroup->button(static_cast<int>(DashboardPage))) {
        first->setChecked(true);
    }

    layout->addStretch();

    auto *footer = new QFrame(sidebar);
    footer->setObjectName(QStringLiteral("SidebarFooter"));
    applySoftShadow(footer, 20, 5);
    auto *footerLayout = new QVBoxLayout(footer);
    footerLayout->setContentsMargins(16, 16, 16, 16);
    footerLayout->setSpacing(9);

    auto *statusRow = new QHBoxLayout();
    statusRow->setSpacing(9);
    m_statusDot = new QLabel(footer);
    m_statusDot->setObjectName(QStringLiteral("StatusDot"));
    m_statusDot->setFixedSize(10, 10);

    m_statusText = new QLabel(QStringLiteral("伺服器未啟動"), footer);
    m_statusText->setObjectName(QStringLiteral("StatusText"));
    m_statusText->setWordWrap(true);
    statusRow->addWidget(m_statusDot, 0, Qt::AlignTop);
    statusRow->addWidget(m_statusText, 1);
    footerLayout->addLayout(statusRow);

    m_localUrlLabel = new QLabel(footer);
    m_localUrlLabel->setObjectName(QStringLiteral("FooterLink"));
    m_localUrlLabel->setWordWrap(true);
    m_localUrlLabel->setTextInteractionFlags(Qt::TextBrowserInteraction);
    m_localUrlLabel->setOpenExternalLinks(false);
    connect(m_localUrlLabel, &QLabel::linkActivated, this, [this](const QString &link) {
        copyTextToClipboard(link);
    });

    m_externalUrlLabel = new QLabel(footer);
    m_externalUrlLabel->setObjectName(QStringLiteral("FooterLink"));
    m_externalUrlLabel->setWordWrap(true);
    m_externalUrlLabel->setTextInteractionFlags(Qt::TextBrowserInteraction);
    m_externalUrlLabel->setOpenExternalLinks(false);
    connect(m_externalUrlLabel, &QLabel::linkActivated, this, [this](const QString &link) {
        copyTextToClipboard(link);
    });

    m_connectionsLabel = new QLabel(footer);
    m_speedLabel = new QLabel(footer);
    m_connectionsLabel->setObjectName(QStringLiteral("FooterMeta"));
    m_speedLabel->setObjectName(QStringLiteral("FooterMeta"));

    footerLayout->addWidget(m_localUrlLabel);
    footerLayout->addWidget(m_externalUrlLabel);
    footerLayout->addSpacing(3);
    footerLayout->addWidget(m_connectionsLabel);
    footerLayout->addWidget(m_speedLabel);
    layout->addWidget(footer);

    rootLayout->addWidget(sidebar);
}

void MainWindow::buildContent(QHBoxLayout *rootLayout)
{
    auto *content = new QFrame(m_central);
    content->setObjectName(QStringLiteral("ContentRoot"));

    auto *layout = new QVBoxLayout(content);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto *header = new QFrame(content);
    header->setObjectName(QStringLiteral("Header"));
    auto *headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(34, 26, 34, 20);
    headerLayout->setSpacing(18);

    auto *titleLayout = new QVBoxLayout();
    titleLayout->setContentsMargins(0, 0, 0, 0);
    titleLayout->setSpacing(5);

    m_pageTitle = new QLabel(header);
    m_pageTitle->setObjectName(QStringLiteral("PageTitle"));
    m_pageTitle->setWordWrap(true);

    m_pageSubtitle = new QLabel(tx(QStringLiteral("輕鬆分享・快速安全・隨時存取"),
                                       QStringLiteral("Simple sharing · Fast and secure · Access anywhere")),
                                    header);
    m_pageSubtitle->setObjectName(QStringLiteral("PageSubtitle"));

    titleLayout->addWidget(m_pageTitle);
    titleLayout->addWidget(m_pageSubtitle);

    m_startStopButton = new QPushButton(header);
    m_startStopButton->setCursor(Qt::PointingHandCursor);
    m_startStopButton->setMinimumHeight(48);
    m_startStopButton->setMinimumWidth(154);
    connect(m_startStopButton, &QPushButton::clicked, this, [this]() {
        if (m_isStoppingServer) {
            return;
        }

        if (m_controller->isServerRunning()) {
            setStoppingUi(true);
            qApp->processEvents();
            QTimer::singleShot(0, this, [this]() {
                m_controller->stopServer();
                setStoppingUi(false);
            });
            return;
        }

        m_controller->startServer();
    });

    headerLayout->addLayout(titleLayout, 1);
    headerLayout->addWidget(m_startStopButton, 0, Qt::AlignVCenter);

    m_pages = new QStackedWidget(content);
    buildPages();

    layout->addWidget(header);
    layout->addWidget(m_pages, 1);
    rootLayout->addWidget(content, 1);
}

void MainWindow::buildPages()
{
    auto *dashboard = new QWidget(m_pages);
    auto *dashboardLayout = new QVBoxLayout(dashboard);
    dashboardLayout->setContentsMargins(34, 18, 34, 30);
    dashboardLayout->setSpacing(24);

    auto *statsGrid = new QGridLayout();
    statsGrid->setHorizontalSpacing(18);
    statsGrid->setVerticalSpacing(18);
    statsGrid->addWidget(makeCard(tx(QStringLiteral("總下載次數"), QStringLiteral("Total Downloads")), &m_totalDownloadsValue, &m_lblTotalDownloadsTitle, UiIcon::Download, QColor(QStringLiteral("#7657e8")), QColor(QStringLiteral("#f0ecff"))), 0, 0);
    statsGrid->addWidget(makeCard(tx(QStringLiteral("總下載流量"), QStringLiteral("Total Data Served")), &m_totalBytesValue, &m_lblTotalBytesTitle, UiIcon::Data, QColor(QStringLiteral("#2f7df4")), QColor(QStringLiteral("#eaf3ff"))), 0, 1);
    statsGrid->addWidget(makeCard(tx(QStringLiteral("目前連線"), QStringLiteral("Active Connections")), &m_activeConnectionsValue, &m_lblActiveConnectionsTitle, UiIcon::Connections, QColor(QStringLiteral("#20a96b")), QColor(QStringLiteral("#e7f8f0"))), 0, 2);
    statsGrid->addWidget(makeCard(tx(QStringLiteral("即時傳輸速率"), QStringLiteral("Current Speed")), &m_liveSpeedValue, &m_lblLiveSpeedTitle, UiIcon::Speed, QColor(QStringLiteral("#f08a24")), QColor(QStringLiteral("#fff1e3"))), 0, 3);
    statsGrid->setColumnStretch(0, 1);
    statsGrid->setColumnStretch(1, 1);
    statsGrid->setColumnStretch(2, 1);
    statsGrid->setColumnStretch(3, 1);
    dashboardLayout->addLayout(statsGrid);

    auto *infoCard = new QFrame(dashboard);
    infoCard->setObjectName(QStringLiteral("InfoCard"));
    applySoftShadow(infoCard, 24, 7);
    auto *infoLayout = new QVBoxLayout(infoCard);
    infoLayout->setContentsMargins(26, 24, 26, 24);
    m_lblServerInfoTitle = new QLabel(tx(QStringLiteral("伺服器運作資訊"), QStringLiteral("Server Information")), infoCard);
    m_lblServerInfoTitle->setObjectName(QStringLiteral("SectionTitle"));
    m_serverInfoLabel = new QLabel(infoCard);
    m_serverInfoLabel->setWordWrap(true);
    m_serverInfoLabel->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    m_serverInfoLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    infoLayout->addWidget(m_lblServerInfoTitle);
    infoLayout->addWidget(m_serverInfoLabel);
    dashboardLayout->addWidget(infoCard);
    dashboardLayout->addStretch();
    m_pages->addWidget(makeScrollablePage(dashboard, m_pages));

    m_sharePage = new QWidget(m_pages);
    auto *shareLayout = new QVBoxLayout(m_sharePage);
    shareLayout->setContentsMargins(24, 22, 24, 22);
    shareLayout->setSpacing(16);

    auto *shareHeader = new QHBoxLayout();
    shareHeader->setSpacing(10);
    m_shareExternalLinkCheck = new QCheckBox(tx(QStringLiteral("啟用外部連結"), QStringLiteral("Enable External Link")), m_sharePage);
    m_shareExternalInfoLabel = new QLabel(tx(QStringLiteral("使用 Cloudflare Tunnel 代理"), QStringLiteral("Use Cloudflare Tunnel Proxy")), m_sharePage);
    m_shareExternalInfoLabel->setObjectName(QStringLiteral("ExternalPrefixLabel"));
    m_shareExternalInfoLabel->setAlignment(Qt::AlignCenter);
    m_shareExternalInfoLabel->setTextFormat(Qt::RichText);
    m_shareExternalInfoLabel->setTextInteractionFlags(Qt::TextBrowserInteraction);
    m_shareExternalInfoLabel->setOpenExternalLinks(false);
    m_shareExternalInfoLabel->setMinimumWidth(300);
    m_shareExternalInfoLabel->setMaximumWidth(380);
    m_shareExternalInfoLabel->setMinimumHeight(40);
    m_btnSaveShares = new QPushButton(tx(QStringLiteral("儲存分享"), QStringLiteral("Save Shares")), m_sharePage);
    m_btnSaveShares->setObjectName(QStringLiteral("SecondaryButton"));
    m_btnAddShare = new QPushButton(tx(QStringLiteral("新增分享"), QStringLiteral("Add Share")), m_sharePage);
    m_btnSaveShares->setCursor(Qt::PointingHandCursor);
    m_btnAddShare->setCursor(Qt::PointingHandCursor);
    m_btnSaveShares->setMinimumHeight(40);
    m_btnAddShare->setMinimumHeight(42);
    m_btnAddShare->setMinimumWidth(128);
    m_shareExternalLinkCheck->setCursor(Qt::PointingHandCursor);
    connect(m_shareExternalLinkCheck, &QCheckBox::toggled, this, [this](bool checked) {
        m_controller->setExternalLinkSettings(checked);
        updateExternalLinkUiState();
    });
    connect(m_shareExternalInfoLabel, &QLabel::linkActivated, this, [this](const QString &link) {
        copyTextToClipboard(link);
    });
    connect(m_btnSaveShares, &QPushButton::clicked, m_controller, &Controller::saveSharesSnapshot);
    connect(m_btnAddShare, &QPushButton::clicked, this, &MainWindow::showShareMenu);
    shareHeader->addWidget(m_btnAddShare);
    shareHeader->addStretch();
    shareHeader->addWidget(m_shareExternalLinkCheck);
    shareHeader->addWidget(m_shareExternalInfoLabel);
    shareHeader->addWidget(m_btnSaveShares);
    shareLayout->addLayout(shareHeader);

    auto *dropArea = new ShareDropArea(m_sharePage);
    dropArea->setObjectName(QStringLiteral("DropArea"));
    dropArea->setMinimumHeight(120);
    auto *dropLayout = new QVBoxLayout(dropArea);
    dropLayout->setContentsMargins(24, 24, 24, 24);
    dropLayout->setSpacing(12);
    m_dropTitle = new QLabel(tx(QStringLiteral("可直接拖曳檔案或資料夾到這裡快速連結"), QStringLiteral("Drag and drop files or folders here to share")), dropArea);
    m_dropTitle->setObjectName(QStringLiteral("DropTitle"));
    m_dropTitle->setAlignment(Qt::AlignCenter);
    dropLayout->addStretch();
    dropLayout->addWidget(m_dropTitle, 0, Qt::AlignCenter);
    dropLayout->addStretch();
    dropArea->onDropPaths = [this](const QStringList &paths) { m_controller->addPaths(paths); };
    shareLayout->addWidget(dropArea);

    auto *listScroll = new QScrollArea(m_sharePage);
    listScroll->setWidgetResizable(true);
    listScroll->setFrameShape(QFrame::NoFrame);
    auto *listContainer = new QWidget(listScroll);
    m_shareListLayout = new QVBoxLayout(listContainer);
    m_shareListLayout->setContentsMargins(0, 0, 0, 0);
    m_shareListLayout->setSpacing(12);
    listScroll->setWidget(listContainer);
    shareLayout->addWidget(listScroll, 1);

    m_pages->addWidget(makeScrollablePage(m_sharePage, m_pages));

    auto *systemPage = new QWidget(m_pages);
    auto *systemLayout = new QVBoxLayout(systemPage);
    systemLayout->setContentsMargins(34, 28, 34, 28);
    systemLayout->setSpacing(20);

    auto *systemCard = new QFrame(systemPage);
    systemCard->setObjectName(QStringLiteral("InfoCard"));
    applySoftShadow(systemCard, 24, 7);
    auto *systemCardLayout = new QVBoxLayout(systemCard);
    systemCardLayout->setContentsMargins(28, 26, 28, 28);
    systemCardLayout->setSpacing(22);
    m_lblSystemTitle = new QLabel(tx(QStringLiteral("系統設定"), QStringLiteral("System Settings")), systemCard);
    m_lblSystemTitle->setObjectName(QStringLiteral("SectionTitle"));
    systemCardLayout->addWidget(m_lblSystemTitle);

    auto *logoRow = new QHBoxLayout();
    m_logoPreview = new QLabel(systemCard);
    m_logoPreview->setFixedSize(112, 112);
    m_btnSelectLogo = new QPushButton(tx(QStringLiteral("選擇 Logo"), QStringLiteral("Select Logo")), systemCard);
    m_btnSelectLogo->setObjectName(QStringLiteral("SecondaryButton"));
    m_btnClearLogo = new QPushButton(tx(QStringLiteral("移除 Logo"), QStringLiteral("Remove Logo")), systemCard);
    m_btnClearLogo->setObjectName(QStringLiteral("SecondaryButton"));
    connect(m_btnSelectLogo, &QPushButton::clicked, this, [this]() {
        const QString filePath = QFileDialog::getOpenFileName(this,
                                                              tx(QStringLiteral("選擇 Logo"), QStringLiteral("Select Logo")),
                                                              QString(),
                                                              QStringLiteral("Images (*.png *.jpg *.jpeg *.webp *.ico)"));
        if (!filePath.isEmpty()) {
            AppSettings settings = m_controller->settings();
            settings.logoPath = filePath;
            m_controller->saveSystemSettings(settings);
        }
    });
    connect(m_btnClearLogo, &QPushButton::clicked, this, [this]() {
        AppSettings settings = m_controller->settings();
        settings.logoPath.clear();
        m_controller->saveSystemSettings(settings);
    });
    auto *logoButtonsLayout = new QVBoxLayout();
    logoButtonsLayout->setSpacing(10);
    logoButtonsLayout->addWidget(m_btnSelectLogo);
    logoButtonsLayout->addWidget(m_btnClearLogo);
    logoButtonsLayout->addStretch();

    logoRow->addWidget(m_logoPreview, 0, Qt::AlignTop);
    logoRow->addLayout(logoButtonsLayout);
    logoRow->addStretch();
    systemCardLayout->addLayout(logoRow);

    auto *form = new QFormLayout();
    form->setLabelAlignment(Qt::AlignCenter);
    form->setFormAlignment(Qt::AlignTop);
    form->setHorizontalSpacing(20);
    form->setVerticalSpacing(18);

    m_siteNameEdit = new QLineEdit(systemCard);
    m_portSpin = new NoWheelSpinBox(systemCard);
    m_portSpin->setRange(1, 65535);
    m_portSpin->setButtonSymbols(QAbstractSpinBox::NoButtons);
    m_languageCombo = new NoWheelComboBox(systemCard);
    m_languageCombo->addItems({QStringLiteral("繁體中文"), QStringLiteral("English")});
    m_themeCombo = new NoWheelComboBox(systemCard);
    m_themeCombo->addItems({tx(QStringLiteral("跟隨系統"), QStringLiteral("Follow System")),
                            tx(QStringLiteral("淺色模式"), QStringLiteral("Light Mode")),
                            tx(QStringLiteral("深色模式"), QStringLiteral("Dark Mode"))});
    m_passwordEdit = new QLineEdit(systemCard);
    m_passwordEdit->setPlaceholderText(tx(QStringLiteral("留空代表不需要下載密碼"), QStringLiteral("Leave empty for no download password")));
    m_resumeCheck = new QCheckBox(tx(QStringLiteral("啟用斷點續傳（支援 HTTP Range 請求）"), QStringLiteral("Enable Resumable Downloads (HTTP Range)")), systemCard);
    m_closeToTrayCheck = new QCheckBox(tx(QStringLiteral("關閉視窗時不直接退出，保持在系統列"), QStringLiteral("Close to system tray instead of exiting")), systemCard);
    m_launchOnStartupCheck = new QCheckBox(tx(QStringLiteral("開機時自動啟動並在系統列執行"), QStringLiteral("Launch on startup and minimize to tray")), systemCard);

    m_lblSiteName = makeFormLabel(tx(QStringLiteral("站台名稱"), QStringLiteral("Site Name")), systemCard);
    m_lblPort = makeFormLabel(tx(QStringLiteral("HTTP 連接埠"), QStringLiteral("HTTP Port")), systemCard);
    m_lblLanguage = makeFormLabel(tx(QStringLiteral("介面語言"), QStringLiteral("UI Language")), systemCard);
    m_lblTheme = makeFormLabel(tx(QStringLiteral("外觀主題"), QStringLiteral("Theme")), systemCard);
    m_lblPassword = makeFormLabel(tx(QStringLiteral("下載密碼"), QStringLiteral("Download Password")), systemCard);

    form->addRow(m_lblSiteName, m_siteNameEdit);
    form->addRow(m_lblPort, m_portSpin);
    form->addRow(m_lblLanguage, m_languageCombo);
    form->addRow(m_lblTheme, m_themeCombo);
    form->addRow(m_lblPassword, m_passwordEdit);
    form->addRow(QString(), m_resumeCheck);
    form->addRow(QString(), m_closeToTrayCheck);
    form->addRow(QString(), m_launchOnStartupCheck);
    systemCardLayout->addLayout(form);

    m_btnSaveSettings = new QPushButton(tx(QStringLiteral("儲存設定"), QStringLiteral("Save Settings")), systemCard);
    connect(m_btnSaveSettings, &QPushButton::clicked, this, &MainWindow::saveSystemSettingsFromForm);
    systemCardLayout->addWidget(m_btnSaveSettings, 0, Qt::AlignLeft);
    systemLayout->addWidget(systemCard);
    systemLayout->addStretch();
    m_pages->addWidget(makeScrollablePage(systemPage, m_pages));

    auto *aboutPage = new QWidget(m_pages);
    auto *aboutLayout = new QVBoxLayout(aboutPage);
    aboutLayout->setContentsMargins(34, 28, 34, 28);
    auto *aboutCard = new QFrame(aboutPage);
    aboutCard->setObjectName(QStringLiteral("InfoCard"));
    applySoftShadow(aboutCard, 24, 7);
    auto *aboutCardLayout = new QVBoxLayout(aboutCard);
    aboutCardLayout->setContentsMargins(34, 30, 34, 30);
    aboutCardLayout->setSpacing(18);
    m_lblAboutTitle = new QLabel(tx(QStringLiteral("關於"), QStringLiteral("About")), aboutCard);
    m_lblAboutTitle->setObjectName(QStringLiteral("SectionTitle"));
    m_aboutLabel = new QLabel(aboutCard);
    m_aboutLabel->setObjectName(QStringLiteral("AboutBody"));
    m_aboutLabel->setWordWrap(true);
    m_aboutLabel->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    m_aboutLabel->setTextInteractionFlags(Qt::TextBrowserInteraction);
    m_aboutLabel->setOpenExternalLinks(true);
    aboutCardLayout->addWidget(m_lblAboutTitle);
    aboutCardLayout->addWidget(m_aboutLabel);
    aboutLayout->addWidget(aboutCard);
    aboutLayout->addStretch();
    m_pages->addWidget(makeScrollablePage(aboutPage, m_pages));
}

void MainWindow::switchPage(PageIndex page, const QString &title)
{
    m_pages->setCurrentIndex(static_cast<int>(page));
    Q_UNUSED(title)
}

void MainWindow::refreshAll()
{
    refreshSettingsForms();
    refreshStats();
    refreshActivityLog();
    refreshFooter();
    refreshShares();
    refreshAbout();
    refreshStartStopButton();
}

void MainWindow::refreshStats()
{
    const ServerStats stats = m_controller->stats();
    m_totalDownloadsValue->setText(QString::number(stats.totalDownloads));
    m_totalBytesValue->setText(humanReadableSize(stats.totalBytes));
    m_activeConnectionsValue->setText(QString::number(stats.activeConnections));
    m_liveSpeedValue->setText(QStringLiteral("%1/s").arg(humanReadableSize(stats.currentBytesPerSecond)));
}

void MainWindow::refreshActivityLog()
{
    const QStringList entries = m_controller->activityLog();
    if (entries.isEmpty()) {
        m_serverInfoLabel->setText(tx(QStringLiteral("伺服器目前處於停止狀態。啟動後會在這裡顯示即時記錄。"), QStringLiteral("Server is currently stopped. Live logs will be shown here once started.")));
        return;
    }

    m_serverInfoLabel->setText(entries.mid(0, 14).join(QLatin1Char('\n')));
}

void MainWindow::refreshFooter()
{
    const QString externalUrl = m_controller->isServerRunning() ? m_controller->primaryExternalUrl() : QString();
    QString status;
    if (m_isStoppingServer) {
        status = tx(QStringLiteral("停止中 ....."), QStringLiteral("Stopping ....."));
    } else {
        status = m_controller->statusMessage().trimmed();
        if (m_controller->settings().language.compare(QStringLiteral("English"), Qt::CaseInsensitive) == 0) {
            status = translateStatusToEn(status);
        }
        if (status.isEmpty()) {
            status = m_controller->isServerRunning() ? tx(QStringLiteral("伺服器上線中"), QStringLiteral("Server Running")) : tx(QStringLiteral("伺服器未啟動"), QStringLiteral("Server Stopped"));
        }
        if (m_controller->settings().externalLinkEnabled && !externalUrl.isEmpty()) {
            status = tx(QStringLiteral("已就緒，點擊下面連結複制網址"), QStringLiteral("Ready, click links below to copy URL"));
        }
    }
    m_statusText->setText(status);
    updateStatusVisuals(status);

    const QString localUrl = m_controller->localBaseUrl();
    m_localUrlLabel->setText(tx(QStringLiteral("內網：<a href='%1'>%1</a>"), QStringLiteral("Local: <a href='%1'>%1</a>")).arg(localUrl));

    if (externalUrl.isEmpty()) {
        m_externalUrlLabel->clear();
    } else {
        m_externalUrlLabel->setText(tx(QStringLiteral("代理：<a href='%1'>%1</a>"), QStringLiteral("Proxy: <a href='%1'>%1</a>")).arg(externalUrl));
    }

    m_connectionsLabel->setText(tx(QStringLiteral("連線數：%1"), QStringLiteral("Connections: %1")).arg(m_controller->stats().activeConnections));
    m_speedLabel->setText(tx(QStringLiteral("即時速度：%1/s"), QStringLiteral("Speed: %1/s")).arg(humanReadableSize(m_controller->stats().currentBytesPerSecond)));

    if (m_trayIcon) {
        m_trayIcon->setToolTip(QStringLiteral("%1\n%2").arg(m_controller->settings().siteName, status));
    }
}

void MainWindow::refreshShares()
{
    const AppSettings settings = m_controller->settings();
    if (m_shareExternalLinkCheck && m_shareExternalInfoLabel) {
        const QSignalBlocker blocker1(m_shareExternalLinkCheck);
        m_shareExternalLinkCheck->setChecked(settings.externalLinkEnabled);
    }

    updateExternalLinkUiState();
    rebuildShareList();
    refreshFooter();
    refreshStats();
}

void MainWindow::refreshSettingsForms()
{
    const AppSettings settings = m_controller->settings();
    refreshHeaderTitle();

    {
        const QSignalBlocker blocker1(m_siteNameEdit);
        const QSignalBlocker blocker2(m_portSpin);
        const QSignalBlocker blocker3(m_languageCombo);
        const QSignalBlocker blocker4(m_themeCombo);
        const QSignalBlocker blocker5(m_passwordEdit);
        const QSignalBlocker blocker6(m_resumeCheck);
        const QSignalBlocker blocker7(m_closeToTrayCheck);
        const QSignalBlocker blocker8(m_launchOnStartupCheck);

        m_siteNameEdit->setText(settings.siteName);
        m_portSpin->setValue(settings.port);
        m_languageCombo->setCurrentText(settings.language);
        m_themeCombo->setCurrentText(settings.theme);
        m_passwordEdit->setText(settings.download.password);
        m_resumeCheck->setChecked(settings.download.resumeEnabled);
        m_closeToTrayCheck->setChecked(settings.minimizeToTrayOnClose);
        m_launchOnStartupCheck->setChecked(settings.launchOnStartup);
        m_closeToTrayCheck->setEnabled(trayAvailable());
        m_launchOnStartupCheck->setEnabled(trayAvailable());
    }

    QPixmap logo = settings.logoPath.isEmpty() ? QPixmap(QStringLiteral(":/logo.png")) : QPixmap(settings.logoPath);
    m_logoPreview->setPixmap(logo.scaled(m_logoPreview->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
    if (m_trayIcon) {
        m_trayIcon->setIcon(windowIcon().isNull() ? QIcon(QStringLiteral(":/desktop_logo.png")) : windowIcon());
        m_trayIcon->setToolTip(settings.siteName);
    }
}

void MainWindow::refreshHeaderTitle()
{
    if (!m_pageTitle) {
        return;
    }

    const QString siteName = m_controller->settings().siteName.trimmed().isEmpty()
                                 ? QStringLiteral("Easy Cloud HFS")
                                 : m_controller->settings().siteName.trimmed();
    m_pageTitle->setText(tx(QStringLiteral("%1 - 檔案分享伺服器").arg(siteName), QStringLiteral("%1 - File Sharing Server").arg(siteName)));
}

void MainWindow::refreshAbout()
{
    m_aboutLabel->setText(tx(
        QStringLiteral(
            "<div style='font-size:22px; font-weight:700; line-height:1.85;'>"
            "Easy Cloud HFS<br>"
            "輕量、直接、可自行部署的桌面檔案分享工具。"
            "</div>"
            "<div style='margin-top:24px; font-size:19px; line-height:1.9;'>"
            "支援快速建立分享、資料夾上傳、下載紀錄與外部連線。<br>"
            "啟用外部連結後，程式可透過 Cloudflare Tunnel 建立公開入口。<br>"
            "代理連接完成後，可直接點擊下方連結複製分享網址。"
            "</div>"
            "<div style='margin-top:24px; font-size:19px; line-height:1.9;'>"
            "本工具適合臨時檔案分享、內部測試、遠端協助與自架分享環境使用。<br>"
            "分享前請自行確認檔案內容、權限設定與網路安全性。"
            "</div>"
            "<div style='margin-top:24px; font-size:19px; line-height:1.9;'>"
            "作者：Terence0816<br>"
            "GitHub：<a href='https://github.com/Terence0816/EasyCloudHFS'>https://github.com/Terence0816/EasyCloudHFS</a><br>"
            "版本：v1.2.0.0"
            "</div>"
            "<div style='margin-top:24px; font-size:18px; line-height:1.9;'>"
            "授權聲明：<br>"
            "本專案原始碼僅提供檢視、學習與個人參考用途。<br>"
            "未經作者事前書面同意，不得商業使用、重新散布、重新打包或製作衍生作品。"
            "</div>"
            "<div style='margin-top:24px; font-size:18px; line-height:1.9;'>"
            "本專案為獨立開發工具，並非 Cloudflare 官方產品。"
            "</div>"),
        QStringLiteral(
            "<div style='font-size:22px; font-weight:700; line-height:1.85;'>"
            "Easy Cloud HFS<br>"
            "Lightweight, direct, self-hosted desktop file sharing tool."
            "</div>"
            "<div style='margin-top:24px; font-size:19px; line-height:1.9;'>"
            "Supports quick sharing, folder upload, download logs, and WAN connections.<br>"
            "When WAN proxy is enabled, it sets up a public tunnel via Cloudflare Tunnel.<br>"
            "Once connected, click the links below to copy the sharing URLs."
            "</div>"
            "<div style='margin-top:24px; font-size:19px; line-height:1.9;'>"
            "Ideal for temporary file sharing, internal testing, remote support, and personal hosting.<br>"
            "Please verify your file contents, permission settings, and network safety before sharing."
            "</div>"
            "<div style='margin-top:24px; font-size:19px; line-height:1.9;'>"
            "Author: Terence0816<br>"
            "GitHub: <a href='https://github.com/Terence0816/EasyCloudHFS'>https://github.com/Terence0816/EasyCloudHFS</a><br>"
            "Version: v1.2.0.0"
            "</div>"
            "<div style='margin-top:24px; font-size:18px; line-height:1.9;'>"
            "License Notice:<br>"
            "This project's source code is provided for reference, learning, and personal use only.<br>"
            "Commercial use, redistribution, repackaging, or derivative work is strictly prohibited without prior written consent from the author."
            "</div>"
            "<div style='margin-top:24px; font-size:18px; line-height:1.9;'>"
            "This is an independent tool and is not an official Cloudflare product."
            "</div>")
    ));
}

void MainWindow::refreshStartStopButton()
{
    if (m_isStoppingServer) {
        m_startStopButton->setText(tx(QStringLiteral("■  停止中…"), QStringLiteral("■  Stopping…")));
        m_startStopButton->setObjectName(QStringLiteral("StopButton"));
        m_startStopButton->setEnabled(false);
    } else if (m_controller->isServerRunning()) {
        m_startStopButton->setText(tx(QStringLiteral("■  停止伺服器"), QStringLiteral("■  Stop Server")));
        m_startStopButton->setObjectName(QStringLiteral("StopButton"));
        m_startStopButton->setEnabled(true);
    } else {
        m_startStopButton->setText(tx(QStringLiteral("▶  啟動伺服器"), QStringLiteral("▶  Start Server")));
        m_startStopButton->setObjectName(QStringLiteral("StartButton"));
        m_startStopButton->setEnabled(true);
    }

    style()->unpolish(m_startStopButton);
    style()->polish(m_startStopButton);
}

void MainWindow::updateStatusVisuals(const QString &status)
{
    QString textColor = QStringLiteral("#64748b");
    QString dotColor = QStringLiteral("#94a3b8");

    if (status.contains(QStringLiteral("已就緒")) || status.contains(QStringLiteral("Ready"))) {
        textColor = QStringLiteral("#169b62");
        dotColor = QStringLiteral("#20b777");
    } else if (status.contains(QStringLiteral("代理伺服器連接失敗")) || status.contains(QStringLiteral("failed"), Qt::CaseInsensitive)) {
        textColor = QStringLiteral("#d94f5c");
        dotColor = QStringLiteral("#e45d68");
    } else if (status.contains(QStringLiteral("代理伺服器連接中")) || status.contains(QStringLiteral("Connecting"))) {
        textColor = QStringLiteral("#2f7df4");
        dotColor = QStringLiteral("#4b8cf7");
    } else if (status.contains(QStringLiteral("停止中")) || status.contains(QStringLiteral("Stopping"))) {
        textColor = QStringLiteral("#cf7b1b");
        dotColor = QStringLiteral("#ee9a32");
    } else if (status.contains(QStringLiteral("伺服器上線中")) || status.contains(QStringLiteral("Started")) || status.contains(QStringLiteral("Running"))) {
        textColor = QStringLiteral("#178c59");
        dotColor = QStringLiteral("#20b777");
    }

    if (m_statusText) {
        m_statusText->setStyleSheet(QStringLiteral("QLabel#StatusText{font-weight:700;color:%1;}").arg(textColor));
    }
    if (m_statusDot) {
        m_statusDot->setStyleSheet(QStringLiteral("QLabel#StatusDot{background:%1;border:0;border-radius:5px;}").arg(dotColor));
    }
}

void MainWindow::updateExternalLinkUiState()
{
    if (!m_shareExternalLinkCheck || !m_shareExternalInfoLabel) {
        return;
    }

    const bool enabled = m_shareExternalLinkCheck->isChecked();
    const QString theme = m_controller->settings().theme;
    const bool dark = theme == QStringLiteral("深色模式") || theme == QStringLiteral("Dark Mode")
                      || ((theme == QStringLiteral("跟隨系統") || theme == QStringLiteral("Follow System"))
                          && systemPrefersDarkTheme());

    const QString externalUrl = enabled && m_controller->isServerRunning()
                                    ? m_controller->primaryExternalUrl()
                                    : QString();
    QString infoText = tx(QStringLiteral("使用 Cloudflare Tunnel 代理"), QStringLiteral("Use Cloudflare Tunnel Proxy"));
    if (enabled && m_controller->isServerRunning()) {
        infoText = externalUrl.isEmpty()
                       ? tx(QStringLiteral("代理伺服器連接中"), QStringLiteral("WAN Proxy Connecting..."))
                       : tx(QStringLiteral("代理伺服器連接完成，點擊後複制網址"), QStringLiteral("WAN Proxy Connected, click to copy URL"));
    }

    m_shareExternalInfoLabel->setEnabled(enabled);
    m_shareExternalInfoLabel->setText(externalInfoLabelText(infoText, externalUrl));
    m_shareExternalInfoLabel->setCursor(enabled && !externalUrl.isEmpty() ? Qt::PointingHandCursor
                                                                          : Qt::ArrowCursor);

    const QString style = enabled
                              ? (dark
                                     ? QStringLiteral("QLabel{background:#121e31;color:#d9e7ff;border:1px solid #2a3c58;border-radius:14px;padding:10px 14px;font-weight:600;}")
                                     : QStringLiteral("QLabel{background:#f7fbff;color:#245b92;border:1px solid #dbe6f5;border-radius:14px;padding:10px 14px;font-weight:600;}"))
                              : (dark
                                     ? QStringLiteral("QLabel{background:#0f1726;color:#617899;border:1px solid #223452;border-radius:14px;padding:10px 14px;font-weight:600;}")
                                     : QStringLiteral("QLabel{background:#f3f6fa;color:#b5c0cf;border:1px solid #e0e7f0;border-radius:14px;padding:10px 14px;font-weight:600;}"));

    m_shareExternalInfoLabel->setStyleSheet(style);
}

void MainWindow::showShareMenu()
{
    QMenu menu(this);
    QAction *fileAction = menu.addAction(tx(QStringLiteral("選擇檔案"), QStringLiteral("Select File")));
    QAction *folderAction = menu.addAction(tx(QStringLiteral("選擇資料夾"), QStringLiteral("Select Folder")));

    QAction *selected = menu.exec(QCursor::pos());
    if (!selected) {
        return;
    }

    if (selected == fileAction) {
        const QStringList files = QFileDialog::getOpenFileNames(this, tx(QStringLiteral("選擇要分享的檔案"), QStringLiteral("Select files to share")));
        if (!files.isEmpty()) {
            m_controller->addPaths(files);
        }
        return;
    }

    const QString folder = QFileDialog::getExistingDirectory(this, tx(QStringLiteral("選擇要分享的資料夾"), QStringLiteral("Select directory to share")));
    if (!folder.isEmpty()) {
        m_controller->addDirectoryShare(folder);
    }
}

void MainWindow::rebuildShareList()
{
    while (QLayoutItem *item = m_shareListLayout->takeAt(0)) {
        if (QWidget *widget = item->widget()) {
            widget->deleteLater();
        }
        delete item;
    }

    const QList<ShareItem> shares = m_controller->shares();
    if (shares.isEmpty()) {
        auto *empty = new QLabel(tx(QStringLiteral("目前尚未建立任何分享項目。你可以拖曳檔案 / 資料夾，或使用上方左側的「新增分享」。"), QStringLiteral("No shares currently. You can drag and drop files / folders, or click \"Add Share\" above.")), m_sharePage);
        empty->setWordWrap(true);
        empty->setObjectName(QStringLiteral("EmptyState"));
        m_shareListLayout->addWidget(empty);
        m_shareListLayout->addStretch();
        return;
    }

    for (const ShareItem &share : shares) {
        auto *card = new QFrame(m_sharePage);
        card->setObjectName(QStringLiteral("ShareCard"));
        auto *cardLayout = new QHBoxLayout(card);
        cardLayout->setContentsMargins(16, 12, 16, 12);
        cardLayout->setSpacing(12);

        auto *iconLabel = new QLabel(card);
        iconLabel->setFixedSize(48, 48);
        iconLabel->setPixmap(buildShareIcon(share));
        iconLabel->setScaledContents(true);

        auto *nameLabel = new QLabel(share.name, card);
        nameLabel->setObjectName(QStringLiteral("ShareTitle"));
        nameLabel->setWordWrap(false);
        nameLabel->setToolTip(share.sourcePath.isEmpty() ? share.storagePath : share.sourcePath);
        nameLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

        auto *typeLabel = new QLabel(shareTypeLabel(share), card);
        typeLabel->setObjectName(QStringLiteral("ShareTypeBadge"));
        typeLabel->setProperty("shareKind", isDirectoryShare(share) ? QStringLiteral("folder") : QStringLiteral("file"));
        typeLabel->setAlignment(Qt::AlignCenter);
        typeLabel->setMinimumWidth(104);
        typeLabel->setMinimumHeight(34);

        auto *removeButton = new QPushButton(tx(QStringLiteral("關閉"), QStringLiteral("Close")), card);
        removeButton->setObjectName(QStringLiteral("DangerButton"));
        removeButton->setMinimumHeight(34);

        connect(removeButton, &QPushButton::clicked, this, [this, share]() {
            const auto reply = QMessageBox::question(this,
                                                     tx(QStringLiteral("關閉分享"), QStringLiteral("Close Share")),
                                                     tx(QStringLiteral("確定要關閉「%1」這個分享嗎？"), QStringLiteral("Are you sure you want to close the share \"%1\"?")).arg(share.name));
            if (reply == QMessageBox::Yes) {
                m_controller->removeShare(share.id);
            }
        });

        cardLayout->addWidget(iconLabel);
        cardLayout->addWidget(nameLabel, 1);

        if (isDirectoryShare(share)) {
            auto *uploadCheck = new QCheckBox(tx(QStringLiteral("寫入"), QStringLiteral("Upload")), card);
            uploadCheck->setChecked(share.allowUpload);
            uploadCheck->setCursor(Qt::PointingHandCursor);
            connect(uploadCheck, &QCheckBox::toggled, this, [this, share](bool checked) {
                m_controller->setShareAllowUpload(share.id, checked);
            });
            cardLayout->addWidget(uploadCheck);

            auto *deleteCheck = new QCheckBox(tx(QStringLiteral("刪除"), QStringLiteral("Delete")), card);
            deleteCheck->setChecked(share.allowDelete);
            deleteCheck->setCursor(Qt::PointingHandCursor);
            connect(deleteCheck, &QCheckBox::toggled, this, [this, share](bool checked) {
                m_controller->setShareAllowDelete(share.id, checked);
            });
            cardLayout->addWidget(deleteCheck);

            auto *createFolderCheck = new QCheckBox(tx(QStringLiteral("建資料夾"), QStringLiteral("New Folder")), card);
            createFolderCheck->setChecked(share.allowCreateDirectory);
            createFolderCheck->setCursor(Qt::PointingHandCursor);
            connect(createFolderCheck, &QCheckBox::toggled, this, [this, share](bool checked) {
                m_controller->setShareAllowCreateDirectory(share.id, checked);
            });
            cardLayout->addWidget(createFolderCheck);
        }

        cardLayout->addWidget(typeLabel);
        cardLayout->addWidget(removeButton);
        m_shareListLayout->addWidget(card);
    }

    m_shareListLayout->addStretch();
}

void MainWindow::saveSystemSettingsFromForm()
{
    AppSettings updatedSettings = m_controller->settings();
    updatedSettings.siteName = m_siteNameEdit->text().trimmed();
    updatedSettings.port = static_cast<quint16>(m_portSpin->value());
    updatedSettings.language = m_languageCombo->currentText();
    updatedSettings.theme = m_themeCombo->currentText();
    updatedSettings.download.password = m_passwordEdit->text();
    updatedSettings.download.resumeEnabled = m_resumeCheck->isChecked();
    updatedSettings.minimizeToTrayOnClose = m_closeToTrayCheck->isChecked();
    updatedSettings.launchOnStartup = m_launchOnStartupCheck->isChecked();
    updatedSettings.download.totalLimitValue = 0;
    updatedSettings.download.totalLimitUnit = QStringLiteral("KB/s");
    updatedSettings.download.perIpLimitEnabled = false;
    updatedSettings.download.perIpLimitValue = 0;
    updatedSettings.download.perIpLimitUnit = QStringLiteral("KB/s");

    const QSize storedSize = isMaximized() ? normalGeometry().size() : size();
    updatedSettings.windowWidth = qMax(minimumWidth(), storedSize.width());
    updatedSettings.windowHeight = qMax(minimumHeight(), storedSize.height());
    updatedSettings.windowMaximized = isMaximized();
    updatedSettings.clearSharesOnExit = false;

    QString autoStartError;
    const bool autoStartApplied = setLaunchOnStartupEnabled(updatedSettings.launchOnStartup, &autoStartError);
    if (!autoStartApplied) {
        updatedSettings.launchOnStartup = false;
        QMessageBox::warning(this, QStringLiteral("儲存設定"), autoStartError);
    }
    m_controller->saveSystemSettings(updatedSettings);
}

void MainWindow::applyTheme()
{
    const QString theme = m_controller->settings().theme;
    const bool dark = theme == QStringLiteral("深色模式") || theme == QStringLiteral("Dark Mode")
                      || ((theme == QStringLiteral("跟隨系統") || theme == QStringLiteral("Follow System"))
                          && systemPrefersDarkTheme());

    const QString style = dark
                              ? QStringLiteral(
                                    "QWidget{background:#0f1420;color:#e9edf6;font-family:'Microsoft JhengHei UI';font-size:14px;}"
                                    "QToolTip{background:#20283a;color:#f5f7fb;border:1px solid #35405a;padding:6px;}"
                                    "#Sidebar{background:#141a28;border-right:1px solid #242d40;}"
                                    "#ContentRoot,#Header{background:#0f1420;}"
                                    "#BrandBox{background:transparent;border:0;}"
                                    "#BrandTitle{font-size:22px;font-weight:800;color:#f5f7fb;}"
                                    "#BrandSub{font-size:13px;font-weight:500;color:#8f9bb0;}"
                                    "#BrandVersion{font-size:11px;font-weight:700;letter-spacing:1px;color:#7467d8;}"
                                    "QPushButton{background:qlineargradient(x1:0,y1:0,x2:1,y2:0,stop:0 #7657e8,stop:1 #4f6df5);color:#ffffff;border:0;border-radius:12px;padding:11px 18px;font-weight:700;}"
                                    "QPushButton:hover{background:qlineargradient(x1:0,y1:0,x2:1,y2:0,stop:0 #8467ee,stop:1 #5e79f7);}"
                                    "QPushButton:disabled{background:#31394b;color:#7d879b;}"
                                    "QPushButton#SidebarButton{background:transparent;color:#9aa5b8;border:0;border-radius:12px;text-align:left;padding:11px 14px;font-weight:600;}"
                                    "QPushButton#SidebarButton:hover{background:#1d2434;color:#d9dff0;}"
                                    "QPushButton#SidebarButton:checked{background:#27233e;color:#b6a9ff;font-weight:800;}"
                                    "QPushButton#SecondaryButton{background:#1b2231;color:#d7deef;border:1px solid #303a50;}"
                                    "QPushButton#SecondaryButton:hover{background:#232c3e;border-color:#45516d;}"
                                    "QPushButton#DangerButton,QPushButton#StopButton{background:#3a2430;color:#ff9aa5;border:1px solid #57323f;}"
                                    "QPushButton#DangerButton:hover,QPushButton#StopButton:hover{background:#4a2936;}"
                                    "QFrame#SidebarFooter,QFrame#InfoCard,QFrame#ShareCard,QFrame#ShareOptions,QFrame#StatCard{background:#171e2d;border:1px solid #2a3449;border-radius:18px;}"
                                    "QFrame#DropArea{background:#151c2a;border:1px dashed #4d5670;border-radius:18px;}"
                                    "QLabel#StatusDot{background:#94a3b8;border-radius:5px;}"
                                    "QLabel#FooterLink{color:#a9b3c6;} QLabel#FooterLink a{color:#9a8cff;}"
                                    "QLabel#FooterMeta{color:#8995aa;font-size:13px;}"
                                    "QLineEdit,QSpinBox,QComboBox{background:#121927;color:#ecf1fb;border:1px solid #303a50;border-radius:11px;padding:10px 12px;selection-background-color:#6d5ce7;}"
                                    "QLineEdit:focus,QSpinBox:focus,QComboBox:focus{border:1px solid #7667e8;}"
                                    "QScrollArea{background:transparent;border:0;} QScrollArea>QWidget>QWidget{background:transparent;}"
                                    "QCheckBox{spacing:9px;color:#c8d0e0;}"
                                    "QCheckBox::indicator{width:18px;height:18px;border:1px solid #46526b;border-radius:5px;background:#121927;}"
                                    "QCheckBox::indicator:checked{background:#6d5ce7;border-color:#6d5ce7;}"
                                    "#PageTitle{font-size:30px;font-weight:800;color:#f4f7fb;}"
                                    "#PageSubtitle{font-size:14px;color:#8e99ae;}"
                                    "#CardTitle{font-size:14px;color:#97a3b7;font-weight:600;}"
                                    "#CardValue{font-size:34px;color:#f5f7fb;font-weight:800;}"
                                    "#SectionTitle{font-size:23px;font-weight:800;color:#eef2f9;background:transparent;border:0;padding:2px 0;}"
                                    "#DropTitle{font-size:18px;font-weight:700;color:#b3bdd0;}"
                                    "#ShareTitle{font-size:16px;font-weight:700;color:#edf1f8;}"
                                    "#ShareSubtext,#EmptyState{color:#8f9bb0;}"
                                    "QLabel#FieldLabel{background:#1b2333;color:#cbd4e5;border:1px solid #303a50;border-radius:10px;padding:8px 10px;font-size:14px;font-weight:700;}"
                                    "QLabel#AboutBody{color:#cbd4e5;}"
                                    "QLabel#ShareTypeBadge[shareKind=\"folder\"]{background:#3a321c;color:#f2c66d;border:1px solid #62522b;border-radius:12px;padding:5px 10px;font-weight:700;}"
                                    "QLabel#ShareTypeBadge[shareKind=\"file\"]{background:#1b3044;color:#7ac7ff;border:1px solid #315472;border-radius:12px;padding:5px 10px;font-weight:700;}"
                                    "QFrame#ShareOptions{background:#141b29;border-radius:14px;}"
                                    "QLabel#ExternalPrefixLabel{color:#9aa5b8;}"
                                    "QMenu{background:#171e2d;color:#eef2f9;border:1px solid #303a50;padding:6px;} QMenu::item{padding:8px 18px;border-radius:7px;} QMenu::item:selected{background:#292540;color:#c6bcff;}"
                                    "QScrollBar:vertical{background:transparent;width:10px;margin:3px;} QScrollBar::handle:vertical{background:#3a445b;border-radius:5px;min-height:28px;} QScrollBar::add-line:vertical,QScrollBar::sub-line:vertical{height:0;}"
                                    "QScrollBar:horizontal{background:transparent;height:10px;margin:3px;} QScrollBar::handle:horizontal{background:#3a445b;border-radius:5px;min-width:28px;} QScrollBar::add-line:horizontal,QScrollBar::sub-line:horizontal{width:0;}")
                              : QStringLiteral(
                                    "QWidget{background:#f7f9fc;color:#18243a;font-family:'Microsoft JhengHei UI';font-size:14px;}"
                                    "QToolTip{background:#1d2940;color:#ffffff;border:0;padding:6px;}"
                                    "#Sidebar{background:#f7f9fc;border-right:1px solid #e3e8f0;}"
                                    "#ContentRoot,#Header{background:#f7f9fc;}"
                                    "#BrandBox{background:transparent;border:0;}"
                                    "#BrandTitle{font-size:22px;font-weight:800;color:#18243a;}"
                                    "#BrandSub{font-size:13px;font-weight:500;color:#7b879b;}"
                                    "#BrandVersion{font-size:11px;font-weight:700;letter-spacing:1px;color:#7a6cf0;}"
                                    "QPushButton{background:qlineargradient(x1:0,y1:0,x2:1,y2:0,stop:0 #7657e8,stop:1 #4f6df5);color:#ffffff;border:0;border-radius:12px;padding:11px 18px;font-weight:700;}"
                                    "QPushButton:hover{background:qlineargradient(x1:0,y1:0,x2:1,y2:0,stop:0 #8467ee,stop:1 #5e79f7);}"
                                    "QPushButton:disabled{background:#e4e8ef;color:#9aa4b4;}"
                                    "QPushButton#SidebarButton{background:transparent;color:#617087;border:0;border-radius:12px;text-align:left;padding:11px 14px;font-weight:600;}"
                                    "QPushButton#SidebarButton:hover{background:#f7f5ff;color:#5d4ed9;}"
                                    "QPushButton#SidebarButton:checked{background:#f0edff;color:#6654e8;font-weight:800;}"
                                    "QPushButton#SecondaryButton{background:#ffffff;color:#53627a;border:1px solid #dfe4ec;}"
                                    "QPushButton#SecondaryButton:hover{background:#f8f7ff;color:#5e50d9;border-color:#cfc7ff;}"
                                    "QPushButton#DangerButton,QPushButton#StopButton{background:#fff0f2;color:#d94f5c;border:1px solid #ffd4d8;}"
                                    "QPushButton#DangerButton:hover,QPushButton#StopButton:hover{background:#ffe6e9;}"
                                    "QFrame#SidebarFooter,QFrame#InfoCard,QFrame#ShareCard,QFrame#ShareOptions,QFrame#StatCard{background:#ffffff;border:1px solid #e6eaf1;border-radius:18px;}"
                                    "QFrame#DropArea{background:#fbfbfe;border:1px dashed #cfd5e1;border-radius:18px;}"
                                    "QLabel#StatusDot{background:#94a3b8;border-radius:5px;}"
                                    "QLabel#FooterLink{color:#6f7d91;} QLabel#FooterLink a{color:#6255e7;}"
                                    "QLabel#FooterMeta{color:#7c899d;font-size:13px;}"
                                    "QLineEdit,QSpinBox,QComboBox{background:#ffffff;color:#1f2b40;border:1px solid #dfe4ec;border-radius:11px;padding:10px 12px;selection-background-color:#6d5ce7;}"
                                    "QLineEdit:focus,QSpinBox:focus,QComboBox:focus{border:1px solid #7566e7;}"
                                    "QScrollArea{background:transparent;border:0;} QScrollArea>QWidget>QWidget{background:transparent;}"
                                    "QCheckBox{spacing:9px;color:#4e5d73;}"
                                    "QCheckBox::indicator{width:18px;height:18px;border:1px solid #cbd2df;border-radius:5px;background:#ffffff;}"
                                    "QCheckBox::indicator:checked{background:#6d5ce7;border-color:#6d5ce7;}"
                                    "#PageTitle{font-size:30px;font-weight:800;color:#17243a;}"
                                    "#PageSubtitle{font-size:14px;color:#7d899d;}"
                                    "#CardTitle{font-size:14px;color:#748096;font-weight:600;}"
                                    "#CardValue{font-size:34px;color:#17243a;font-weight:800;}"
                                    "#SectionTitle{font-size:23px;font-weight:800;color:#1b2940;background:transparent;border:0;padding:2px 0;}"
                                    "#DropTitle{font-size:18px;font-weight:700;color:#607087;}"
                                    "#ShareTitle{font-size:16px;font-weight:700;color:#1f2d44;}"
                                    "#ShareSubtext,#EmptyState{color:#7d899d;}"
                                    "QLabel#FieldLabel{background:#f8f9fc;color:#526179;border:1px solid #e2e6ee;border-radius:10px;padding:8px 10px;font-size:14px;font-weight:700;}"
                                    "QLabel#AboutBody{color:#43516a;}"
                                    "QLabel#ShareTypeBadge[shareKind=\"folder\"]{background:#fff4d9;color:#b77b08;border:1px solid #f2ddb0;border-radius:12px;padding:5px 10px;font-weight:700;}"
                                    "QLabel#ShareTypeBadge[shareKind=\"file\"]{background:#eaf4ff;color:#2f7df4;border:1px solid #cfe4fb;border-radius:12px;padding:5px 10px;font-weight:700;}"
                                    "QFrame#ShareOptions{background:#fafbfe;border-radius:14px;}"
                                    "QLabel#ExternalPrefixLabel{color:#768399;}"
                                    "QMenu{background:#ffffff;color:#213049;border:1px solid #dfe4ec;padding:6px;} QMenu::item{padding:8px 18px;border-radius:7px;} QMenu::item:selected{background:#f0edff;color:#6255e7;}"
                                    "QScrollBar:vertical{background:transparent;width:10px;margin:3px;} QScrollBar::handle:vertical{background:#cfd5df;border-radius:5px;min-height:28px;} QScrollBar::add-line:vertical,QScrollBar::sub-line:vertical{height:0;}"
                                    "QScrollBar:horizontal{background:transparent;height:10px;margin:3px;} QScrollBar::handle:horizontal{background:#cfd5df;border-radius:5px;min-width:28px;} QScrollBar::add-line:horizontal,QScrollBar::sub-line:horizontal{width:0;}");

    qApp->setStyleSheet(style);
}


QString MainWindow::tx(const QString &zh, const QString &en) const
{
    if (m_controller && m_controller->settings().language.compare(QStringLiteral("English"), Qt::CaseInsensitive) == 0) {
        return en;
    }
    return zh;
}

QString MainWindow::shareTypeLabel(const ShareItem &share) const
{
    if (isDirectoryShare(share)) {
        return tx(QStringLiteral("資料夾"), QStringLiteral("Folder"));
    }

    if (share.type == ShareType::Link) {
        return tx(QStringLiteral("連結"), QStringLiteral("Link"));
    }

    const QFileInfo info(share.sourcePath);
    const QString suffix = info.suffix().trimmed().toUpper();
    return suffix.isEmpty() ? tx(QStringLiteral("檔案"), QStringLiteral("File")) 
                            : tx(QStringLiteral("%1 檔案").arg(suffix), QStringLiteral("%1 File").arg(suffix));
}

QString MainWindow::translateStatusToEn(const QString &msg) const
{
    if (msg.contains(QStringLiteral("伺服器已停止"))) return QStringLiteral("Server Stopped");
    if (msg.contains(QStringLiteral("伺服器已啟動"))) {
        return QStringLiteral("Server Started: ") + msg.section(QChar('：'), 1);
    }
    if (msg.contains(QStringLiteral("請先啟用外部連結"))) return QStringLiteral("Please enable external link first");
    if (msg.contains(QStringLiteral("代理伺服器連接中"))) return QStringLiteral("WAN Proxy Connecting...");
    if (msg.contains(QStringLiteral("代理伺服器連接失敗"))) {
        return msg.contains(QStringLiteral("：")) ? QStringLiteral("WAN Proxy connection failed: ") + msg.section(QChar(':'), 1) : QStringLiteral("WAN Proxy connection failed");
    }
    if (msg.contains(QStringLiteral("代理伺服器已就緒"))) return QStringLiteral("WAN Proxy Ready");
    if (msg.contains(QStringLiteral("正在下載 cloudflared"))) return QStringLiteral("Downloading cloudflared...");
    if (msg.contains(QStringLiteral("cloudflared 已安裝"))) return QStringLiteral("cloudflared installed");
    if (msg.contains(QStringLiteral("cloudflared 下載失敗"))) {
        return msg.contains(QStringLiteral("：")) ? QStringLiteral("Failed to download cloudflared: ") + msg.section(QChar(':'), 1) : QStringLiteral("Failed to download cloudflared");
    }
    if (msg.contains(QStringLiteral("正在向 is.gd / v.gd"))) return QStringLiteral("Requesting short URL...");
    if (msg.contains(QStringLiteral("is.gd / v.gd 短網址申請失敗"))) return QStringLiteral("Short URL failed, using original URL");
    if (msg.contains(QStringLiteral("外網短網址："))) {
        return QStringLiteral("Short URL: ") + msg.section(QChar('：'), 1);
    }
    if (msg.contains(QStringLiteral("已為原始外網連結："))) {
        return QStringLiteral("Using original public URL: ") + msg.section(QChar('：'), 1);
    }
    if (msg.contains(QStringLiteral("分享設定已儲存"))) return QStringLiteral("Shares saved to shares.ini");
    if (msg.contains(QStringLiteral("分享設定儲存失敗"))) return QStringLiteral("Failed to save shares");
    return msg;
}

void MainWindow::retranslateUi()
{
    refreshHeaderTitle();

    if (m_btnDashboard) m_btnDashboard->setText(tx(QStringLiteral("儀表板"), QStringLiteral("Dashboard")));
    if (m_btnShares) m_btnShares->setText(tx(QStringLiteral("分享管理"), QStringLiteral("Shares")));
    if (m_btnSettings) m_btnSettings->setText(tx(QStringLiteral("系統設定"), QStringLiteral("Settings")));
    if (m_btnAbout) m_btnAbout->setText(tx(QStringLiteral("關於"), QStringLiteral("About")));
    if (m_brandSubtitle) m_brandSubtitle->setText(tx(QStringLiteral("檔案分享伺服器"), QStringLiteral("File Sharing Server")));
    if (m_pageSubtitle) m_pageSubtitle->setText(tx(QStringLiteral("輕鬆分享・快速安全・隨時存取"), QStringLiteral("Simple sharing · Fast and secure · Access anywhere")));

    if (m_lblTotalDownloadsTitle) m_lblTotalDownloadsTitle->setText(tx(QStringLiteral("總下載次數"), QStringLiteral("Total Downloads")));
    if (m_lblTotalBytesTitle) m_lblTotalBytesTitle->setText(tx(QStringLiteral("總下載流量"), QStringLiteral("Total Data Served")));
    if (m_lblActiveConnectionsTitle) m_lblActiveConnectionsTitle->setText(tx(QStringLiteral("目前連線"), QStringLiteral("Active Connections")));
    if (m_lblLiveSpeedTitle) m_lblLiveSpeedTitle->setText(tx(QStringLiteral("即時傳輸速率"), QStringLiteral("Current Speed")));

    if (m_lblServerInfoTitle) m_lblServerInfoTitle->setText(tx(QStringLiteral("伺服器運作資訊"), QStringLiteral("Server Information")));

    if (m_btnSaveShares) m_btnSaveShares->setText(tx(QStringLiteral("儲存分享"), QStringLiteral("Save Shares")));
    if (m_btnAddShare) m_btnAddShare->setText(tx(QStringLiteral("新增分享"), QStringLiteral("Add Share")));
    if (m_shareExternalLinkCheck) m_shareExternalLinkCheck->setText(tx(QStringLiteral("啟用外部連結"), QStringLiteral("Enable External Link")));
    if (m_dropTitle) m_dropTitle->setText(tx(QStringLiteral("可直接拖曳檔案或資料夾到這裡快速連結"), QStringLiteral("Drag and drop files or folders here to share")));

    if (m_lblSystemTitle) m_lblSystemTitle->setText(tx(QStringLiteral("系統設定"), QStringLiteral("System Settings")));
    if (m_btnSelectLogo) m_btnSelectLogo->setText(tx(QStringLiteral("選擇 Logo"), QStringLiteral("Select Logo")));
    if (m_btnClearLogo) m_btnClearLogo->setText(tx(QStringLiteral("移除 Logo"), QStringLiteral("Remove Logo")));

    if (m_lblSiteName) m_lblSiteName->setText(tx(QStringLiteral("站台名稱"), QStringLiteral("Site Name")));
    if (m_lblPort) m_lblPort->setText(tx(QStringLiteral("HTTP 連接埠"), QStringLiteral("HTTP Port")));
    if (m_lblLanguage) m_lblLanguage->setText(tx(QStringLiteral("介面語言"), QStringLiteral("UI Language")));
    if (m_lblTheme) m_lblTheme->setText(tx(QStringLiteral("外觀主題"), QStringLiteral("Theme")));
    if (m_lblPassword) m_lblPassword->setText(tx(QStringLiteral("下載密碼"), QStringLiteral("Download Password")));
    if (m_btnSaveSettings) m_btnSaveSettings->setText(tx(QStringLiteral("儲存設定"), QStringLiteral("Save Settings")));

    if (m_passwordEdit) m_passwordEdit->setPlaceholderText(tx(QStringLiteral("留空代表不需要下載密碼"), QStringLiteral("Leave empty for no download password")));
    if (m_resumeCheck) m_resumeCheck->setText(tx(QStringLiteral("啟用斷點續傳（支援 HTTP Range 請求）"), QStringLiteral("Enable Resumable Downloads (HTTP Range)")));
    if (m_closeToTrayCheck) m_closeToTrayCheck->setText(tx(QStringLiteral("關閉視窗時不直接退出，保持在系統列"), QStringLiteral("Close to system tray instead of exiting")));
    if (m_launchOnStartupCheck) m_launchOnStartupCheck->setText(tx(QStringLiteral("開機時自動啟動並在系統列執行"), QStringLiteral("Launch on startup and minimize to tray")));

    if (m_themeCombo) {
        const QSignalBlocker blocker(m_themeCombo);
        const int index = m_themeCombo->currentIndex();
        m_themeCombo->clear();
        m_themeCombo->addItems({tx(QStringLiteral("跟隨系統"), QStringLiteral("Follow System")),
                                tx(QStringLiteral("淺色模式"), QStringLiteral("Light Mode")),
                                tx(QStringLiteral("深色模式"), QStringLiteral("Dark Mode"))});
        m_themeCombo->setCurrentIndex(index);
    }

    if (m_languageCombo) {
        const QSignalBlocker blocker(m_languageCombo);
        const int index = m_languageCombo->currentIndex();
        m_languageCombo->clear();
        m_languageCombo->addItems({QStringLiteral("繁體中文"), QStringLiteral("English")});
        m_languageCombo->setCurrentIndex(index);
    }

    if (m_trayShowAction) m_trayShowAction->setText(tx(QStringLiteral("顯示主頁"), QStringLiteral("Show Main Window")));
    if (m_trayExitAction) m_trayExitAction->setText(tx(QStringLiteral("退出"), QStringLiteral("Exit")));

    refreshAbout();
    refreshStartStopButton();
    refreshFooter();
    refreshShares();
    refreshSettingsForms();
}

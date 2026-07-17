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
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFontMetrics>
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
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QScreen>
#include <QScrollArea>
#include <QSettings>
#include <QSet>
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
qreal g_uiScale = 1.0;

qreal calculateUiScale()
{
    QScreen *screen = QGuiApplication::primaryScreen();
    if (!screen) {
        return 1.0;
    }

    // QScreen geometry is expressed in device-independent pixels when Qt high-DPI
    // scaling is active. This means Windows 150%/200% scaling is already taken
    // into account, and we only add a modest boost when a display still exposes
    // substantially more logical workspace than a 1920x1080 reference screen.
    const QSize available = screen->availableGeometry().size();
    const qreal widthRatio = available.width() / 1920.0;
    const qreal heightRatio = available.height() / 1080.0;
    const qreal workspaceRatio = qMin(widthRatio, heightRatio);
    if (workspaceRatio <= 1.0) {
        return 1.0;
    }

    // Square-root growth keeps 2K comfortable without making 4K oversized:
    // 2560x1440 -> ~1.15x, 3840x2160 -> ~1.41x (at 100% OS scaling).
    return qBound<qreal>(1.0, std::sqrt(workspaceRatio), 1.50);
}

int sp(int value)
{
    if (value <= 0) {
        return value;
    }
    return qMax(1, qRound(value * g_uiScale));
}


QString scaleStyleSheetPixels(const QString &style, qreal scale)
{
    if (scale <= 1.001) {
        return style;
    }

    static const QRegularExpression pixelPattern(QStringLiteral(R"((\d+(?:\.\d+)?)px)"));
    QString result;
    result.reserve(style.size() + style.size() / 12);

    int last = 0;
    auto matchIt = pixelPattern.globalMatch(style);
    while (matchIt.hasNext()) {
        const QRegularExpressionMatch match = matchIt.next();
        result += style.mid(last, match.capturedStart() - last);

        const qreal value = match.captured(1).toDouble();
        const int scaledValue = value <= 0.0 ? 0 : qMax(1, qRound(value * scale));
        result += QString::number(scaledValue) + QStringLiteral("px");
        last = match.capturedEnd();
    }
    result += style.mid(last);
    return result;
}

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
    Chat,
    Upload,
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
    case UiIcon::Chat:
        painter.drawRoundedRect(R(7, 9, 34, 26), 8 * scale, 8 * scale);
        painter.drawLine(P(15, 35), P(11, 41));
        painter.drawLine(P(11, 41), P(21, 36));
        painter.setBrush(foreground);
        painter.setPen(Qt::NoPen);
        painter.drawEllipse(R(14, 20, 4, 4));
        painter.drawEllipse(R(22, 20, 4, 4));
        painter.drawEllipse(R(30, 20, 4, 4));
        painter.setBrush(Qt::NoBrush);
        painter.setPen(pen);
        break;
    case UiIcon::Upload:
        painter.drawRoundedRect(R(7, 21, 34, 18), 7 * scale, 7 * scale);
        painter.drawLine(P(24, 31), P(24, 9));
        painter.drawLine(P(16, 17), P(24, 9));
        painter.drawLine(P(32, 17), P(24, 9));
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
    const QPixmap normal = drawUiIcon(kind, QColor(QStringLiteral("#64748b")), Qt::transparent, sp(24));
    const QPixmap active = drawUiIcon(kind, QColor(QStringLiteral("#6d5ce7")), Qt::transparent, sp(24));
    icon.addPixmap(normal, QIcon::Normal, QIcon::Off);
    icon.addPixmap(active, QIcon::Normal, QIcon::On);
    icon.addPixmap(active, QIcon::Active, QIcon::Off);
    icon.addPixmap(active, QIcon::Selected, QIcon::On);
    return icon;
}

void applySoftShadow(QWidget *widget, int blurRadius = 24, int yOffset = 7)
{
    auto *shadow = new QGraphicsDropShadowEffect(widget);
    shadow->setBlurRadius(sp(blurRadius));
    shadow->setOffset(0, sp(yOffset));
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
    layout->setContentsMargins(sp(22), sp(20), sp(22), sp(22));
    layout->setSpacing(sp(9));

    auto *icon = new QLabel(card);
    icon->setObjectName(QStringLiteral("StatIcon"));
    icon->setFixedSize(sp(48), sp(48));
    icon->setPixmap(drawUiIcon(iconKind, iconColor, iconBackground, sp(48)));
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
    button->setMinimumHeight(sp(50));
    button->setIcon(buildSidebarIcon(iconKind));
    button->setIconSize(QSize(sp(22), sp(22)));
    button->setObjectName(QStringLiteral("SidebarButton"));
    return button;
}

QLabel *makeFormLabel(const QString &text, QWidget *parent = nullptr)
{
    auto *label = new QLabel(text, parent);
    label->setObjectName(QStringLiteral("FieldLabel"));
    label->setAlignment(Qt::AlignCenter);
    label->setMinimumHeight(sp(40));
    label->setMinimumWidth(sp(118));
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

    QPixmap pixmap(sp(72), sp(72));
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(isFolder ? QColor(QStringLiteral("#ffe8a3")) : QColor(QStringLiteral("#cfe9ff")));
    painter.drawRoundedRect(pixmap.rect(), sp(20), sp(20));

    painter.setPen(isFolder ? QColor(QStringLiteral("#cf9600")) : QColor(QStringLiteral("#148fe8")));
    painter.setFont(QFont(QStringLiteral("Microsoft JhengHei UI"), sp(24), QFont::Bold));
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
    g_uiScale = calculateUiScale();

    // Keep the whole interface visible on first launch while still allowing
    // proportional resizing.  The minimum size is capped to the current
    // screen so smaller displays are never forced beyond their work area.
    QSize availableSize(sp(1380), sp(820));
    if (QScreen *screen = QGuiApplication::primaryScreen()) {
        availableSize = screen->availableGeometry().size();
    }
    const int safeWidth = qMax(sp(960), availableSize.width() - sp(24));
    const int safeHeight = qMax(sp(620), availableSize.height() - sp(48));
    setMinimumSize(qMin(sp(1120), safeWidth), qMin(sp(700), safeHeight));
    resize(qMin(sp(1380), safeWidth), qMin(sp(820), safeHeight));

    buildUi();
    setupTrayIcon();
    restoreWindowSettings();
    refreshAll();
    retranslateUi();

    connect(m_controller, &Controller::sharesChanged, this, &MainWindow::refreshShares);
    connect(m_controller, &Controller::downloadsChanged, this, &MainWindow::refreshStats);
    connect(m_controller, &Controller::activeTransfersChanged, this, &MainWindow::refreshActiveTransfers);
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

    QSize targetSize(sp(settings.windowWidth), sp(settings.windowHeight));
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
    settings.windowWidth = qMax(900, qRound(storedSize.width() / g_uiScale));
    settings.windowHeight = qMax(600, qRound(storedSize.height() / g_uiScale));
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
    sidebar->setMinimumWidth(sp(240));
    sidebar->setMaximumWidth(sp(320));
    sidebar->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);

    auto *layout = new QVBoxLayout(sidebar);
    layout->setContentsMargins(sp(20), sp(22), sp(20), sp(20));
    layout->setSpacing(sp(10));

    auto *brandBox = new QFrame(sidebar);
    brandBox->setObjectName(QStringLiteral("BrandBox"));
    auto *brandLayout = new QVBoxLayout(brandBox);
    brandLayout->setContentsMargins(sp(8), sp(8), sp(8), sp(16));
    brandLayout->setSpacing(sp(5));

    auto *logo = new QLabel(brandBox);
    logo->setPixmap(QPixmap(QStringLiteral(":/desktop_logo.png")).scaled(sp(104), sp(104), Qt::KeepAspectRatio, Qt::SmoothTransformation));
    logo->setFixedSize(sp(104), sp(104));
    logo->setAlignment(Qt::AlignCenter);

    auto *brandTitle = new QLabel(QStringLiteral("Easy Cloud HFS"), brandBox);
    brandTitle->setObjectName(QStringLiteral("BrandTitle"));
    brandTitle->setAlignment(Qt::AlignCenter);

    m_brandSubtitle = new QLabel(tx(QStringLiteral("檔案分享伺服器"), QStringLiteral("File Sharing Server")), brandBox);
    m_brandSubtitle->setObjectName(QStringLiteral("BrandSub"));
    m_brandSubtitle->setAlignment(Qt::AlignCenter);

    auto *brandVersion = new QLabel(QStringLiteral("V1.2.2.0"), brandBox);
    brandVersion->setObjectName(QStringLiteral("BrandVersion"));
    brandVersion->setAlignment(Qt::AlignCenter);

    brandLayout->addWidget(logo, 0, Qt::AlignHCenter);
    brandLayout->addWidget(brandTitle);
    brandLayout->addWidget(m_brandSubtitle);
    brandLayout->addWidget(brandVersion);
    layout->addWidget(brandBox);
    layout->addSpacing(sp(6));

    auto *buttonGroup = new QButtonGroup(this);
    buttonGroup->setExclusive(true);

    m_btnDashboard = makeSidebarButton(tx(QStringLiteral("儀表板"), QStringLiteral("Dashboard")), UiIcon::Dashboard, sidebar);
    m_btnShares = makeSidebarButton(tx(QStringLiteral("分享管理"), QStringLiteral("Shares")), UiIcon::Shares, sidebar);
    m_btnChat = makeSidebarButton(tx(QStringLiteral("聊天室"), QStringLiteral("Chatroom")), UiIcon::Chat, sidebar);
    m_btnChat->setCheckable(false);
    m_btnSettings = makeSidebarButton(tx(QStringLiteral("系統設定"), QStringLiteral("Settings")), UiIcon::Settings, sidebar);
    m_btnAbout = makeSidebarButton(tx(QStringLiteral("關於"), QStringLiteral("About")), UiIcon::About, sidebar);

    buttonGroup->addButton(m_btnDashboard, static_cast<int>(DashboardPage));
    buttonGroup->addButton(m_btnShares, static_cast<int>(SharesPage));
    buttonGroup->addButton(m_btnSettings, static_cast<int>(SystemSettingsPage));
    buttonGroup->addButton(m_btnAbout, static_cast<int>(AboutPage));

    layout->addWidget(m_btnDashboard);
    layout->addWidget(m_btnShares);
    layout->addWidget(m_btnChat);
    layout->addWidget(m_btnSettings);
    layout->addWidget(m_btnAbout);

    connect(m_btnChat, &QPushButton::clicked, this, [this]() {
        if (!m_controller->settings().chatEnabled) {
            QMessageBox::information(this,
                                     tx(QStringLiteral("聊天室未啟用"), QStringLiteral("Chatroom Disabled")),
                                     tx(QStringLiteral("請先到「分享管理」勾選「啟用聊天室功能」。"),
                                        QStringLiteral("Enable the chatroom first from the Shares page.")));
            return;
        }
        if (!m_controller->isServerRunning()) {
            QMessageBox::information(this,
                                     tx(QStringLiteral("伺服器尚未啟動"), QStringLiteral("Server Not Running")),
                                     tx(QStringLiteral("請先啟動伺服器，再進入聊天室。"),
                                        QStringLiteral("Start the server before opening the chatroom.")));
            return;
        }

        QString chatUrl = m_controller->localBaseUrl();
        if (chatUrl.endsWith(QLatin1Char('/'))) {
            chatUrl.chop(1);
        }
        QDesktopServices::openUrl(QUrl(chatUrl + QStringLiteral("/chat")));
    });

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
    footerLayout->setContentsMargins(sp(16), sp(16), sp(16), sp(16));
    footerLayout->setSpacing(sp(9));

    auto *statusRow = new QHBoxLayout();
    statusRow->setSpacing(sp(9));
    m_statusDot = new QLabel(footer);
    m_statusDot->setObjectName(QStringLiteral("StatusDot"));
    m_statusDot->setFixedSize(sp(10), sp(10));

    m_statusText = new QLabel(QStringLiteral("伺服器未啟動"), footer);
    m_statusText->setObjectName(QStringLiteral("StatusText"));
    m_statusText->setWordWrap(false);
    m_statusText->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_statusText->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    statusRow->addWidget(m_statusDot, 0, Qt::AlignVCenter);
    statusRow->addWidget(m_statusText, 1);
    footerLayout->addLayout(statusRow);

    m_localUrlLabel = new QLabel(footer);
    m_localUrlLabel->setObjectName(QStringLiteral("FooterLocalLink"));
    m_localUrlLabel->setWordWrap(false);
    m_localUrlLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_localUrlLabel->setTextInteractionFlags(Qt::TextBrowserInteraction);
    m_localUrlLabel->setOpenExternalLinks(false);
    connect(m_localUrlLabel, &QLabel::linkActivated, this, [this](const QString &link) {
        copyTextToClipboard(link);
    });

    m_externalUrlLabel = new QLabel(footer);
    m_externalUrlLabel->setObjectName(QStringLiteral("FooterLink"));
    m_externalUrlLabel->setWordWrap(false);
    m_externalUrlLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
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

    rootLayout->addWidget(sidebar, 22);
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
    headerLayout->setContentsMargins(sp(34), sp(26), sp(34), sp(20));
    headerLayout->setSpacing(sp(18));

    auto *titleLayout = new QVBoxLayout();
    titleLayout->setContentsMargins(0, 0, 0, 0);
    titleLayout->setSpacing(sp(5));

    m_pageTitle = new QLabel(header);
    m_pageTitle->setObjectName(QStringLiteral("PageTitle"));
    m_pageTitle->setWordWrap(true);

    m_pageSubtitle = new QLabel(tx(QStringLiteral("輕鬆分享・快速安全・隨時存取"),
                                       QStringLiteral("Simple sharing · Fast and secure · Access anywhere")),
                                    header);
    m_pageSubtitle->setObjectName(QStringLiteral("PageSubtitle"));

    titleLayout->addWidget(m_pageTitle);
    titleLayout->addWidget(m_pageSubtitle);

    m_openWebButton = new QPushButton(header);
    m_openWebButton->setObjectName(QStringLiteral("OpenWebButton"));
    m_openWebButton->setCursor(Qt::PointingHandCursor);
    m_openWebButton->setMinimumHeight(sp(48));
    m_openWebButton->setMinimumWidth(sp(154));
    connect(m_openWebButton, &QPushButton::clicked, this, [this]() {
        if (!m_controller->isServerRunning()) {
            return;
        }

        const QString localUrl = m_controller->localBaseUrl().trimmed();
        if (!localUrl.isEmpty()) {
            QDesktopServices::openUrl(QUrl(localUrl));
        }
    });

    m_startStopButton = new QPushButton(header);
    m_startStopButton->setCursor(Qt::PointingHandCursor);
    m_startStopButton->setMinimumHeight(sp(48));
    m_startStopButton->setMinimumWidth(sp(154));
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
    headerLayout->addWidget(m_openWebButton, 0, Qt::AlignVCenter);
    headerLayout->addWidget(m_startStopButton, 0, Qt::AlignVCenter);

    m_pages = new QStackedWidget(content);
    buildPages();

    layout->addWidget(header);
    layout->addWidget(m_pages, 1);
    rootLayout->addWidget(content, 78);
}

void MainWindow::buildPages()
{
    auto *dashboard = new QWidget(m_pages);
    auto *dashboardLayout = new QVBoxLayout(dashboard);
    dashboardLayout->setContentsMargins(sp(34), sp(18), sp(34), sp(30));
    dashboardLayout->setSpacing(sp(24));

    auto *statsGrid = new QGridLayout();
    statsGrid->setHorizontalSpacing(sp(18));
    statsGrid->setVerticalSpacing(sp(18));
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
    infoLayout->setContentsMargins(sp(26), sp(24), sp(26), sp(24));
    m_lblServerInfoTitle = new QLabel(tx(QStringLiteral("伺服器運作資訊"), QStringLiteral("Server Information")), infoCard);
    m_lblServerInfoTitle->setObjectName(QStringLiteral("SectionTitle"));
    m_serverInfoLabel = new QLabel(infoCard);
    m_serverInfoLabel->setWordWrap(true);
    m_serverInfoLabel->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    m_serverInfoLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    infoLayout->addWidget(m_lblServerInfoTitle);
    infoLayout->addWidget(m_serverInfoLabel);
    dashboardLayout->addWidget(infoCard);

    auto *transferCard = new QFrame(dashboard);
    transferCard->setObjectName(QStringLiteral("InfoCard"));
    applySoftShadow(transferCard, 24, 7);
    auto *transferCardLayout = new QVBoxLayout(transferCard);
    transferCardLayout->setContentsMargins(sp(26), sp(24), sp(26), sp(24));
    transferCardLayout->setSpacing(sp(14));
    m_lblActiveTransfersTitle = new QLabel(tx(QStringLiteral("目前檔案下載與打包進度"), QStringLiteral("Current Downloads and Packaging")), transferCard);
    m_lblActiveTransfersTitle->setObjectName(QStringLiteral("SectionTitle"));
    m_activeTransfersContainer = new QWidget(transferCard);
    m_activeTransfersLayout = new QVBoxLayout(m_activeTransfersContainer);
    m_activeTransfersLayout->setContentsMargins(0, 0, 0, 0);
    m_activeTransfersLayout->setSpacing(sp(10));
    transferCardLayout->addWidget(m_lblActiveTransfersTitle);
    transferCardLayout->addWidget(m_activeTransfersContainer);
    dashboardLayout->addWidget(transferCard);
    dashboardLayout->addStretch();
    m_pages->addWidget(makeScrollablePage(dashboard, m_pages));

    m_sharePage = new QWidget(m_pages);
    auto *shareLayout = new QVBoxLayout(m_sharePage);
    shareLayout->setContentsMargins(sp(24), sp(22), sp(24), sp(22));
    shareLayout->setSpacing(sp(16));

    auto *shareHeader = new QHBoxLayout();
    shareHeader->setSpacing(sp(10));
    m_shareExternalLinkCheck = new QCheckBox(tx(QStringLiteral("啟用外部連結"), QStringLiteral("Enable External Link")), m_sharePage);
    m_chatEnabledCheck = new QCheckBox(tx(QStringLiteral("啟用聊天室功能"), QStringLiteral("Enable Chatroom")), m_sharePage);
    m_chatEnabledCheck->setChecked(m_controller->settings().chatEnabled);
    m_chatEnabledCheck->setCursor(Qt::PointingHandCursor);
    connect(m_chatEnabledCheck, &QCheckBox::toggled, m_controller, &Controller::setChatEnabled);
    m_shareExternalInfoLabel = new QLabel(tx(QStringLiteral("使用 Cloudflare Tunnel 代理"), QStringLiteral("Use Cloudflare Tunnel Proxy")), m_sharePage);
    m_shareExternalInfoLabel->setObjectName(QStringLiteral("ExternalPrefixLabel"));
    m_shareExternalInfoLabel->setAlignment(Qt::AlignCenter);
    m_shareExternalInfoLabel->setTextFormat(Qt::RichText);
    m_shareExternalInfoLabel->setTextInteractionFlags(Qt::TextBrowserInteraction);
    m_shareExternalInfoLabel->setOpenExternalLinks(false);
    m_shareExternalInfoLabel->setMinimumWidth(sp(170));
    m_shareExternalInfoLabel->setMaximumWidth(sp(360));
    m_shareExternalInfoLabel->setMinimumHeight(sp(40));
    m_shareExternalInfoLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    m_btnSaveShares = new QPushButton(tx(QStringLiteral("儲存分享"), QStringLiteral("Save Shares")), m_sharePage);
    m_btnSaveShares->setObjectName(QStringLiteral("SecondaryButton"));
    m_btnAddShare = new QPushButton(tx(QStringLiteral("新增分享"), QStringLiteral("Add Share")), m_sharePage);
    m_btnSaveShares->setCursor(Qt::PointingHandCursor);
    m_btnAddShare->setCursor(Qt::PointingHandCursor);
    m_btnSaveShares->setMinimumHeight(sp(40));
    m_btnSaveShares->setMinimumWidth(sp(104));
    m_btnAddShare->setMinimumHeight(sp(42));
    m_btnAddShare->setMinimumWidth(sp(128));
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
    shareHeader->addWidget(m_btnAddShare, 0);
    shareHeader->addWidget(m_chatEnabledCheck, 0);
    shareHeader->addWidget(m_shareExternalLinkCheck, 0);
    shareHeader->addWidget(m_shareExternalInfoLabel, 1);
    shareHeader->addWidget(m_btnSaveShares, 0);
    shareLayout->addLayout(shareHeader);

    auto *dropArea = new ShareDropArea(m_sharePage);
    dropArea->setObjectName(QStringLiteral("DropArea"));
    dropArea->setMinimumHeight(sp(190));
    auto *dropLayout = new QVBoxLayout(dropArea);
    dropLayout->setContentsMargins(sp(24), sp(22), sp(24), sp(22));
    dropLayout->setSpacing(sp(9));

    m_dropIcon = new QLabel(dropArea);
    m_dropIcon->setObjectName(QStringLiteral("DropIcon"));
    m_dropIcon->setFixedSize(sp(56), sp(56));
    m_dropIcon->setPixmap(drawUiIcon(UiIcon::Upload, QColor(QStringLiteral("#6d5ce7")), Qt::transparent, sp(56)));
    m_dropIcon->setAlignment(Qt::AlignCenter);

    m_dropTitle = new QLabel(tx(QStringLiteral("可直接拖曳檔案或資料夾到這裡快速連結"), QStringLiteral("Drag and drop files or folders here to share")), dropArea);
    m_dropTitle->setObjectName(QStringLiteral("DropTitle"));
    m_dropTitle->setAlignment(Qt::AlignCenter);
    m_dropTitle->setWordWrap(true);

    m_dropSubtitle = new QLabel(tx(QStringLiteral("請將檔案或資料夾拖曳至此區域，或使用上方「新增分享」按鈕"),
                                      QStringLiteral("Drop files or folders here, or use the Add Share button above")),
                                  dropArea);
    m_dropSubtitle->setObjectName(QStringLiteral("DropHint"));
    m_dropSubtitle->setAlignment(Qt::AlignCenter);
    m_dropSubtitle->setWordWrap(true);

    dropLayout->addStretch();
    dropLayout->addWidget(m_dropIcon, 0, Qt::AlignHCenter);
    dropLayout->addWidget(m_dropTitle, 0, Qt::AlignCenter);
    dropLayout->addWidget(m_dropSubtitle, 0, Qt::AlignCenter);
    dropLayout->addStretch();
    dropArea->onDropPaths = [this](const QStringList &paths) { m_controller->addPaths(paths); };
    shareLayout->addWidget(dropArea);

    auto *listScroll = new QScrollArea(m_sharePage);
    listScroll->setWidgetResizable(true);
    listScroll->setFrameShape(QFrame::NoFrame);
    auto *listContainer = new QWidget(listScroll);
    m_shareListLayout = new QVBoxLayout(listContainer);
    m_shareListLayout->setContentsMargins(0, 0, 0, 0);
    m_shareListLayout->setSpacing(sp(12));
    listScroll->setWidget(listContainer);
    shareLayout->addWidget(listScroll, 1);

    auto *tipBar = new QFrame(m_sharePage);
    tipBar->setObjectName(QStringLiteral("TipBar"));
    auto *tipLayout = new QHBoxLayout(tipBar);
    tipLayout->setContentsMargins(sp(14), sp(10), sp(14), sp(10));
    tipLayout->setSpacing(sp(10));

    auto *tipIcon = new QLabel(tipBar);
    tipIcon->setObjectName(QStringLiteral("TipIcon"));
    tipIcon->setFixedSize(sp(24), sp(24));
    const QPixmap tipPixmap(QStringLiteral(":/tip_info.png"));
    tipIcon->setPixmap(tipPixmap.scaled(sp(24), sp(24), Qt::KeepAspectRatio, Qt::SmoothTransformation));
    tipIcon->setAlignment(Qt::AlignCenter);

    m_shareTipLabel = new QLabel(tx(QStringLiteral("小提示：啟用聊天室後，使用者可以在首頁點擊「聊天室」進入聊天介面，即可開始交流！"),
                                      QStringLiteral("Tip: After enabling chat, visitors can open Chatroom from the home page and start chatting.")),
                                  tipBar);
    m_shareTipLabel->setObjectName(QStringLiteral("ShareTip"));
    m_shareTipLabel->setWordWrap(true);

    tipLayout->addWidget(tipIcon, 0, Qt::AlignVCenter);
    tipLayout->addWidget(m_shareTipLabel, 1, Qt::AlignVCenter);
    shareLayout->addWidget(tipBar);

    m_pages->addWidget(makeScrollablePage(m_sharePage, m_pages));

    auto *systemPage = new QWidget(m_pages);
    auto *systemLayout = new QVBoxLayout(systemPage);
    systemLayout->setContentsMargins(sp(34), sp(28), sp(34), sp(28));
    systemLayout->setSpacing(sp(20));

    auto *systemCard = new QFrame(systemPage);
    systemCard->setObjectName(QStringLiteral("InfoCard"));
    applySoftShadow(systemCard, 24, 7);
    auto *systemCardLayout = new QVBoxLayout(systemCard);
    systemCardLayout->setContentsMargins(sp(28), sp(26), sp(28), sp(28));
    systemCardLayout->setSpacing(sp(22));
    m_lblSystemTitle = new QLabel(tx(QStringLiteral("系統設定"), QStringLiteral("System Settings")), systemCard);
    m_lblSystemTitle->setObjectName(QStringLiteral("SectionTitle"));
    systemCardLayout->addWidget(m_lblSystemTitle);

    auto *logoRow = new QHBoxLayout();
    m_logoPreview = new QLabel(systemCard);
    m_logoPreview->setFixedSize(sp(112), sp(112));
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
    logoButtonsLayout->setSpacing(sp(10));
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
    form->setHorizontalSpacing(sp(20));
    form->setVerticalSpacing(sp(18));

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
    m_startServerOnLaunchCheck = new QCheckBox(tx(QStringLiteral("啟動程式時自動啟用伺服器"), QStringLiteral("Automatically start server when the program opens")), systemCard);
    connect(m_launchOnStartupCheck, &QCheckBox::toggled, this, [this](bool checked) {
        if (!m_startServerOnLaunchCheck) {
            return;
        }
        if (checked) {
            m_startServerOnLaunchCheck->setChecked(true);
            m_startServerOnLaunchCheck->setEnabled(false);
        } else {
            m_startServerOnLaunchCheck->setEnabled(true);
        }
    });

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
    form->addRow(QString(), m_startServerOnLaunchCheck);
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
    aboutLayout->setContentsMargins(sp(34), sp(28), sp(34), sp(28));
    auto *aboutCard = new QFrame(aboutPage);
    aboutCard->setObjectName(QStringLiteral("InfoCard"));
    applySoftShadow(aboutCard, 24, 7);
    auto *aboutCardLayout = new QVBoxLayout(aboutCard);
    aboutCardLayout->setContentsMargins(sp(34), sp(30), sp(34), sp(30));
    aboutCardLayout->setSpacing(sp(18));
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
    refreshActiveTransfers();
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

void MainWindow::refreshActiveTransfers()
{
    if (!m_activeTransfersLayout) {
        return;
    }

    const QList<ActiveTransferInfo> transfers = m_controller->activeTransfers();
    QSet<QString> activeIds;
    activeIds.reserve(transfers.size());
    for (const ActiveTransferInfo &transfer : transfers) {
        activeIds.insert(transfer.id);
    }

    // Remove only rows that actually finished.  The previous implementation
    // deleted and rebuilt every widget twice to four times per second, which
    // caused visible stutter during a fast download.
    const QStringList existingIds = m_activeTransferRows.keys();
    for (const QString &id : existingIds) {
        if (activeIds.contains(id)) {
            continue;
        }
        ActiveTransferRowWidgets widgets = m_activeTransferRows.take(id);
        if (widgets.row) {
            m_activeTransfersLayout->removeWidget(widgets.row);
            widgets.row->deleteLater();
        }
    }

    if (transfers.isEmpty()) {
        if (!m_activeTransfersEmptyLabel) {
            m_activeTransfersEmptyLabel = new QLabel(m_activeTransfersContainer);
            m_activeTransfersEmptyLabel->setObjectName(QStringLiteral("EmptyState"));
            m_activeTransfersEmptyLabel->setWordWrap(true);
            m_activeTransfersLayout->addWidget(m_activeTransfersEmptyLabel);
        }
        m_activeTransfersEmptyLabel->setText(tx(QStringLiteral("目前沒有正在下載或打包的檔案。"),
                                                QStringLiteral("No files are currently downloading or being packaged.")));
        m_activeTransfersEmptyLabel->show();
        return;
    }

    if (m_activeTransfersEmptyLabel) {
        m_activeTransfersEmptyLabel->hide();
    }

    for (const ActiveTransferInfo &transfer : transfers) {
        ActiveTransferRowWidgets widgets = m_activeTransferRows.value(transfer.id);
        if (!widgets.row) {
            widgets.row = new QFrame(m_activeTransfersContainer);
            widgets.row->setObjectName(QStringLiteral("TransferRow"));
            auto *layout = new QVBoxLayout(widgets.row);
            layout->setContentsMargins(sp(14), sp(12), sp(14), sp(12));
            layout->setSpacing(sp(7));

            auto *top = new QHBoxLayout();
            widgets.name = new QLabel(widgets.row);
            widgets.name->setObjectName(QStringLiteral("TransferName"));
            widgets.name->setWordWrap(true);
            widgets.status = new QLabel(widgets.row);
            widgets.status->setObjectName(QStringLiteral("TransferStatus"));
            top->addWidget(widgets.name, 1);
            top->addWidget(widgets.status, 0, Qt::AlignTop);

            widgets.progress = new QProgressBar(widgets.row);
            widgets.progress->setRange(0, 1000);
            widgets.progress->setTextVisible(true);

            widgets.detail = new QLabel(widgets.row);
            widgets.detail->setObjectName(QStringLiteral("TransferDetail"));
            widgets.detail->setWordWrap(true);

            layout->addLayout(top);
            layout->addWidget(widgets.progress);
            layout->addWidget(widgets.detail);
            m_activeTransfersLayout->addWidget(widgets.row);
            m_activeTransferRows.insert(transfer.id, widgets);
        }

        const QString statusText = transfer.status == QStringLiteral("packaging")
                                       ? tx(QStringLiteral("打包中"), QStringLiteral("Packaging"))
                                       : tx(QStringLiteral("下載中"), QStringLiteral("Downloading"));
        const qint64 processed = qBound<qint64>(0, transfer.bytesProcessed, qMax<qint64>(0, transfer.totalBytes));
        const int progressValue = transfer.totalBytes > 0
                                      ? qBound(0, static_cast<int>((processed * 1000) / transfer.totalBytes), 1000)
                                      : 0;
        const QString progressText = progressValue >= 1000
                                         ? QStringLiteral("100%")
                                         : QString::number(progressValue / 10.0, 'f', 1) + QLatin1Char('%');
        const QString clientText = transfer.clientAddress.trimmed().isEmpty()
                                       ? QString()
                                       : tx(QStringLiteral("用戶：%1  ・  "), QStringLiteral("Client: %1  ·  ")).arg(transfer.clientAddress);
        const QString detailText = QStringLiteral("%1%2 / %3")
                                       .arg(clientText,
                                            humanReadableSize(processed),
                                            humanReadableSize(transfer.totalBytes));

        if (widgets.name->text() != transfer.fileName) widgets.name->setText(transfer.fileName);
        if (widgets.status->text() != statusText) widgets.status->setText(statusText);
        if (widgets.progress->value() != progressValue) widgets.progress->setValue(progressValue);
        if (widgets.progress->format() != progressText) widgets.progress->setFormat(progressText);
        if (widgets.detail->text() != detailText) widgets.detail->setText(detailText);
    }
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
    const QString fullStatus = status;
    if (m_controller->settings().externalLinkEnabled && !externalUrl.isEmpty()) {
        status = tx(QStringLiteral("已就緒・點擊連結複製"), QStringLiteral("Ready · click link to copy"));
    }

    const int statusWidth = qMax(sp(120), m_statusText->contentsRect().width());
    m_statusText->setText(m_statusText->fontMetrics().elidedText(status, Qt::ElideRight, statusWidth));
    m_statusText->setToolTip(fullStatus);
    updateStatusVisuals(fullStatus);

    const auto setFooterLink = [this](QLabel *label, const QString &prefixZh, const QString &prefixEn,
                                      const QString &url, bool preferFullLocalAddress = false) {
        const QString prefix = tx(prefixZh, prefixEn);
        int availableWidth = label->contentsRect().width();
        if (availableWidth < sp(100)) {
            availableWidth = sp(220);
        }

        const QFontMetrics metrics(label->font());
        const int urlWidth = qMax(sp(70), availableWidth - metrics.horizontalAdvance(prefix) - sp(8));
        QString displayCandidate = url;

        // The LAN address is more useful when the complete IP is visible than when
        // the scheme consumes space. Keep the real href untouched so clicking still
        // copies the complete URL (including http:// and port).
        if (preferFullLocalAddress && metrics.horizontalAdvance(displayCandidate) > urlWidth) {
            const QUrl parsed(url);
            QString host = parsed.host();
            if (!host.isEmpty()) {
                if (host.contains(QLatin1Char(':')) && !host.startsWith(QLatin1Char('['))) {
                    host = QStringLiteral("[%1]").arg(host);
                }
                const int port = parsed.port(-1);
                if (port > 0) {
                    host += QStringLiteral(":%1").arg(port);
                }
                displayCandidate = host;
            }
        }

        const QString displayUrl = metrics.elidedText(displayCandidate, Qt::ElideRight, urlWidth);
        label->setText(QStringLiteral("%1<a href='%2'>%3</a>")
                           .arg(prefix.toHtmlEscaped(), url.toHtmlEscaped(), displayUrl.toHtmlEscaped()));
        label->setToolTip(url);
    };

    const QString localUrl = m_controller->localBaseUrl();
    setFooterLink(m_localUrlLabel, QStringLiteral("內網："), QStringLiteral("Local: "), localUrl, true);

    if (externalUrl.isEmpty()) {
        m_externalUrlLabel->clear();
        m_externalUrlLabel->setToolTip(QString());
    } else {
        setFooterLink(m_externalUrlLabel, QStringLiteral("代理："), QStringLiteral("Proxy: "), externalUrl, false);
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
    if (m_chatEnabledCheck) {
        const QSignalBlocker blocker2(m_chatEnabledCheck);
        m_chatEnabledCheck->setChecked(settings.chatEnabled);
    }
    if (m_btnChat) {
        m_btnChat->setEnabled(settings.chatEnabled);
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
        const QSignalBlocker blocker9(m_startServerOnLaunchCheck);

        m_siteNameEdit->setText(settings.siteName);
        m_portSpin->setValue(settings.port);
        m_languageCombo->setCurrentText(settings.language);
        m_themeCombo->setCurrentText(settings.theme);
        m_passwordEdit->setText(settings.download.password);
        m_resumeCheck->setChecked(settings.download.resumeEnabled);
        m_closeToTrayCheck->setChecked(settings.minimizeToTrayOnClose);
        m_launchOnStartupCheck->setChecked(settings.launchOnStartup);
        m_startServerOnLaunchCheck->setChecked(settings.launchOnStartup ? true : settings.startServerOnLaunch);
        m_closeToTrayCheck->setEnabled(trayAvailable());
        m_launchOnStartupCheck->setEnabled(trayAvailable());
        m_startServerOnLaunchCheck->setEnabled(!settings.launchOnStartup);
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
    m_aboutLabel->setText(scaleStyleSheetPixels(tx(
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
            "版本：v1.2.2.0"
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
            "Version: v1.2.2.0"
            "</div>"
            "<div style='margin-top:24px; font-size:18px; line-height:1.9;'>"
            "License Notice:<br>"
            "This project's source code is provided for reference, learning, and personal use only.<br>"
            "Commercial use, redistribution, repackaging, or derivative work is strictly prohibited without prior written consent from the author."
            "</div>"
            "<div style='margin-top:24px; font-size:18px; line-height:1.9;'>"
            "This is an independent tool and is not an official Cloudflare product."
            "</div>")
    ), g_uiScale));
}

void MainWindow::refreshStartStopButton()
{
    if (m_openWebButton) {
        m_openWebButton->setText(tx(QStringLiteral("開啟連結網頁"), QStringLiteral("Open Web Page")));
        m_openWebButton->setEnabled(m_controller->isServerRunning() && !m_isStoppingServer);
        m_openWebButton->setCursor(m_openWebButton->isEnabled() ? Qt::PointingHandCursor : Qt::ArrowCursor);
        style()->unpolish(m_openWebButton);
        style()->polish(m_openWebButton);
    }

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
        m_statusDot->setStyleSheet(scaleStyleSheetPixels(QStringLiteral("QLabel#StatusDot{background:%1;border:0;border-radius:5px;}").arg(dotColor), g_uiScale));
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

    m_shareExternalInfoLabel->setStyleSheet(scaleStyleSheetPixels(style, g_uiScale));
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

void MainWindow::showShareEditor(const ShareItem &share)
{
    if (!isDirectoryShare(share)) {
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle(tx(QStringLiteral("修改資料夾分享名稱"), QStringLiteral("Rename Folder Share")));
    dialog.setMinimumWidth(sp(520));

    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(sp(24), sp(22), sp(24), sp(20));
    layout->setSpacing(sp(14));

    auto *pathLabel = new QLabel(tx(QStringLiteral("實際路徑"), QStringLiteral("Folder Path")), &dialog);
    pathLabel->setObjectName(QStringLiteral("FieldLabel"));
    auto *pathEdit = new QLineEdit(&dialog);
    pathEdit->setReadOnly(true);
    pathEdit->setText(share.sourcePath.isEmpty() ? share.storagePath : share.sourcePath);

    auto *nameLabel = new QLabel(tx(QStringLiteral("分享名稱"), QStringLiteral("Share Name")), &dialog);
    nameLabel->setObjectName(QStringLiteral("FieldLabel"));
    auto *nameEdit = new QLineEdit(&dialog);
    nameEdit->setText(share.name);
    nameEdit->selectAll();

    auto *hint = new QLabel(tx(QStringLiteral("修改後，連線用戶在網頁上會看到新的資料夾名稱；實際資料夾路徑不會變更。"),
                                  QStringLiteral("Visitors will see the new folder name on the web page. The actual folder path will not change.")),
                             &dialog);
    hint->setWordWrap(true);
    hint->setObjectName(QStringLiteral("ShareEditorHint"));

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(tx(QStringLiteral("確認"), QStringLiteral("OK")));
    buttons->button(QDialogButtonBox::Cancel)->setText(tx(QStringLiteral("取消"), QStringLiteral("Cancel")));
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    layout->addWidget(pathLabel);
    layout->addWidget(pathEdit);
    layout->addWidget(nameLabel);
    layout->addWidget(nameEdit);
    layout->addWidget(hint);
    layout->addWidget(buttons);

    nameEdit->setFocus();
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    const QString newName = nameEdit->text().trimmed();
    if (newName.isEmpty()) {
        QMessageBox::warning(this,
                             tx(QStringLiteral("分享名稱"), QStringLiteral("Share Name")),
                             tx(QStringLiteral("分享名稱不可空白。"), QStringLiteral("The share name cannot be empty.")));
        return;
    }
    m_controller->setShareName(share.id, newName);
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
        cardLayout->setContentsMargins(sp(16), sp(12), sp(16), sp(12));
        cardLayout->setSpacing(sp(12));

        auto *iconButton = new QPushButton(card);
        iconButton->setObjectName(QStringLiteral("ShareIconButton"));
        iconButton->setFixedSize(sp(52), sp(52));
        iconButton->setIcon(QIcon(buildShareIcon(share)));
        iconButton->setIconSize(QSize(sp(48), sp(48)));
        iconButton->setFlat(true);
        iconButton->setCursor(Qt::ArrowCursor);
        iconButton->setAttribute(Qt::WA_TransparentForMouseEvents, true);
        iconButton->setFocusPolicy(Qt::NoFocus);

        QWidget *nameArea = nullptr;
        if (isDirectoryShare(share)) {
            auto *nameButton = new QPushButton(card);
            nameButton->setObjectName(QStringLiteral("ShareNameButton"));
            nameButton->setFlat(true);
            nameButton->setCursor(Qt::PointingHandCursor);
            nameButton->setFocusPolicy(Qt::StrongFocus);
            nameButton->setToolTip(tx(QStringLiteral("點擊查看路徑及修改分享名稱"),
                                      QStringLiteral("Click to view the path and rename the share")));
            connect(nameButton, &QPushButton::clicked, this, [this, share]() { showShareEditor(share); });
            nameArea = nameButton;
        } else {
            nameArea = new QWidget(card);
            nameArea->setObjectName(QStringLiteral("ShareNameArea"));
        }
        nameArea->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

        auto *nameLayout = new QVBoxLayout(nameArea);
        nameLayout->setContentsMargins(sp(8), sp(4), sp(8), sp(4));
        nameLayout->setSpacing(sp(3));

        auto *nameLabel = new QLabel(share.name, nameArea);
        nameLabel->setObjectName(QStringLiteral("ShareTitle"));
        nameLabel->setWordWrap(false);
        nameLabel->setToolTip(share.sourcePath.isEmpty() ? share.storagePath : share.sourcePath);
        nameLabel->setAttribute(Qt::WA_TransparentForMouseEvents, true);
        nameLayout->addWidget(nameLabel);

        if (isDirectoryShare(share)) {
            auto *renameHint = new QLabel(tx(QStringLiteral("點擊後可更改分享名稱"),
                                             QStringLiteral("Click to change the share name")),
                                          nameArea);
            renameHint->setObjectName(QStringLiteral("ShareRenameHint"));
            renameHint->setAttribute(Qt::WA_TransparentForMouseEvents, true);
            nameLayout->addWidget(renameHint);
        }

        auto *typeLabel = new QLabel(shareTypeLabel(share), card);
        typeLabel->setObjectName(QStringLiteral("ShareTypeBadge"));
        typeLabel->setProperty("shareKind", isDirectoryShare(share) ? QStringLiteral("folder") : QStringLiteral("file"));
        typeLabel->setAlignment(Qt::AlignCenter);
        typeLabel->setMinimumWidth(sp(104));
        typeLabel->setMinimumHeight(sp(34));

        auto *removeButton = new QPushButton(tx(QStringLiteral("關閉"), QStringLiteral("Close")), card);
        removeButton->setObjectName(QStringLiteral("DangerButton"));
        removeButton->setMinimumHeight(sp(34));

        connect(removeButton, &QPushButton::clicked, this, [this, share]() {
            const auto reply = QMessageBox::question(this,
                                                     tx(QStringLiteral("關閉分享"), QStringLiteral("Close Share")),
                                                     tx(QStringLiteral("確定要關閉「%1」這個分享嗎？"), QStringLiteral("Are you sure you want to close the share \"%1\"?")).arg(share.name));
            if (reply == QMessageBox::Yes) {
                m_controller->removeShare(share.id);
            }
        });

        cardLayout->addWidget(iconButton);
        cardLayout->addWidget(nameArea, 1);

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
    updatedSettings.startServerOnLaunch = updatedSettings.launchOnStartup
                                              ? true
                                              : m_startServerOnLaunchCheck->isChecked();
    updatedSettings.download.totalLimitValue = 0;
    updatedSettings.download.totalLimitUnit = QStringLiteral("KB/s");
    updatedSettings.download.perIpLimitEnabled = false;
    updatedSettings.download.perIpLimitValue = 0;
    updatedSettings.download.perIpLimitUnit = QStringLiteral("KB/s");

    const QSize storedSize = isMaximized() ? normalGeometry().size() : size();
    updatedSettings.windowWidth = qMax(900, qRound(storedSize.width() / g_uiScale));
    updatedSettings.windowHeight = qMax(600, qRound(storedSize.height() / g_uiScale));
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
                                    "QPushButton#OpenWebButton{background:#1b2231;color:#d7deef;border:1px solid #303a50;}"
                                    "QPushButton#OpenWebButton:hover{background:#232c3e;border-color:#45516d;}"
                                    "QPushButton#OpenWebButton:disabled{background:#252c3a;color:#697386;border:1px solid #303746;}"
                                    "QPushButton#DangerButton,QPushButton#StopButton{background:#3a2430;color:#ff9aa5;border:1px solid #57323f;}"
                                    "QPushButton#DangerButton:hover,QPushButton#StopButton:hover{background:#4a2936;}"
                                    "QFrame#SidebarFooter,QFrame#InfoCard,QFrame#ShareCard,QFrame#ShareOptions,QFrame#StatCard{background:#171e2d;border:1px solid #2a3449;border-radius:18px;}"
                                    "QFrame#TransferRow{background:#121927;border:1px solid #303a50;border-radius:14px;}"
                                    "QLabel#TransferName{background:transparent;color:#eef2f9;font-weight:800;} QLabel#TransferStatus{background:#27233e;color:#b6a9ff;border-radius:9px;padding:4px 9px;font-weight:800;} QLabel#TransferDetail{background:transparent;color:#8f9bb0;}"
                                    "QProgressBar{min-height:13px;background:#242d40;border:0;border-radius:6px;text-align:center;color:#ffffff;font-size:11px;font-weight:800;} QProgressBar::chunk{background:qlineargradient(x1:0,y1:0,x2:1,y2:0,stop:0 #7657e8,stop:1 #4f6df5);border-radius:6px;}"
                                    "QFrame#DropArea{background:#182238;border:2px dashed #8ea0cf;border-radius:18px;}"
                                    "QFrame#TipBar{background:#182238;border:1px solid #34415f;border-radius:12px;}"
                                    "QLabel#StatusDot{background:#94a3b8;border-radius:5px;}"
                                    "QLabel#FooterLink{color:#a9b3c6;} QLabel#FooterLink a{color:#9a8cff;} QLabel#FooterLocalLink{color:#a9b3c6;font-size:12px;} QLabel#FooterLocalLink a{color:#a99cff;}"
                                    "QFrame#SidebarFooter QLabel,QFrame#TipBar QLabel{background:transparent;border:0;}"
                                    "QLabel#FooterMeta{color:#8995aa;font-size:13px;}"
                                    "QLineEdit,QSpinBox,QComboBox{background:#121927;color:#ecf1fb;border:1px solid #303a50;border-radius:11px;padding:10px 12px;selection-background-color:#6d5ce7;}"
                                    "QLineEdit:focus,QSpinBox:focus,QComboBox:focus{border:1px solid #7667e8;}"
                                    "QScrollArea{background:transparent;border:0;} QScrollArea>QWidget>QWidget{background:transparent;}"
                                    "QCheckBox{spacing:9px;color:#c8d0e0;}"
                                    "QCheckBox::indicator{width:18px;height:18px;border:1px solid #46526b;border-radius:5px;background:#121927;}"
                                    "QCheckBox::indicator:checked{background:transparent;border:0;border-image:url(:/checkbox_checked.png) 0 0 0 0 stretch stretch;}"
                                    "#PageTitle{font-size:30px;font-weight:800;color:#f4f7fb;}"
                                    "#PageSubtitle{font-size:14px;color:#8e99ae;}"
                                    "#CardTitle{font-size:14px;color:#97a3b7;font-weight:600;}"
                                    "#CardValue{font-size:34px;color:#f5f7fb;font-weight:800;}"
                                    "#SectionTitle{font-size:23px;font-weight:800;color:#eef2f9;background:transparent;border:0;padding:2px 0;}"
                                    "#DropTitle{font-size:18px;font-weight:800;color:#e1e9ff;}"
                                    "#DropHint{font-size:13px;color:#9daccc;}"
                                    "#ShareTip{font-size:12px;color:#b9c7df;background:transparent;}"
                                    "QPushButton#ShareIconButton{background:transparent;border:0;padding:0;}"
                                    "QWidget#ShareNameArea{background:transparent;border:0;}"
                                    "QPushButton#ShareNameButton{background:transparent;border:0;border-radius:10px;padding:0;text-align:left;}"
                                    "QPushButton#ShareNameButton:hover{background:#20283a;}"
                                    "QPushButton#ShareNameButton:focus{border:1px solid #3a465f;}"
                                    "#ShareTitle{font-size:16px;font-weight:700;color:#edf1f8;background:transparent;border:0;}"
                                    "#ShareRenameHint{font-size:12px;color:#8f9bb0;background:transparent;border:0;font-weight:500;}"
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
                                    "QPushButton#OpenWebButton{background:#ffffff;color:#53627a;border:1px solid #dfe4ec;}"
                                    "QPushButton#OpenWebButton:hover{background:#f8f7ff;color:#5e50d9;border-color:#cfc7ff;}"
                                    "QPushButton#OpenWebButton:disabled{background:#eef1f5;color:#a7afbd;border:1px solid #e2e6ec;}"
                                    "QPushButton#DangerButton,QPushButton#StopButton{background:#fff0f2;color:#d94f5c;border:1px solid #ffd4d8;}"
                                    "QPushButton#DangerButton:hover,QPushButton#StopButton:hover{background:#ffe6e9;}"
                                    "QFrame#SidebarFooter,QFrame#InfoCard,QFrame#ShareCard,QFrame#ShareOptions,QFrame#StatCard{background:#ffffff;border:1px solid #e6eaf1;border-radius:18px;}"
                                    "QFrame#TransferRow{background:#f8faff;border:1px solid #e0e7f2;border-radius:14px;}"
                                    "QLabel#TransferName{background:transparent;color:#1f2d44;font-weight:800;} QLabel#TransferStatus{background:#f0edff;color:#6654e8;border-radius:9px;padding:4px 9px;font-weight:800;} QLabel#TransferDetail{background:transparent;color:#718096;}"
                                    "QProgressBar{min-height:13px;background:#e8edf5;border:0;border-radius:6px;text-align:center;color:#25344d;font-size:11px;font-weight:800;} QProgressBar::chunk{background:qlineargradient(x1:0,y1:0,x2:1,y2:0,stop:0 #7657e8,stop:1 #4f6df5);border-radius:6px;}"
                                    "QFrame#DropArea{background:#f6f7ff;border:2px dashed #8e97e8;border-radius:18px;}"
                                    "QFrame#TipBar{background:#f4f7ff;border:1px solid #d8e2ff;border-radius:12px;}"
                                    "QLabel#StatusDot{background:#94a3b8;border-radius:5px;}"
                                    "QLabel#FooterLink{color:#6f7d91;} QLabel#FooterLink a{color:#6255e7;} QLabel#FooterLocalLink{color:#6f7d91;font-size:12px;} QLabel#FooterLocalLink a{color:#5a4ee0;}"
                                    "QFrame#SidebarFooter QLabel,QFrame#TipBar QLabel{background:transparent;border:0;}"
                                    "QLabel#FooterMeta{color:#7c899d;font-size:13px;}"
                                    "QLineEdit,QSpinBox,QComboBox{background:#ffffff;color:#1f2b40;border:1px solid #dfe4ec;border-radius:11px;padding:10px 12px;selection-background-color:#6d5ce7;}"
                                    "QLineEdit:focus,QSpinBox:focus,QComboBox:focus{border:1px solid #7566e7;}"
                                    "QScrollArea{background:transparent;border:0;} QScrollArea>QWidget>QWidget{background:transparent;}"
                                    "QCheckBox{spacing:9px;color:#4e5d73;}"
                                    "QCheckBox::indicator{width:18px;height:18px;border:1px solid #cbd2df;border-radius:5px;background:#ffffff;}"
                                    "QCheckBox::indicator:checked{background:transparent;border:0;border-image:url(:/checkbox_checked.png) 0 0 0 0 stretch stretch;}"
                                    "#PageTitle{font-size:30px;font-weight:800;color:#17243a;}"
                                    "#PageSubtitle{font-size:14px;color:#7d899d;}"
                                    "#CardTitle{font-size:14px;color:#748096;font-weight:600;}"
                                    "#CardValue{font-size:34px;color:#17243a;font-weight:800;}"
                                    "#SectionTitle{font-size:23px;font-weight:800;color:#1b2940;background:transparent;border:0;padding:2px 0;}"
                                    "#DropTitle{font-size:18px;font-weight:800;color:#243c74;}"
                                    "#DropHint{font-size:13px;color:#7d899d;}"
                                    "#ShareTip{font-size:12px;color:#64748b;background:transparent;}"
                                    "QPushButton#ShareIconButton{background:transparent;border:0;padding:0;}"
                                    "QWidget#ShareNameArea{background:transparent;border:0;}"
                                    "QPushButton#ShareNameButton{background:transparent;border:0;border-radius:10px;padding:0;text-align:left;}"
                                    "QPushButton#ShareNameButton:hover{background:#f2f5fa;}"
                                    "QPushButton#ShareNameButton:focus{border:1px solid #d7dee9;}"
                                    "#ShareTitle{font-size:16px;font-weight:700;color:#1f2d44;background:transparent;border:0;}"
                                    "#ShareRenameHint{font-size:12px;color:#7b879b;background:transparent;border:0;font-weight:500;}"
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

    qApp->setStyleSheet(scaleStyleSheetPixels(style, g_uiScale));
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
    if (m_btnChat) m_btnChat->setText(tx(QStringLiteral("聊天室"), QStringLiteral("Chatroom")));
    if (m_btnSettings) m_btnSettings->setText(tx(QStringLiteral("系統設定"), QStringLiteral("Settings")));
    if (m_btnAbout) m_btnAbout->setText(tx(QStringLiteral("關於"), QStringLiteral("About")));
    if (m_brandSubtitle) m_brandSubtitle->setText(tx(QStringLiteral("檔案分享伺服器"), QStringLiteral("File Sharing Server")));
    if (m_pageSubtitle) m_pageSubtitle->setText(tx(QStringLiteral("輕鬆分享・快速安全・隨時存取"), QStringLiteral("Simple sharing · Fast and secure · Access anywhere")));

    if (m_lblTotalDownloadsTitle) m_lblTotalDownloadsTitle->setText(tx(QStringLiteral("總下載次數"), QStringLiteral("Total Downloads")));
    if (m_lblTotalBytesTitle) m_lblTotalBytesTitle->setText(tx(QStringLiteral("總下載流量"), QStringLiteral("Total Data Served")));
    if (m_lblActiveConnectionsTitle) m_lblActiveConnectionsTitle->setText(tx(QStringLiteral("目前連線"), QStringLiteral("Active Connections")));
    if (m_lblLiveSpeedTitle) m_lblLiveSpeedTitle->setText(tx(QStringLiteral("即時傳輸速率"), QStringLiteral("Current Speed")));

    if (m_lblServerInfoTitle) m_lblServerInfoTitle->setText(tx(QStringLiteral("伺服器運作資訊"), QStringLiteral("Server Information")));
    if (m_lblActiveTransfersTitle) m_lblActiveTransfersTitle->setText(tx(QStringLiteral("目前檔案下載與打包進度"), QStringLiteral("Current Downloads and Packaging")));

    if (m_btnSaveShares) m_btnSaveShares->setText(tx(QStringLiteral("儲存分享"), QStringLiteral("Save Shares")));
    if (m_btnAddShare) m_btnAddShare->setText(tx(QStringLiteral("新增分享"), QStringLiteral("Add Share")));
    if (m_shareExternalLinkCheck) m_shareExternalLinkCheck->setText(tx(QStringLiteral("啟用外部連結"), QStringLiteral("Enable External Link")));
    if (m_chatEnabledCheck) m_chatEnabledCheck->setText(tx(QStringLiteral("啟用聊天室功能"), QStringLiteral("Enable Chatroom")));
    if (m_dropTitle) m_dropTitle->setText(tx(QStringLiteral("可直接拖曳檔案或資料夾到這裡快速連結"), QStringLiteral("Drag and drop files or folders here to share")));
    if (m_dropSubtitle) m_dropSubtitle->setText(tx(QStringLiteral("請將檔案或資料夾拖曳至此區域，或使用上方「新增分享」按鈕"), QStringLiteral("Drop files or folders here, or use the Add Share button above")));
    if (m_shareTipLabel) m_shareTipLabel->setText(tx(QStringLiteral("小提示：啟用聊天室後，使用者可以在首頁點擊「聊天室」進入聊天介面，即可開始交流！"), QStringLiteral("Tip: After enabling chat, visitors can open Chatroom from the home page and start chatting.")));

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
    if (m_startServerOnLaunchCheck) m_startServerOnLaunchCheck->setText(tx(QStringLiteral("啟動程式時自動啟用伺服器"), QStringLiteral("Automatically start server when the program opens")));
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

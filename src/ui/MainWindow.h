#pragma once

#include <QMainWindow>

class Controller;
class QCloseEvent;
class QLabel;
class QLineEdit;
class QPushButton;
class QStackedWidget;
class QHBoxLayout;
class QVBoxLayout;
class QCheckBox;
class QSpinBox;
class QComboBox;
class QSystemTrayIcon;
class QMenu;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(Controller *controller, QWidget *parent = nullptr);
    [[nodiscard]] bool trayAvailable() const;
    void showMainWindow();

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    enum PageIndex {
        DashboardPage = 0,
        SharesPage,
        SystemSettingsPage,
        AboutPage
    };

    void buildUi();
    void buildSidebar(QHBoxLayout *rootLayout);
    void buildContent(QHBoxLayout *rootLayout);
    void buildPages();
    void switchPage(PageIndex page, const QString &title);
    void refreshAll();
    void refreshStats();
    void refreshActivityLog();
    void refreshFooter();
    void refreshShares();
    void refreshSettingsForms();
    void refreshAbout();
    void refreshHeaderTitle();
    void refreshStartStopButton();
    void updateExternalLinkUiState();
    void updateStatusVisuals(const QString &status);
    void showShareMenu();
    void rebuildShareList();
    void saveSystemSettingsFromForm();
    void restoreWindowSettings();
    void persistWindowSettings();
    void applyTheme();
    void setupTrayIcon();
    void hideToTray();
    void copyTextToClipboard(const QString &text);
    void setStoppingUi(bool stopping);
    [[nodiscard]] bool shouldCloseToTray() const;

    [[nodiscard]] QString tx(const QString &zh, const QString &en) const;
    [[nodiscard]] QString shareTypeLabel(const struct ShareItem &share) const;
    [[nodiscard]] QString translateStatusToEn(const QString &msg) const;
    void retranslateUi();

    Controller *m_controller = nullptr;

    QWidget *m_central = nullptr;
    QStackedWidget *m_pages = nullptr;
    QLabel *m_pageTitle = nullptr;
    QLabel *m_pageSubtitle = nullptr;
    QLabel *m_brandSubtitle = nullptr;
    QLabel *m_statusDot = nullptr;
    QPushButton *m_startStopButton = nullptr;

    QLabel *m_statusText = nullptr;
    QLabel *m_localUrlLabel = nullptr;
    QLabel *m_externalUrlLabel = nullptr;
    QLabel *m_connectionsLabel = nullptr;
    QLabel *m_speedLabel = nullptr;

    // Sidebar buttons
    QPushButton *m_btnDashboard = nullptr;
    QPushButton *m_btnShares = nullptr;
    QPushButton *m_btnChat = nullptr;
    QPushButton *m_btnSettings = nullptr;
    QPushButton *m_btnAbout = nullptr;

    // Stats card labels
    QLabel *m_lblTotalDownloadsTitle = nullptr;
    QLabel *m_lblTotalBytesTitle = nullptr;
    QLabel *m_lblActiveConnectionsTitle = nullptr;
    QLabel *m_lblLiveSpeedTitle = nullptr;
    QLabel *m_totalDownloadsValue = nullptr;
    QLabel *m_totalBytesValue = nullptr;
    QLabel *m_liveSpeedValue = nullptr;
    QLabel *m_activeConnectionsValue = nullptr;
    QLabel *m_serverInfoLabel = nullptr;
    QLabel *m_lblServerInfoTitle = nullptr;

    // Share page controls
    QWidget *m_sharePage = nullptr;
    QVBoxLayout *m_shareListLayout = nullptr;
    QCheckBox *m_shareExternalLinkCheck = nullptr;
    QCheckBox *m_chatEnabledCheck = nullptr;
    QLabel *m_shareExternalInfoLabel = nullptr;
    QPushButton *m_btnSaveShares = nullptr;
    QPushButton *m_btnAddShare = nullptr;
    QLabel *m_dropIcon = nullptr;
    QLabel *m_dropTitle = nullptr;
    QLabel *m_dropSubtitle = nullptr;
    QLabel *m_shareTipLabel = nullptr;

    // Settings page controls
    QLabel *m_lblSystemTitle = nullptr;
    QPushButton *m_btnSelectLogo = nullptr;
    QPushButton *m_btnClearLogo = nullptr;
    QLabel *m_lblSiteName = nullptr;
    QLabel *m_lblPort = nullptr;
    QLabel *m_lblLanguage = nullptr;
    QLabel *m_lblTheme = nullptr;
    QLabel *m_lblPassword = nullptr;
    QPushButton *m_btnSaveSettings = nullptr;

    QLabel *m_logoPreview = nullptr;
    QLineEdit *m_siteNameEdit = nullptr;
    QSpinBox *m_portSpin = nullptr;
    QComboBox *m_languageCombo = nullptr;
    QComboBox *m_themeCombo = nullptr;
    QLineEdit *m_passwordEdit = nullptr;
    QCheckBox *m_resumeCheck = nullptr;
    QCheckBox *m_closeToTrayCheck = nullptr;
    QCheckBox *m_launchOnStartupCheck = nullptr;

    QLabel *m_lblAboutTitle = nullptr;
    QLabel *m_aboutLabel = nullptr;
    QSystemTrayIcon *m_trayIcon = nullptr;
    QMenu *m_trayMenu = nullptr;
    class QAction *m_trayShowAction = nullptr;
    class QAction *m_trayExitAction = nullptr;
    bool m_forceExit = false;
    bool m_isStoppingServer = false;
    bool m_trayNoticeShown = false;
};

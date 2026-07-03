#include "app/Controller.h"

#include <QAbstractSocket>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QNetworkAccessManager>
#include <QNetworkInterface>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

namespace {
constexpr int kExternalSyncIntervalMs = 60 * 1000;
const char *const kShortenerApiUrls[] = {
    "https://is.gd/create.php",
    "https://v.gd/create.php",
};

QString externalProxyConnectingStatus()
{
    return QStringLiteral("代理伺服器連接中");
}

QString externalProxyFailedStatus()
{
    return QStringLiteral("代理伺服器連接失敗");
}

QString externalProxyReadyStatus()
{
    return QStringLiteral("已就緒，點擊下面連結複制網址");
}

QString formatHttpBaseUrl(const QString &host, quint16 port)
{
    if (port == 80) {
        return QStringLiteral("http://%1").arg(host);
    }
    return QStringLiteral("http://%1:%2").arg(host).arg(port);
}

bool isPrivateIpv4(const QHostAddress &address)
{
    const quint32 value = address.toIPv4Address();
    const quint8 a = static_cast<quint8>((value >> 24) & 0xff);
    const quint8 b = static_cast<quint8>((value >> 16) & 0xff);

    if (a == 10) {
        return true;
    }
    if (a == 172 && b >= 16 && b <= 31) {
        return true;
    }
    if (a == 192 && b == 168) {
        return true;
    }
    return false;
}

QString publicTunnelStatus(const QString &message, bool serverRunning)
{
    const QString lower = message.trimmed().toLower();
    if (lower.isEmpty()) {
        return message;
    }

    if (lower.contains(QStringLiteral("cloudflared download failed"))
        || lower.contains(QStringLiteral("cloudflared write failed"))
        || lower.contains(QStringLiteral("cloudflare tunnel start failed"))
        || lower.contains(QStringLiteral("cloudflared.exe was not found"))
        || lower.contains(QStringLiteral("cloudflared was not found"))) {
        return externalProxyFailedStatus();
    }

    if (lower.contains(QStringLiteral("cloudflared installed"))) {
        return QStringLiteral("cloudflared 已安裝");
    }

    if (lower.contains(QStringLiteral("downloading cloudflared"))
        || lower.contains(QStringLiteral("starting cloudflare tunnel"))
        || lower.contains(QStringLiteral("cloudflare tunnel is running"))) {
        return externalProxyConnectingStatus();
    }

    if (lower.contains(QStringLiteral("cloudflare tunnel is stopped"))) {
        return serverRunning ? QStringLiteral("伺服器上線中") : QStringLiteral("伺服器未啟動");
    }

    return message;
}

QString publicTunnelError(const QString &message)
{
    const QString lower = message.trimmed().toLower();
    if (lower.contains(QStringLiteral("cloudflared")) || lower.contains(QStringLiteral("cloudflare"))) {
        return externalProxyFailedStatus();
    }
    return message;
}

QString shortenerHost(const QString &serviceUrl)
{
    return QUrl(serviceUrl).host();
}

bool isTryCloudflareUrl(const QString &url)
{
    const QString host = QUrl(url).host().trimmed().toLower();
    return host.endsWith(QStringLiteral(".trycloudflare.com"));
}
}

Controller::Controller(QObject *parent)
    : QObject(parent)
    , m_store(this)
    , m_tunnelManager(m_store.managedBinRoot(), m_store.runtimeRoot(), this)
    , m_qrCodeGenerator(this)
    , m_externalApi(new QNetworkAccessManager(this))
{
    m_httpServer = new HttpFileServer();
    m_httpServer->moveToThread(&m_httpServerThread);
    connect(&m_httpServerThread, &QThread::finished, m_httpServer, &QObject::deleteLater);
    m_httpServerThread.setObjectName(QStringLiteral("EasyCloudHfsHttpServerThread"));
    m_httpServerThread.start();

    m_externalSyncTimer.setInterval(kExternalSyncIntervalMs);
    m_externalSyncTimer.setSingleShot(false);
    m_settings = m_store.loadSettings();
    m_shares = m_store.loadShares();
    m_downloads = m_store.loadDownloads();
    bool settingsUpdated = false;
    if (m_settings.instanceId.trimmed().isEmpty()) {
        m_settings.instanceId = createId();
        settingsUpdated = true;
    }
    m_settings.clearSharesOnExit = false;
    if (m_settings.uploadsRoot.isEmpty()) {
        m_settings.uploadsRoot = m_store.managedVirtualRoot();
        settingsUpdated = true;
    }
    if (settingsUpdated) {
        m_store.saveSettings(m_settings);
    }

    connect(m_httpServer, &HttpFileServer::statusMessageChanged, this, &Controller::pushStatus);
    connect(m_httpServer, &HttpFileServer::activityEvent, this, &Controller::appendActivity);
    connect(m_httpServer, &HttpFileServer::downloadRecorded, this, &Controller::appendDownloadRecord);
    connect(m_httpServer,
            &HttpFileServer::statsChanged,
            this,
            [this](quint64 totalDownloads, quint64 totalBytes, int activeConnections, qint64 currentBytesPerSecond) {
                m_stats.totalDownloads = totalDownloads;
                m_stats.totalBytes = totalBytes;
                m_stats.activeConnections = activeConnections;
                m_stats.currentBytesPerSecond = currentBytesPerSecond;
                refreshShareCount();
                emit statsChanged();
            });

    connect(&m_tunnelManager, &TunnelManager::statusChanged, this, [this](const QString &message) {
        const QString translated = publicTunnelStatus(message, isServerRunning());
        if (translated.isEmpty()) {
            return;
        }
        if (m_externalReadyAnnounced && translated == externalProxyConnectingStatus()) {
            return;
        }
        pushStatus(translated);
        emit tunnelStateChanged();
    });
    connect(&m_tunnelManager,
            &TunnelManager::errorOccurred,
            this,
            [this](const QString &message) {
                pushStatus(publicTunnelError(message));
                if (m_externalTunnelRequested
                    && isServerRunning()
                    && m_tunnelManager.isInstalled()
                    && !m_tunnelManager.isRunning()) {
                    appendActivity(QStringLiteral("代理伺服器連接中，正在重新嘗試連線"));
                    m_tunnelManager.startQuickTunnel(m_settings.port);
                }
            });
    connect(&m_tunnelManager, &TunnelManager::logsChanged, this, &Controller::tunnelLogsChanged);
    connect(&m_tunnelManager, &TunnelManager::logsChanged, this, [this]() { emit tunnelStateChanged(); });
    connect(&m_tunnelManager,
            &TunnelManager::publicUrlChanged,
            this,
            [this](const QString &url) {
                emit urlsChanged();
                emit tunnelStateChanged();
                const QString targetUrl = url.trimmed();
                if (targetUrl.isEmpty()) {
                    m_externalReadyAnnounced = false;
                    clearExternalShortLink();
                } else {
                    appendActivity(QStringLiteral("代理伺服器連接完成"));
                    if (m_externalShortUrlTarget != targetUrl) {
                        clearExternalShortLink();
                    }
                    if (hasExternalSharesEnabled()) {
                        refreshExternalShortLink();
                    }
                }
            });
    connect(&m_tunnelManager,
            &TunnelManager::runningChanged,
            this,
            [this]() {
                emit tunnelStateChanged();
                emit urlsChanged();
            });
    connect(&m_tunnelManager,
            &TunnelManager::executablePathChanged,
            this,
            [this](const QString &path) {
                if (path != m_settings.cloudflaredPath) {
                    m_settings.cloudflaredPath = path;
                    m_store.saveSettings(m_settings);
                    emit settingsChanged();
                }
                emit tunnelStateChanged();

                if (m_externalTunnelRequested
                    && isServerRunning()
                    && !path.isEmpty()
                    && !m_tunnelManager.isRunning()) {
                    m_tunnelManager.startQuickTunnel(m_settings.port);
                }
            });
    connect(&m_externalSyncTimer, &QTimer::timeout, this, [this]() {
        if (!isServerRunning() || !hasExternalSharesEnabled()) {
            return;
        }
        refreshExternalShortLink();
    });

    reloadServerConfiguration();
    refreshShareCount();
    m_statusMessage.clear();
    updateExternalSyncTimer();
}

Controller::~Controller()
{
    m_externalSyncTimer.stop();
    clearExternalShortLink();
    if (m_tunnelManager.isRunning()) {
        m_tunnelManager.stop();
    }

    stopHttpServer();
    m_httpServerThread.quit();
    m_httpServerThread.wait();
    m_httpServer = nullptr;
}

const QList<ShareItem> &Controller::shares() const
{
    return m_shares;
}

const QList<DownloadRecord> &Controller::downloads() const
{
    return m_downloads;
}

const AppSettings &Controller::settings() const
{
    return m_settings;
}

ServerStats Controller::stats() const
{
    return m_stats;
}

bool Controller::isServerRunning() const
{
    return httpServerRunning();
}

QString Controller::localBaseUrl() const
{
    return formatHttpBaseUrl(detectLanAddress(), m_settings.port);
}

QString Controller::cloudflareUrl() const
{
    return m_tunnelManager.publicUrl();
}

QString Controller::activeShareBaseUrl() const
{
    const QString external = primaryExternalUrl();
    if (!external.isEmpty()) {
        return external;
    }
    return !cloudflareUrl().isEmpty() ? cloudflareUrl() : localBaseUrl();
}

QString Controller::shareUrl(const ShareItem &share, bool preferPublic) const
{
    if (preferPublic) {
        const QString external = externalShareUrl(share);
        if (!external.isEmpty()) {
            return external;
        }
    }

    const QString base = preferPublic && !cloudflareUrl().isEmpty() ? cloudflareUrl() : localBaseUrl();
    const QString encodedRoute = QString::fromUtf8(QUrl::toPercentEncoding(share.routeSegment));
    return QStringLiteral("%1/%2").arg(base, encodedRoute);
}

QString Controller::externalShareUrl(const ShareItem &share) const
{
    const QString base = cloudflareUrl().trimmed();
    if (base.isEmpty()) {
        return {};
    }

    const QString encodedRoute = QString::fromUtf8(QUrl::toPercentEncoding(share.routeSegment));
    return QStringLiteral("%1/%2").arg(base, encodedRoute);
}

QString Controller::primaryExternalUrl() const
{
    if (!m_settings.externalLinkEnabled || m_externalShortUrl.isEmpty()) {
        return {};
    }
    if (m_externalShortUrlTarget != cloudflareUrl().trimmed()) {
        return {};
    }
    return m_externalShortUrl;
}

QString Controller::statusMessage() const
{
    return m_statusMessage;
}

QStringList Controller::activityLog() const
{
    return m_activityLog;
}

QString Controller::tunnelLogs() const
{
    return m_tunnelManager.logs();
}

bool Controller::isTunnelRunning() const
{
    return m_tunnelManager.isRunning();
}

bool Controller::isCloudflaredInstalled() const
{
    return m_tunnelManager.isInstalled();
}

QString Controller::cloudflaredPath() const
{
    return m_tunnelManager.executablePath();
}

QString Controller::appDataRoot() const
{
    return m_store.appDataRoot();
}

QImage Controller::buildQrCode(const QString &text, int size)
{
    return m_qrCodeGenerator.buildCode(text, size);
}

bool Controller::startServer()
{
    reloadServerConfiguration();
    const bool ok = startHttpServer(m_settings.port);
    emit serverStateChanged(ok);
    emit urlsChanged();
    if (!ok) {
        return false;
    }

    pushStatus(QStringLiteral("伺服器已啟動：%1").arg(localBaseUrl()));
    appendActivity(QStringLiteral("伺服器已啟動：%1").arg(localBaseUrl()));

    if (hasExternalSharesEnabled()) {
        ensureExternalTunnel();
    } else {
        m_externalTunnelRequested = false;
        stopTunnel();
    }
    updateExternalSyncTimer();
    return true;
}

void Controller::stopServer()
{
    m_externalTunnelRequested = false;
    m_externalReadyAnnounced = false;
    stopHttpServer();
    updateExternalSyncTimer();
    stopTunnel();
    clearExternalShortLink();
    emit serverStateChanged(false);
    emit urlsChanged();
    appendActivity(QStringLiteral("伺服器已停止"));
    pushStatus(QStringLiteral("伺服器已停止"));
}

void Controller::toggleServer()
{
    if (isServerRunning()) {
        stopServer();
    } else {
        startServer();
    }
}

void Controller::addPaths(const QStringList &paths)
{
    for (const QString &path : paths) {
        const QFileInfo info(path);
        if (!info.exists()) {
            continue;
        }

        if (info.isDir()) {
            addDirectoryShare(info.absoluteFilePath());
        } else if (info.isFile()) {
            addFileShare(info.absoluteFilePath());
        }
    }
}

void Controller::addFileShare(const QString &filePath)
{
    m_shares.append(makeFileShare(filePath));
    persistAll();
    reloadServerConfiguration();
    refreshShareCount();
    emit sharesChanged();
    emit urlsChanged();
}

void Controller::addDirectoryShare(const QString &directoryPath)
{
    m_shares.append(makeDirectoryShare(directoryPath));
    persistAll();
    reloadServerConfiguration();
    refreshShareCount();
    emit sharesChanged();
    emit urlsChanged();
}

void Controller::addVirtualDirectory(const QString &name)
{
    m_shares.append(makeVirtualDirectoryShare(name));
    persistAll();
    reloadServerConfiguration();
    refreshShareCount();
    emit sharesChanged();
    emit urlsChanged();
}

void Controller::addLinkShare(const QString &name, const QString &url)
{
    m_shares.append(makeLinkShare(name, url));
    persistAll();
    reloadServerConfiguration();
    refreshShareCount();
    emit sharesChanged();
    emit urlsChanged();
}

void Controller::removeShare(const QString &id)
{
    for (qsizetype index = 0; index < m_shares.size(); ++index) {
        if (m_shares.at(index).id != id) {
            continue;
        }

        m_shares.removeAt(index);
        persistAll();
        reloadServerConfiguration();
        refreshShareCount();
        emit sharesChanged();
        emit urlsChanged();
        return;
    }
}

void Controller::setShareEnabled(const QString &id, bool enabled)
{
    for (ShareItem &share : m_shares) {
        if (share.id != id) {
            continue;
        }

        share.enabled = enabled;
        persistAll();
        reloadServerConfiguration();
        emit sharesChanged();
        emit urlsChanged();
        return;
    }
}

void Controller::setShareAllowUpload(const QString &id, bool allowUpload)
{
    for (ShareItem &share : m_shares) {
        if (share.id != id) {
            continue;
        }

        if (share.type != ShareType::Directory && share.type != ShareType::VirtualDirectory) {
            share.allowUpload = false;
        } else {
            share.allowUpload = allowUpload;
        }

        persistAll();
        reloadServerConfiguration();
        emit sharesChanged();
        return;
    }
}

void Controller::setShareAllowDelete(const QString &id, bool allowDelete)
{
    for (ShareItem &share : m_shares) {
        if (share.id != id) {
            continue;
        }

        if (share.type != ShareType::Directory && share.type != ShareType::VirtualDirectory) {
            share.allowDelete = false;
        } else {
            share.allowDelete = allowDelete;
        }

        persistAll();
        reloadServerConfiguration();
        emit sharesChanged();
        return;
    }
}

void Controller::setShareAllowCreateDirectory(const QString &id, bool allowCreateDirectory)
{
    for (ShareItem &share : m_shares) {
        if (share.id != id) {
            continue;
        }

        if (share.type != ShareType::Directory && share.type != ShareType::VirtualDirectory) {
            share.allowCreateDirectory = false;
        } else {
            share.allowCreateDirectory = allowCreateDirectory;
        }

        persistAll();
        reloadServerConfiguration();
        emit sharesChanged();
        return;
    }
}

void Controller::setExternalLinkSettings(bool enabled)
{
    if (m_settings.externalLinkEnabled == enabled) {
        return;
    }

    m_externalReadyAnnounced = false;
    m_settings.externalLinkEnabled = enabled;
    persistAll();
    emit settingsChanged();
    emit urlsChanged();

    if (!hasExternalSharesEnabled()) {
        m_externalTunnelRequested = false;
        stopTunnel();
        clearExternalShortLink();
        updateExternalSyncTimer();
        return;
    }

    if (isServerRunning()) {
        ensureExternalTunnel();
        refreshExternalShortLink();
    }
    updateExternalSyncTimer();
}

void Controller::saveSharesSnapshot()
{
    const bool sharesSaved = m_store.saveShares(m_shares);
    const bool settingsSaved = m_store.saveSettings(m_settings);
    if (sharesSaved && settingsSaved) {
        pushStatus(QStringLiteral("分享設定已儲存到 shares.ini"));
        appendActivity(QStringLiteral("分享設定已儲存"));
    } else {
        pushStatus(QStringLiteral("分享設定儲存失敗"));
        appendActivity(QStringLiteral("分享設定儲存失敗"));
    }
}

bool Controller::exportShares(const QString &filePath) const
{
    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }

    file.write(QJsonDocument(toJson(m_shares)).toJson(QJsonDocument::Indented));
    return true;
}

bool Controller::importShares(const QString &filePath)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }

    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    if (!document.isArray()) {
        return false;
    }

    QList<ShareItem> imported = shareListFromJson(document.array());
    QList<ShareItem> existing = m_shares;

    for (ShareItem &item : imported) {
        if (item.id.isEmpty()) {
            item.id = createId();
        }
        if (item.type == ShareType::VirtualDirectory && item.storagePath.isEmpty()) {
            item.storagePath = QDir(m_store.managedVirtualRoot()).filePath(item.id);
            QDir().mkpath(item.storagePath);
        }
        item.routeSegment = ensureUniqueRoute(item.routeSegment.isEmpty() ? slugify(item.name) : item.routeSegment, existing);
        existing.append(item);
    }

    m_shares.append(imported);
    persistAll();
    reloadServerConfiguration();
    refreshShareCount();
    emit sharesChanged();
    emit urlsChanged();
    return true;
}

void Controller::clearDownloads()
{
    m_downloads.clear();
    persistAll();
    emit downloadsChanged();
}

void Controller::saveSystemSettings(const AppSettings &settings)
{
    const bool portChanged = m_settings.port != settings.port;
    m_settings.siteName = settings.siteName.trimmed().isEmpty() ? QStringLiteral("Easy Cloud HFS") : settings.siteName.trimmed();
    m_settings.port = settings.port;
    m_settings.language = settings.language;
    m_settings.theme = settings.theme;
    m_settings.updateFrequency = settings.updateFrequency;
    m_settings.logoPath = settings.logoPath;
    m_settings.externalLinkEnabled = settings.externalLinkEnabled;
    if (m_settings.instanceId.trimmed().isEmpty()) {
        m_settings.instanceId = createId();
    }
    m_settings.instanceId = settings.instanceId.trimmed().isEmpty() ? m_settings.instanceId : settings.instanceId.trimmed();
    m_settings.windowWidth = settings.windowWidth;
    m_settings.windowHeight = settings.windowHeight;
    m_settings.windowMaximized = settings.windowMaximized;
    m_settings.minimizeToTrayOnClose = settings.minimizeToTrayOnClose;
    m_settings.launchOnStartup = settings.launchOnStartup;
    m_settings.clearSharesOnExit = false;
    m_settings.cloudflaredPath = settings.cloudflaredPath;
    m_settings.download = settings.download;
    if (m_settings.uploadsRoot.isEmpty()) {
        m_settings.uploadsRoot = m_store.managedVirtualRoot();
    }

    persistAll();
    reloadServerConfiguration();

    if (portChanged && isServerRunning()) {
        startServer();
    } else if (isServerRunning()) {
        if (hasExternalSharesEnabled()) {
            ensureExternalTunnel();
            refreshExternalShortLink();
        } else {
            m_externalTunnelRequested = false;
            stopTunnel();
            clearExternalShortLink();
        }
    }

    emit settingsChanged();
    emit urlsChanged();
}

void Controller::saveDownloadSettings(const DownloadSettings &settings)
{
    m_settings.download = settings;
    persistAll();
    reloadServerConfiguration();
    emit settingsChanged();
}

void Controller::setCloudflaredPath(const QString &path)
{
    m_settings.cloudflaredPath = path;
    persistAll();
    m_tunnelManager.setExecutableHint(path);
    emit settingsChanged();
    emit tunnelStateChanged();
}

void Controller::downloadCloudflared()
{
    if (!hasExternalSharesEnabled()) {
        pushStatus(QStringLiteral("請先啟用外部連結"));
        return;
    }

    m_tunnelManager.downloadCloudflared();
}

void Controller::startTunnel()
{
    if (!isServerRunning() && !startServer()) {
        return;
    }

    m_externalTunnelRequested = true;
    m_tunnelManager.startQuickTunnel(m_settings.port);
}

void Controller::stopTunnel()
{
    m_tunnelManager.stop();
}

QString Controller::detectLanAddress() const
{
    QString privateWifiAddress;
    QString privateAddress;
    QString fallbackAddress;

    const QList<QNetworkInterface> interfaces = QNetworkInterface::allInterfaces();
    for (const QNetworkInterface &interface : interfaces) {
        const auto flags = interface.flags();
        if (!flags.testFlag(QNetworkInterface::IsUp)
            || !flags.testFlag(QNetworkInterface::IsRunning)
            || flags.testFlag(QNetworkInterface::IsLoopBack)) {
            continue;
        }

        const QString interfaceName = interface.name().toLower();
        const QString humanName = interface.humanReadableName().toLower();
        const bool looksLikeWifi = interfaceName.contains(QStringLiteral("wlan"))
                                   || interfaceName.contains(QStringLiteral("wifi"))
                                   || humanName.contains(QStringLiteral("wlan"))
                                   || humanName.contains(QStringLiteral("wifi"));

        for (const QNetworkAddressEntry &entry : interface.addressEntries()) {
            const QHostAddress address = entry.ip();
            if (address.protocol() != QAbstractSocket::IPv4Protocol || address.isLoopback()) {
                continue;
            }

            const QString addressText = address.toString();
            if (looksLikeWifi && isPrivateIpv4(address)) {
                return addressText;
            }

            if (privateWifiAddress.isEmpty() && looksLikeWifi) {
                privateWifiAddress = addressText;
            }

            if (privateAddress.isEmpty() && isPrivateIpv4(address)) {
                privateAddress = addressText;
            }

            if (fallbackAddress.isEmpty()) {
                fallbackAddress = addressText;
            }
        }
    }

    if (!privateWifiAddress.isEmpty()) {
        return privateWifiAddress;
    }
    if (!privateAddress.isEmpty()) {
        return privateAddress;
    }
    if (!fallbackAddress.isEmpty()) {
        return fallbackAddress;
    }

    return QStringLiteral("127.0.0.1");
}

ShareItem Controller::makeFileShare(const QString &filePath) const
{
    const QFileInfo info(filePath);

    ShareItem item;
    item.id = createId();
    item.type = ShareType::File;
    item.name = info.fileName();
    item.routeSegment = ensureUniqueRoute(slugify(info.completeBaseName().isEmpty() ? info.fileName() : info.completeBaseName()), m_shares);
    item.sourcePath = info.absoluteFilePath();
    item.pinnedSize = info.size();
    return item;
}

ShareItem Controller::makeDirectoryShare(const QString &directoryPath) const
{
    const QFileInfo info(directoryPath);

    ShareItem item;
    item.id = createId();
    item.type = ShareType::Directory;
    item.name = info.fileName().isEmpty() ? info.absoluteFilePath() : info.fileName();
    item.routeSegment = ensureUniqueRoute(slugify(item.name), m_shares);
    item.sourcePath = info.absoluteFilePath();
    item.allowUpload = false;
    item.allowDelete = false;
    item.allowCreateDirectory = false;
    return item;
}

ShareItem Controller::makeVirtualDirectoryShare(const QString &name) const
{
    ShareItem item;
    item.id = createId();
    item.type = ShareType::VirtualDirectory;
    item.name = name.trimmed().isEmpty() ? QStringLiteral("虛擬資料夾") : name.trimmed();
    item.routeSegment = ensureUniqueRoute(slugify(item.name), m_shares);
    item.storagePath = QDir(m_store.managedVirtualRoot()).filePath(item.id);
    item.allowUpload = false;
    item.allowDelete = false;
    item.allowCreateDirectory = false;
    QDir().mkpath(item.storagePath);
    return item;
}

ShareItem Controller::makeLinkShare(const QString &name, const QString &url) const
{
    ShareItem item;
    item.id = createId();
    item.type = ShareType::Link;
    item.name = name.trimmed().isEmpty() ? url.trimmed() : name.trimmed();
    item.routeSegment = ensureUniqueRoute(slugify(item.name), m_shares);
    item.sourcePath = url.trimmed();
    return item;
}

bool Controller::hasExternalSharesEnabled() const
{
    return m_settings.externalLinkEnabled;
}

bool Controller::startHttpServer(quint16 port)
{
    if (!m_httpServer) {
        return false;
    }

    if (m_httpServer->thread() == QThread::currentThread()) {
        return m_httpServer->start(port);
    }

    bool ok = false;
    QMetaObject::invokeMethod(
        m_httpServer,
        [this, port, &ok]() {
            ok = m_httpServer->start(port);
        },
        Qt::BlockingQueuedConnection);
    return ok;
}

void Controller::stopHttpServer()
{
    if (!m_httpServer) {
        return;
    }

    if (m_httpServer->thread() == QThread::currentThread()) {
        m_httpServer->stop();
        return;
    }

    QMetaObject::invokeMethod(
        m_httpServer,
        [this]() {
            m_httpServer->stop();
        },
        Qt::BlockingQueuedConnection);
}

bool Controller::httpServerRunning() const
{
    if (!m_httpServer) {
        return false;
    }

    if (m_httpServer->thread() == QThread::currentThread()) {
        return m_httpServer->isRunning();
    }

    bool running = false;
    QMetaObject::invokeMethod(
        m_httpServer,
        [this, &running]() {
            running = m_httpServer->isRunning();
        },
        Qt::BlockingQueuedConnection);
    return running;
}

void Controller::persistAll()
{
    m_store.saveSettings(m_settings);
    m_store.saveDownloads(m_downloads);
}

void Controller::reloadServerConfiguration()
{
    if (m_httpServer) {
        const QString siteName = m_settings.siteName;
        const QString logoPath = m_settings.logoPath;
        const DownloadSettings downloadSettings = m_settings.download;
        const QList<ShareItem> shares = m_shares;

        if (m_httpServer->thread() == QThread::currentThread()) {
            m_httpServer->setSiteName(siteName);
            m_httpServer->setLogoPath(logoPath);
            m_httpServer->setDownloadSettings(downloadSettings);
            m_httpServer->setShares(shares);
        } else {
            QMetaObject::invokeMethod(
                m_httpServer,
                [this, siteName, logoPath, downloadSettings, shares]() {
                    m_httpServer->setSiteName(siteName);
                    m_httpServer->setLogoPath(logoPath);
                    m_httpServer->setDownloadSettings(downloadSettings);
                    m_httpServer->setShares(shares);
                },
                Qt::BlockingQueuedConnection);
        }
    }

    m_tunnelManager.setExecutableHint(m_settings.cloudflaredPath);
}

void Controller::pushStatus(const QString &message)
{
    if (message.isEmpty()) {
        return;
    }

    m_statusMessage = message;
    emit statusMessageChanged(m_statusMessage);
}

void Controller::appendActivity(const QString &message)
{
    if (message.isEmpty()) {
        return;
    }

    const QString line = QStringLiteral("[%1] %2")
                             .arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss")),
                                  message);
    m_activityLog.prepend(line);
    while (m_activityLog.size() > 120) {
        m_activityLog.removeLast();
    }
    emit activityLogChanged();
}

void Controller::appendDownloadRecord(const DownloadRecord &record)
{
    m_downloads.prepend(record);
    while (m_downloads.size() > 1000) {
        m_downloads.removeLast();
    }
    m_store.saveDownloads(m_downloads);
    emit downloadsChanged();
}

void Controller::refreshShareCount()
{
    m_stats.shareCount = m_shares.size();
}

void Controller::ensureExternalTunnel()
{
    updateExternalSyncTimer();
    if (!hasExternalSharesEnabled()) {
        m_externalTunnelRequested = false;
        stopTunnel();
        clearExternalShortLink();
        return;
    }

    m_externalTunnelRequested = true;
    pushStatus(externalProxyConnectingStatus());

    if (m_tunnelManager.isRunning()) {
        if (!m_tunnelManager.publicUrl().isEmpty()) {
            refreshExternalShortLink();
        }
        return;
    }

    m_tunnelManager.downloadCloudflared();
}

void Controller::updateExternalSyncTimer()
{
    const bool shouldRun = isServerRunning() && hasExternalSharesEnabled();
    if (shouldRun) {
        if (!m_externalSyncTimer.isActive()) {
            m_externalSyncTimer.start();
        }
        return;
    }

    m_externalSyncTimer.stop();
}

void Controller::clearExternalShortLink()
{
    const bool hadValue = !m_externalShortUrl.isEmpty()
                          || !m_externalShortUrlTarget.isEmpty()
                          || !m_externalShortenerName.isEmpty()
                          || m_externalShortLinkPending;
    m_externalShortUrl.clear();
    m_externalShortUrlTarget.clear();
    m_externalShortenerName.clear();
    m_pendingExternalShortUrlTarget.clear();
    m_externalShortLinkPending = false;
    if (hadValue) {
        emit urlsChanged();
    }
}

void Controller::refreshExternalShortLink()
{
    if (!hasExternalSharesEnabled()) {
        clearExternalShortLink();
        return;
    }

    const QString targetUrl = m_tunnelManager.publicUrl().trimmed();
    if (targetUrl.isEmpty()) {
        return;
    }

    if (!m_externalShortUrl.isEmpty() && m_externalShortUrlTarget == targetUrl) {
        if (!m_externalReadyAnnounced) {
            pushStatus(externalProxyReadyStatus());
            m_externalReadyAnnounced = true;
        }
        return;
    }

    if (m_externalShortLinkPending && m_pendingExternalShortUrlTarget == targetUrl) {
        return;
    }

    clearExternalShortLink();
    m_externalReadyAnnounced = false;

    if (isTryCloudflareUrl(targetUrl)) {
        applyExternalShortLink(targetUrl, targetUrl, QStringLiteral("Cloudflare Quick Tunnel"));
        return;
    }

    pushStatus(externalProxyConnectingStatus());
    requestExternalShortLink(targetUrl);
}

void Controller::requestExternalShortLink(const QString &targetUrl)
{
    QStringList serviceUrls;
    serviceUrls.reserve(2);
    for (const char *serviceUrl : kShortenerApiUrls) {
        serviceUrls.append(QString::fromLatin1(serviceUrl));
    }

    if (serviceUrls.size() > 1 && QRandomGenerator::global()->bounded(serviceUrls.size()) == 1) {
        serviceUrls.swapItemsAt(0, 1);
    }

    m_externalShortLinkPending = true;
    m_pendingExternalShortUrlTarget = targetUrl;
    requestExternalShortLinkFromServices(targetUrl, serviceUrls, 0);
}

void Controller::requestExternalShortLinkFromServices(const QString &targetUrl,
                                                      const QStringList &serviceUrls,
                                                      qsizetype index)
{
    if (index >= serviceUrls.size()) {
        m_externalShortLinkPending = false;
        m_pendingExternalShortUrlTarget.clear();
        appendActivity(QStringLiteral("is.gd / v.gd 短網址申請失敗，改用原始外網連結"));
        applyExternalShortLink(targetUrl, targetUrl, QStringLiteral("Cloudflare Quick Tunnel"));
        return;
    }

    const QString serviceUrl = serviceUrls.at(index);
    const QString serviceName = shortenerHost(serviceUrl);
    appendActivity(QStringLiteral("正在向 %1 申請外部短網址").arg(serviceName));

    QUrl requestUrl(serviceUrl);
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("format"), QStringLiteral("json"));
    query.addQueryItem(QStringLiteral("url"), targetUrl);
    requestUrl.setQuery(query);

    QNetworkRequest request(requestUrl);
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("EasyCloudHFS/1.0"));

    QNetworkReply *reply = m_externalApi->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, targetUrl, serviceUrls, index, serviceName]() {
        const QByteArray body = reply->readAll();
        const QString networkError = reply->error() == QNetworkReply::NoError
                                         ? QString()
                                         : reply->errorString();
        reply->deleteLater();

        if (!m_settings.externalLinkEnabled || !isServerRunning()) {
            if (m_pendingExternalShortUrlTarget == targetUrl) {
                m_externalShortLinkPending = false;
                m_pendingExternalShortUrlTarget.clear();
            }
            return;
        }

        if (m_pendingExternalShortUrlTarget != targetUrl) {
            return;
        }

        if (m_tunnelManager.publicUrl().trimmed() != targetUrl) {
            m_externalShortLinkPending = false;
            m_pendingExternalShortUrlTarget.clear();
            refreshExternalShortLink();
            return;
        }

        const QJsonDocument document = QJsonDocument::fromJson(body);
        const QJsonObject object = document.isObject() ? document.object() : QJsonObject{};
        const QString shortUrl = object.value(QStringLiteral("shorturl")).toString().trimmed();
        if (!shortUrl.isEmpty()) {
            applyExternalShortLink(targetUrl, shortUrl, serviceName);
            return;
        }

        QString detail = networkError.trimmed();
        if (detail.isEmpty()) {
            detail = object.value(QStringLiteral("errormessage")).toString().trimmed();
        }
        if (detail.isEmpty()) {
            detail = QString::fromUtf8(body).trimmed();
        }
        if (detail.isEmpty()) {
            detail = QStringLiteral("未知錯誤");
        }

        const qsizetype nextIndex = index + 1;
        if (nextIndex < serviceUrls.size()) {
            const QString nextServiceName = shortenerHost(serviceUrls.at(nextIndex));
            appendActivity(QStringLiteral("%1 申請失敗，改試 %2：%3")
                               .arg(serviceName, nextServiceName, detail));
            requestExternalShortLinkFromServices(targetUrl, serviceUrls, nextIndex);
            return;
        }

        m_externalShortLinkPending = false;
        m_pendingExternalShortUrlTarget.clear();
        appendActivity(QStringLiteral("外部短網址申請失敗，已改用代理網址"));
        applyExternalShortLink(targetUrl, targetUrl, QStringLiteral("Cloudflare Quick Tunnel"));
    });
}

void Controller::applyExternalShortLink(const QString &targetUrl,
                                        const QString &shortUrl,
                                        const QString &serviceName)
{
    if (!m_settings.externalLinkEnabled || m_tunnelManager.publicUrl().trimmed() != targetUrl) {
        return;
    }

    m_externalShortLinkPending = false;
    m_pendingExternalShortUrlTarget.clear();
    m_externalShortUrl = shortUrl;
    m_externalShortUrlTarget = targetUrl;
    m_externalShortenerName = serviceName;
    m_externalReadyAnnounced = true;
    pushStatus(externalProxyReadyStatus());
    if (shortUrl == targetUrl) {
        appendActivity(QStringLiteral("代理網址已建立：%1").arg(targetUrl));
    } else {
        appendActivity(QStringLiteral("外部短網址已建立：%1 (%2)").arg(shortUrl, serviceName));
    }
    emit urlsChanged();
}

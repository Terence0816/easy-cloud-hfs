#pragma once

#include "app/DataStore.h"
#include "app/HttpFileServer.h"
#include "app/QrCodeGenerator.h"
#include "app/TunnelManager.h"

#include <QByteArray>
#include <QImage>
#include <QJsonArray>
#include <QObject>
#include <QTimer>
#include <QThread>

class QNetworkAccessManager;
class QNetworkReply;

class Controller : public QObject {
    Q_OBJECT

public:
    explicit Controller(QObject *parent = nullptr);
    ~Controller() override;

    [[nodiscard]] const QList<ShareItem> &shares() const;
    [[nodiscard]] const QList<DownloadRecord> &downloads() const;
    [[nodiscard]] const AppSettings &settings() const;
    [[nodiscard]] ServerStats stats() const;
    [[nodiscard]] bool isServerRunning() const;
    [[nodiscard]] QString localBaseUrl() const;
    [[nodiscard]] QString cloudflareUrl() const;
    [[nodiscard]] QString activeShareBaseUrl() const;
    [[nodiscard]] QString shareUrl(const ShareItem &share, bool preferPublic = true) const;
    [[nodiscard]] QString externalShareUrl(const ShareItem &share) const;
    [[nodiscard]] QString primaryExternalUrl() const;
    [[nodiscard]] QString statusMessage() const;
    [[nodiscard]] QStringList activityLog() const;
    [[nodiscard]] QString tunnelLogs() const;
    [[nodiscard]] bool isTunnelRunning() const;
    [[nodiscard]] bool isCloudflaredInstalled() const;
    [[nodiscard]] QString cloudflaredPath() const;
    [[nodiscard]] QString appDataRoot() const;
    [[nodiscard]] QImage buildQrCode(const QString &text, int size = 220);

public slots:
    bool startServer();
    void stopServer();
    void toggleServer();

    void addPaths(const QStringList &paths);
    void addFileShare(const QString &filePath);
    void addDirectoryShare(const QString &directoryPath);
    void addVirtualDirectory(const QString &name);
    void addLinkShare(const QString &name, const QString &url);
    void removeShare(const QString &id);
    void setShareEnabled(const QString &id, bool enabled);
    void setShareAllowUpload(const QString &id, bool allowUpload);
    void setShareAllowDelete(const QString &id, bool allowDelete);
    void setShareAllowCreateDirectory(const QString &id, bool allowCreateDirectory);
    void setExternalLinkSettings(bool enabled);
    void saveSharesSnapshot();

    bool exportShares(const QString &filePath) const;
    bool importShares(const QString &filePath);
    void clearDownloads();

    void saveSystemSettings(const AppSettings &settings);
    void saveDownloadSettings(const DownloadSettings &settings);
    void setCloudflaredPath(const QString &path);

    void downloadCloudflared();
    void startTunnel();
    void stopTunnel();

signals:
    void sharesChanged();
    void downloadsChanged();
    void settingsChanged();
    void statsChanged();
    void serverStateChanged(bool running);
    void urlsChanged();
    void statusMessageChanged(const QString &message);
    void activityLogChanged();
    void tunnelStateChanged();
    void tunnelLogsChanged(const QString &logs);

private:
    [[nodiscard]] QString detectLanAddress() const;
    [[nodiscard]] ShareItem makeFileShare(const QString &filePath) const;
    [[nodiscard]] ShareItem makeDirectoryShare(const QString &directoryPath) const;
    [[nodiscard]] ShareItem makeVirtualDirectoryShare(const QString &name) const;
    [[nodiscard]] ShareItem makeLinkShare(const QString &name, const QString &url) const;
    [[nodiscard]] bool hasExternalSharesEnabled() const;
    void persistAll();
    void reloadServerConfiguration();
    void pushStatus(const QString &message);
    void appendActivity(const QString &message);
    void appendDownloadRecord(const DownloadRecord &record);
    void refreshShareCount();
    void ensureExternalTunnel();
    void clearExternalShortLink();
    void refreshExternalShortLink();
    void requestExternalShortLink(const QString &targetUrl);
    void requestExternalShortLinkFromServices(const QString &targetUrl,
                                              const QStringList &serviceUrls,
                                              qsizetype index);
    void applyExternalShortLink(const QString &targetUrl,
                                const QString &shortUrl,
                                const QString &serviceName);
    [[nodiscard]] bool startHttpServer(quint16 port);
    void stopHttpServer();
    [[nodiscard]] bool httpServerRunning() const;
    void updateExternalSyncTimer();

    DataStore m_store;
    HttpFileServer *m_httpServer = nullptr;
    QThread m_httpServerThread;
    TunnelManager m_tunnelManager;
    QrCodeGenerator m_qrCodeGenerator;
    QNetworkAccessManager *m_externalApi = nullptr;
    AppSettings m_settings;
    QList<ShareItem> m_shares;
    QList<DownloadRecord> m_downloads;
    ServerStats m_stats;
    QString m_statusMessage;
    QStringList m_activityLog;
    QString m_externalShortUrl;
    QString m_externalShortUrlTarget;
    QString m_externalShortenerName;
    QString m_pendingExternalShortUrlTarget;
    bool m_externalTunnelRequested = false;
    bool m_externalReadyAnnounced = false;
    bool m_externalShortLinkPending = false;
    QTimer m_externalSyncTimer;
};

#pragma once

#include "app/Models.h"

#include <QObject>
#include <QPointer>
#include <QHash>
#include <QMap>
#include <QSet>
#include <QTimer>

class QFile;
class QTcpServer;
class QTcpSocket;
class QUrlQuery;

class HttpFileServer : public QObject {
    Q_OBJECT

public:
    explicit HttpFileServer(QObject *parent = nullptr);
    ~HttpFileServer() override;

    void setSiteName(const QString &siteName);
    void setLogoPath(const QString &logoPath);
    void setWebLanguage(const QString &language);
    void setDownloadSettings(const DownloadSettings &settings);
    void setShares(const QList<ShareItem> &shares);

    bool start(quint16 port);
    void stop();
    [[nodiscard]] bool isRunning() const;
    [[nodiscard]] quint16 port() const;

signals:
    void statusMessageChanged(const QString &message);
    void activityEvent(const QString &message);
    void downloadRecorded(const DownloadRecord &record);
    void statsChanged(quint64 totalDownloads, quint64 totalBytes, int activeConnections, qint64 currentBytesPerSecond);

private:
    struct ConnectionState {
        QByteArray buffer;
        QString method;
        QString target;
        QString httpVersion;
        QMap<QString, QString> headers;
        qint64 contentLength = 0;
        qint64 bodyReceived = 0;
        bool parsedHeaders = false;
        bool uploadRequest = false;
        QString uploadFilePath;
        QString uploadFileName;
        QFile *uploadFile = nullptr;
    };

    struct FileTransfer {
        QPointer<QTcpSocket> socket;
        QString socketKey;
        QString shareId;
        QString fileName;
        QString relativePath;
        QString clientAddress;
        QFile *file = nullptr;
        qint64 startOffset = 0;
        qint64 endOffset = -1;
        qint64 remaining = 0;
        qint64 bytesSent = 0;
        bool success = true;
        bool trackAsDownload = true;
    };

    struct ChunkUploadSession {
        QString tempPath;
        QString finalPath;
        QString fileName;
        QString clientAddress;
        qint64 expectedSize = 0;
        qint64 chunkSize = 0;
        qint64 bytesWritten = 0;
        int totalChunks = 0;
        QSet<int> receivedChunks;
    };

    void handleNewConnection();
    void handleSocketReadyRead(QTcpSocket *socket);
    void handleSocketDisconnected(QTcpSocket *socket);
    void handleSocketBytesWritten(QTcpSocket *socket, qint64 bytes);
    void processRequest(QTcpSocket *socket, ConnectionState &state);
    bool prepareUploadRequest(QTcpSocket *socket, ConnectionState &state);
    void consumeUploadData(QTcpSocket *socket, ConnectionState &state);
    void handleCreateDirectoryRequest(QTcpSocket *socket, const QUrlQuery &query);
    void handleDeleteRequest(QTcpSocket *socket, const QUrlQuery &query);
    void cleanupUploadState(ConnectionState &state, bool removePartialFile);
    void removeChunkUploadSession(const QString &uploadId, bool removePartialFile);
    bool handleChunkUpload(QTcpSocket *socket, const QString &requestPath, const QUrlQuery &query, const QByteArray &body);

    void sendResponse(QTcpSocket *socket, int statusCode, const QByteArray &statusText, const QByteArray &body, const QByteArray &contentType);
    void sendRedirect(QTcpSocket *socket, const QString &target);
    void sendNotFound(QTcpSocket *socket);
    void sendUnauthorized(QTcpSocket *socket, bool badPassword = false);
    void sendBadRequest(QTcpSocket *socket, const QString &message);
    void sendJson(QTcpSocket *socket, const QByteArray &body);
    void sendPlaylistFile(QTcpSocket *socket,
                          const ShareItem &share,
                          const QString &relativePath,
                          const QString &displayName);
    void sendFile(QTcpSocket *socket,
                  const ShareItem &share,
                  const QString &absolutePath,
                  const QString &relativePath,
                  const QString &rangeHeader,
                  bool trackAsDownload = true,
                  bool inlineDisposition = false);

    [[nodiscard]] bool isAuthenticated(const QMap<QString, QString> &headers) const;
    [[nodiscard]] QString authToken() const;
    [[nodiscard]] QString headerValue(const QMap<QString, QString> &headers, const QString &name) const;
    [[nodiscard]] QString detectClientAddress(QTcpSocket *socket) const;
    [[nodiscard]] QString canonicalSafePath(const QString &root, const QString &relativePath, bool allowMissingLeaf = false) const;
    [[nodiscard]] QByteArray renderHomePage() const;
    [[nodiscard]] QByteArray renderLoginPage(bool badPassword) const;
    [[nodiscard]] QByteArray renderDirectoryPage(const ShareItem &share,
                                                 const QString &rootPath,
                                                 const QString &relativePath,
                                                 const QUrlQuery &query) const;
    [[nodiscard]] QByteArray renderImageViewerPage(const QString &pageTitle,
                                                   const QString &imageName,
                                                   const QString &imageSrc,
                                                   const QString &backHref,
                                                   const QString &downloadHref,
                                                   const QString &previousHref,
                                                   const QString &nextHref,
                                                   const QString &metaText) const;
    [[nodiscard]] QByteArray renderMediaViewerPage(const QString &pageTitle,
                                                   const QString &mediaName,
                                                   const QString &mediaSrc,
                                                   const QString &backHref,
                                                   const QString &downloadHref,
                                                   const QString &potplayerPath,
                                                   const QString &previousHref,
                                                   const QString &nextHref,
                                                   const QString &metaText,
                                                   const QString &playlistJson,
                                                   int currentIndex,
                                                   bool isVideo,
                                                   const QString &subtitleHref = QString()) const;
    [[nodiscard]] QByteArray renderSharedImageViewerPage(const ShareItem &share) const;
    [[nodiscard]] QByteArray renderSharedMediaViewerPage(const ShareItem &share, bool isVideo) const;
    [[nodiscard]] QByteArray renderDirectoryImageViewerPage(const ShareItem &share,
                                                            const QString &rootPath,
                                                            const QString &relativePath,
                                                            const QUrlQuery &query) const;
    [[nodiscard]] QByteArray renderDirectoryMediaViewerPage(const ShareItem &share,
                                                            const QString &rootPath,
                                                            const QString &relativePath,
                                                            const QUrlQuery &query,
                                                            bool isVideo) const;
    [[nodiscard]] QString subtitlePathForMedia(const QString &mediaPath) const;
    [[nodiscard]] QByteArray renderSubtitleTrack(const QString &mediaPath) const;
    [[nodiscard]] QByteArray renderMessagePage(const QString &title, const QString &body) const;
    [[nodiscard]] QByteArray serveLogo() const;
    [[nodiscard]] QString escapeHtml(const QString &value) const;
    [[nodiscard]] QString webTx(const QString &zh, const QString &en) const;
    [[nodiscard]] QString localizeWebHtml(QString html) const;
    [[nodiscard]] QString routeForShare(const ShareItem &share) const;
    [[nodiscard]] QString mediaRelayUrl(const ShareItem &share,
                                        const QString &relativePath = QString()) const;
    [[nodiscard]] QString detectLanAddress() const;
    [[nodiscard]] bool decodePotplayerToken(const QString &token, QString *routeSegment, QString *relativePath) const;
    [[nodiscard]] QString displaySourcePath(const ShareItem &share) const;
    [[nodiscard]] QList<ShareItem> rootShares() const;
    [[nodiscard]] const ShareItem *findShareByRoute(const QString &routeSegment) const;
    [[nodiscard]] bool shareAllowsUpload(const ShareItem &share) const;
    [[nodiscard]] bool shareAllowsDelete(const ShareItem &share) const;
    [[nodiscard]] bool shareAllowsCreateDirectory(const ShareItem &share) const;
    [[nodiscard]] int activeTransferCount() const;
    [[nodiscard]] qint64 totalLimitBytesPerSecond() const;
    [[nodiscard]] qint64 perIpLimitBytesPerSecond() const;

    void serviceTransfers();
    void finalizeTransfer(QTcpSocket *socket, bool success);
    void updateStats();

    QTcpServer *m_server = nullptr;
    quint16 m_port = 0;
    QString m_siteName = "Easy Cloud HFS";
    QString m_logoPath;
    QString m_webLanguage = QStringLiteral("\u7e41\u9ad4\u4e2d\u6587");
    DownloadSettings m_downloadSettings;
    QList<ShareItem> m_shares;
    QHash<QTcpSocket *, ConnectionState> m_connections;
    QHash<QTcpSocket *, FileTransfer *> m_transfers;
    QHash<QString, ChunkUploadSession> m_chunkUploads;
    QTimer m_transferTimer;
    QTimer m_statsTimer;
    quint64 m_totalDownloads = 0;
    quint64 m_totalBytes = 0;
    qint64 m_windowBytes = 0;
    qint64 m_lastBytesPerSecond = 0;
};

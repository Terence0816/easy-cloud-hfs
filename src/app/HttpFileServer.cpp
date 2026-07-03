#include "app/HttpFileServer.h"

#include <QAbstractSocket>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeDatabase>
#include <QNetworkInterface>
#include <QRegularExpression>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>
#include <memory>

namespace {
enum class DirectorySortKey {
    Name,
    Date,
    Size,
};

QByteArray statusTextFor(int code)
{
    switch (code) {
    case 200:
        return QByteArrayLiteral("OK");
    case 206:
        return QByteArrayLiteral("Partial Content");
    case 302:
        return QByteArrayLiteral("Found");
    case 400:
        return QByteArrayLiteral("Bad Request");
    case 401:
        return QByteArrayLiteral("Unauthorized");
    case 403:
        return QByteArrayLiteral("Forbidden");
    case 404:
        return QByteArrayLiteral("Not Found");
    case 416:
        return QByteArrayLiteral("Range Not Satisfiable");
    default:
        return QByteArrayLiteral("Internal Server Error");
    }
}

QString urlDecode(const QString &value)
{
    return QUrl::fromPercentEncoding(value.toUtf8());
}

QString urlEncode(const QString &value)
{
    return QString::fromUtf8(QUrl::toPercentEncoding(value));
}

QString urlEncodePath(const QString &value)
{
    if (value.isEmpty()) {
        return QString();
    }

    const QStringList parts = value.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    QStringList encoded;
    encoded.reserve(parts.size());
    for (const QString &part : parts) {
        encoded.append(urlEncode(part));
    }
    return encoded.join(QLatin1Char('/'));
}

QString directoryOfPath(const QString &relativePath)
{
    const int lastSlash = relativePath.lastIndexOf(QLatin1Char('/'));
    if (lastSlash < 0) {
        return QString();
    }
    return relativePath.left(lastSlash);
}

QString joinDecodedSegments(const QStringList &segments, int startIndex)
{
    QStringList decoded;
    for (int index = startIndex; index < segments.size(); ++index) {
        decoded.append(urlDecode(segments.at(index)));
    }
    return decoded.join(QLatin1Char('/'));
}

DirectorySortKey parseDirectorySortKey(const QString &value)
{
    const QString normalized = value.trimmed().toLower();
    if (normalized.isEmpty()) {
        return DirectorySortKey::Date;
    }
    if (normalized == QStringLiteral("date")) {
        return DirectorySortKey::Date;
    }
    if (normalized == QStringLiteral("size")) {
        return DirectorySortKey::Size;
    }
    return DirectorySortKey::Name;
}

QString directorySortKeyValue(DirectorySortKey key)
{
    switch (key) {
    case DirectorySortKey::Date:
        return QStringLiteral("date");
    case DirectorySortKey::Size:
        return QStringLiteral("size");
    case DirectorySortKey::Name:
    default:
        return QStringLiteral("name");
    }
}

bool parseDirectorySortDescending(const QString &value)
{
    if (value.trimmed().isEmpty()) {
        return true;
    }
    return value.trimmed().compare(QStringLiteral("desc"), Qt::CaseInsensitive) == 0;
}

QString directorySortOrderValue(bool descending)
{
    return descending ? QStringLiteral("desc") : QStringLiteral("asc");
}

QString buildDirectorySortHref(const QString &baseHref, DirectorySortKey key, bool descending)
{
    return QStringLiteral("%1?sort=%2&order=%3")
        .arg(baseHref, directorySortKeyValue(key), directorySortOrderValue(descending));
}

int compareDirectoryEntries(const QFileInfo &left, const QFileInfo &right, DirectorySortKey key)
{
    const auto compareByName = [&left, &right]() {
        return left.fileName().localeAwareCompare(right.fileName());
    };

    switch (key) {
    case DirectorySortKey::Date: {
        const QDateTime leftDate = left.lastModified();
        const QDateTime rightDate = right.lastModified();
        if (leftDate < rightDate) {
            return -1;
        }
        if (leftDate > rightDate) {
            return 1;
        }
        return compareByName();
    }
    case DirectorySortKey::Size: {
        const qint64 leftSize = left.isDir() ? 0 : left.size();
        const qint64 rightSize = right.isDir() ? 0 : right.size();
        if (leftSize < rightSize) {
            return -1;
        }
        if (leftSize > rightSize) {
            return 1;
        }
        return compareByName();
    }
    case DirectorySortKey::Name:
    default:
        return compareByName();
    }
}

bool isImageMimeType(const QString &mimeType)
{
    return mimeType.startsWith(QStringLiteral("image/"), Qt::CaseInsensitive);
}

bool isAudioMimeType(const QString &mimeType)
{
    return mimeType.startsWith(QStringLiteral("audio/"), Qt::CaseInsensitive);
}

bool isVideoMimeType(const QString &mimeType)
{
    return mimeType.startsWith(QStringLiteral("video/"), Qt::CaseInsensitive);
}

QString mimeTypeForPath(const QString &path)
{
    if (path.isEmpty()) {
        return QString();
    }

    const QMimeDatabase database;
    return database.mimeTypeForFile(path).name();
}

bool isImageFilePath(const QString &path)
{
    if (path.isEmpty()) {
        return false;
    }

    return isImageMimeType(mimeTypeForPath(path));
}

bool isAudioFilePath(const QString &path)
{
    if (path.isEmpty()) {
        return false;
    }

    return isAudioMimeType(mimeTypeForPath(path));
}

bool isVideoFilePath(const QString &path)
{
    if (path.isEmpty()) {
        return false;
    }

    return isVideoMimeType(mimeTypeForPath(path));
}

bool removeFileSystemEntry(const QString &path)
{
    const QFileInfo info(path);
    if (!info.exists()) {
        return false;
    }

    if (info.isDir() && !info.isSymLink()) {
        QDir directory(path);
        return directory.removeRecursively();
    }

    return QFile::remove(path);
}

QByteArray srtToVtt(const QByteArray &source)
{
    QString text = QString::fromUtf8(source);
    if (!text.isEmpty() && text.front() == QChar(0xfeff)) {
        text.remove(0, 1);
    }

    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    text.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    text.replace(QRegularExpression(QStringLiteral(R"((\d{2}:\d{2}:\d{2}),(\d{3}))")),
                 QStringLiteral("\\1.\\2"));

    return (QStringLiteral("WEBVTT\n\n") + text).toUtf8();
}
}

HttpFileServer::HttpFileServer(QObject *parent)
    : QObject(parent)
    , m_server(new QTcpServer(this))
{
    connect(m_server, &QTcpServer::newConnection, this, &HttpFileServer::handleNewConnection);

    m_transferTimer.setParent(this);
    m_transferTimer.setInterval(50);
    connect(&m_transferTimer, &QTimer::timeout, this, &HttpFileServer::serviceTransfers);

    m_statsTimer.setParent(this);
    m_statsTimer.setInterval(1000);
    connect(&m_statsTimer, &QTimer::timeout, this, [this]() {
        m_lastBytesPerSecond = m_windowBytes;
        m_windowBytes = 0;
        updateStats();
    });
}

HttpFileServer::~HttpFileServer()
{
    stop();
}

void HttpFileServer::setSiteName(const QString &siteName)
{
    m_siteName = siteName.trimmed().isEmpty() ? QStringLiteral("Easy Cloud HFS") : siteName.trimmed();
}

void HttpFileServer::setLogoPath(const QString &logoPath)
{
    m_logoPath = logoPath;
}

void HttpFileServer::setDownloadSettings(const DownloadSettings &settings)
{
    m_downloadSettings = settings;
}

void HttpFileServer::setShares(const QList<ShareItem> &shares)
{
    m_shares = shares;
    updateStats();
}

bool HttpFileServer::start(quint16 port)
{
    stop();

    if (!m_server->listen(QHostAddress::AnyIPv4, port)) {
        emit statusMessageChanged(QStringLiteral("HTTP 伺服器啟動失敗：%1").arg(m_server->errorString()));
        return false;
    }

    m_port = port;
    m_transferTimer.start();
    m_statsTimer.start();
    emit statusMessageChanged(QStringLiteral("HTTP 伺服器已啟動"));
    updateStats();
    return true;
}

void HttpFileServer::stop()
{
    const QList<QTcpSocket *> transferSockets = m_transfers.keys();
    for (QTcpSocket *socket : transferSockets) {
        finalizeTransfer(socket, false);
    }
    m_transfers.clear();

    const QList<QTcpSocket *> connectionSockets = m_connections.keys();
    for (QTcpSocket *socket : connectionSockets) {
        if (m_connections.contains(socket)) {
            ConnectionState &state = m_connections[socket];
            cleanupUploadState(state, state.uploadFile && state.bodyReceived < state.contentLength);
        }
        if (socket) {
            socket->disconnectFromHost();
        }
    }
    m_connections.clear();

    const QStringList chunkUploadIds = m_chunkUploads.keys();
    for (const QString &uploadId : chunkUploadIds) {
        removeChunkUploadSession(uploadId, true);
    }
    m_chunkUploads.clear();

    if (m_server->isListening()) {
        m_server->close();
    }

    m_transferTimer.stop();
    m_statsTimer.stop();
    m_port = 0;
    updateStats();
}

bool HttpFileServer::isRunning() const
{
    return m_server->isListening();
}

quint16 HttpFileServer::port() const
{
    return m_port;
}

void HttpFileServer::handleNewConnection()
{
    while (m_server->hasPendingConnections()) {
        QTcpSocket *socket = m_server->nextPendingConnection();
        m_connections.insert(socket, ConnectionState{});

        connect(socket, &QTcpSocket::readyRead, this, [this, socket]() { handleSocketReadyRead(socket); });
        connect(socket, &QTcpSocket::disconnected, this, [this, socket]() { handleSocketDisconnected(socket); });
        connect(socket, &QTcpSocket::bytesWritten, this, [this, socket](qint64 bytes) { handleSocketBytesWritten(socket, bytes); });
    }

    updateStats();
}

void HttpFileServer::handleSocketReadyRead(QTcpSocket *socket)
{
    if (!socket || !m_connections.contains(socket)) {
        return;
    }

    ConnectionState &state = m_connections[socket];
    state.buffer.append(socket->readAll());

    if (!state.parsedHeaders) {
        const qsizetype headersEnd = state.buffer.indexOf("\r\n\r\n");
        if (headersEnd < 0) {
            return;
        }

        const QList<QByteArray> lines = state.buffer.left(headersEnd).split('\n');
        if (lines.isEmpty()) {
            sendBadRequest(socket, QStringLiteral("收到空白的 HTTP 請求。"));
            return;
        }

        const QList<QByteArray> requestLine = lines.first().trimmed().split(' ');
        if (requestLine.size() < 3) {
            sendBadRequest(socket, QStringLiteral("HTTP 請求列格式不正確。"));
            return;
        }

        state.method = QString::fromUtf8(requestLine.at(0)).trimmed().toUpper();
        state.target = QString::fromUtf8(requestLine.at(1)).trimmed();
        state.httpVersion = QString::fromUtf8(requestLine.at(2)).trimmed();

        for (int index = 1; index < lines.size(); ++index) {
            const QByteArray line = lines.at(index).trimmed();
            const int colonIndex = line.indexOf(':');
            if (colonIndex <= 0) {
                continue;
            }

            const QString key = QString::fromUtf8(line.left(colonIndex)).trimmed().toLower();
            const QString value = QString::fromUtf8(line.mid(colonIndex + 1)).trimmed();
            state.headers.insert(key, value);
        }

        state.contentLength = qMax<qint64>(0, headerValue(state.headers, QStringLiteral("content-length")).toLongLong());
        state.buffer.remove(0, headersEnd + 4);
        state.parsedHeaders = true;

        const QUrl url(QStringLiteral("http://local") + state.target);
        const QUrlQuery query(url);
        state.uploadRequest = (url.path(QUrl::FullyDecoded) == QStringLiteral("/__upload")
                               && state.method == QStringLiteral("POST")
                               && !query.hasQueryItem(QStringLiteral("__upload_chunk")));
        if (state.uploadRequest && !prepareUploadRequest(socket, state)) {
            return;
        }
    }

    if (state.uploadRequest) {
        consumeUploadData(socket, state);
        return;
    }

    if (state.buffer.size() < state.contentLength) {
        return;
    }

    processRequest(socket, state);
}

void HttpFileServer::handleSocketDisconnected(QTcpSocket *socket)
{
    if (socket && m_connections.contains(socket)) {
        ConnectionState &state = m_connections[socket];
        if (state.uploadFile && state.bodyReceived < state.contentLength) {
            const QString name = state.uploadFileName;
            cleanupUploadState(state, true);
            if (!name.isEmpty()) {
                emit activityEvent(QStringLiteral("%1 上傳中斷：%2").arg(detectClientAddress(socket), name));
            }
        } else {
            cleanupUploadState(state, false);
        }
    }

    finalizeTransfer(socket, false);
    m_connections.remove(socket);
    if (socket) {
        socket->deleteLater();
    }
    updateStats();
}

void HttpFileServer::handleSocketBytesWritten(QTcpSocket *socket, qint64)
{
    if (!socket || !m_transfers.contains(socket)) {
        return;
    }
    serviceTransfers();
}

void HttpFileServer::processRequest(QTcpSocket *socket, ConnectionState &state)
{
    const QByteArray body = state.contentLength > 0 ? state.buffer.left(static_cast<qsizetype>(state.contentLength)) : QByteArray();
    state.buffer.clear();

    const QUrl url(QStringLiteral("http://local") + state.target);
    const QString path = url.path(QUrl::FullyDecoded);
    const QUrlQuery query(url);
    const bool potplayerRequest = path.startsWith(QStringLiteral("/__pot/"));

    if (path == QStringLiteral("/__logo")) {
        QByteArray contentType = QByteArrayLiteral("image/png");
        if (!m_logoPath.isEmpty()) {
            const QMimeDatabase database;
            const QByteArray detected = database.mimeTypeForFile(m_logoPath).name().toUtf8();
            if (!detected.isEmpty()) {
                contentType = detected;
            }
        }

        sendResponse(socket, 200, statusTextFor(200), serveLogo(), contentType);
        return;
    }

    if (!m_downloadSettings.password.isEmpty()
        && path != QStringLiteral("/__auth")
        && !(potplayerRequest && query.queryItemValue(QStringLiteral("k"), QUrl::FullyDecoded) == authToken())
        && !isAuthenticated(state.headers)) {
        sendUnauthorized(socket);
        return;
    }

    if (path == QStringLiteral("/__auth") && state.method == QStringLiteral("POST")) {
        const QUrlQuery form(QString::fromUtf8(body));
        const QString password = form.queryItemValue(QStringLiteral("password"), QUrl::FullyDecoded);
        if (password == m_downloadSettings.password) {
            QByteArray response;
            response += QByteArrayLiteral("HTTP/1.1 302 Found\r\n");
            response += QByteArrayLiteral("Location: /\r\n");
            response += QByteArrayLiteral("Set-Cookie: hfs_auth=") + authToken().toUtf8() + QByteArrayLiteral("; Path=/; Max-Age=86400\r\n");
            response += QByteArrayLiteral("Connection: close\r\n");
            response += QByteArrayLiteral("Content-Length: 0\r\n\r\n");
            socket->write(response);
            emit activityEvent(QStringLiteral("%1 已登入下載頁面").arg(detectClientAddress(socket)));
            socket->disconnectFromHost();
            return;
        }

        sendUnauthorized(socket, true);
        return;
    }

    if (path == QStringLiteral("/") || path.isEmpty()) {
        sendResponse(socket, 200, statusTextFor(200), renderHomePage(), QByteArrayLiteral("text/html; charset=utf-8"));
        return;
    }

    if (potplayerRequest) {
        const QString relayPath = path.mid(QStringLiteral("/__pot/").size());
        const int separatorIndex = relayPath.indexOf(QLatin1Char('/'));
        const QString token = separatorIndex >= 0 ? relayPath.left(separatorIndex) : relayPath;
        QString routeSegment;
        QString relativePath;
        if (!decodePotplayerToken(token, &routeSegment, &relativePath)) {
            sendNotFound(socket);
            return;
        }

        const ShareItem *share = findShareByRoute(routeSegment);
        if (!share || !share->enabled || share->type == ShareType::Link) {
            sendNotFound(socket);
            return;
        }

        QString absolutePath;
        QString downloadRelativePath;
        if (share->type == ShareType::File) {
            absolutePath = share->sourcePath;
            downloadRelativePath = share->name;
        } else {
            const QString baseRoot = (share->type == ShareType::VirtualDirectory) ? share->storagePath : share->sourcePath;
            const QString safePath = canonicalSafePath(baseRoot, relativePath, false);
            if (safePath.isEmpty()) {
                sendNotFound(socket);
                return;
            }

            const QFileInfo fileInfo(safePath);
            if (!fileInfo.exists() || fileInfo.isDir()) {
                sendNotFound(socket);
                return;
            }

            absolutePath = safePath;
            downloadRelativePath = relativePath;
        }

        if (!isAudioFilePath(absolutePath) && !isVideoFilePath(absolutePath)) {
            sendNotFound(socket);
            return;
        }

        sendFile(socket,
                 *share,
                 absolutePath,
                 downloadRelativePath,
                 headerValue(state.headers, QStringLiteral("range")),
                 true,
                 false);
        return;
    }

    if (state.method == QStringLiteral("POST") && query.hasQueryItem(QStringLiteral("__upload_chunk"))) {
        if (!handleChunkUpload(socket, path, query, body)) {
            return;
        }
        return;
    }

    if (path == QStringLiteral("/__upload") && state.method == QStringLiteral("POST")) {
        sendBadRequest(socket, QStringLiteral("上傳工作初始化失敗，請重新整理頁面後再試一次。"));
        return;
    }

    if (path == QStringLiteral("/__mkdir") && state.method == QStringLiteral("POST")) {
        handleCreateDirectoryRequest(socket, query);
        return;
    }

    if (path == QStringLiteral("/__delete") && state.method == QStringLiteral("POST")) {
        handleDeleteRequest(socket, query);
        return;
    }

    const bool inlinePreview = query.queryItemValue(QStringLiteral("__inline"), QUrl::FullyDecoded) == QStringLiteral("1");
    const bool imageViewer = query.queryItemValue(QStringLiteral("__viewer"), QUrl::FullyDecoded) == QStringLiteral("1");
    const bool mediaViewer = query.queryItemValue(QStringLiteral("__media"), QUrl::FullyDecoded) == QStringLiteral("1");
    const bool playlistFile = query.queryItemValue(QStringLiteral("__m3u8"), QUrl::FullyDecoded) == QStringLiteral("1");
    const bool subtitleTrack = query.queryItemValue(QStringLiteral("__subtitle"), QUrl::FullyDecoded) == QStringLiteral("1");

    const QStringList segments = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (segments.isEmpty()) {
        sendNotFound(socket);
        return;
    }

    const QString routeSegment = urlDecode(segments.first());
    const ShareItem *share = findShareByRoute(routeSegment);
    if (!share || !share->enabled) {
        sendNotFound(socket);
        return;
    }

    const QString relativePath = joinDecodedSegments(segments, 1);
    if (share->type == ShareType::Link) {
        sendRedirect(socket, share->sourcePath);
        return;
    }

    if (share->type == ShareType::File) {
        if (imageViewer && isImageFilePath(share->sourcePath)) {
            sendResponse(socket,
                         200,
                         statusTextFor(200),
                         renderSharedImageViewerPage(*share),
                         QByteArrayLiteral("text/html; charset=utf-8"));
            return;
        }
        if (playlistFile && (isAudioFilePath(share->sourcePath) || isVideoFilePath(share->sourcePath))) {
            sendPlaylistFile(socket, *share, QString(), share->name);
            return;
        }
        if (mediaViewer && (isAudioFilePath(share->sourcePath) || isVideoFilePath(share->sourcePath))) {
            sendResponse(socket,
                         200,
                         statusTextFor(200),
                         renderSharedMediaViewerPage(*share, isVideoFilePath(share->sourcePath)),
                         QByteArrayLiteral("text/html; charset=utf-8"));
            return;
        }
        if (subtitleTrack && isVideoFilePath(share->sourcePath)) {
            const QByteArray subtitleBody = renderSubtitleTrack(share->sourcePath);
            if (subtitleBody.isEmpty()) {
                sendNotFound(socket);
                return;
            }

            sendResponse(socket, 200, statusTextFor(200), subtitleBody, QByteArrayLiteral("text/vtt; charset=utf-8"));
            return;
        }
        sendFile(socket,
                 *share,
                 share->sourcePath,
                 share->name,
                 headerValue(state.headers, QStringLiteral("range")),
                 !inlinePreview,
                 inlinePreview);
        return;
    }

    const QString baseRoot = (share->type == ShareType::VirtualDirectory) ? share->storagePath : share->sourcePath;
    const QString safePath = canonicalSafePath(baseRoot, relativePath, false);
    const QString resolvedPath = safePath.isEmpty() && relativePath.isEmpty() ? baseRoot : safePath;
    if (resolvedPath.isEmpty()) {
        sendBadRequest(socket, QStringLiteral("請求路徑超出分享範圍。"));
        return;
    }

    const QFileInfo info(resolvedPath);
    if (!info.exists()) {
        sendNotFound(socket);
        return;
    }

    if (info.isDir()) {
        sendResponse(socket,
                     200,
                     statusTextFor(200),
                    renderDirectoryPage(*share, baseRoot, relativePath, query),
                    QByteArrayLiteral("text/html; charset=utf-8"));
        return;
    }

    if (imageViewer && isImageFilePath(resolvedPath)) {
        sendResponse(socket,
                     200,
                     statusTextFor(200),
                     renderDirectoryImageViewerPage(*share, baseRoot, relativePath, query),
                     QByteArrayLiteral("text/html; charset=utf-8"));
        return;
    }

    if (playlistFile && (isAudioFilePath(resolvedPath) || isVideoFilePath(resolvedPath))) {
        sendPlaylistFile(socket, *share, relativePath, QFileInfo(relativePath).fileName());
        return;
    }

    if (mediaViewer && (isAudioFilePath(resolvedPath) || isVideoFilePath(resolvedPath))) {
        sendResponse(socket,
                     200,
                     statusTextFor(200),
                     renderDirectoryMediaViewerPage(*share, baseRoot, relativePath, query, isVideoFilePath(resolvedPath)),
                     QByteArrayLiteral("text/html; charset=utf-8"));
        return;
    }

    if (subtitleTrack && isVideoFilePath(resolvedPath)) {
        const QByteArray subtitleBody = renderSubtitleTrack(resolvedPath);
        if (subtitleBody.isEmpty()) {
            sendNotFound(socket);
            return;
        }

        sendResponse(socket, 200, statusTextFor(200), subtitleBody, QByteArrayLiteral("text/vtt; charset=utf-8"));
        return;
    }

    sendFile(socket,
             *share,
             resolvedPath,
             relativePath,
             headerValue(state.headers, QStringLiteral("range")),
             !inlinePreview,
             inlinePreview);
}

bool HttpFileServer::prepareUploadRequest(QTcpSocket *socket, ConnectionState &state)
{
    const QUrl url(QStringLiteral("http://local") + state.target);
    const QUrlQuery query(url);
    const QString shareRoute = query.queryItemValue(QStringLiteral("share"), QUrl::FullyDecoded);
    const QString targetDirectory = query.queryItemValue(QStringLiteral("path"), QUrl::FullyDecoded);
    const QString fileName = query.queryItemValue(QStringLiteral("name"), QUrl::FullyDecoded);

    const ShareItem *share = findShareByRoute(shareRoute);
    if (!share || !shareAllowsUpload(*share)) {
        sendBadRequest(socket, QStringLiteral("這個分享項目不允許上傳。"));
        return false;
    }

    const QString basePath = (share->type == ShareType::VirtualDirectory) ? share->storagePath : share->sourcePath;
    const QString safeDirectory = canonicalSafePath(basePath, targetDirectory, true);
    if (safeDirectory.isEmpty()) {
        sendBadRequest(socket, QStringLiteral("上傳目錄無效。"));
        return false;
    }

    const QString safeName = QFileInfo(fileName).fileName();
    if (safeName.isEmpty()) {
        sendBadRequest(socket, QStringLiteral("上傳檔名不可為空。"));
        return false;
    }

    if (state.contentLength <= 0) {
        sendBadRequest(socket, QStringLiteral("上傳內容不可為空。"));
        return false;
    }

    QDir().mkpath(safeDirectory);
    state.uploadFilePath = QDir(safeDirectory).filePath(safeName);
    state.uploadFileName = safeName;
    state.uploadFile = new QFile(state.uploadFilePath);
    if (!state.uploadFile->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        cleanupUploadState(state, false);
        sendBadRequest(socket, QStringLiteral("無法寫入上傳檔案。"));
        return false;
    }

    state.bodyReceived = 0;
    emit activityEvent(QStringLiteral("%1 開始上傳：%2").arg(detectClientAddress(socket), safeName));
    updateStats();
    return true;
}

void HttpFileServer::consumeUploadData(QTcpSocket *socket, ConnectionState &state)
{
    if (!state.uploadFile) {
        sendBadRequest(socket, QStringLiteral("上傳工作尚未初始化。"));
        return;
    }

    const qint64 remaining = state.contentLength - state.bodyReceived;
    const qint64 bytesToWrite = qMin<qint64>(remaining, static_cast<qint64>(state.buffer.size()));
    if (bytesToWrite > 0) {
        const qint64 written = state.uploadFile->write(state.buffer.constData(), bytesToWrite);
        if (written != bytesToWrite) {
            const QString name = state.uploadFileName;
            cleanupUploadState(state, true);
            sendBadRequest(socket, QStringLiteral("寫入上傳檔案時發生錯誤。"));
            emit activityEvent(QStringLiteral("%1 上傳失敗：%2").arg(detectClientAddress(socket), name));
            return;
        }

        state.bodyReceived += written;
        m_windowBytes += written;
        state.buffer.remove(0, static_cast<qsizetype>(written));
    }

    if (state.bodyReceived < state.contentLength) {
        return;
    }

    const QString completedName = state.uploadFileName;
    const qint64 completedSize = state.contentLength;
    state.uploadFile->flush();
    cleanupUploadState(state, false);
    updateStats();
    sendJson(socket, QByteArrayLiteral("{\"ok\":true}"));
    emit activityEvent(QStringLiteral("%1 上傳完成：%2（%3）")
                           .arg(detectClientAddress(socket), completedName, humanReadableSize(completedSize)));
}

void HttpFileServer::handleCreateDirectoryRequest(QTcpSocket *socket, const QUrlQuery &query)
{
    const QString shareRoute = query.queryItemValue(QStringLiteral("share"), QUrl::FullyDecoded);
    const QString currentRelativePath = query.queryItemValue(QStringLiteral("path"), QUrl::FullyDecoded);
    const QString requestedName = query.queryItemValue(QStringLiteral("name"), QUrl::FullyDecoded).trimmed();

    const ShareItem *share = findShareByRoute(shareRoute);
    if (!share || !shareAllowsCreateDirectory(*share)) {
        sendBadRequest(socket, QStringLiteral("這個分享項目不允許建立資料夾。"));
        return;
    }

    const QString safeName = QFileInfo(requestedName).fileName().trimmed();
    if (safeName.isEmpty() || safeName == QStringLiteral(".") || safeName == QStringLiteral("..")) {
        sendBadRequest(socket, QStringLiteral("資料夾名稱無效。"));
        return;
    }

    const QString basePath = (share->type == ShareType::VirtualDirectory) ? share->storagePath : share->sourcePath;
    const QString safeCurrentDirectory = currentRelativePath.isEmpty()
                                             ? basePath
                                             : canonicalSafePath(basePath, currentRelativePath, false);
    const QFileInfo currentDirectoryInfo(safeCurrentDirectory);
    if (safeCurrentDirectory.isEmpty() || !currentDirectoryInfo.exists() || !currentDirectoryInfo.isDir()) {
        sendBadRequest(socket, QStringLiteral("目前目錄不存在或無效。"));
        return;
    }

    const QString childRelativePath = currentRelativePath.isEmpty()
                                          ? safeName
                                          : currentRelativePath + QStringLiteral("/") + safeName;
    const QString targetDirectory = canonicalSafePath(basePath, childRelativePath, true);
    if (targetDirectory.isEmpty()) {
        sendBadRequest(socket, QStringLiteral("建立資料夾的目標路徑無效。"));
        return;
    }

    if (QFileInfo::exists(targetDirectory)) {
        sendBadRequest(socket, QStringLiteral("同名資料夾或檔案已存在。"));
        return;
    }

    if (!QDir().mkpath(targetDirectory)) {
        sendBadRequest(socket, QStringLiteral("建立資料夾失敗。"));
        return;
    }

    emit activityEvent(QStringLiteral("%1 建立資料夾：%2")
                           .arg(detectClientAddress(socket), childRelativePath));
    sendJson(socket, QByteArrayLiteral("{\"ok\":true}"));
}

void HttpFileServer::handleDeleteRequest(QTcpSocket *socket, const QUrlQuery &query)
{
    const QString shareRoute = query.queryItemValue(QStringLiteral("share"), QUrl::FullyDecoded);
    const QString targetRelativePath = query.queryItemValue(QStringLiteral("path"), QUrl::FullyDecoded);

    const ShareItem *share = findShareByRoute(shareRoute);
    if (!share || !shareAllowsDelete(*share)) {
        sendBadRequest(socket, QStringLiteral("這個分享項目不允許刪除。"));
        return;
    }

    if (targetRelativePath.trimmed().isEmpty()) {
        sendBadRequest(socket, QStringLiteral("刪除目標不可為空。"));
        return;
    }

    const QString basePath = (share->type == ShareType::VirtualDirectory) ? share->storagePath : share->sourcePath;
    const QString safeTargetPath = canonicalSafePath(basePath, targetRelativePath, false);
    const QString safeRootPath = canonicalSafePath(basePath, QString(), false);
    if (safeTargetPath.isEmpty()) {
        sendBadRequest(socket, QStringLiteral("刪除目標超出分享範圍。"));
        return;
    }

    if (safeTargetPath.compare(safeRootPath, Qt::CaseInsensitive) == 0) {
        sendBadRequest(socket, QStringLiteral("不可刪除分享根目錄。"));
        return;
    }

    if (!QFileInfo::exists(safeTargetPath)) {
        sendBadRequest(socket, QStringLiteral("要刪除的項目不存在。"));
        return;
    }

    if (!removeFileSystemEntry(safeTargetPath)) {
        sendBadRequest(socket, QStringLiteral("刪除失敗，檔案可能正在使用中。"));
        return;
    }

    emit activityEvent(QStringLiteral("%1 刪除完成：%2")
                           .arg(detectClientAddress(socket), targetRelativePath));
    sendJson(socket, QByteArrayLiteral("{\"ok\":true}"));
}

void HttpFileServer::cleanupUploadState(ConnectionState &state, bool removePartialFile)
{
    const QString filePath = state.uploadFilePath;
    if (state.uploadFile) {
        if (state.uploadFile->isOpen()) {
            state.uploadFile->close();
        }
        delete state.uploadFile;
        state.uploadFile = nullptr;
    }

    if (removePartialFile && !filePath.isEmpty()) {
        QFile::remove(filePath);
    }

    state.buffer.clear();
    state.bodyReceived = 0;
    state.contentLength = 0;
    state.parsedHeaders = false;
    state.method.clear();
    state.target.clear();
    state.httpVersion.clear();
    state.headers.clear();
    state.uploadRequest = false;
    state.uploadFilePath.clear();
    state.uploadFileName.clear();
}

void HttpFileServer::removeChunkUploadSession(const QString &uploadId, bool removePartialFile)
{
    if (uploadId.isEmpty() || !m_chunkUploads.contains(uploadId)) {
        return;
    }

    const ChunkUploadSession session = m_chunkUploads.take(uploadId);
    if (removePartialFile && !session.tempPath.isEmpty()) {
        QFile::remove(session.tempPath);
    }
}

bool HttpFileServer::handleChunkUpload(QTcpSocket *socket,
                                       const QString &requestPath,
                                       const QUrlQuery &query,
                                       const QByteArray &body)
{
    const QString fileName = query.queryItemValue(QStringLiteral("name"), QUrl::FullyDecoded);
    const QString uploadId = query.queryItemValue(QStringLiteral("id"), QUrl::FullyDecoded);
    const int chunkIndex = query.queryItemValue(QStringLiteral("index"), QUrl::FullyDecoded).toInt();
    const int totalChunks = query.queryItemValue(QStringLiteral("total"), QUrl::FullyDecoded).toInt();
    const qint64 totalSize = query.queryItemValue(QStringLiteral("size"), QUrl::FullyDecoded).toLongLong();
    const qint64 chunkSize = query.queryItemValue(QStringLiteral("chunkSize"), QUrl::FullyDecoded).toLongLong();

    QString routeSegment;
    QString currentRelativePath;
    const QStringList segments = requestPath.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (requestPath == QStringLiteral("/__upload")) {
        routeSegment = query.queryItemValue(QStringLiteral("share"), QUrl::FullyDecoded);
        currentRelativePath = query.queryItemValue(QStringLiteral("path"), QUrl::FullyDecoded);
    } else {
        if (segments.isEmpty()) {
            sendBadRequest(socket, QStringLiteral("上傳路徑不正確。"));
            return false;
        }
        routeSegment = urlDecode(segments.first());
        currentRelativePath = joinDecodedSegments(segments, 1);
    }

    if (routeSegment.isEmpty()) {
        sendBadRequest(socket, QStringLiteral("找不到分享項目。"));
        return false;
    }

    const ShareItem *share = findShareByRoute(routeSegment);
    if (!share || !shareAllowsUpload(*share)) {
        sendBadRequest(socket, QStringLiteral("這個分享項目不允許上傳。"));
        return false;
    }

    const QString safeName = QFileInfo(fileName).fileName();
    if (safeName.isEmpty()
        || uploadId.isEmpty()
        || totalChunks <= 0
        || chunkIndex < 0
        || chunkIndex >= totalChunks
        || totalSize <= 0
        || chunkSize <= 0) {
        sendBadRequest(socket, QStringLiteral("上傳分塊參數不正確。"));
        return false;
    }

    const QString basePath = (share->type == ShareType::VirtualDirectory) ? share->storagePath : share->sourcePath;
    const QString safeDirectory = currentRelativePath.isEmpty()
                                      ? basePath
                                      : canonicalSafePath(basePath, currentRelativePath, false);
    const QFileInfo targetDirectoryInfo(safeDirectory);
    if (safeDirectory.isEmpty() || !targetDirectoryInfo.exists() || !targetDirectoryInfo.isDir()) {
        sendBadRequest(socket, QStringLiteral("上傳目錄無效。"));
        return false;
    }

    QDir().mkpath(safeDirectory);

    ChunkUploadSession session;
    if (!m_chunkUploads.contains(uploadId)) {
        session.tempPath = QDir(safeDirectory).filePath(QStringLiteral(".hfs-upload-%1.part").arg(uploadId));
        session.finalPath = QDir(safeDirectory).filePath(safeName);
        session.fileName = safeName;
        session.clientAddress = detectClientAddress(socket);
        session.expectedSize = totalSize;
        session.chunkSize = chunkSize;
        session.totalChunks = totalChunks;
        session.bytesWritten = 0;
        session.receivedChunks.clear();

        QFile::remove(session.tempPath);
        QFile tempInit(session.tempPath);
        if (!tempInit.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            sendBadRequest(socket, QStringLiteral("無法建立暫存上傳檔案。"));
            return false;
        }
        if (!tempInit.resize(totalSize)) {
            tempInit.close();
            QFile::remove(session.tempPath);
            sendBadRequest(socket, QStringLiteral("無法預先配置上傳暫存空間。"));
            return false;
        }
        tempInit.close();
        m_chunkUploads.insert(uploadId, session);
        emit activityEvent(QStringLiteral("%1 開始上傳：%2").arg(session.clientAddress, safeName));
        updateStats();
    }

    session = m_chunkUploads.value(uploadId);
    if (session.totalChunks != totalChunks
        || session.expectedSize != totalSize
        || session.chunkSize != chunkSize
        || session.fileName != safeName) {
        removeChunkUploadSession(uploadId, true);
        sendBadRequest(socket, QStringLiteral("上傳分塊順序錯誤，請重新上傳檔案。"));
        return false;
    }

    QFile tempFile(session.tempPath);
    if (!tempFile.open(QIODevice::ReadWrite)) {
        removeChunkUploadSession(uploadId, true);
        sendBadRequest(socket, QStringLiteral("無法寫入暫存上傳檔案。"));
        return false;
    }

    const qint64 offset = static_cast<qint64>(chunkIndex) * session.chunkSize;
    const qint64 maxAllowedChunkBytes = qMin(session.chunkSize, session.expectedSize - offset);
    if (offset < 0 || offset >= session.expectedSize || body.isEmpty() || body.size() > maxAllowedChunkBytes) {
        tempFile.close();
        removeChunkUploadSession(uploadId, true);
        sendBadRequest(socket, QStringLiteral("上傳分塊內容不正確。"));
        return false;
    }

    if (session.receivedChunks.contains(chunkIndex)) {
        tempFile.close();
        sendJson(socket, QByteArrayLiteral("{\"ok\":true}"));
        return true;
    }

    if (!tempFile.seek(offset)) {
        tempFile.close();
        removeChunkUploadSession(uploadId, true);
        sendBadRequest(socket, QStringLiteral("無法定位上傳分塊位置。"));
        return false;
    }

    const qint64 written = tempFile.write(body);
    tempFile.close();
    if (written != body.size()) {
        removeChunkUploadSession(uploadId, true);
        sendBadRequest(socket, QStringLiteral("寫入上傳分塊失敗。"));
        return false;
    }

    session.bytesWritten += written;
    session.receivedChunks.insert(chunkIndex);
    m_windowBytes += written;
    m_chunkUploads.insert(uploadId, session);
    updateStats();

    if (session.receivedChunks.size() < session.totalChunks) {
        sendJson(socket, QByteArrayLiteral("{\"ok\":true}"));
        return true;
    }

    if (session.bytesWritten != session.expectedSize) {
        removeChunkUploadSession(uploadId, true);
        sendBadRequest(socket, QStringLiteral("上傳檔案大小不一致，請重新上傳。"));
        return false;
    }

    QFile::remove(session.finalPath);
    if (!QFile::rename(session.tempPath, session.finalPath)) {
        removeChunkUploadSession(uploadId, true);
        sendBadRequest(socket, QStringLiteral("完成上傳時無法建立最終檔案。"));
        return false;
    }

    emit activityEvent(QStringLiteral("%1 上傳完成：%2（%3）")
                           .arg(session.clientAddress, session.fileName, humanReadableSize(session.expectedSize)));
    removeChunkUploadSession(uploadId, false);
    updateStats();
    sendJson(socket, QByteArrayLiteral("{\"ok\":true}"));
    return true;
}

void HttpFileServer::sendResponse(QTcpSocket *socket,
                                  int statusCode,
                                  const QByteArray &statusText,
                                  const QByteArray &body,
                                  const QByteArray &contentType)
{
    if (!socket) {
        return;
    }

    QByteArray response;
    response += QByteArrayLiteral("HTTP/1.1 ") + QByteArray::number(statusCode) + QByteArrayLiteral(" ") + statusText + QByteArrayLiteral("\r\n");
    response += QByteArrayLiteral("Connection: close\r\n");
    response += QByteArrayLiteral("Content-Type: ") + contentType + QByteArrayLiteral("\r\n");
    response += QByteArrayLiteral("Content-Length: ") + QByteArray::number(body.size()) + QByteArrayLiteral("\r\n\r\n");
    response += body;
    socket->write(response);
    socket->disconnectFromHost();
}

void HttpFileServer::sendRedirect(QTcpSocket *socket, const QString &target)
{
    if (!socket) {
        return;
    }

    QByteArray response;
    response += QByteArrayLiteral("HTTP/1.1 302 Found\r\n");
    response += QByteArrayLiteral("Location: ") + target.toUtf8() + QByteArrayLiteral("\r\n");
    response += QByteArrayLiteral("Connection: close\r\n");
    response += QByteArrayLiteral("Content-Length: 0\r\n\r\n");
    socket->write(response);
    socket->disconnectFromHost();
}

void HttpFileServer::sendNotFound(QTcpSocket *socket)
{
    sendResponse(socket,
                 404,
                 statusTextFor(404),
                 renderMessagePage(QStringLiteral("找不到內容"),
                                   QStringLiteral("指定的分享項目或路徑不存在，請返回上一頁重新確認。")),
                 QByteArrayLiteral("text/html; charset=utf-8"));
}

void HttpFileServer::sendUnauthorized(QTcpSocket *socket, bool badPassword)
{
    sendResponse(socket, 401, statusTextFor(401), renderLoginPage(badPassword), QByteArrayLiteral("text/html; charset=utf-8"));
}

void HttpFileServer::sendBadRequest(QTcpSocket *socket, const QString &message)
{
    sendResponse(socket,
                 400,
                 statusTextFor(400),
                 renderMessagePage(QStringLiteral("請求無效"), message),
                 QByteArrayLiteral("text/html; charset=utf-8"));
}

void HttpFileServer::sendJson(QTcpSocket *socket, const QByteArray &body)
{
    sendResponse(socket, 200, statusTextFor(200), body, QByteArrayLiteral("application/json; charset=utf-8"));
}

void HttpFileServer::sendPlaylistFile(QTcpSocket *socket,
                                      const ShareItem &share,
                                      const QString &relativePath,
                                      const QString &displayName)
{
    const QString cleanDisplayName = QFileInfo(displayName).fileName().trimmed().isEmpty()
                                         ? QStringLiteral("playlist")
                                         : QFileInfo(displayName).fileName();
    QString playlistName = cleanDisplayName;
    if (!playlistName.endsWith(QStringLiteral(".m3u8"), Qt::CaseInsensitive)) {
        playlistName += QStringLiteral(".m3u8");
    }

    QByteArray body;
    body += QByteArrayLiteral("#EXTM3U\r\n");
    body += QByteArrayLiteral("#EXTINF:-1,");
    body += cleanDisplayName.toUtf8();
    body += QByteArrayLiteral("\r\n");
    body += mediaRelayUrl(share, relativePath).toUtf8();
    body += QByteArrayLiteral("\r\n");

    QByteArray response;
    response += QByteArrayLiteral("HTTP/1.1 200 OK\r\n");
    response += QByteArrayLiteral("Content-Type: application/octet-stream\r\n");
    response += QByteArrayLiteral("Content-Disposition: attachment; filename=\"")
                + playlistName.toUtf8().replace('"', QByteArrayLiteral("_"))
                + QByteArrayLiteral("\"\r\n");
    response += QByteArrayLiteral("Cache-Control: no-store\r\n");
    response += QByteArrayLiteral("Pragma: no-cache\r\n");
    response += QByteArrayLiteral("X-Content-Type-Options: nosniff\r\n");
    response += QByteArrayLiteral("Connection: close\r\n");
    response += QByteArrayLiteral("Content-Length: ") + QByteArray::number(body.size()) + QByteArrayLiteral("\r\n\r\n");
    response += body;
    socket->write(response);
    socket->disconnectFromHost();
}

void HttpFileServer::sendFile(QTcpSocket *socket,
                              const ShareItem &share,
                              const QString &absolutePath,
                              const QString &relativePath,
                              const QString &rangeHeader,
                              bool trackAsDownload,
                              bool inlineDisposition)
{
    if (!socket) {
        return;
    }

    auto *file = new QFile(absolutePath, this);
    if (!file->open(QIODevice::ReadOnly)) {
        delete file;
        sendNotFound(socket);
        return;
    }

    qint64 start = 0;
    qint64 end = file->size() - 1;
    bool partial = false;

    if (m_downloadSettings.resumeEnabled && !rangeHeader.isEmpty()) {
        const QRegularExpression rangePattern(QStringLiteral(R"(bytes=(\d*)-(\d*))"));
        const QRegularExpressionMatch match = rangePattern.match(rangeHeader);
        if (match.hasMatch()) {
            const QString startToken = match.captured(1);
            const QString endToken = match.captured(2);
            if (!startToken.isEmpty()) {
                start = startToken.toLongLong();
            }
            if (!endToken.isEmpty()) {
                end = endToken.toLongLong();
            }
            if (end < start || start < 0 || start >= file->size()) {
                delete file;
                sendResponse(socket, 416, statusTextFor(416), QByteArray(), QByteArrayLiteral("text/plain; charset=utf-8"));
                return;
            }
            if (end >= file->size()) {
                end = file->size() - 1;
            }
            partial = true;
        }
    }

    file->seek(start);
    const qint64 contentLength = end - start + 1;
    const QMimeDatabase database;
    const QByteArray mimeType = database.mimeTypeForFile(absolutePath).name().toUtf8();

    QString downloadName = QFileInfo(absolutePath).fileName();
    downloadName.replace(QLatin1Char('"'), QLatin1Char('_'));

    QByteArray headers;
    headers += QByteArrayLiteral("HTTP/1.1 ")
               + QByteArray::number(partial ? 206 : 200)
               + QByteArrayLiteral(" ")
               + statusTextFor(partial ? 206 : 200)
               + QByteArrayLiteral("\r\n");
    headers += QByteArrayLiteral("Connection: close\r\n");
    headers += QByteArrayLiteral("Accept-Ranges: bytes\r\n");
    headers += QByteArrayLiteral("Content-Type: ")
               + (mimeType.isEmpty() ? QByteArrayLiteral("application/octet-stream") : mimeType)
               + QByteArrayLiteral("\r\n");
    headers += QByteArrayLiteral("Content-Disposition: ")
               + (inlineDisposition ? QByteArrayLiteral("inline; filename=\"")
                                    : QByteArrayLiteral("attachment; filename=\""))
               + downloadName.toUtf8()
               + QByteArrayLiteral("\"\r\n");
    headers += QByteArrayLiteral("Content-Length: ") + QByteArray::number(contentLength) + QByteArrayLiteral("\r\n");
    if (partial) {
        headers += QByteArrayLiteral("Content-Range: bytes ")
                   + QByteArray::number(start)
                   + QByteArrayLiteral("-")
                   + QByteArray::number(end)
                   + QByteArrayLiteral("/")
                   + QByteArray::number(file->size())
                   + QByteArrayLiteral("\r\n");
    }
    headers += QByteArrayLiteral("\r\n");

    socket->write(headers);

    auto *transfer = new FileTransfer{
        socket,
        QString::number(reinterpret_cast<quintptr>(socket)),
        share.id,
        downloadName,
        relativePath,
        detectClientAddress(socket),
        file,
        start,
        end,
        contentLength,
        0,
        true,
        trackAsDownload,
    };
    m_transfers.insert(socket, transfer);
    if (trackAsDownload) {
        emit activityEvent(QStringLiteral("%1 開始下載：%2").arg(transfer->clientAddress, transfer->fileName));
    }
    serviceTransfers();
}

bool HttpFileServer::isAuthenticated(const QMap<QString, QString> &headers) const
{
    if (m_downloadSettings.password.isEmpty()) {
        return true;
    }

    const QString cookieHeader = headerValue(headers, QStringLiteral("cookie"));
    const QStringList cookies = cookieHeader.split(QLatin1Char(';'), Qt::SkipEmptyParts);
    for (const QString &cookie : cookies) {
        const QString trimmed = cookie.trimmed();
        if (trimmed.startsWith(QStringLiteral("hfs_auth="))) {
            return trimmed.mid(9) == authToken();
        }
    }
    return false;
}

QString HttpFileServer::authToken() const
{
    const QByteArray digest =
        QCryptographicHash::hash((m_downloadSettings.password + QStringLiteral("::EasyCloudHFS")).toUtf8(),
                                 QCryptographicHash::Sha1)
            .toHex();
    return QString::fromUtf8(digest);
}

QString HttpFileServer::headerValue(const QMap<QString, QString> &headers, const QString &name) const
{
    return headers.value(name.toLower());
}

QString HttpFileServer::detectClientAddress(QTcpSocket *socket) const
{
    if (!socket) {
        return QStringLiteral("unknown");
    }

    const auto it = m_connections.constFind(socket);
    if (it != m_connections.constEnd()) {
        const QString forwardedFor = headerValue(it->headers, QStringLiteral("x-forwarded-for"));
        if (!forwardedFor.trimmed().isEmpty()) {
            const QStringList parts = forwardedFor.split(QLatin1Char(','), Qt::SkipEmptyParts);
            if (!parts.isEmpty()) {
                const QString client = parts.first().trimmed();
                if (!client.isEmpty()) {
                    return client;
                }
            }
        }

        const QString realIp = headerValue(it->headers, QStringLiteral("cf-connecting-ip"));
        if (!realIp.trimmed().isEmpty()) {
            return realIp.trimmed();
        }
    }

    return socket->peerAddress().toString();
}

QString HttpFileServer::canonicalSafePath(const QString &root, const QString &relativePath, bool allowMissingLeaf) const
{
    const QFileInfo rootInfo(root);
    const QString rootCanonical = rootInfo.canonicalFilePath().isEmpty()
                                      ? QDir::cleanPath(rootInfo.absoluteFilePath())
                                      : QDir::cleanPath(rootInfo.canonicalFilePath());
    QString cleanRelative = QDir::cleanPath(relativePath);
    if (cleanRelative == QStringLiteral(".")) {
        cleanRelative.clear();
    }

    const QString absoluteCandidate = QDir(rootCanonical).absoluteFilePath(cleanRelative);
    const QFileInfo candidateInfo(absoluteCandidate);
    QString candidateCanonical = candidateInfo.canonicalFilePath();
    if (candidateCanonical.isEmpty() && allowMissingLeaf) {
        candidateCanonical = QDir::cleanPath(candidateInfo.absoluteFilePath());
    } else {
        candidateCanonical = QDir::cleanPath(candidateCanonical);
    }

    if (candidateCanonical == rootCanonical) {
        return candidateCanonical;
    }
    if (!candidateCanonical.startsWith(rootCanonical + QLatin1Char('/'), Qt::CaseInsensitive)) {
        return QString();
    }
    return candidateCanonical;
}

QByteArray HttpFileServer::renderHomePage() const
{
#if 0
    QString cards;
    for (const ShareItem &share : rootShares()) {
        const QString typeText =
            share.type == ShareType::File ? QStringLiteral("本機檔案")
            : share.type == ShareType::Directory ? QStringLiteral("真實資料夾")
            : share.type == ShareType::VirtualDirectory ? QStringLiteral("虛擬資料夾")
                                                        : QStringLiteral("連結網址");

        cards += QStringLiteral(
                     "<a class='card' href='%1'>"
                     "<div class='card-title'>%2</div>"
                     "<div class='card-meta'>%3</div>"
                     "<div class='card-sub'>%4</div>"
                     "</a>")
                     .arg(routeForShare(share),
                          escapeHtml(share.name),
                          escapeHtml(typeText),
                          escapeHtml(displaySourcePath(share)));
    }

    if (cards.isEmpty()) {
        cards = QStringLiteral("<div class='empty'>目前還沒有任何分享項目，請回到桌面程式拖曳檔案或資料夾加入分享。</div>");
    }

    const QString hint = m_downloadSettings.password.isEmpty()
                             ? QStringLiteral("可直接瀏覽或下載分享內容")
                             : QStringLiteral("此站台已啟用下載密碼保護");

    const QString html = QStringLiteral(
        "<!doctype html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width, initial-scale=1'>"
        "<title>%1</title>"
        "<style>"
        "body{font-family:'Microsoft JhengHei UI',sans-serif;background:#eef5ff;color:#163152;margin:0;}"
        ".wrap{max-width:1120px;margin:0 auto;padding:36px 22px 42px;}"
        ".hero{display:flex;gap:16px;align-items:center;background:#fff;border-radius:24px;padding:24px;box-shadow:0 12px 30px rgba(27,70,139,.08);}"
        ".hero img{width:72px;height:72px;border-radius:18px;background:#f3f8ff;padding:10px;}"
        ".title{font-size:32px;font-weight:800;margin:0;}"
        ".sub{margin-top:8px;color:#6d83a6;}"
        ".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(260px,1fr));gap:18px;margin-top:24px;}"
        ".card{display:block;text-decoration:none;color:#163152;background:#fff;border-radius:20px;padding:20px;box-shadow:0 10px 24px rgba(27,70,139,.08);transition:transform .18s ease;}"
        ".card:hover{transform:translateY(-2px);}"
        ".card-title{font-size:20px;font-weight:800;margin-bottom:10px;word-break:break-word;}"
        ".card-meta{display:inline-block;background:#e8f4ff;color:#1184dc;padding:6px 10px;border-radius:999px;font-size:12px;font-weight:700;}"
        ".card-sub{margin-top:12px;color:#6d83a6;word-break:break-all;}"
        ".empty{background:#fff;border-radius:20px;padding:22px;color:#6d83a6;box-shadow:0 10px 24px rgba(27,70,139,.08);}"
        "</style></head>"
        "<body><div class='wrap'><div class='hero'><img src='/__logo'><div><h1 class='title'>%1</h1><div class='sub'>%2</div></div></div><div class='grid'>%3</div></div></body></html>")
                             .arg(escapeHtml(m_siteName), hint, cards);
    return html.toUtf8();
#endif
    QString rows;
    QJsonArray galleryItems;
    for (const ShareItem &share : rootShares()) {
        const bool isFolder = share.type == ShareType::Directory || share.type == ShareType::VirtualDirectory;
        const bool isImageShare = share.type == ShareType::File && isImageFilePath(share.sourcePath);
        const bool isAudioShare = share.type == ShareType::File && isAudioFilePath(share.sourcePath);
        const bool isVideoShare = share.type == ShareType::File && isVideoFilePath(share.sourcePath);
        const QString infoText = isFolder ? QStringLiteral("可直接進入資料夾") : humanReadableSize(share.pinnedSize);
        if (isImageShare) {
            const QString href = routeForShare(share);
            const QString previewHref = href + QStringLiteral("?__inline=1");
            const QString viewerHref = href + QStringLiteral("?__viewer=1");

            rows += QStringLiteral(
                        "<div class='row image-row'>"
                        "<a class='row-main-link' href='%1'>"
                        "<img class='thumb' src='%2' alt='%3' loading='lazy' decoding='async'>"
                        "<div class='row-main'>"
                        "<div class='row-name'>%4</div>"
                        "<div class='row-note'>圖片預覽</div>"
                        "</div>"
                        "</a>"
                        "<a class='row-download' href='%5' download>直接下載</a>"
                        "</div>")
                        .arg(escapeHtml(viewerHref),
                             escapeHtml(previewHref),
                             escapeHtml(share.name),
                             escapeHtml(share.name),
                             escapeHtml(href));
            continue;
        }

        if (isAudioShare || isVideoShare) {
            const QString href = routeForShare(share);
            const QString viewerHref = href + QStringLiteral("?__media=1");
            const QString iconClass = isVideoShare ? QStringLiteral("icon video") : QStringLiteral("icon audio");
            const QString iconText = isVideoShare ? QStringLiteral("V") : QStringLiteral("A");
            const QString noteText = isVideoShare ? QStringLiteral("線上播放") : QStringLiteral("音樂播放");

            rows += QStringLiteral(
                        "<div class='row media-row'>"
                        "<a class='row-main-link' href='%1'>"
                        "<div class='%2'>%3</div>"
                        "<div class='row-main'>"
                        "<div class='row-name'>%4</div>"
                        "<div class='row-note'>%5</div>"
                        "</div>"
                        "</a>"
                        "<a class='row-download' href='%6' download>直接下載</a>"
                        "</div>")
                        .arg(escapeHtml(viewerHref),
                             iconClass,
                             iconText,
                             escapeHtml(share.name),
                             noteText,
                             escapeHtml(href));
            continue;
        }

        const QString iconClass = isFolder ? QStringLiteral("icon folder") : QStringLiteral("icon file");
        const QString iconText = isFolder ? QStringLiteral("D") : QStringLiteral("F");

        rows += QStringLiteral(
                    "<a class='row' href='%1'>"
                    "<div class='%2'>%3</div>"
                    "<div class='row-main'>"
                    "<div class='row-name'>%4</div>"
                    "<div class='row-note'>%5</div>"
                    "</div>"
                    "<div class='row-size'>%6</div>"
                    "</a>")
                    .arg(routeForShare(share),
                         iconClass,
                         iconText,
                         escapeHtml(share.name),
                         isFolder ? QStringLiteral("資料夾") : QStringLiteral("檔案"),
                         escapeHtml(infoText));
    }

    if (rows.isEmpty()) {
        rows = QStringLiteral("<div class='empty'>目前尚無可下載的分享項目。</div>");
    }

    const QString hint = m_downloadSettings.password.isEmpty()
                             ? QStringLiteral("可直接瀏覽或下載分享內容")
                             : QStringLiteral("進入前需要輸入下載密碼");

    QString galleryBlock;
    if (false && !galleryItems.isEmpty()) {
        QString galleryJson = QString::fromUtf8(QJsonDocument(galleryItems).toJson(QJsonDocument::Compact));
        galleryJson.replace(QStringLiteral("</"), QStringLiteral("<\\/"));
        galleryBlock = QStringLiteral(
            "<div id='galleryModal' class='gallery-modal' hidden aria-hidden='true'>"
            "<div id='galleryBackdrop' class='gallery-backdrop'></div>"
            "<div class='gallery-shell'>"
            "<button id='galleryClose' type='button' class='gallery-close' aria-label='關閉預覽'>×</button>"
            "<button id='galleryPrev' type='button' class='gallery-nav prev' aria-label='上一張'>‹</button>"
            "<div id='galleryStage' class='gallery-stage'><img id='galleryImage' class='gallery-image' alt=''></div>"
            "<button id='galleryNext' type='button' class='gallery-nav next' aria-label='下一張'>›</button>"
            "<div class='gallery-caption'>"
            "<div><div id='galleryName' class='gallery-name'></div><div id='galleryMeta' class='gallery-meta'></div></div>"
            "<a id='galleryDownload' class='gallery-download' href='#'>下載原圖</a>"
            "</div>"
            "</div>"
            "</div>"
            "<script>"
            "const galleryItems=%1;"
            "let galleryIndex=-1;"
            "const galleryModal=document.getElementById('galleryModal');"
            "const galleryBackdrop=document.getElementById('galleryBackdrop');"
            "const galleryImage=document.getElementById('galleryImage');"
            "const galleryName=document.getElementById('galleryName');"
            "const galleryMeta=document.getElementById('galleryMeta');"
            "const galleryDownload=document.getElementById('galleryDownload');"
            "const galleryPrev=document.getElementById('galleryPrev');"
            "const galleryNext=document.getElementById('galleryNext');"
            "const galleryClose=document.getElementById('galleryClose');"
            "const galleryStage=document.getElementById('galleryStage');"
            "const preloadGallery=(index)=>{"
            "if(index<0||index>=galleryItems.length){return;}"
            "const image=new Image();"
            "image.src=galleryItems[index].src;"
            "};"
            "const renderGallery=()=>{"
            "if(galleryIndex<0||galleryIndex>=galleryItems.length){return;}"
            "const item=galleryItems[galleryIndex];"
            "galleryImage.src=item.src;"
            "galleryImage.alt=item.name;"
            "galleryName.textContent=item.name;"
            "galleryMeta.textContent=(galleryIndex+1)+' / '+galleryItems.length+' · '+item.size;"
            "galleryDownload.href=item.download;"
            "galleryDownload.setAttribute('download',item.name);"
            "const showNav=galleryItems.length>1;"
            "galleryPrev.style.display=showNav?'inline-flex':'none';"
            "galleryNext.style.display=showNav?'inline-flex':'none';"
            "preloadGallery((galleryIndex+1)%%galleryItems.length);"
            "preloadGallery((galleryIndex+galleryItems.length-1)%%galleryItems.length);"
            "};"
            "const openGallery=(index)=>{"
            "galleryIndex=index;"
            "galleryModal.hidden=false;"
            "galleryModal.setAttribute('aria-hidden','false');"
            "document.body.classList.add('gallery-open');"
            "renderGallery();"
            "};"
            "window.hfsOpenGallery=function(index){openGallery(index);return false;};"
            "const closeGallery=()=>{"
            "galleryModal.hidden=true;"
            "galleryModal.setAttribute('aria-hidden','true');"
            "galleryImage.removeAttribute('src');"
            "document.body.classList.remove('gallery-open');"
            "galleryIndex=-1;"
            "};"
            "const stepGallery=(offset)=>{"
            "if(galleryItems.length<2||galleryIndex<0){return;}"
            "galleryIndex=(galleryIndex+offset+galleryItems.length)%%galleryItems.length;"
            "renderGallery();"
            "};"
            "if(galleryPrev){galleryPrev.addEventListener('click',function(){stepGallery(-1);});}"
            "if(galleryNext){galleryNext.addEventListener('click',function(){stepGallery(1);});}"
            "if(galleryClose){galleryClose.addEventListener('click',closeGallery);}"
            "if(galleryBackdrop){galleryBackdrop.addEventListener('click',closeGallery);}"
            "document.addEventListener('keydown',(event)=>{"
            "if(galleryModal.hidden){return;}"
            "if(event.key==='Escape'){closeGallery();}"
            "else if(event.key==='ArrowLeft'){stepGallery(-1);}"
            "else if(event.key==='ArrowRight'){stepGallery(1);}"
            "});"
            "let touchStartX=0;"
            "let touchActive=false;"
            "if(galleryStage){"
            "galleryStage.addEventListener('touchstart',function(event){"
            "if(event.touches.length!==1){return;}"
            "touchStartX=event.touches[0].clientX;"
            "touchActive=true;"
            "},{passive:true});"
            "galleryStage.addEventListener('touchend',function(event){"
            "if(!touchActive||event.changedTouches.length!==1){return;}"
            "const delta=event.changedTouches[0].clientX-touchStartX;"
            "touchActive=false;"
            "if(Math.abs(delta)>=40){stepGallery(delta<0?1:-1);}"
            "},{passive:true});"
            "}"
            "</script>")
                             .arg(galleryJson);
    }

    const QString html = QStringLiteral(
        "<!doctype html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width, initial-scale=1'>"
        "<title>%1</title>"
        "<style>"
        "body{font-family:'Microsoft JhengHei UI',sans-serif;background:#eef5ff;color:#163152;margin:0;}"
        ".wrap{max-width:1120px;margin:0 auto;padding:36px 22px 42px;}"
        ".hero{display:flex;gap:18px;align-items:center;background:#fff;border-radius:24px;padding:24px;box-shadow:0 12px 30px rgba(27,70,139,.08);}"
        ".hero img{width:78px;height:78px;border-radius:20px;background:#f3f8ff;padding:10px;}"
        ".title{font-size:32px;font-weight:800;margin:0;}"
        ".sub{margin-top:8px;color:#6d83a6;}"
        ".list{display:grid;gap:14px;margin-top:24px;}"
        ".row{display:flex;align-items:center;gap:16px;text-decoration:none;color:#163152;background:#fff;border-radius:20px;padding:18px 20px;box-shadow:0 10px 24px rgba(27,70,139,.08);}"
        ".row:hover{transform:translateY(-1px);}"
        ".icon{width:54px;height:54px;display:flex;align-items:center;justify-content:center;border-radius:16px;font-size:22px;font-weight:800;flex:none;}"
        ".icon.folder{background:#fff2c9;color:#d79b00;}"
        ".icon.file{background:#dff1ff;color:#1f9df2;}"
        ".icon.audio{background:#eaf7ec;color:#2d9b52;}"
        ".icon.video{background:#ffe8e3;color:#d96445;}"
        ".thumb{width:54px;height:54px;object-fit:cover;border-radius:16px;flex:none;background:#dff1ff;border:1px solid #dbe6f5;}"
        ".image-row{cursor:default;justify-content:space-between;}"
        ".row-main-link{display:flex;align-items:center;gap:16px;min-width:0;flex:1;text-decoration:none;color:#163152;}"
        ".row-main{min-width:0;flex:1;}"
        ".row-name{font-size:22px;font-weight:800;word-break:break-word;}"
        ".row-note{margin-top:6px;color:#6d83a6;}"
        ".row-size{white-space:nowrap;color:#47658c;font-weight:700;}"
        ".row-download{display:inline-flex;align-items:center;justify-content:center;min-width:112px;padding:12px 16px;border-radius:14px;background:#1f9df2;color:#fff;text-decoration:none;font-weight:800;white-space:nowrap;flex:none;}"
        "body.gallery-open{overflow:hidden;}"
        ".gallery-modal{position:fixed;inset:0;z-index:80;}"
        ".gallery-backdrop{position:absolute;inset:0;background:rgba(12,24,43,.78);backdrop-filter:blur(6px);}"
        ".gallery-shell{position:relative;z-index:1;max-width:min(1120px,96vw);height:min(88vh,920px);margin:6vh auto 0;display:grid;grid-template-columns:auto minmax(0,1fr) auto;grid-template-rows:minmax(0,1fr) auto;gap:16px;align-items:center;}"
        ".gallery-stage{grid-column:2;min-height:0;height:100%%;display:flex;align-items:center;justify-content:center;background:rgba(255,255,255,.08);border:1px solid rgba(255,255,255,.16);border-radius:24px;padding:18px;box-shadow:0 18px 50px rgba(0,0,0,.24);}"
        ".gallery-image{max-width:100%%;max-height:100%%;object-fit:contain;border-radius:18px;background:#fff;}"
        ".gallery-nav,.gallery-close{border:0;background:rgba(255,255,255,.94);color:#163152;display:inline-flex;align-items:center;justify-content:center;cursor:pointer;box-shadow:0 10px 30px rgba(0,0,0,.2);}"
        ".gallery-nav{width:58px;height:58px;border-radius:999px;font-size:38px;line-height:1;}"
        ".gallery-close{position:absolute;top:0;right:0;transform:translate(12px,-12px);width:52px;height:52px;border-radius:999px;font-size:34px;z-index:2;}"
        ".gallery-caption{grid-column:1 / span 3;display:flex;align-items:center;justify-content:space-between;gap:16px;background:#fff;border-radius:22px;padding:16px 18px;box-shadow:0 12px 28px rgba(0,0,0,.18);}"
        ".gallery-name{font-size:20px;font-weight:800;word-break:break-all;}"
        ".gallery-meta{margin-top:6px;color:#6d83a6;}"
        ".gallery-download{display:inline-flex;align-items:center;justify-content:center;min-width:120px;padding:12px 18px;border-radius:14px;background:#1f9df2;color:#fff;text-decoration:none;font-weight:800;}"
        ".empty{background:#fff;border-radius:20px;padding:22px;color:#6d83a6;box-shadow:0 10px 24px rgba(27,70,139,.08);}"
        "@media (max-width:720px){"
        ".row{align-items:flex-start;}"
        ".row-main-link{align-items:flex-start;}"
        ".row-size{padding-top:4px;}"
        ".row-download{min-width:96px;padding:10px 14px;}"
        ".thumb,.icon{width:48px;height:48px;border-radius:14px;}"
        ".gallery-shell{width:94vw;height:84vh;grid-template-columns:1fr;grid-template-rows:minmax(0,1fr) auto;gap:12px;margin:5vh auto 0;}"
        ".gallery-stage{grid-column:1;padding:12px;}"
        ".gallery-caption{grid-column:1;flex-direction:column;align-items:flex-start;padding-right:78px;}"
        ".gallery-nav{position:absolute;top:50%%;transform:translateY(-50%%);z-index:2;width:50px;height:50px;font-size:32px;}"
        ".gallery-nav.prev{left:8px;}"
        ".gallery-nav.next{right:8px;}"
        ".gallery-close{top:8px;right:8px;transform:none;width:46px;height:46px;font-size:30px;}"
        "}"
        "</style></head>"
        "<body><div class='wrap'><div class='hero'><img src='/__logo'><div><h1 class='title'>%1</h1><div class='sub'>%2</div></div></div><div class='list'>%3</div>%4</div></body></html>")
                             .arg(escapeHtml(m_siteName), hint, rows, galleryBlock);
    return html.toUtf8();
}

QByteArray HttpFileServer::renderLoginPage(bool badPassword) const
{
    const QString alert = badPassword
                              ? QStringLiteral("<div class='warn'>密碼錯誤，請重新輸入。</div>")
                              : QString();
    const QString html = QStringLiteral(
        "<!doctype html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width, initial-scale=1'>"
        "<title>%1</title>"
        "<style>"
        "body{font-family:'Microsoft JhengHei UI',sans-serif;background:#eef5ff;color:#163152;display:flex;align-items:center;justify-content:center;min-height:100vh;margin:0;}"
        ".box{width:min(420px,92vw);background:#fff;padding:30px;border-radius:24px;box-shadow:0 16px 40px rgba(27,70,139,.12);}"
        "h1{margin:0 0 12px;font-size:28px;}"
        "p{color:#6d83a6;line-height:1.7;}"
        "input{width:100%%;padding:14px 16px;border-radius:14px;border:1px solid #dbe6f5;background:#f7fbff;font-size:16px;box-sizing:border-box;}"
        "button{margin-top:16px;width:100%%;padding:14px 18px;border:0;border-radius:14px;background:#1f9df2;color:#fff;font-size:17px;font-weight:800;cursor:pointer;}"
        ".warn{background:#fff3f1;color:#d85041;padding:10px 14px;border-radius:12px;margin-bottom:12px;}"
        "</style></head>"
        "<body><div class='box'><h1>%1</h1><p>此分享頁面已加上下載密碼。請輸入正確密碼後繼續。</p>%2"
        "<form method='post' action='/__auth'>"
        "<input name='password' type='password' placeholder='請輸入下載密碼'>"
        "<button type='submit'>進入下載頁面</button>"
        "</form></div></body></html>")
                             .arg(escapeHtml(m_siteName), alert);
    return html.toUtf8();
}

QByteArray HttpFileServer::renderDirectoryPage(const ShareItem &share,
                                               const QString &rootPath,
                                               const QString &relativePath,
                                               const QUrlQuery &query) const
{
    const QString safeCurrent = relativePath.isEmpty() ? rootPath : canonicalSafePath(rootPath, relativePath, false);
    if (safeCurrent.isEmpty()) {
        return renderMessagePage(QStringLiteral("路徑錯誤"),
                                 QStringLiteral("請求的目錄超出分享範圍。"));
    }

    const QDir dir(safeCurrent);
    if (!dir.exists()) {
        return renderMessagePage(QStringLiteral("資料夾不存在"),
                                 QStringLiteral("這個目錄目前已不存在，請返回上一頁。"));
    }

    QString parentLink = QStringLiteral("/");
    if (!relativePath.isEmpty()) {
        const QString parentRelative = directoryOfPath(relativePath);
        parentLink = routeForShare(share);
        if (!parentRelative.isEmpty()) {
            parentLink += QStringLiteral("/") + urlEncodePath(parentRelative);
        }
    }

    const DirectorySortKey sortKey =
        parseDirectorySortKey(query.queryItemValue(QStringLiteral("sort"), QUrl::FullyDecoded));
    const bool sortDescending =
        parseDirectorySortDescending(query.queryItemValue(QStringLiteral("order"), QUrl::FullyDecoded));
    const QString currentPageHref = relativePath.isEmpty()
                                        ? routeForShare(share)
                                        : routeForShare(share) + QStringLiteral("/") + urlEncodePath(relativePath);
    const bool deleteAllowed = shareAllowsDelete(share);
    const bool createDirectoryAllowed = shareAllowsCreateDirectory(share);
    const auto nextOrderForKey = [sortKey, sortDescending](DirectorySortKey key) {
        if (sortKey == key) {
            return !sortDescending;
        }
        return key == DirectorySortKey::Name ? false : true;
    };

    QString itemsHtml;
    QJsonArray galleryItems;
    QFileInfoList entries = dir.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot, QDir::NoSort);
    std::sort(entries.begin(), entries.end(), [sortKey, sortDescending](const QFileInfo &left, const QFileInfo &right) {
        if (left.isDir() != right.isDir()) {
            return left.isDir();
        }

        const int compare = compareDirectoryEntries(left, right, sortKey);
        if (compare == 0) {
            return left.fileName().localeAwareCompare(right.fileName()) < 0;
        }
        return sortDescending ? compare > 0 : compare < 0;
    });

    const QString currentSortSuffix = QStringLiteral("?sort=%1&order=%2")
                                          .arg(directorySortKeyValue(sortKey), directorySortOrderValue(sortDescending));
    const QString parentLinkWithSort = parentLink + currentSortSuffix;
    for (const QFileInfo &entry : entries) {
        const bool isDirectory = entry.isDir();
        const QString childRelative = relativePath.isEmpty()
                                          ? entry.fileName()
                                          : relativePath + QStringLiteral("/") + entry.fileName();
        const QString href = routeForShare(share) + QStringLiteral("/") + urlEncodePath(childRelative);
        const QString navigableHref = isDirectory ? href + currentSortSuffix : href;
        const bool isAudioEntry = !isDirectory && isAudioFilePath(entry.absoluteFilePath());
        const bool isVideoEntry = !isDirectory && isVideoFilePath(entry.absoluteFilePath());
        if (!isDirectory && isImageFilePath(entry.absoluteFilePath())) {
            const QString previewHref = href + QStringLiteral("?__inline=1");
            const QString viewerHref = href + QStringLiteral("?__viewer=1&sort=%1&order=%2")
                                                 .arg(directorySortKeyValue(sortKey), directorySortOrderValue(sortDescending));

            itemsHtml += QStringLiteral(
                             "<div class='row selectable-row image-row' data-kind='file' data-path='%1' data-download='%2'>"
                             "<div class='row-select'><input class='row-check' type='checkbox' aria-label='選取 %3'></div>"
                             "<a class='row-main-link' href='%4'>"
                             "<img class='thumb' src='%5' alt='%3' loading='lazy' decoding='async'>"
                             "<div class='row-main'>"
                             "<div class='row-name'>%3</div>"
                             "<div class='row-note'>圖片預覽</div>"
                             "</div>"
                             "</a>"
                             "<a class='row-download' href='%2' download onclick='event.stopPropagation()'>直接下載</a>"
                             "</div>")
                             .arg(escapeHtml(childRelative),
                                  escapeHtml(href),
                                  escapeHtml(entry.fileName()),
                                  escapeHtml(viewerHref),
                                  escapeHtml(previewHref));
            continue;
        }

        if (isAudioEntry || isVideoEntry) {
            const QString viewerHref = href + QStringLiteral("?__media=1&sort=%1&order=%2")
                                                 .arg(directorySortKeyValue(sortKey), directorySortOrderValue(sortDescending));
            const QString iconClass = isVideoEntry ? QStringLiteral("icon video") : QStringLiteral("icon audio");
            const QString iconText = isVideoEntry ? QStringLiteral("V") : QStringLiteral("A");
            const QString noteText = isVideoEntry ? QStringLiteral("線上播放") : QStringLiteral("音樂播放");

            itemsHtml += QStringLiteral(
                             "<div class='row selectable-row media-row' data-kind='file' data-path='%1' data-download='%2'>"
                             "<div class='row-select'><input class='row-check' type='checkbox' aria-label='選取 %3'></div>"
                             "<a class='row-main-link' href='%4'>"
                             "<div class='%5'>%6</div>"
                             "<div class='row-main'>"
                             "<div class='row-name'>%3</div>"
                             "<div class='row-note'>%7</div>"
                             "</div>"
                             "</a>"
                             "<a class='row-download' href='%2' download onclick='event.stopPropagation()'>直接下載</a>"
                             "</div>")
                             .arg(escapeHtml(childRelative),
                                  escapeHtml(href),
                                  escapeHtml(entry.fileName()),
                                  escapeHtml(viewerHref),
                                  iconClass,
                                  iconText,
                                  noteText);
            continue;
        }

        if (isDirectory) {
            itemsHtml += QStringLiteral(
                             "<div class='row selectable-row' data-kind='directory' data-path='%1'>"
                             "<div class='row-select'><input class='row-check' type='checkbox' aria-label='選取 %2'></div>"
                             "<a class='row-main-link' href='%3'>"
                             "<div class='icon folder'>D</div>"
                             "<div class='row-main'>"
                             "<div class='row-name'>%2</div>"
                             "<div class='row-note'>資料夾</div>"
                             "</div>"
                             "</a>"
                             "<div class='row-size'>資料夾</div>"
                             "</div>")
                             .arg(escapeHtml(childRelative),
                                  escapeHtml(entry.fileName()),
                                  escapeHtml(navigableHref));
            continue;
        }

        itemsHtml += QStringLiteral(
                         "<div class='row selectable-row' data-kind='file' data-path='%1' data-download='%2'>"
                         "<div class='row-select'><input class='row-check' type='checkbox' aria-label='選取 %3'></div>"
                         "<a class='row-main-link' href='%2'>"
                         "<div class='icon file'>F</div>"
                         "<div class='row-main'>"
                         "<div class='row-name'>%3</div>"
                         "<div class='row-note'>一般檔案</div>"
                         "</div>"
                         "</a>"
                         "<div class='row-size'>%4</div>"
                         "</div>")
                         .arg(escapeHtml(childRelative),
                              escapeHtml(href),
                              escapeHtml(entry.fileName()),
                              escapeHtml(humanReadableSize(entry.size())));
    }

    if (itemsHtml.isEmpty()) {
        itemsHtml = QStringLiteral("<div class='empty'>這個資料夾目前沒有任何檔案。</div>");
    }

    QString uploadBlock;
    if (shareAllowsUpload(share)) {
        uploadBlock = QStringLiteral(
            "<div class='upload-box'>"
            "<div class='upload-title'>上傳檔案</div>"
            "<input id='uploader' type='file' multiple>"
            "<div class='upload-note'>可一次選擇多個檔案，完成後會自動重新整理清單。</div>"
            "<div id='uploadLog' class='upload-log'></div>"
            "</div>"
            "<script>"
            "const picker=document.getElementById('uploader');"
            "const log=document.getElementById('uploadLog');"
            "const uploadBase='%1';"
            "const externalChunkThreshold=80*1024*1024;"
            "const externalChunkSize=64*1024*1024;"
            "const externalChunkConcurrency=2;"
            "const externalChunkMaxRetries=3;"
            "const externalChunkRetryDelayMs=1200;"
            "const isProxyExternalUpload=/^\\/(?:PHONE|HFS)(?:\\/|$)/i.test(location.pathname||'');"
            "const ensureRow=(name)=>{"
            "const safe='upload-'+name.replace(/[^\\w.-]+/g,'_');"
            "let row=document.getElementById(safe);"
            "if(!row){"
            "row=document.createElement('div');"
            "row.id=safe;"
            "row.className='upload-row progress';"
            "row.innerHTML=\"<div class='upload-row-title'></div><div class='upload-row-bar'><div class='upload-row-fill'></div></div>\";"
            "log.appendChild(row);"
            "}"
            "return row;"
            "};"
            "const updateRow=(name,percent,text,state)=>{"
            "const row=ensureRow(name);"
            "row.className='upload-row '+state;"
            "row.querySelector('.upload-row-title').textContent=text;"
            "row.querySelector('.upload-row-fill').style.width=Math.max(0,Math.min(100,percent))+'%';"
            "};"
            "const makeUploadId=()=>{"
            "if(window.crypto&&typeof window.crypto.randomUUID==='function'){return window.crypto.randomUUID();}"
            "return 'upload-'+Date.now().toString(36)+'-'+Math.random().toString(36).slice(2);"
            "};"
            "const setProgressText=(fileName,loaded,total,prefix)=>{"
            "const percent=total>0?Math.min(100,Math.round((loaded/total)*100)):100;"
            "updateRow(fileName,percent,prefix+fileName+' ('+percent+'%)','progress');"
            "};"
            "const delay=(ms)=>new Promise(resolve=>setTimeout(resolve,ms));"
            "const shouldUseChunkUpload=(file)=>isProxyExternalUpload&&file.size>externalChunkThreshold;"
            "async function uploadChunk(file,uploadId,chunkIndex,totalChunks,chunkSize,progress){"
            "const start=chunkIndex*chunkSize;"
            "const end=Math.min(file.size,start+chunkSize);"
            "const blob=file.slice(start,end);"
            "const uploadUrl=uploadBase+'&name='+encodeURIComponent(file.name)"
            "+'&__upload_chunk=1&id='+encodeURIComponent(uploadId)"
            "+'&index='+chunkIndex"
            "+'&total='+totalChunks"
            "+'&size='+file.size"
            "+'&chunkSize='+chunkSize;"
            "let lastError=null;"
            "for(let attempt=1;attempt<=externalChunkMaxRetries;attempt++){"
            "try{"
            "await new Promise((resolve,reject)=>{"
            "const xhr=new XMLHttpRequest();"
            "xhr.open('POST',uploadUrl,true);"
            "xhr.upload.onprogress=(event)=>{"
            "progress[chunkIndex]=Math.min(blob.size,Math.max(0,event.loaded||0));"
            "const loaded=progress.reduce((sum,value)=>sum+value,0);"
            "setProgressText(file.name,loaded,file.size,attempt>1?'分段重試中：':'分段上傳中：');"
            "};"
            "xhr.onload=()=>{"
            "if(xhr.status>=200&&xhr.status<300){"
            "progress[chunkIndex]=blob.size;"
            "const loaded=progress.reduce((sum,value)=>sum+value,0);"
            "setProgressText(file.name,loaded,file.size,'分段上傳中：');"
            "resolve();"
            "}else{"
            "const detail=(xhr.responseText||'').replace(/<[^>]+>/g,' ').replace(/\\s+/g,' ').trim().slice(0,220);"
            "reject(new Error(detail?('HTTP '+xhr.status+' - '+detail):('HTTP '+xhr.status)));"
            "}"
            "};"
            "xhr.onerror=()=>reject(new Error('network'));"
            "xhr.send(blob);"
            "});"
            "return;"
            "}catch(error){"
            "lastError=error;"
            "progress[chunkIndex]=0;"
            "const loaded=progress.reduce((sum,value)=>sum+value,0);"
            "setProgressText(file.name,loaded,file.size,attempt<externalChunkMaxRetries?'分段重試中：':'分段上傳中：');"
            "if(attempt<externalChunkMaxRetries){await delay(externalChunkRetryDelayMs*attempt);}"
            "}"
            "}"
            "throw lastError||new Error('chunk failed');"
            "}"
            "async function uploadChunkedFile(file){"
            "const chunkSize=externalChunkSize;"
            "const totalChunks=Math.max(1,Math.ceil(file.size/chunkSize));"
            "const progress=new Array(totalChunks).fill(0);"
            "const uploadId=makeUploadId();"
            "let nextChunkIndex=0;"
            "let abortedError=null;"
            "setProgressText(file.name,0,file.size,'分段上傳中：');"
            "const worker=async()=>{"
            "while(true){"
            "if(abortedError){return;}"
            "const currentIndex=nextChunkIndex++;"
            "if(currentIndex>=totalChunks){return;}"
            "try{"
            "await uploadChunk(file,uploadId,currentIndex,totalChunks,chunkSize,progress);"
            "}catch(error){"
            "abortedError=error;"
            "throw error;"
            "}"
            "}"
            "};"
            "const workerCount=Math.min(externalChunkConcurrency,totalChunks);"
            "await Promise.all(Array.from({length:workerCount},()=>worker()));"
            "if(abortedError){throw abortedError;}"
            "}"
            "async function uploadFile(file){"
            "if(shouldUseChunkUpload(file)){"
            "await uploadChunkedFile(file);"
            "return;"
            "}"
            "const uploadUrl=uploadBase+'&name='+encodeURIComponent(file.name);"
            "updateRow(file.name,0,'上傳中：'+file.name+' (0%)','progress');"
            "await new Promise((resolve,reject)=>{"
            "const xhr=new XMLHttpRequest();"
            "xhr.open('POST',uploadUrl,true);"
            "xhr.upload.onprogress=(event)=>{"
            "if(event.lengthComputable){"
            "const percent=file.size>0?Math.min(100,Math.round((event.loaded/file.size)*100)):100;"
            "updateRow(file.name,percent,'上傳中：'+file.name+' ('+percent+'%)','progress');"
            "}"
            "};"
            "xhr.onload=()=>{"
            "if(xhr.status>=200&&xhr.status<300){resolve();}"
            "else{"
            "const detail=(xhr.responseText||'').replace(/\\s+/g,' ').trim().slice(0,180);"
            "reject(new Error(detail?('HTTP '+xhr.status+' - '+detail):('HTTP '+xhr.status)));"
            "}"
            "};"
            "xhr.onerror=()=>reject(new Error('network'));"
            "xhr.send(file);"
            "});"
            "}"
            "picker?.addEventListener('change', async ()=>{"
            "let anySuccess=false;"
            "for(const file of picker.files){"
            "try{await uploadFile(file);updateRow(file.name,100,'完成：'+file.name+' (100%)','done');anySuccess=true;}"
            "catch(err){updateRow(file.name,100,'失敗：'+file.name+' ('+(err&&err.message?err.message:'unknown')+')','error');}"
            "}"
            "picker.value='';"
            "if(anySuccess){setTimeout(()=>location.reload(),500);}"
            "});"
            "</script>")
                          .arg(QStringLiteral("/__upload?share=%1&path=%2")
                                   .arg(urlEncode(share.routeSegment), urlEncode(relativePath)));
    }

    const QString selectionConfigJson = QString::fromUtf8(
        QJsonDocument(QJsonObject{
                          {QStringLiteral("share"), share.routeSegment},
                          {QStringLiteral("currentPath"), relativePath},
                          {QStringLiteral("deleteAllowed"), deleteAllowed},
                          {QStringLiteral("createDirectoryAllowed"), createDirectoryAllowed},
                      })
            .toJson(QJsonDocument::Compact));

    const QString actionBar = QStringLiteral(
                                  "<div class='sort-group action-group'>"
                                  "<button id='selectionDelete' class='sort-chip action-btn' type='button' disabled>刪除所選</button>"
                                  "<button id='selectionDownload' class='sort-chip action-btn' type='button' disabled>下載所選</button>"
                                  "<button id='selectionMkdir' class='sort-chip action-btn' type='button' %1>建立資料夾</button>"
                                  "<span id='selectionSummary' class='sort-label selection-summary'>未選取項目</span>"
                                  "</div>")
                                  .arg(createDirectoryAllowed ? QString() : QStringLiteral("disabled"));

    QString galleryBlock;
    if (false && !galleryItems.isEmpty()) {
        QString galleryJson = QString::fromUtf8(QJsonDocument(galleryItems).toJson(QJsonDocument::Compact));
        galleryJson.replace(QStringLiteral("</"), QStringLiteral("<\\/"));
        galleryBlock = QStringLiteral(
            "<div id='galleryModal' class='gallery-modal' hidden aria-hidden='true'>"
            "<div id='galleryBackdrop' class='gallery-backdrop'></div>"
            "<div class='gallery-shell'>"
            "<button id='galleryClose' type='button' class='gallery-close' aria-label='關閉預覽'>×</button>"
            "<button id='galleryPrev' type='button' class='gallery-nav prev' aria-label='上一張'>‹</button>"
            "<div id='galleryStage' class='gallery-stage'><img id='galleryImage' class='gallery-image' alt=''></div>"
            "<button id='galleryNext' type='button' class='gallery-nav next' aria-label='下一張'>›</button>"
            "<div class='gallery-caption'>"
            "<div><div id='galleryName' class='gallery-name'></div><div id='galleryMeta' class='gallery-meta'></div></div>"
            "<a id='galleryDownload' class='gallery-download' href='#'>下載原圖</a>"
            "</div>"
            "</div>"
            "</div>"
            "<script>"
            "const galleryItems=%1;"
            "let galleryIndex=-1;"
            "const galleryModal=document.getElementById('galleryModal');"
            "const galleryBackdrop=document.getElementById('galleryBackdrop');"
            "const galleryImage=document.getElementById('galleryImage');"
            "const galleryName=document.getElementById('galleryName');"
            "const galleryMeta=document.getElementById('galleryMeta');"
            "const galleryDownload=document.getElementById('galleryDownload');"
            "const galleryPrev=document.getElementById('galleryPrev');"
            "const galleryNext=document.getElementById('galleryNext');"
            "const galleryClose=document.getElementById('galleryClose');"
            "const galleryStage=document.getElementById('galleryStage');"
            "const preloadGallery=(index)=>{"
            "if(index<0||index>=galleryItems.length){return;}"
            "const image=new Image();"
            "image.src=galleryItems[index].src;"
            "};"
            "const renderGallery=()=>{"
            "if(galleryIndex<0||galleryIndex>=galleryItems.length){return;}"
            "const item=galleryItems[galleryIndex];"
            "galleryImage.src=item.src;"
            "galleryImage.alt=item.name;"
            "galleryName.textContent=item.name;"
            "galleryMeta.textContent=(galleryIndex+1)+' / '+galleryItems.length+' · '+item.size;"
            "galleryDownload.href=item.download;"
            "galleryDownload.setAttribute('download',item.name);"
            "const showNav=galleryItems.length>1;"
            "galleryPrev.style.display=showNav?'inline-flex':'none';"
            "galleryNext.style.display=showNav?'inline-flex':'none';"
            "preloadGallery((galleryIndex+1)%%galleryItems.length);"
            "preloadGallery((galleryIndex+galleryItems.length-1)%%galleryItems.length);"
            "};"
            "const openGallery=(index)=>{"
            "galleryIndex=index;"
            "galleryModal.hidden=false;"
            "galleryModal.setAttribute('aria-hidden','false');"
            "document.body.classList.add('gallery-open');"
            "renderGallery();"
            "};"
            "const closeGallery=()=>{"
            "galleryModal.hidden=true;"
            "galleryModal.setAttribute('aria-hidden','true');"
            "galleryImage.removeAttribute('src');"
            "document.body.classList.remove('gallery-open');"
            "galleryIndex=-1;"
            "};"
            "const stepGallery=(offset)=>{"
            "if(galleryItems.length<2||galleryIndex<0){return;}"
            "galleryIndex=(galleryIndex+offset+galleryItems.length)%%galleryItems.length;"
            "renderGallery();"
            "};"
            "document.querySelectorAll('.image-row[data-gallery-index]').forEach((row)=>{"
            "row.addEventListener('click',(event)=>{"
            "if(event.defaultPrevented||event.button!==0||event.metaKey||event.ctrlKey||event.shiftKey||event.altKey){return;}"
            "event.preventDefault();"
            "openGallery(Number(row.dataset.galleryIndex));"
            "});"
            "});"
            "galleryPrev?.addEventListener('click',()=>stepGallery(-1));"
            "galleryNext?.addEventListener('click',()=>stepGallery(1));"
            "galleryClose?.addEventListener('click',closeGallery);"
            "galleryBackdrop?.addEventListener('click',closeGallery);"
            "document.addEventListener('keydown',(event)=>{"
            "if(galleryModal.hidden){return;}"
            "if(event.key==='Escape'){closeGallery();}"
            "else if(event.key==='ArrowLeft'){stepGallery(-1);}"
            "else if(event.key==='ArrowRight'){stepGallery(1);}"
            "});"
            "let touchStartX=0;"
            "let touchActive=false;"
            "galleryStage?.addEventListener('touchstart',(event)=>{"
            "if(event.touches.length!==1){return;}"
            "touchStartX=event.touches[0].clientX;"
            "touchActive=true;"
            "},{passive:true});"
            "galleryStage?.addEventListener('touchend',(event)=>{"
            "if(!touchActive||event.changedTouches.length!==1){return;}"
            "const delta=event.changedTouches[0].clientX-touchStartX;"
            "touchActive=false;"
            "if(Math.abs(delta)>=40){stepGallery(delta<0?1:-1);}"
            "},{passive:true});"
            "</script>")
                             .arg(galleryJson);
    }

    const QString currentLabel = relativePath.isEmpty() ? QStringLiteral("根目錄") : relativePath;
    const QString sortBar = QStringLiteral(
                                "<div class='sort-bar'>"
                                "%1"
                                "<div class='sort-cluster'>"
                                "<div class='sort-group'>"
                                "<span class='sort-label'>排序</span>"
                                "<a class='sort-chip %2' href='%3'>檔名</a>"
                                "<a class='sort-chip %4' href='%5'>日期</a>"
                                "<a class='sort-chip %6' href='%7'>大小</a>"
                                "</div>"
                                "<div class='sort-group'>"
                                "<a class='sort-chip %8' href='%9'>升冪</a>"
                                "<a class='sort-chip %10' href='%11'>降冪</a>"
                                "</div>"
                                "</div>"
                                "</div>")
                                .arg(actionBar,
                                     sortKey == DirectorySortKey::Name ? QStringLiteral("active") : QString(),
                                     escapeHtml(buildDirectorySortHref(currentPageHref,
                                                                       DirectorySortKey::Name,
                                                                       nextOrderForKey(DirectorySortKey::Name))),
                                     sortKey == DirectorySortKey::Date ? QStringLiteral("active") : QString(),
                                     escapeHtml(buildDirectorySortHref(currentPageHref,
                                                                       DirectorySortKey::Date,
                                                                       nextOrderForKey(DirectorySortKey::Date))),
                                     sortKey == DirectorySortKey::Size ? QStringLiteral("active") : QString(),
                                     escapeHtml(buildDirectorySortHref(currentPageHref,
                                                                       DirectorySortKey::Size,
                                                                       nextOrderForKey(DirectorySortKey::Size))),
                                     !sortDescending ? QStringLiteral("active") : QString(),
                                     escapeHtml(buildDirectorySortHref(currentPageHref, sortKey, false)),
                                     sortDescending ? QStringLiteral("active") : QString(),
                                     escapeHtml(buildDirectorySortHref(currentPageHref, sortKey, true)));
    const QString selectionScript = QStringLiteral(
                                        "<script>"
                                        "const hfsSelectionConfig=%1;"
                                        "const selectionDelete=document.getElementById('selectionDelete');"
                                        "const selectionDownload=document.getElementById('selectionDownload');"
                                        "const selectionMkdir=document.getElementById('selectionMkdir');"
                                        "const selectionSummary=document.getElementById('selectionSummary');"
                                        "const selectionInputs=Array.from(document.querySelectorAll('.row-check'));"
                                        "const getSelectedRows=()=>selectionInputs.filter((input)=>input.checked).map((input)=>input.closest('.selectable-row')).filter(Boolean);"
                                        "const readError=async(response)=>{const text=(await response.text()).replace(/\\s+/g,' ').trim().slice(0,180);return text||('HTTP '+response.status);};"
                                        "const updateSelectionState=()=>{"
                                        "const rows=getSelectedRows();"
                                        "const count=rows.length;"
                                        "const hasDirectory=rows.some((row)=>row.dataset.kind==='directory');"
                                        "const canDelete=Boolean(hfsSelectionConfig.deleteAllowed)&&count>0;"
                                        "const canDownload=count>0&&!hasDirectory;"
                                        "if(selectionDelete){selectionDelete.disabled=!canDelete;selectionDelete.textContent=count>0?'刪除所選 ('+count+')':'刪除所選';}"
                                        "if(selectionDownload){selectionDownload.disabled=!canDownload;selectionDownload.textContent=count>0?'下載所選 ('+count+')':'下載所選';}"
                                        "if(selectionSummary){selectionSummary.textContent=count>0?'已選取 '+count+' 項':(hfsSelectionConfig.createDirectoryAllowed?'可建立資料夾':'未選取項目');}"
                                        "};"
                                        "selectionInputs.forEach((input)=>{"
                                        "input.addEventListener('click',(event)=>event.stopPropagation());"
                                        "input.addEventListener('change',updateSelectionState);"
                                        "});"
                                        "document.querySelectorAll('.row-select').forEach((node)=>node.addEventListener('click',(event)=>event.stopPropagation()));"
                                        "selectionDownload?.addEventListener('click',()=>{"
                                        "const rows=getSelectedRows().filter((row)=>row.dataset.kind==='file');"
                                        "let delay=0;"
                                        "rows.forEach((row)=>{"
                                        "const href=row.dataset.download;"
                                        "if(!href){return;}"
                                        "window.setTimeout(()=>{"
                                        "const iframe=document.createElement('iframe');"
                                        "iframe.style.display='none';"
                                        "iframe.src=href;"
                                        "document.body.appendChild(iframe);"
                                        "window.setTimeout(()=>iframe.remove(),4000);"
                                        "},delay);"
                                        "delay+=220;"
                                        "});"
                                        "});"
                                        "selectionDelete?.addEventListener('click',async()=>{"
                                        "const rows=getSelectedRows();"
                                        "if(selectionDelete.disabled||rows.length===0){return;}"
                                        "if(!window.confirm('確定要刪除已選取的 '+rows.length+' 個項目嗎？')){return;}"
                                        "const originalLabel=selectionDelete.textContent;"
                                        "selectionDelete.disabled=true;"
                                        "selectionDelete.textContent='刪除中...';"
                                        "try{"
                                        "for(const row of rows){"
                                        "const response=await fetch('/__delete?share='+encodeURIComponent(hfsSelectionConfig.share)+'&path='+encodeURIComponent(row.dataset.path||''),{method:'POST'});"
                                        "if(!response.ok){throw new Error(await readError(response));}"
                                        "}"
                                        "window.location.reload();"
                                        "}catch(error){"
                                        "window.alert('刪除失敗：'+(error&&error.message?error.message:'unknown'));"
                                        "selectionDelete.textContent=originalLabel;"
                                        "updateSelectionState();"
                                        "}"
                                        "});"
                                        "selectionMkdir?.addEventListener('click',async()=>{"
                                        "if(selectionMkdir.disabled||!hfsSelectionConfig.createDirectoryAllowed){return;}"
                                        "const name=window.prompt('請輸入新資料夾名稱');"
                                        "if(name===null){return;}"
                                        "const trimmed=(name||'').trim();"
                                        "if(!trimmed){window.alert('資料夾名稱不可為空。');return;}"
                                        "const originalLabel=selectionMkdir.textContent;"
                                        "selectionMkdir.disabled=true;"
                                        "selectionMkdir.textContent='建立中...';"
                                        "try{"
                                        "const response=await fetch('/__mkdir?share='+encodeURIComponent(hfsSelectionConfig.share)+'&path='+encodeURIComponent(hfsSelectionConfig.currentPath||'')+'&name='+encodeURIComponent(trimmed),{method:'POST'});"
                                        "if(!response.ok){throw new Error(await readError(response));}"
                                        "window.location.reload();"
                                        "}catch(error){"
                                        "window.alert('建立資料夾失敗：'+(error&&error.message?error.message:'unknown'));"
                                        "selectionMkdir.textContent=originalLabel;"
                                        "selectionMkdir.disabled=!hfsSelectionConfig.createDirectoryAllowed;"
                                        "}"
                                        "});"
                                        "updateSelectionState();"
                                        "</script>")
                                        .arg(selectionConfigJson);
    const QString html = QStringLiteral(
        "<!doctype html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width, initial-scale=1'>"
        "<title>%1 - %2</title>"
        "<style>"
        "body{font-family:'Microsoft JhengHei UI',sans-serif;background:#eef5ff;color:#163152;margin:0;}"
        ".wrap{max-width:1120px;margin:0 auto;padding:30px 22px 40px;}"
        ".panel{background:#fff;border-radius:22px;padding:24px;box-shadow:0 10px 28px rgba(27,70,139,.09);}"
        ".back{display:inline-flex;margin-bottom:16px;text-decoration:none;color:#1f9df2;font-weight:700;}"
        "h1{margin:0;font-size:30px;}"
        ".meta{margin:8px 0 20px;color:#6d83a6;word-break:break-all;}"
        ".sort-bar{display:flex;justify-content:space-between;align-items:center;gap:12px;flex-wrap:wrap;margin:0 0 18px;}"
        ".sort-cluster{display:flex;gap:12px;align-items:center;flex-wrap:wrap;justify-content:flex-end;}"
        ".sort-group{display:flex;gap:10px;align-items:center;flex-wrap:wrap;}"
        ".sort-label{color:#6d83a6;font-weight:700;}"
        ".sort-chip{display:inline-flex;align-items:center;justify-content:center;padding:9px 14px;border-radius:999px;border:1px solid #dbe6f5;background:#f7fbff;color:#335781;text-decoration:none;font-weight:700;}"
        "button.sort-chip{cursor:pointer;}"
        ".sort-chip:disabled{background:#e9eef6;border-color:#dbe6f5;color:#9aaecc;box-shadow:none;cursor:not-allowed;}"
        ".sort-chip.active{background:#1f9df2;border-color:#1f9df2;color:#fff;box-shadow:0 10px 24px rgba(31,157,242,.18);}"
        ".list{display:grid;gap:14px;}"
        ".row{display:flex;justify-content:space-between;gap:14px;align-items:center;text-decoration:none;color:#163152;background:#f7fbff;border:1px solid #dbe6f5;border-radius:18px;padding:16px 18px;}"
        ".row-select{display:flex;align-items:center;justify-content:center;flex:none;width:24px;}"
        ".row-check{width:18px;height:18px;accent-color:#1f9df2;cursor:pointer;}"
        ".icon{width:48px;height:48px;display:flex;align-items:center;justify-content:center;border-radius:14px;font-size:20px;font-weight:800;flex:none;}"
        ".icon.folder{background:#fff2c9;color:#d79b00;}"
        ".icon.file{background:#dff1ff;color:#1f9df2;}"
        ".icon.audio{background:#eaf7ec;color:#2d9b52;}"
        ".icon.video{background:#ffe8e3;color:#d96445;}"
        ".thumb{width:64px;height:64px;object-fit:cover;border-radius:16px;flex:none;background:#dff1ff;border:1px solid #dbe6f5;}"
        ".image-row{cursor:default;}"
        ".row-main-link{display:flex;align-items:center;gap:14px;min-width:0;flex:1;text-decoration:none;color:#163152;}"
        ".row-main{min-width:0;flex:1;}"
        ".row-name{font-size:18px;font-weight:700;word-break:break-all;}"
        ".row-note{margin-top:6px;color:#6d83a6;font-size:14px;}"
        ".row-size{color:#5d7398;font-weight:700;white-space:nowrap;}"
        ".row-download{display:inline-flex;align-items:center;justify-content:center;min-width:112px;padding:10px 16px;border-radius:14px;background:#1f9df2;color:#fff;text-decoration:none;font-weight:800;white-space:nowrap;flex:none;}"
        ".selection-summary{padding-left:6px;}"
        ".upload-box{margin-top:20px;border-top:1px solid #e8eef8;padding-top:18px;}"
        ".upload-title{font-size:20px;font-weight:800;margin-bottom:10px;}"
        ".upload-note{color:#6d83a6;margin-top:10px;}"
        ".upload-log{display:grid;gap:10px;margin-top:12px;}"
        ".upload-row{background:#f7fbff;border:1px solid #dbe6f5;border-radius:14px;padding:12px 14px;}"
        ".upload-row-title{font-weight:700;color:#244b74;word-break:break-all;}"
        ".upload-row-bar{height:8px;background:#e7f0fb;border-radius:999px;overflow:hidden;margin-top:8px;}"
        ".upload-row-fill{height:100%%;width:0;background:#1f9df2;border-radius:999px;transition:width .18s ease;}"
        ".upload-row.done .upload-row-fill{background:#34b26f;}"
        ".upload-row.error .upload-row-fill{background:#d94f45;}"
        "body.gallery-open{overflow:hidden;}"
        ".gallery-modal{position:fixed;inset:0;z-index:80;}"
        ".gallery-backdrop{position:absolute;inset:0;background:rgba(12,24,43,.78);backdrop-filter:blur(6px);}"
        ".gallery-shell{position:relative;z-index:1;max-width:min(1120px,96vw);height:min(88vh,920px);margin:6vh auto 0;display:grid;grid-template-columns:auto minmax(0,1fr) auto;grid-template-rows:minmax(0,1fr) auto;gap:16px;align-items:center;}"
        ".gallery-stage{grid-column:2;min-height:0;height:100%%;display:flex;align-items:center;justify-content:center;background:rgba(255,255,255,.08);border:1px solid rgba(255,255,255,.16);border-radius:24px;padding:18px;box-shadow:0 18px 50px rgba(0,0,0,.24);}"
        ".gallery-image{max-width:100%%;max-height:100%%;object-fit:contain;border-radius:18px;background:#fff;}"
        ".gallery-nav,.gallery-close{border:0;background:rgba(255,255,255,.94);color:#163152;display:inline-flex;align-items:center;justify-content:center;cursor:pointer;box-shadow:0 10px 30px rgba(0,0,0,.2);}"
        ".gallery-nav{width:58px;height:58px;border-radius:999px;font-size:38px;line-height:1;}"
        ".gallery-close{position:absolute;top:0;right:0;transform:translate(12px,-12px);width:52px;height:52px;border-radius:999px;font-size:34px;z-index:2;}"
        ".gallery-caption{grid-column:1 / span 3;display:flex;align-items:center;justify-content:space-between;gap:16px;background:#fff;border-radius:22px;padding:16px 18px;box-shadow:0 12px 28px rgba(0,0,0,.18);}"
        ".gallery-name{font-size:20px;font-weight:800;word-break:break-all;}"
        ".gallery-meta{margin-top:6px;color:#6d83a6;}"
        ".gallery-download{display:inline-flex;align-items:center;justify-content:center;min-width:120px;padding:12px 18px;border-radius:14px;background:#1f9df2;color:#fff;text-decoration:none;font-weight:800;}"
        ".empty{padding:18px;color:#6d83a6;background:#f7fbff;border-radius:18px;}"
        "@media (max-width:720px){"
        ".wrap{padding:18px 14px 26px;}"
        ".panel{padding:18px;}"
        ".sort-bar{align-items:flex-start;}"
        ".sort-cluster{justify-content:flex-start;}"
        ".row{padding:14px;align-items:flex-start;}"
        ".row-main-link{align-items:flex-start;}"
        ".thumb,.icon{width:56px;height:56px;border-radius:14px;}"
        ".row-size{padding-top:4px;}"
        ".row-download{min-width:96px;padding:10px 14px;}"
        ".gallery-shell{width:94vw;height:84vh;grid-template-columns:1fr;grid-template-rows:minmax(0,1fr) auto;gap:12px;margin:5vh auto 0;}"
        ".gallery-stage{grid-column:1;padding:12px;}"
        ".gallery-caption{grid-column:1;flex-direction:column;align-items:flex-start;padding-right:78px;}"
        ".gallery-nav{position:absolute;top:50%%;transform:translateY(-50%%);z-index:2;width:50px;height:50px;font-size:32px;}"
        ".gallery-nav.prev{left:8px;}"
        ".gallery-nav.next{right:8px;}"
        ".gallery-close{top:8px;right:8px;transform:none;width:46px;height:46px;font-size:30px;}"
        "}"
        "</style></head>"
        "<body><div class='wrap'><div class='panel'>"
        "<a class='back' href='%3'>返回上一層</a>"
        "<h1>%1</h1>"
        "<div class='meta'>目前路徑：%2</div>"
        "%4"
        "<div class='list'>%5</div>"
        "%6"
        "%7"
        "%8"
        "</div></div></body></html>")
                             .arg(escapeHtml(share.name),
                                  escapeHtml(currentLabel),
                                  escapeHtml(parentLinkWithSort),
                                  sortBar,
                                  itemsHtml,
                                  uploadBlock,
                                  galleryBlock,
                                  selectionScript);
    return html.toUtf8();
}

QByteArray HttpFileServer::renderImageViewerPage(const QString &pageTitle,
                                                 const QString &imageName,
                                                 const QString &imageSrc,
                                                 const QString &backHref,
                                                 const QString &downloadHref,
                                                 const QString &previousHref,
                                                 const QString &nextHref,
                                                 const QString &metaText) const
{
    const QString previousButton = previousHref.isEmpty()
                                       ? QStringLiteral("<span class='nav-btn disabled'>‹ 上一張</span>")
                                       : QStringLiteral("<a class='nav-btn' href='%1'>‹ 上一張</a>").arg(escapeHtml(previousHref));
    const QString nextButton = nextHref.isEmpty()
                                   ? QStringLiteral("<span class='nav-btn disabled'>下一張 ›</span>")
                                   : QStringLiteral("<a class='nav-btn' href='%1'>下一張 ›</a>").arg(escapeHtml(nextHref));

    QString html = QStringLiteral(
        "<!doctype html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width, initial-scale=1'>"
        "<title>{{PAGE_TITLE}}</title>"
        "<style>"
        "body{font-family:'Microsoft JhengHei UI',sans-serif;background:#eef5ff;color:#163152;margin:0;}"
        ".wrap{max-width:1180px;margin:0 auto;padding:28px 18px 34px;}"
        ".top{display:flex;align-items:center;justify-content:space-between;gap:14px;margin-bottom:16px;}"
        ".back{display:inline-flex;text-decoration:none;color:#1f9df2;font-weight:800;}"
        ".download{display:inline-flex;align-items:center;justify-content:center;padding:12px 18px;border-radius:14px;background:#1f9df2;color:#fff;text-decoration:none;font-weight:800;}"
        ".panel{background:#fff;border-radius:26px;padding:24px;box-shadow:0 12px 30px rgba(27,70,139,.10);}"
        ".title{font-size:30px;font-weight:900;word-break:break-word;margin:0;}"
        ".meta{margin-top:10px;color:#6d83a6;}"
        ".stage{margin-top:20px;display:flex;align-items:center;justify-content:center;min-height:62vh;background:#f7fbff;border:1px solid #dbe6f5;border-radius:22px;padding:18px;touch-action:pan-y;user-select:none;-webkit-user-select:none;}"
        ".stage img{max-width:100%%;max-height:72vh;object-fit:contain;border-radius:18px;box-shadow:0 12px 32px rgba(27,70,139,.10);background:#fff;}"
        ".nav{display:flex;justify-content:space-between;gap:14px;margin-top:18px;}"
        ".nav-btn{display:inline-flex;align-items:center;justify-content:center;min-width:140px;padding:12px 18px;border-radius:14px;background:#1f9df2;color:#fff;text-decoration:none;font-weight:800;}"
        ".nav-btn.disabled{background:#d9e6f5;color:#8ba1bf;cursor:default;}"
        "@media (max-width:720px){"
        ".wrap{padding:18px 12px 24px;}"
        ".top{flex-direction:column;align-items:flex-start;}"
        ".panel{padding:16px;}"
        ".title{font-size:22px;}"
        ".stage{min-height:48vh;padding:12px;}"
        ".stage img{max-height:62vh;}"
        ".nav{flex-direction:column;}"
        ".nav-btn{width:100%%;}"
        "}"
        "</style></head>"
        "<body><div class='wrap'>"
        "<div class='top'><a class='back' href='{{BACK_HREF}}'>返回相簿</a><a class='download' href='{{DOWNLOAD_HREF}}' download>下載原圖</a></div>"
        "<div class='panel'>"
        "<h1 class='title'>{{IMAGE_NAME}}</h1>"
        "<div class='meta'>{{META_TEXT}}</div>"
        "<div id='viewerStage' class='stage' data-prev='{{PREVIOUS_HREF}}' data-next='{{NEXT_HREF}}'><img src='{{IMAGE_SRC}}' alt='{{IMAGE_NAME}}'></div>"
        "<div class='nav'>{{PREVIOUS_BUTTON}}{{NEXT_BUTTON}}</div>"
        "</div></div>"
        "<script>"
        "const viewerStage=document.getElementById('viewerStage');"
        "if(viewerStage){"
        "let touchStartX=0;"
        "let touchStartY=0;"
        "const navigateImage=(direction)=>{"
        "const target=direction<0?viewerStage.dataset.next:viewerStage.dataset.prev;"
        "if(target){window.location.href=target;}"
        "};"
        "viewerStage.addEventListener('touchstart',function(event){"
        "if(event.touches.length!==1){return;}"
        "touchStartX=event.touches[0].clientX;"
        "touchStartY=event.touches[0].clientY;"
        "},{passive:true});"
        "viewerStage.addEventListener('touchend',function(event){"
        "if(event.changedTouches.length!==1){return;}"
        "const deltaX=event.changedTouches[0].clientX-touchStartX;"
        "const deltaY=event.changedTouches[0].clientY-touchStartY;"
        "if(Math.abs(deltaX)>=48&&Math.abs(deltaX)>Math.abs(deltaY)*1.2){navigateImage(deltaX<0?-1:1);}"
        "},{passive:true});"
        "document.addEventListener('keydown',function(event){"
        "if(event.key==='ArrowLeft'){navigateImage(1);}"
        "else if(event.key==='ArrowRight'){navigateImage(-1);}"
        "});"
        "}"
        "</script>"
        "</body></html>");
    html.replace(QStringLiteral("{{PAGE_TITLE}}"), escapeHtml(pageTitle));
    html.replace(QStringLiteral("{{IMAGE_NAME}}"), escapeHtml(imageName));
    html.replace(QStringLiteral("{{META_TEXT}}"), escapeHtml(metaText));
    html.replace(QStringLiteral("{{IMAGE_SRC}}"), escapeHtml(imageSrc));
    html.replace(QStringLiteral("{{BACK_HREF}}"), escapeHtml(backHref));
    html.replace(QStringLiteral("{{DOWNLOAD_HREF}}"), escapeHtml(downloadHref));
    html.replace(QStringLiteral("{{PREVIOUS_HREF}}"), escapeHtml(previousHref));
    html.replace(QStringLiteral("{{NEXT_HREF}}"), escapeHtml(nextHref));
    html.replace(QStringLiteral("{{PREVIOUS_BUTTON}}"), previousButton);
    html.replace(QStringLiteral("{{NEXT_BUTTON}}"), nextButton);
    return html.toUtf8();
}

QByteArray HttpFileServer::renderMediaViewerPage(const QString &pageTitle,
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
                                                 const QString &subtitleHref) const
{
    const QString previousButton = previousHref.isEmpty()
                                       ? QStringLiteral("<span class='nav-btn disabled'>‹ 上一個</span>")
                                       : QStringLiteral("<a class='nav-btn' href='%1'>‹ 上一個</a>").arg(escapeHtml(previousHref));
    const QString nextButton = nextHref.isEmpty()
                                   ? QStringLiteral("<span class='nav-btn disabled'>下一個 ›</span>")
                                   : QStringLiteral("<a class='nav-btn' href='%1'>下一個 ›</a>").arg(escapeHtml(nextHref));
    const QString subtitleTrack = isVideo && !subtitleHref.isEmpty()
                                      ? QStringLiteral("<track kind='subtitles' srclang='zh-Hant' label='中文字幕' src='%1' default>")
                                            .arg(escapeHtml(subtitleHref))
                                      : QString();
    const QString mediaElement = isVideo
                                     ? QStringLiteral("<video id='player' class='player-video' controls preload='metadata' autoplay playsinline src='%1'>%2</video>")
                                           .arg(escapeHtml(mediaSrc), subtitleTrack)
                                     : QStringLiteral("<audio id='player' class='player-audio' controls preload='metadata' autoplay src='%1'></audio>")
                                           .arg(escapeHtml(mediaSrc));
    QString playlistDownloadName = QFileInfo(mediaName).fileName().trimmed();
    if (playlistDownloadName.isEmpty()) {
        playlistDownloadName = QStringLiteral("playlist");
    }
    playlistDownloadName += QStringLiteral(".m3u8");
    const QString externalPlayerButton = isVideo && !potplayerPath.isEmpty()
                                             ? QStringLiteral(
                                                   "<a id='playlistDownload' class='potplayer' href='%1' download='%2'>PotPlayer 播放</a>")
                                                   .arg(escapeHtml(potplayerPath), escapeHtml(playlistDownloadName))
                                             : QString();
    const QString mediaHint = isVideo
                                  ? QStringLiteral("可直接線上播放影片；若想改用外部播放器，可使用 PotPlayer 播放。")
                                  : QStringLiteral("可直接播放音樂，播放結束後會自動切換下一首，也可開啟亂數播放。");

    QString html = QStringLiteral(
        "<!doctype html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width, initial-scale=1'>"
        "<title>{{PAGE_TITLE}}</title>"
        "<style>"
        "body{font-family:'Microsoft JhengHei UI',sans-serif;background:#eef5ff;color:#163152;margin:0;}"
        ".wrap{max-width:1180px;margin:0 auto;padding:28px 18px 34px;}"
        ".top{display:flex;align-items:center;justify-content:space-between;gap:14px;margin-bottom:16px;flex-wrap:wrap;}"
        ".back{display:inline-flex;text-decoration:none;color:#1f9df2;font-weight:800;}"
        ".actions{display:flex;align-items:center;gap:10px;flex-wrap:wrap;}"
        ".action-btn,.download,.potplayer{display:inline-flex;align-items:center;justify-content:center;padding:12px 18px;border-radius:14px;text-decoration:none;font-weight:800;border:0;cursor:pointer;font-family:inherit;font-size:15px;}"
        ".download{background:#1f9df2;color:#fff;}"
        ".potplayer{background:#25364f;color:#fff;}"
        ".action-btn{background:#e6f1ff;color:#1f5d99;}"
        ".action-btn.active{background:#2d9b52;color:#fff;}"
        ".panel{background:#fff;border-radius:26px;padding:24px;box-shadow:0 12px 30px rgba(27,70,139,.10);}"
        ".title{font-size:30px;font-weight:900;word-break:break-word;margin:0;}"
        ".meta{margin-top:10px;color:#6d83a6;}"
        ".hint{margin-top:8px;color:#47658c;font-size:14px;line-height:1.7;}"
        ".stage{margin-top:20px;display:flex;align-items:center;justify-content:center;min-height:46vh;background:#f7fbff;border:1px solid #dbe6f5;border-radius:22px;padding:18px;}"
        ".player-video{width:100%%;max-height:72vh;border-radius:18px;background:#000;box-shadow:0 12px 32px rgba(27,70,139,.10);}"
        ".player-audio{width:min(780px,100%%);}"
        ".nav{display:flex;justify-content:space-between;gap:14px;margin-top:18px;flex-wrap:wrap;}"
        ".nav-btn{display:inline-flex;align-items:center;justify-content:center;min-width:140px;padding:12px 18px;border-radius:14px;background:#1f9df2;color:#fff;text-decoration:none;font-weight:800;}"
        ".nav-btn.disabled{background:#d9e6f5;color:#8ba1bf;cursor:default;}"
        "@media (max-width:720px){"
        ".wrap{padding:18px 12px 24px;}"
        ".top{align-items:flex-start;}"
        ".actions{width:100%%;}"
        ".action-btn,.download,.potplayer,.nav-btn{width:100%%;}"
        ".panel{padding:16px;}"
        ".title{font-size:22px;}"
        ".stage{min-height:34vh;padding:12px;}"
        ".player-video{max-height:58vh;}"
        ".nav{flex-direction:column;}"
        "}"
        "</style></head>"
        "<body><div class='wrap'>"
        "<div class='top'>"
        "<a class='back' href='{{BACK_HREF}}'>返回清單</a>"
        "<div class='actions'>"
        "<button id='shuffleToggle' type='button' class='action-btn'>亂數播放：關閉</button>"
        "<a class='download' href='{{DOWNLOAD_HREF}}' download>直接下載</a>"
        "{{EXTERNAL_PLAYER_BUTTON}}"
        "</div>"
        "</div>"
        "<div class='panel'>"
        "<h1 class='title'>{{MEDIA_NAME}}</h1>"
        "<div class='meta'>{{META_TEXT}}</div>"
        "<div class='hint'>{{MEDIA_HINT}}</div>"
        "<div class='stage'>{{MEDIA_ELEMENT}}</div>"
        "<div class='nav'>{{PREVIOUS_BUTTON}}{{NEXT_BUTTON}}</div>"
        "</div></div>"
        "<script>"
        "const playlist={{PLAYLIST_JSON}};"
        "const currentIndex={{CURRENT_INDEX}};"
        "const player=document.getElementById('player');"
        "const shuffleToggle=document.getElementById('shuffleToggle');"
        "const playlistDownload=document.getElementById('playlistDownload');"
        "const shuffleKey='easycloudhfs-media-shuffle';"
        "let shuffleEnabled=window.localStorage.getItem(shuffleKey)==='1';"
        "const updateShuffle=()=>{"
        "if(!shuffleToggle){return;}"
        "shuffleToggle.textContent=shuffleEnabled?'亂數播放：開啟':'亂數播放：關閉';"
        "shuffleToggle.classList.toggle('active',shuffleEnabled);"
        "};"
        "const navigateToIndex=(index)=>{"
        "if(index>=0&&index<playlist.length){window.location.href=playlist[index];}"
        "};"
        "const navigateRelative=(delta)=>{"
        "const index=currentIndex+delta;"
        "if(index>=0&&index<playlist.length){navigateToIndex(index);}"
        "};"
        "const playNext=()=>{"
        "if(playlist.length<=0){return;}"
        "if(shuffleEnabled&&playlist.length>1){"
        "let next=currentIndex;"
        "while(next===currentIndex){next=Math.floor(Math.random()*playlist.length);}"
        "navigateToIndex(next);"
        "return;"
        "}"
        "navigateRelative(1);"
        "};"
        "updateShuffle();"
        "shuffleToggle?.addEventListener('click',()=>{"
        "shuffleEnabled=!shuffleEnabled;"
        "window.localStorage.setItem(shuffleKey,shuffleEnabled?'1':'0');"
        "updateShuffle();"
        "});"
        "playlistDownload?.addEventListener('click',async(event)=>{"
        "event.preventDefault();"
        "const href=playlistDownload.getAttribute('href');"
        "const fileName=playlistDownload.getAttribute('download')||'playlist.m3u8';"
        "if(!href){return;}"
        "try{"
        "const response=await fetch(href,{credentials:'same-origin',cache:'no-store'});"
        "if(!response.ok){window.location.href=href;return;}"
        "const blob=await response.blob();"
        "const objectUrl=URL.createObjectURL(blob);"
        "const tempLink=document.createElement('a');"
        "tempLink.href=objectUrl;"
        "tempLink.download=fileName;"
        "tempLink.style.display='none';"
        "document.body.appendChild(tempLink);"
        "tempLink.click();"
        "tempLink.remove();"
        "window.setTimeout(()=>URL.revokeObjectURL(objectUrl),5000);"
        "}catch(error){"
        "window.location.href=href;"
        "}"
        "});"
        "player?.addEventListener('ended',playNext);"
        "document.addEventListener('keydown',(event)=>{"
        "if(event.key==='ArrowLeft'){navigateRelative(-1);}"
        "else if(event.key==='ArrowRight'){navigateRelative(1);}"
        "});"
        "</script>"
        "</body></html>");

    html.replace(QStringLiteral("{{PAGE_TITLE}}"), escapeHtml(pageTitle));
    html.replace(QStringLiteral("{{MEDIA_NAME}}"), escapeHtml(mediaName));
    html.replace(QStringLiteral("{{BACK_HREF}}"), escapeHtml(backHref));
    html.replace(QStringLiteral("{{DOWNLOAD_HREF}}"), escapeHtml(downloadHref));
    html.replace(QStringLiteral("{{EXTERNAL_PLAYER_BUTTON}}"), externalPlayerButton);
    html.replace(QStringLiteral("{{PREVIOUS_HREF}}"), escapeHtml(previousHref));
    html.replace(QStringLiteral("{{NEXT_HREF}}"), escapeHtml(nextHref));
    html.replace(QStringLiteral("{{META_TEXT}}"), escapeHtml(metaText));
    html.replace(QStringLiteral("{{PLAYLIST_JSON}}"), playlistJson.isEmpty() ? QStringLiteral("[]") : playlistJson);
    html.replace(QStringLiteral("{{CURRENT_INDEX}}"), QString::number(qMax(0, currentIndex)));
    html.replace(QStringLiteral("{{MEDIA_HINT}}"), escapeHtml(mediaHint));
    html.replace(QStringLiteral("{{MEDIA_SRC}}"), escapeHtml(mediaSrc));
    html.replace(QStringLiteral("{{MEDIA_ELEMENT}}"), mediaElement);
    html.replace(QStringLiteral("{{PREVIOUS_BUTTON}}"), previousButton);
    html.replace(QStringLiteral("{{NEXT_BUTTON}}"), nextButton);
    return html.toUtf8();
}

QByteArray HttpFileServer::renderSharedImageViewerPage(const ShareItem &share) const
{
    QList<ShareItem> imageShares;
    for (const ShareItem &item : rootShares()) {
        if (item.type == ShareType::File && isImageFilePath(item.sourcePath)) {
            imageShares.append(item);
        }
    }

    int currentIndex = -1;
    for (int index = 0; index < imageShares.size(); ++index) {
        if (imageShares.at(index).routeSegment.compare(share.routeSegment, Qt::CaseInsensitive) == 0) {
            currentIndex = index;
            break;
        }
    }

    if (currentIndex < 0) {
        return renderMessagePage(QStringLiteral("找不到相片"),
                                 QStringLiteral("目前無法建立這張相片的檢視頁。"));
    }

    const QString baseHref = routeForShare(share);
    const QString previousHref = currentIndex > 0
                                     ? routeForShare(imageShares.at(currentIndex - 1)) + QStringLiteral("?__viewer=1")
                                     : QString();
    const QString nextHref = currentIndex + 1 < imageShares.size()
                                 ? routeForShare(imageShares.at(currentIndex + 1)) + QStringLiteral("?__viewer=1")
                                 : QString();
    const QString metaText =
        QStringLiteral("%1 / %2 · %3").arg(currentIndex + 1).arg(imageShares.size()).arg(humanReadableSize(share.pinnedSize));

    return renderImageViewerPage(m_siteName,
                                 share.name,
                                 baseHref + QStringLiteral("?__inline=1"),
                                 QStringLiteral("/"),
                                 baseHref,
                                 previousHref,
                                 nextHref,
                                 metaText);
}

QByteArray HttpFileServer::renderDirectoryImageViewerPage(const ShareItem &share,
                                                          const QString &rootPath,
                                                          const QString &relativePath,
                                                          const QUrlQuery &query) const
{
    const QString currentDirectoryRelative = directoryOfPath(relativePath);
    const QString currentDirectoryPath = currentDirectoryRelative.isEmpty()
                                             ? rootPath
                                             : canonicalSafePath(rootPath, currentDirectoryRelative, false);
    if (currentDirectoryPath.isEmpty()) {
        return renderMessagePage(QStringLiteral("找不到相片"),
                                 QStringLiteral("目前無法建立這張相片的檢視頁。"));
    }

    const QDir dir(currentDirectoryPath);
    QFileInfoList entries = dir.entryInfoList(QDir::Files | QDir::NoDotAndDotDot, QDir::NoSort);
    const DirectorySortKey sortKey =
        parseDirectorySortKey(query.queryItemValue(QStringLiteral("sort"), QUrl::FullyDecoded));
    const bool sortDescending =
        parseDirectorySortDescending(query.queryItemValue(QStringLiteral("order"), QUrl::FullyDecoded));
    std::sort(entries.begin(), entries.end(), [sortKey, sortDescending](const QFileInfo &left, const QFileInfo &right) {
        const int compare = compareDirectoryEntries(left, right, sortKey);
        if (compare == 0) {
            return left.fileName().localeAwareCompare(right.fileName()) < 0;
        }
        return sortDescending ? compare > 0 : compare < 0;
    });

    QStringList imageRelativePaths;
    for (const QFileInfo &entry : entries) {
        if (!isImageFilePath(entry.absoluteFilePath())) {
            continue;
        }

        imageRelativePaths.append(currentDirectoryRelative.isEmpty()
                                      ? entry.fileName()
                                      : currentDirectoryRelative + QStringLiteral("/") + entry.fileName());
    }

    const int currentIndex = imageRelativePaths.indexOf(relativePath);
    if (currentIndex < 0) {
        return renderMessagePage(QStringLiteral("找不到相片"),
                                 QStringLiteral("這張相片目前不在可預覽清單內。"));
    }

    const QString baseRoute = routeForShare(share);
    const QString currentSortSuffix = QStringLiteral("?sort=%1&order=%2")
                                          .arg(directorySortKeyValue(sortKey), directorySortOrderValue(sortDescending));
    const QString fileHref = baseRoute + QStringLiteral("/") + urlEncodePath(relativePath);
    const QString backHref = currentDirectoryRelative.isEmpty()
                                 ? baseRoute + currentSortSuffix
                                 : baseRoute + QStringLiteral("/") + urlEncodePath(currentDirectoryRelative) + currentSortSuffix;
    const QString previousHref = currentIndex > 0
                                     ? baseRoute + QStringLiteral("/") + urlEncodePath(imageRelativePaths.at(currentIndex - 1))
                                           + QStringLiteral("?__viewer=1&sort=%1&order=%2")
                                                 .arg(directorySortKeyValue(sortKey), directorySortOrderValue(sortDescending))
                                     : QString();
    const QString nextHref = currentIndex + 1 < imageRelativePaths.size()
                                 ? baseRoute + QStringLiteral("/") + urlEncodePath(imageRelativePaths.at(currentIndex + 1))
                                       + QStringLiteral("?__viewer=1&sort=%1&order=%2")
                                             .arg(directorySortKeyValue(sortKey), directorySortOrderValue(sortDescending))
                                 : QString();
    const QString currentFilePath = currentDirectoryRelative.isEmpty()
                                        ? QDir(currentDirectoryPath).filePath(QFileInfo(relativePath).fileName())
                                        : QDir(currentDirectoryPath).filePath(QFileInfo(relativePath).fileName());
    const QString metaText = QStringLiteral("%1 / %2 · %3")
                                 .arg(currentIndex + 1)
                                 .arg(imageRelativePaths.size())
                                 .arg(humanReadableSize(QFileInfo(currentFilePath).size()));

    return renderImageViewerPage(share.name,
                                 QFileInfo(relativePath).fileName(),
                                 fileHref + QStringLiteral("?__inline=1"),
                                 backHref,
                                 fileHref,
                                 previousHref,
                                 nextHref,
                                 metaText);
}

QByteArray HttpFileServer::renderSharedMediaViewerPage(const ShareItem &share, bool isVideo) const
{
    QList<ShareItem> mediaShares;
    QStringList playlistUrls;
    for (const ShareItem &item : rootShares()) {
        if (item.type != ShareType::File) {
            continue;
        }

        const bool sameKind = isVideo ? isVideoFilePath(item.sourcePath) : isAudioFilePath(item.sourcePath);
        if (!sameKind) {
            continue;
        }

        mediaShares.append(item);
        playlistUrls.append(routeForShare(item) + QStringLiteral("?__media=1"));
    }

    int currentIndex = -1;
    for (int index = 0; index < mediaShares.size(); ++index) {
        if (mediaShares.at(index).routeSegment.compare(share.routeSegment, Qt::CaseInsensitive) == 0) {
            currentIndex = index;
            break;
        }
    }

    if (currentIndex < 0) {
        return renderMessagePage(QStringLiteral("找不到媒體"),
                                 QStringLiteral("目前無法建立這個檔案的播放頁面。"));
    }

    const QString baseHref = routeForShare(share);
    const QString previousHref = currentIndex > 0 ? playlistUrls.at(currentIndex - 1) : QString();
    const QString nextHref = currentIndex + 1 < playlistUrls.size() ? playlistUrls.at(currentIndex + 1) : QString();
    QString metaText =
        QStringLiteral("%1 / %2 · %3").arg(currentIndex + 1).arg(playlistUrls.size()).arg(humanReadableSize(share.pinnedSize));
    const QString subtitlePath = isVideo ? subtitlePathForMedia(share.sourcePath) : QString();
    const QString subtitleHref = !subtitlePath.isEmpty() ? baseHref + QStringLiteral("?__subtitle=1") : QString();
    const QString potplayerPath = baseHref + QStringLiteral("?__m3u8=1");
    if (!subtitleHref.isEmpty()) {
        metaText += QStringLiteral(" · 已載入字幕");
    }

    const QString playlistJson =
        QString::fromUtf8(QJsonDocument(QJsonArray::fromStringList(playlistUrls)).toJson(QJsonDocument::Compact));

    return renderMediaViewerPage(m_siteName,
                                 share.name,
                                 baseHref + QStringLiteral("?__inline=1"),
                                 QStringLiteral("/"),
                                 baseHref,
                                 potplayerPath,
                                 previousHref,
                                 nextHref,
                                 metaText,
                                 playlistJson,
                                 currentIndex,
                                 isVideo,
                                 subtitleHref);
}

QByteArray HttpFileServer::renderDirectoryMediaViewerPage(const ShareItem &share,
                                                          const QString &rootPath,
                                                          const QString &relativePath,
                                                          const QUrlQuery &query,
                                                          bool isVideo) const
{
    const QString currentDirectoryRelative = directoryOfPath(relativePath);
    const QString currentDirectoryPath = currentDirectoryRelative.isEmpty()
                                             ? rootPath
                                             : canonicalSafePath(rootPath, currentDirectoryRelative, false);
    if (currentDirectoryPath.isEmpty()) {
        return renderMessagePage(QStringLiteral("找不到媒體"),
                                 QStringLiteral("目前無法建立這個檔案的播放頁面。"));
    }

    const QDir dir(currentDirectoryPath);
    QFileInfoList entries = dir.entryInfoList(QDir::Files | QDir::NoDotAndDotDot, QDir::NoSort);
    const DirectorySortKey sortKey =
        parseDirectorySortKey(query.queryItemValue(QStringLiteral("sort"), QUrl::FullyDecoded));
    const bool sortDescending =
        parseDirectorySortDescending(query.queryItemValue(QStringLiteral("order"), QUrl::FullyDecoded));
    std::sort(entries.begin(), entries.end(), [sortKey, sortDescending](const QFileInfo &left, const QFileInfo &right) {
        const int compare = compareDirectoryEntries(left, right, sortKey);
        if (compare == 0) {
            return left.fileName().localeAwareCompare(right.fileName()) < 0;
        }
        return sortDescending ? compare > 0 : compare < 0;
    });

    QStringList mediaRelativePaths;
    for (const QFileInfo &entry : entries) {
        const bool sameKind = isVideo ? isVideoFilePath(entry.absoluteFilePath()) : isAudioFilePath(entry.absoluteFilePath());
        if (!sameKind) {
            continue;
        }

        mediaRelativePaths.append(currentDirectoryRelative.isEmpty()
                                      ? entry.fileName()
                                      : currentDirectoryRelative + QStringLiteral("/") + entry.fileName());
    }

    const int currentIndex = mediaRelativePaths.indexOf(relativePath);
    if (currentIndex < 0) {
        return renderMessagePage(QStringLiteral("找不到媒體"),
                                 QStringLiteral("這個檔案目前不在可播放清單內。"));
    }

    const QString baseRoute = routeForShare(share);
    QStringList playlistUrls;
    playlistUrls.reserve(mediaRelativePaths.size());
    for (const QString &itemPath : mediaRelativePaths) {
        playlistUrls.append(baseRoute + QStringLiteral("/") + urlEncodePath(itemPath)
                            + QStringLiteral("?__media=1&sort=%1&order=%2")
                                  .arg(directorySortKeyValue(sortKey), directorySortOrderValue(sortDescending)));
    }

    const QString fileHref = baseRoute + QStringLiteral("/") + urlEncodePath(relativePath);
    const QString currentSortSuffix = QStringLiteral("?sort=%1&order=%2")
                                          .arg(directorySortKeyValue(sortKey), directorySortOrderValue(sortDescending));
    const QString backHref = currentDirectoryRelative.isEmpty()
                                 ? baseRoute + currentSortSuffix
                                 : baseRoute + QStringLiteral("/") + urlEncodePath(currentDirectoryRelative) + currentSortSuffix;
    const QString previousHref = currentIndex > 0 ? playlistUrls.at(currentIndex - 1) : QString();
    const QString nextHref = currentIndex + 1 < playlistUrls.size() ? playlistUrls.at(currentIndex + 1) : QString();

    const QString currentFilePath = currentDirectoryRelative.isEmpty()
                                        ? QDir(currentDirectoryPath).filePath(QFileInfo(relativePath).fileName())
                                        : QDir(currentDirectoryPath).filePath(QFileInfo(relativePath).fileName());
    QString metaText = QStringLiteral("%1 / %2 · %3")
                           .arg(currentIndex + 1)
                           .arg(mediaRelativePaths.size())
                           .arg(humanReadableSize(QFileInfo(currentFilePath).size()));
    const QString subtitlePath = isVideo ? subtitlePathForMedia(currentFilePath) : QString();
    const QString subtitleHref = !subtitlePath.isEmpty() ? fileHref + QStringLiteral("?__subtitle=1") : QString();
    const QString potplayerPath = fileHref + QStringLiteral("?__m3u8=1");
    if (!subtitleHref.isEmpty()) {
        metaText += QStringLiteral(" · 已載入字幕");
    }

    const QString playlistJson =
        QString::fromUtf8(QJsonDocument(QJsonArray::fromStringList(playlistUrls)).toJson(QJsonDocument::Compact));

    return renderMediaViewerPage(share.name,
                                 QFileInfo(relativePath).fileName(),
                                 fileHref + QStringLiteral("?__inline=1"),
                                 backHref,
                                 fileHref,
                                 potplayerPath,
                                 previousHref,
                                 nextHref,
                                 metaText,
                                 playlistJson,
                                 currentIndex,
                                 isVideo,
                                 subtitleHref);
}

QString HttpFileServer::subtitlePathForMedia(const QString &mediaPath) const
{
    const QFileInfo mediaInfo(mediaPath);
    if (!mediaInfo.exists()) {
        return QString();
    }

    const QDir dir = mediaInfo.dir();
    const QFileInfoList entries = dir.entryInfoList(QDir::Files | QDir::NoDotAndDotDot,
                                                    QDir::Name | QDir::IgnoreCase);
    for (const QFileInfo &entry : entries) {
        if (entry.completeBaseName().compare(mediaInfo.completeBaseName(), Qt::CaseInsensitive) != 0) {
            continue;
        }

        if (entry.suffix().compare(QStringLiteral("srt"), Qt::CaseInsensitive) == 0
            || entry.suffix().compare(QStringLiteral("vtt"), Qt::CaseInsensitive) == 0) {
            return entry.absoluteFilePath();
        }
    }

    return QString();
}

QByteArray HttpFileServer::renderSubtitleTrack(const QString &mediaPath) const
{
    const QString subtitlePath = subtitlePathForMedia(mediaPath);
    if (subtitlePath.isEmpty()) {
        return QByteArray();
    }

    QFile file(subtitlePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return QByteArray();
    }

    const QByteArray raw = file.readAll();
    if (QFileInfo(subtitlePath).suffix().compare(QStringLiteral("vtt"), Qt::CaseInsensitive) == 0) {
        return raw;
    }

    return srtToVtt(raw);
}

QByteArray HttpFileServer::renderMessagePage(const QString &title, const QString &body) const
{
    const QString html = QStringLiteral(
        "<!doctype html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width, initial-scale=1'>"
        "<title>%1</title>"
        "<style>"
        "body{font-family:'Microsoft JhengHei UI',sans-serif;background:#eef5ff;color:#163152;display:flex;align-items:center;justify-content:center;min-height:100vh;margin:0;}"
        ".box{width:min(560px,92vw);background:#fff;border-radius:24px;padding:30px;box-shadow:0 16px 40px rgba(27,70,139,.12);}"
        "h1{margin:0 0 10px;font-size:28px;}"
        "p{color:#6d83a6;font-size:16px;line-height:1.7;}"
        "</style></head>"
        "<body><div class='box'><h1>%1</h1><p>%2</p></div></body></html>")
                             .arg(escapeHtml(title), escapeHtml(body));
    return html.toUtf8();
}

QByteArray HttpFileServer::serveLogo() const
{
    if (!m_logoPath.isEmpty()) {
        QFile file(m_logoPath);
        if (file.open(QIODevice::ReadOnly)) {
            return file.readAll();
        }
    }

    QFile fallback(QStringLiteral(":/logo.png"));
    if (fallback.open(QIODevice::ReadOnly)) {
        return fallback.readAll();
    }

    return QByteArray();
}

QString HttpFileServer::escapeHtml(const QString &value) const
{
    QString escaped = value;
    escaped.replace(QLatin1Char('&'), QStringLiteral("&amp;"));
    escaped.replace(QLatin1Char('<'), QStringLiteral("&lt;"));
    escaped.replace(QLatin1Char('>'), QStringLiteral("&gt;"));
    escaped.replace(QLatin1Char('"'), QStringLiteral("&quot;"));
    escaped.replace(QLatin1Char('\''), QStringLiteral("&#39;"));
    return escaped;
}

QString HttpFileServer::routeForShare(const ShareItem &share) const
{
    return QStringLiteral("/") + urlEncode(share.routeSegment);
}

QString HttpFileServer::detectLanAddress() const
{
    const QList<QNetworkInterface> interfaces = QNetworkInterface::allInterfaces();
    for (const QNetworkInterface &interface : interfaces) {
        const auto flags = interface.flags();
        if (!flags.testFlag(QNetworkInterface::IsUp)
            || !flags.testFlag(QNetworkInterface::IsRunning)
            || flags.testFlag(QNetworkInterface::IsLoopBack)) {
            continue;
        }

        for (const QNetworkAddressEntry &entry : interface.addressEntries()) {
            if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol && !entry.ip().isLoopback()) {
                return entry.ip().toString();
            }
        }
    }

    return QStringLiteral("127.0.0.1");
}

QString HttpFileServer::mediaRelayUrl(const ShareItem &share,
                                      const QString &relativePath) const
{
    QJsonObject payload;
    payload.insert(QStringLiteral("route"), share.routeSegment);
    payload.insert(QStringLiteral("path"), relativePath);

    const QByteArray json = QJsonDocument(payload).toJson(QJsonDocument::Compact);
    const QByteArray token =
        json.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);

    QString fileName;
    if (!relativePath.isEmpty()) {
        fileName = QFileInfo(relativePath).fileName();
    } else {
        fileName = share.name;
    }
    if (fileName.trimmed().isEmpty()) {
        fileName = QStringLiteral("media.bin");
    }

    QString relayPath = QStringLiteral("/__pot/") + QString::fromLatin1(token)
                        + QStringLiteral("/") + urlEncode(fileName);
    if (!m_downloadSettings.password.isEmpty()) {
        relayPath += QStringLiteral("?k=") + authToken();
    }

    const QString host = detectLanAddress();
    const QString portSuffix = (m_port > 0 && m_port != 80) ? QStringLiteral(":%1").arg(QString::number(m_port))
                                                            : QString();
    return QStringLiteral("http://%1%2%3").arg(host, portSuffix, relayPath);
}

bool HttpFileServer::decodePotplayerToken(const QString &token, QString *routeSegment, QString *relativePath) const
{
    if (token.trimmed().isEmpty()) {
        return false;
    }

    QJsonParseError parseError;
    const QByteArray json =
        QByteArray::fromBase64(token.toLatin1(), QByteArray::Base64UrlEncoding);
    const QJsonDocument document = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return false;
    }

    const QJsonObject payload = document.object();
    const QString decodedRoute = payload.value(QStringLiteral("route")).toString().trimmed();
    const QString decodedPath = payload.value(QStringLiteral("path")).toString();
    if (decodedRoute.isEmpty()) {
        return false;
    }

    if (routeSegment) {
        *routeSegment = decodedRoute;
    }
    if (relativePath) {
        *relativePath = decodedPath;
    }
    return true;
}

QString HttpFileServer::displaySourcePath(const ShareItem &share) const
{
    switch (share.type) {
    case ShareType::File:
    case ShareType::Directory:
        return share.sourcePath;
    case ShareType::VirtualDirectory:
        return share.storagePath;
    case ShareType::Link:
        return share.sourcePath;
    }
    return QString();
}

QList<ShareItem> HttpFileServer::rootShares() const
{
    QList<ShareItem> items;
    for (const ShareItem &item : m_shares) {
        if (item.enabled && item.visibleOnHome) {
            items.append(item);
        }
    }
    std::sort(items.begin(), items.end(), [](const ShareItem &left, const ShareItem &right) {
        const bool leftFolder = left.type == ShareType::Directory || left.type == ShareType::VirtualDirectory;
        const bool rightFolder = right.type == ShareType::Directory || right.type == ShareType::VirtualDirectory;
        if (leftFolder != rightFolder) {
            return leftFolder;
        }
        return left.name.localeAwareCompare(right.name) < 0;
    });
    return items;
}

const ShareItem *HttpFileServer::findShareByRoute(const QString &routeSegment) const
{
    for (const ShareItem &share : m_shares) {
        if (share.routeSegment.compare(routeSegment, Qt::CaseInsensitive) == 0) {
            return &share;
        }
    }
    return nullptr;
}

bool HttpFileServer::shareAllowsUpload(const ShareItem &share) const
{
    return share.enabled
           && share.allowUpload
           && (share.type == ShareType::Directory || share.type == ShareType::VirtualDirectory);
}

bool HttpFileServer::shareAllowsDelete(const ShareItem &share) const
{
    return share.enabled
           && share.allowDelete
           && (share.type == ShareType::Directory || share.type == ShareType::VirtualDirectory);
}

bool HttpFileServer::shareAllowsCreateDirectory(const ShareItem &share) const
{
    return share.enabled
           && share.allowCreateDirectory
           && (share.type == ShareType::Directory || share.type == ShareType::VirtualDirectory);
}

int HttpFileServer::activeTransferCount() const
{
    int activeUploads = 0;
    for (auto it = m_connections.constBegin(); it != m_connections.constEnd(); ++it) {
        if (it.value().uploadFile) {
            ++activeUploads;
        }
    }

    int activeDownloads = 0;
    for (auto it = m_transfers.constBegin(); it != m_transfers.constEnd(); ++it) {
        if (it.value() && it.value()->trackAsDownload) {
            ++activeDownloads;
        }
    }

    return activeDownloads + m_chunkUploads.size() + activeUploads;
}

qint64 HttpFileServer::totalLimitBytesPerSecond() const
{
    return limitToBytesPerSecond(m_downloadSettings.totalLimitValue, m_downloadSettings.totalLimitUnit);
}

qint64 HttpFileServer::perIpLimitBytesPerSecond() const
{
    if (!m_downloadSettings.perIpLimitEnabled) {
        return 0;
    }
    return limitToBytesPerSecond(m_downloadSettings.perIpLimitValue, m_downloadSettings.perIpLimitUnit);
}

void HttpFileServer::serviceTransfers()
{
    if (m_transfers.isEmpty()) {
        return;
    }

    qint64 globalBudget = totalLimitBytesPerSecond() > 0
                              ? qMax<qint64>(1, totalLimitBytesPerSecond() / 20)
                              : (256 * 1024);
    QHash<QString, qint64> perIpRemaining;
    const qint64 perIpLimit = perIpLimitBytesPerSecond();

    const QList<QTcpSocket *> keys = m_transfers.keys();
    for (QTcpSocket *socket : keys) {
        FileTransfer *transfer = m_transfers.value(socket, nullptr);
        if (!transfer || !transfer->socket || !transfer->file) {
            finalizeTransfer(socket, false);
            continue;
        }

        qint64 chunkBudget = 256 * 1024;
        if (totalLimitBytesPerSecond() > 0) {
            chunkBudget = qMin(chunkBudget, globalBudget);
        }
        if (perIpLimit > 0) {
            if (!perIpRemaining.contains(transfer->clientAddress)) {
                perIpRemaining.insert(transfer->clientAddress, qMax<qint64>(1, perIpLimit / 20));
            }
            chunkBudget = qMin(chunkBudget, perIpRemaining.value(transfer->clientAddress));
        }
        if (chunkBudget <= 0) {
            continue;
        }

        const qint64 readSize = qMin(chunkBudget, transfer->remaining);
        const QByteArray chunk = transfer->file->read(readSize);
        if (chunk.isEmpty()) {
            finalizeTransfer(socket, true);
            continue;
        }

        transfer->socket->write(chunk);
        transfer->remaining -= chunk.size();
        transfer->bytesSent += chunk.size();
        if (transfer->trackAsDownload) {
            m_totalBytes += chunk.size();
            m_windowBytes += chunk.size();
        }

        if (totalLimitBytesPerSecond() > 0) {
            globalBudget -= chunk.size();
        }
        if (perIpLimit > 0) {
            perIpRemaining[transfer->clientAddress] -= chunk.size();
        }

        if (transfer->remaining <= 0) {
            finalizeTransfer(socket, true);
        }
    }

    updateStats();
}

void HttpFileServer::finalizeTransfer(QTcpSocket *socket, bool success)
{
    if (!socket || !m_transfers.contains(socket)) {
        return;
    }

    std::unique_ptr<FileTransfer> transfer(m_transfers.take(socket));
    if (transfer->file) {
        transfer->file->close();
        transfer->file->deleteLater();
    }

    if (transfer->trackAsDownload) {
        DownloadRecord record;
        record.id = createId();
        record.shareId = transfer->shareId;
        record.fileName = transfer->fileName;
        record.relativePath = transfer->relativePath;
        record.clientAddress = transfer->clientAddress;
        record.bytesTransferred = transfer->bytesSent;
        record.success = success;
        record.timestamp = QDateTime::currentDateTime();

        if (success) {
            ++m_totalDownloads;
        }
        emit downloadRecorded(record);
        emit activityEvent(success
                               ? QStringLiteral("%1 下載完成：%2（%3）")
                                     .arg(record.clientAddress, record.fileName, humanReadableSize(record.bytesTransferred))
                               : QStringLiteral("%1 下載中斷：%2").arg(record.clientAddress, record.fileName));
    }

    if (socket->state() == QAbstractSocket::ConnectedState) {
        socket->disconnectFromHost();
    }
    updateStats();
}

void HttpFileServer::updateStats()
{
    emit statsChanged(m_totalDownloads, m_totalBytes, activeTransferCount(), m_lastBytesPerSecond);
}

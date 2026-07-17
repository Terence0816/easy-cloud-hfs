#include "app/HttpFileServer.h"

#include <QAbstractSocket>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeDatabase>
#include <QNetworkInterface>
#include <QPair>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>
#include <array>
#include <limits>
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
    case 409:
        return QByteArrayLiteral("Conflict");
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

void appendLe16(QByteArray &target, quint16 value)
{
    target.append(static_cast<char>(value & 0xff));
    target.append(static_cast<char>((value >> 8) & 0xff));
}

void appendLe32(QByteArray &target, quint32 value)
{
    target.append(static_cast<char>(value & 0xff));
    target.append(static_cast<char>((value >> 8) & 0xff));
    target.append(static_cast<char>((value >> 16) & 0xff));
    target.append(static_cast<char>((value >> 24) & 0xff));
}

quint16 zipDosTime(const QDateTime &dateTime)
{
    const QTime time = dateTime.time();
    return static_cast<quint16>(((time.hour() & 0x1f) << 11)
                                | ((time.minute() & 0x3f) << 5)
                                | ((time.second() / 2) & 0x1f));
}

quint16 zipDosDate(const QDateTime &dateTime)
{
    const QDate date = dateTime.date();
    const int year = qBound(1980, date.year(), 2107);
    return static_cast<quint16>(((year - 1980) << 9)
                                | ((date.month() & 0x0f) << 5)
                                | (date.day() & 0x1f));
}

quint32 updateZipCrc32(quint32 crc, const QByteArray &data)
{
    static const std::array<quint32, 256> table = []() {
        std::array<quint32, 256> values{};
        for (quint32 index = 0; index < values.size(); ++index) {
            quint32 current = index;
            for (int bit = 0; bit < 8; ++bit) {
                current = (current & 1u) ? (0xedb88320u ^ (current >> 1)) : (current >> 1);
            }
            values[index] = current;
        }
        return values;
    }();

    for (const unsigned char byte : data) {
        crc = table[(crc ^ byte) & 0xffu] ^ (crc >> 8);
    }
    return crc;
}

bool writeAll(QFile *file, const QByteArray &data)
{
    return file && file->write(data) == data.size();
}

QString safeArchiveName(QString name)
{
    name = name.trimmed();
    name.replace(QRegularExpression(QStringLiteral(R"([<>:"/\\|?*\x00-\x1f])")), QStringLiteral("_"));
    while (name.endsWith(QLatin1Char('.')) || name.endsWith(QLatin1Char(' '))) {
        name.chop(1);
    }
    if (name.isEmpty()) {
        name = QStringLiteral("download");
    }
    return name.left(120);
}

QString archiveTempRoot()
{
    QString root = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    if (root.isEmpty()) {
        root = QDir::tempPath();
    }
    root = QDir(root).filePath(QStringLiteral("EasyCloudHFS/archives"));
    QDir().mkpath(root);
    return root;
}


QString archiveUiStyles()
{
    return QStringLiteral(R"HTML(
.folder-row{cursor:pointer;transition:transform .18s ease,box-shadow .18s ease,border-color .18s ease;}
.folder-row:hover{transform:translateY(-1px);border-color:#b7d7f5;box-shadow:0 12px 26px rgba(27,70,139,.1);}
.archive-download{display:inline-flex;align-items:center;justify-content:center;min-width:112px;padding:10px 16px;border:0;border-radius:14px;background:#f3aa2d;color:#fff;font:inherit;font-weight:800;white-space:nowrap;cursor:pointer;flex:none;box-shadow:0 8px 18px rgba(211,143,24,.22);}
.archive-download:hover{filter:brightness(1.03);}
.archive-download:disabled{cursor:wait;opacity:.65;}
.archive-modal[hidden]{display:none;}
.archive-modal{position:fixed;inset:0;z-index:120;display:flex;align-items:center;justify-content:center;padding:20px;}
.archive-backdrop{position:absolute;inset:0;background:rgba(12,24,43,.62);backdrop-filter:blur(5px);}
.archive-dialog{position:relative;z-index:1;width:min(520px,calc(100vw - 32px));background:#fff;border-radius:24px;padding:24px;box-shadow:0 24px 70px rgba(0,0,0,.28);}
.archive-title{font-size:23px;font-weight:900;color:#163152;word-break:break-all;}
.archive-detail{margin-top:8px;color:#6d83a6;line-height:1.65;word-break:break-all;}
.archive-progress{height:16px;margin-top:20px;background:#e7f0fb;border-radius:999px;overflow:hidden;}
.archive-progress-fill{height:100%;width:0;background:linear-gradient(90deg,#f3aa2d,#1f9df2);border-radius:999px;transition:width .2s ease;}
.archive-percent{margin-top:10px;text-align:right;color:#244b74;font-size:18px;font-weight:900;}
.archive-close{display:block;margin:20px 0 0 auto;padding:10px 18px;border:0;border-radius:13px;background:#e8f1fb;color:#244b74;font:inherit;font-weight:800;cursor:pointer;}
.archive-close:disabled{display:none;}
@media (max-width:720px){.folder-row{flex-wrap:wrap}.folder-row .row-size{display:none}.folder-row .archive-download{margin-left:auto;min-width:94px;padding:9px 12px}.archive-dialog{padding:20px}.archive-title{font-size:20px}}
)HTML");
}

QString archiveUiMarkup()
{
    return QStringLiteral(R"HTML(
<div id='archiveModal' class='archive-modal' hidden aria-hidden='true'>
<div class='archive-backdrop'></div>
<div class='archive-dialog' role='dialog' aria-modal='true' aria-labelledby='archiveTitle'>
<div id='archiveTitle' class='archive-title'>正在準備打包…</div>
<div id='archiveDetail' class='archive-detail'>正在讀取資料夾內容，請勿關閉此頁面。</div>
<div class='archive-progress'><div id='archiveProgressFill' class='archive-progress-fill'></div></div>
<div id='archivePercent' class='archive-percent'>0%</div>
<button id='archiveClose' type='button' class='archive-close' disabled>關閉</button>
</div>
</div>
)HTML");
}

QString archiveUiScript()
{
    return QStringLiteral(R"HTML(
<script>
(()=>{
const modal=document.getElementById('archiveModal');
const title=document.getElementById('archiveTitle');
const detail=document.getElementById('archiveDetail');
const fill=document.getElementById('archiveProgressFill');
const percentText=document.getElementById('archivePercent');
const closeButton=document.getElementById('archiveClose');
let activeButton=null;
const delay=(ms)=>new Promise((resolve)=>window.setTimeout(resolve,ms));
const show=()=>{modal.hidden=false;modal.setAttribute('aria-hidden','false');document.body.style.overflow='hidden';};
const hide=()=>{modal.hidden=true;modal.setAttribute('aria-hidden','true');document.body.style.overflow='';if(activeButton){activeButton.disabled=false;activeButton.dataset.busy='';activeButton=null;}};
const update=(value,text)=>{const percent=Math.max(0,Math.min(100,Number(value)||0));fill.style.width=percent+'%';percentText.textContent=percent+'%';if(text){detail.textContent=text;}};
const parseJson=async(response)=>{const raw=await response.text();try{return JSON.parse(raw);}catch(error){throw new Error(raw.replace(/<[^>]+>/g,' ').replace(/\s+/g,' ').trim().slice(0,180)||('HTTP '+response.status));}};
const fail=(message)=>{title.textContent='打包失敗';detail.textContent=message||'建立壓縮檔時發生錯誤。';fill.style.width='100%';percentText.textContent='!';closeButton.disabled=false;};
closeButton?.addEventListener('click',hide);
document.querySelectorAll('.folder-row[data-open]').forEach((row)=>{
 row.addEventListener('click',(event)=>{
  if(event.defaultPrevented||event.button!==0||event.metaKey||event.ctrlKey||event.shiftKey||event.altKey){return;}
  if(event.target.closest('a,button,input,label')){return;}
  const target=row.dataset.open;
  if(target){window.location.href=target;}
 });
});
document.querySelectorAll('.file-row[data-download]').forEach((row)=>{
 row.addEventListener('click',(event)=>{
  if(event.defaultPrevented||event.button!==0||event.metaKey||event.ctrlKey||event.shiftKey||event.altKey){return;}
  if(event.target.closest('a,button,input,label')){return;}
  const target=row.dataset.download;
  if(!target){return;}
  const link=document.createElement('a');
  link.href=target;
  link.download='';
  link.style.display='none';
  document.body.appendChild(link);
  link.click();
  link.remove();
 });
});
document.querySelectorAll('.archive-download').forEach((button)=>{
 button.addEventListener('click',async(event)=>{
  event.preventDefault();event.stopPropagation();
  if(button.dataset.busy==='1'){return;}
  button.dataset.busy='1';button.disabled=true;activeButton=button;
  title.textContent='正在準備打包…';detail.textContent='正在讀取資料夾內容，請勿關閉此頁面。';closeButton.disabled=true;update(0);
  show();
  try{
   const startUrl='/__archive/start?share='+encodeURIComponent(button.dataset.share||'')+'&path='+encodeURIComponent(button.dataset.path||'');
   const start=await parseJson(await fetch(startUrl,{method:'POST',credentials:'same-origin',cache:'no-store'}));
   if(!start.ok||!start.id){throw new Error(start.message||'無法建立打包工作。');}
   title.textContent='正在打包 '+(start.name||'資料夾');
   while(true){
    await delay(350);
    const status=await parseJson(await fetch('/__archive/status?id='+encodeURIComponent(start.id),{credentials:'same-origin',cache:'no-store'}));
    if(status.state==='error'||status.state==='missing'||!status.ok){throw new Error(status.message||'打包工作失敗。');}
    update(status.percent,'正在建立壓縮檔：'+(status.name||start.name||''));
    if(status.state==='ready'&&status.downloadUrl){
     title.textContent='打包完成，開始下載…';update(100,'壓縮檔已完成，瀏覽器將自動開始下載。');
     const link=document.createElement('a');link.href=status.downloadUrl;link.download=status.name||start.name||'download.zip';link.style.display='none';document.body.appendChild(link);link.click();link.remove();
     window.setTimeout(hide,1600);
     return;
    }
   }
  }catch(error){fail(error&&error.message?error.message:String(error));}
 });
});
})();
</script>
)HTML");
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
        cleanupExpiredArchiveJobs();
        updateStats();
    });

    m_archiveTimer.setParent(this);
    m_archiveTimer.setInterval(10);
    connect(&m_archiveTimer, &QTimer::timeout, this, &HttpFileServer::processArchiveJobs);

    m_activeTransfersTimer.setParent(this);
    // The dashboard only needs a human-readable refresh rate.  Rebuilding or
    // updating it four times per second while a fast LAN download is running
    // wastes UI time without making the progress bar look smoother.
    m_activeTransfersTimer.setInterval(500);
    connect(&m_activeTransfersTimer, &QTimer::timeout, this, &HttpFileServer::publishActiveTransfers);
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

void HttpFileServer::setWebLanguage(const QString &language)
{
    m_webLanguage = language.trimmed().compare(QStringLiteral("English"), Qt::CaseInsensitive) == 0
                        ? QStringLiteral("English")
                        : QStringLiteral("繁體中文");
}

void HttpFileServer::setDownloadSettings(const DownloadSettings &settings)
{
    m_downloadSettings = settings;
}

void HttpFileServer::setChatEnabled(bool enabled)
{
    m_chatEnabled = enabled;
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
    m_archiveTimer.start();
    m_activeTransfersTimer.start();
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

    const QStringList archiveJobIds = m_archiveJobs.keys();
    for (const QString &jobId : archiveJobIds) {
        cleanupArchiveJob(jobId, true);
    }
    m_archiveJobs.clear();

    if (m_server->isListening()) {
        m_server->close();
    }

    m_transferTimer.stop();
    m_statsTimer.stop();
    m_archiveTimer.stop();
    m_activeTransfersTimer.stop();
    m_transferServicePending = false;
    m_port = 0;
    updateStats();
    publishActiveTransfers();
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

void HttpFileServer::handleSocketBytesWritten(QTcpSocket *socket, qint64 bytes)
{
    Q_UNUSED(bytes)

    if (!socket || !m_transfers.contains(socket)) {
        return;
    }

    FileTransfer *transfer = m_transfers.value(socket, nullptr);
    if (!transfer) {
        return;
    }

    // Do not calculate progress from QTcpSocket::bytesWritten().  On a very
    // fast local connection that signal can arrive while write() is still
    // unwinding, before bytesQueued has been updated, which made the dashboard
    // stay at 0 B.  It also used to call serviceTransfers() recursively for
    // almost every small write and could make the whole application stutter.
    if (transfer->remaining <= 0 && socket->bytesToWrite() <= 0) {
        finalizeTransfer(socket, true);
        return;
    }

    // Rate-limited transfers are serviced by the fixed 50 ms timer so the
    // per-tick budget remains accurate.  Unlimited transfers refill as soon as
    // Qt reports free socket-buffer space.
    if (totalLimitBytesPerSecond() > 0 || perIpLimitBytesPerSecond() > 0) {
        return;
    }

    if (!m_transferServicePending) {
        m_transferServicePending = true;
        QTimer::singleShot(0, this, [this]() {
            m_transferServicePending = false;
            serviceTransfers();
        });
    }
}

void HttpFileServer::processRequest(QTcpSocket *socket, ConnectionState &state)
{
    const QByteArray body = state.contentLength > 0 ? state.buffer.left(static_cast<qsizetype>(state.contentLength)) : QByteArray();
    state.buffer.clear();

    const QUrl url(QStringLiteral("http://local") + state.target);
    const QString path = url.path(QUrl::FullyDecoded);
    const QUrlQuery query(url);

    // Web language is selected by each visitor, not by the desktop application's
    // global settings. A ?lang=... request stores a per-browser cookie and then
    // redirects to the same URL without the temporary query parameter.
    const QString requestedLanguage = query.queryItemValue(QStringLiteral("lang"), QUrl::FullyDecoded).trimmed();
    if (!requestedLanguage.isEmpty()) {
        const bool english = requestedLanguage.compare(QStringLiteral("en"), Qt::CaseInsensitive) == 0
                             || requestedLanguage.compare(QStringLiteral("en-US"), Qt::CaseInsensitive) == 0
                             || requestedLanguage.compare(QStringLiteral("English"), Qt::CaseInsensitive) == 0;
        const QByteArray cookieValue = english ? QByteArrayLiteral("en") : QByteArrayLiteral("zh-TW");

        QUrl cleanUrl = url;
        QUrlQuery cleanQuery(cleanUrl);
        cleanQuery.removeAllQueryItems(QStringLiteral("lang"));
        cleanUrl.setQuery(cleanQuery);

        QString redirectTarget = cleanUrl.path(QUrl::FullyEncoded);
        if (redirectTarget.isEmpty()) {
            redirectTarget = QStringLiteral("/");
        }
        const QString encodedQuery = cleanUrl.query(QUrl::FullyEncoded);
        if (!encodedQuery.isEmpty()) {
            redirectTarget += QStringLiteral("?") + encodedQuery;
        }

        QByteArray response;
        response += QByteArrayLiteral("HTTP/1.1 302 Found\r\n");
        response += QByteArrayLiteral("Location: ") + redirectTarget.toUtf8() + QByteArrayLiteral("\r\n");
        response += QByteArrayLiteral("Set-Cookie: hfs_lang=") + cookieValue
                    + QByteArrayLiteral("; Path=/; Max-Age=31536000; SameSite=Lax\r\n");
        response += QByteArrayLiteral("Cache-Control: no-store\r\n");
        response += QByteArrayLiteral("Connection: close\r\n");
        response += QByteArrayLiteral("Content-Length: 0\r\n\r\n");
        socket->write(response);
        socket->disconnectFromHost();
        return;
    }

    QString requestWebLanguage = QStringLiteral("繁體中文");
    const QString cookieHeader = headerValue(state.headers, QStringLiteral("cookie"));
    const QStringList cookies = cookieHeader.split(QLatin1Char(';'), Qt::SkipEmptyParts);
    for (const QString &cookie : cookies) {
        const QString trimmed = cookie.trimmed();
        if (!trimmed.startsWith(QStringLiteral("hfs_lang="), Qt::CaseInsensitive)) {
            continue;
        }
        const QString value = trimmed.mid(QStringLiteral("hfs_lang=").size()).trimmed();
        if (value.compare(QStringLiteral("en"), Qt::CaseInsensitive) == 0
            || value.compare(QStringLiteral("English"), Qt::CaseInsensitive) == 0) {
            requestWebLanguage = QStringLiteral("English");
        }
        break;
    }

    const QString previousWebLanguage = m_webLanguage;
    m_webLanguage = requestWebLanguage;
    struct WebLanguageRestore {
        QString *target = nullptr;
        QString previous;
        ~WebLanguageRestore() { if (target) *target = previous; }
    } webLanguageRestore{&m_webLanguage, previousWebLanguage};

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
        && path != QStringLiteral("/chat")
        && !path.startsWith(QStringLiteral("/__chat/"))
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

    if (path == QStringLiteral("/chat")) {
        if (!m_chatEnabled) { sendNotFound(socket); return; }
        sendResponse(socket, 200, statusTextFor(200), renderChatPage(), QByteArrayLiteral("text/html; charset=utf-8"));
        return;
    }
    if (path == QStringLiteral("/__chat/messages") && state.method == QStringLiteral("GET")) {
        if (!m_chatEnabled) { sendNotFound(socket); return; }
        sendJson(socket, QJsonDocument(m_chatMessages).toJson(QJsonDocument::Compact));
        return;
    }
    if (path == QStringLiteral("/__chat/send") && state.method == QStringLiteral("POST")) {
        if (!m_chatEnabled) { sendNotFound(socket); return; }
        const QUrlQuery form(QString::fromUtf8(body));
        QString name = form.queryItemValue(QStringLiteral("name"), QUrl::FullyDecoded).trimmed();
        QString message = form.queryItemValue(QStringLiteral("message"), QUrl::FullyDecoded).trimmed();
        if (name.isEmpty()) name = QStringLiteral("訪客");
        if (name.size() > 30) name = name.left(30);
        if (message.isEmpty()) { sendBadRequest(socket, QStringLiteral("留言不可空白")); return; }
        if (message.size() > 1000) message = message.left(1000);
        QJsonObject item{{QStringLiteral("name"), name}, {QStringLiteral("message"), message}, {QStringLiteral("ip"), detectClientAddress(socket)}, {QStringLiteral("time"), QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"))}};
        m_chatMessages.append(item);
        while (m_chatMessages.size() > 200) m_chatMessages.removeAt(0);
        sendJson(socket, QByteArrayLiteral("{\"ok\":true}"));
        return;
    }

    if (path == QStringLiteral("/__archive/start") && state.method == QStringLiteral("POST")) {
        handleArchiveStart(socket, query);
        return;
    }
    if (path == QStringLiteral("/__archive/status") && state.method == QStringLiteral("GET")) {
        handleArchiveStatus(socket, query);
        return;
    }
    if (path == QStringLiteral("/__archive/download") && state.method == QStringLiteral("GET")) {
        handleArchiveDownload(socket, query, headerValue(state.headers, QStringLiteral("range")));
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
                                      : canonicalSafePath(basePath, currentRelativePath, true);
    if (safeDirectory.isEmpty()) {
        sendBadRequest(socket, QStringLiteral("上傳目錄無效。"));
        return false;
    }

    if (!QDir().mkpath(safeDirectory)) {
        sendBadRequest(socket, QStringLiteral("無法建立上傳目錄。"));
        return false;
    }

    const QFileInfo targetDirectoryInfo(safeDirectory);
    if (!targetDirectoryInfo.exists() || !targetDirectoryInfo.isDir()) {
        sendBadRequest(socket, QStringLiteral("上傳目錄無效。"));
        return false;
    }

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

void HttpFileServer::handleArchiveStart(QTcpSocket *socket, const QUrlQuery &query)
{
    const QString routeSegment = query.queryItemValue(QStringLiteral("share"), QUrl::FullyDecoded).trimmed();
    QString relativePath = QDir::cleanPath(query.queryItemValue(QStringLiteral("path"), QUrl::FullyDecoded));
    if (relativePath == QStringLiteral(".")) {
        relativePath.clear();
    }

    const ShareItem *share = findShareByRoute(routeSegment);
    if (!share || !share->enabled
        || (share->type != ShareType::Directory && share->type != ShareType::VirtualDirectory)) {
        sendJson(socket, QByteArrayLiteral("{\"ok\":false,\"message\":\"找不到可打包的資料夾分享。\"}"));
        return;
    }

    const QString clientAddress = detectClientAddress(socket);
    int clientJobCount = 0;
    for (const ArchiveJob *existingJob : m_archiveJobs) {
        if (existingJob
            && existingJob->state != QStringLiteral("error")
            && existingJob->clientAddress == clientAddress) {
            ++clientJobCount;
        }
    }
    if (m_archiveJobs.size() >= 8 || clientJobCount >= 3) {
        sendJson(socket, QByteArrayLiteral("{\"ok\":false,\"message\":\"目前打包工作較多，請稍後再試。\"}"));
        return;
    }

    const QString baseRoot = share->type == ShareType::VirtualDirectory ? share->storagePath : share->sourcePath;
    const QString safeSource = relativePath.isEmpty() ? canonicalSafePath(baseRoot, QString(), false)
                                                      : canonicalSafePath(baseRoot, relativePath, false);
    const QFileInfo sourceInfo(safeSource);
    if (safeSource.isEmpty() || !sourceInfo.exists() || !sourceInfo.isDir()) {
        sendJson(socket, QByteArrayLiteral("{\"ok\":false,\"message\":\"資料夾路徑不存在或已超出分享範圍。\"}"));
        return;
    }

    auto *job = new ArchiveJob();
    job->id = createId();
    job->shareId = share->id;
    job->sourcePath = safeSource;
    job->relativePath = relativePath;
    job->clientAddress = clientAddress;
    job->createdAt = QDateTime::currentDateTime();

    const QString sourceDisplayName = relativePath.isEmpty() ? share->name : sourceInfo.fileName();
    const QString rootEntryName = safeArchiveName(sourceDisplayName);
    job->archiveName = rootEntryName + QStringLiteral(".zip");

    ArchiveEntry rootEntry;
    rootEntry.sourcePath = safeSource;
    rootEntry.archivePath = rootEntryName + QLatin1Char('/');
    rootEntry.directory = true;
    rootEntry.dosTime = zipDosTime(sourceInfo.lastModified());
    rootEntry.dosDate = zipDosDate(sourceInfo.lastModified());
    job->entries.append(rootEntry);

    QDirIterator iterator(safeSource,
                          QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
                          QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        iterator.next();
        const QFileInfo info = iterator.fileInfo();
        if (info.isSymLink()) {
            continue;
        }
        const bool isDirectory = info.isDir();
        const qint64 fileSize = isDirectory ? 0 : info.size();
        if (fileSize < 0 || static_cast<quint64>(fileSize) > std::numeric_limits<quint32>::max()) {
            delete job;
            sendJson(socket, QByteArrayLiteral("{\"ok\":false,\"message\":\"資料夾內含超過 4GB 的單一檔案，目前無法打包。\"}"));
            return;
        }

        QString entryRelative = QDir(safeSource).relativeFilePath(info.absoluteFilePath());
        entryRelative.replace(QLatin1Char('\\'), QLatin1Char('/'));
        ArchiveEntry entry;
        entry.sourcePath = info.absoluteFilePath();
        entry.archivePath = rootEntryName + QLatin1Char('/') + entryRelative;
        if (isDirectory && !entry.archivePath.endsWith(QLatin1Char('/'))) {
            entry.archivePath += QLatin1Char('/');
        }
        entry.size = fileSize;
        entry.directory = isDirectory;
        entry.dosTime = zipDosTime(info.lastModified());
        entry.dosDate = zipDosDate(info.lastModified());
        job->entries.append(entry);
        job->totalBytes += fileSize;

        if (job->entries.size() > std::numeric_limits<quint16>::max()) {
            delete job;
            sendJson(socket, QByteArrayLiteral("{\"ok\":false,\"message\":\"資料夾項目超過 65535 個，目前無法打包。\"}"));
            return;
        }
    }

    const quint64 estimatedSize = static_cast<quint64>(job->totalBytes)
                                  + static_cast<quint64>(job->entries.size()) * 320u
                                  + 4096u;
    if (estimatedSize > std::numeric_limits<quint32>::max()) {
        delete job;
        sendJson(socket, QByteArrayLiteral("{\"ok\":false,\"message\":\"資料夾打包後預估超過 4GB，目前無法建立 ZIP。\"}"));
        return;
    }

    const QString jobDirectory = QDir(archiveTempRoot()).filePath(job->id);
    if (!QDir().mkpath(jobDirectory)) {
        delete job;
        sendJson(socket, QByteArrayLiteral("{\"ok\":false,\"message\":\"無法建立打包暫存目錄。\"}"));
        return;
    }
    job->outputPath = QDir(jobDirectory).filePath(job->archiveName);
    job->outputFile = new QFile(job->outputPath, this);
    if (!job->outputFile->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        delete job->outputFile;
        job->outputFile = nullptr;
        QDir(jobDirectory).removeRecursively();
        delete job;
        sendJson(socket, QByteArrayLiteral("{\"ok\":false,\"message\":\"無法建立打包檔案。\"}"));
        return;
    }

    m_archiveJobs.insert(job->id, job);
    emit activityEvent(QStringLiteral("%1 開始打包：%2").arg(job->clientAddress, job->archiveName));
    publishActiveTransfers();

    QJsonObject response{
        {QStringLiteral("ok"), true},
        {QStringLiteral("id"), job->id},
        {QStringLiteral("name"), job->archiveName},
    };
    sendJson(socket, QJsonDocument(response).toJson(QJsonDocument::Compact));
}

void HttpFileServer::handleArchiveStatus(QTcpSocket *socket, const QUrlQuery &query)
{
    const QString jobId = query.queryItemValue(QStringLiteral("id"), QUrl::FullyDecoded).trimmed();
    ArchiveJob *job = m_archiveJobs.value(jobId, nullptr);
    if (!job) {
        const QJsonObject response{
            {QStringLiteral("ok"), false},
            {QStringLiteral("state"), QStringLiteral("missing")},
            {QStringLiteral("message"), QStringLiteral("打包工作已不存在，請重新操作。")},
        };
        sendJson(socket, QJsonDocument(response).toJson(QJsonDocument::Compact));
        return;
    }

    int percent = 0;
    if (job->state == QStringLiteral("ready") || job->state == QStringLiteral("downloading")) {
        percent = 100;
    } else if (job->totalBytes > 0) {
        percent = qBound(0, static_cast<int>((job->processedBytes * 100) / job->totalBytes), 99);
    }

    QJsonObject response{
        {QStringLiteral("ok"), job->state != QStringLiteral("error")},
        {QStringLiteral("state"), job->state},
        {QStringLiteral("name"), job->archiveName},
        {QStringLiteral("percent"), percent},
        {QStringLiteral("processed"), QString::number(job->processedBytes)},
        {QStringLiteral("total"), QString::number(job->totalBytes)},
        {QStringLiteral("message"), job->errorMessage},
    };
    if (job->state == QStringLiteral("ready")) {
        response.insert(QStringLiteral("downloadUrl"), QStringLiteral("/__archive/download?id=%1").arg(urlEncode(job->id)));
    }
    sendJson(socket, QJsonDocument(response).toJson(QJsonDocument::Compact));
}

void HttpFileServer::handleArchiveDownload(QTcpSocket *socket,
                                           const QUrlQuery &query,
                                           const QString &rangeHeader)
{
    const QString jobId = query.queryItemValue(QStringLiteral("id"), QUrl::FullyDecoded).trimmed();
    ArchiveJob *job = m_archiveJobs.value(jobId, nullptr);
    if (!job || job->state != QStringLiteral("ready") || !QFileInfo::exists(job->outputPath)) {
        sendResponse(socket,
                     409,
                     statusTextFor(409),
                     renderMessagePage(QStringLiteral("打包檔案尚未完成"),
                                       QStringLiteral("請返回原頁面重新執行打包下載。")),
                     QByteArrayLiteral("text/html; charset=utf-8"));
        return;
    }

    job->state = QStringLiteral("downloading");
    publishActiveTransfers();

    ShareItem archiveShare;
    archiveShare.id = job->shareId;
    archiveShare.name = job->archiveName;
    archiveShare.type = ShareType::File;
    sendFile(socket,
             archiveShare,
             job->outputPath,
             job->archiveName,
             rangeHeader,
             true,
             false,
             job->id);
}

void HttpFileServer::processArchiveJobs()
{
    const QList<ArchiveJob *> jobs = m_archiveJobs.values();
    for (ArchiveJob *job : jobs) {
        if (!job || job->state != QStringLiteral("packaging") || !job->outputFile) {
            continue;
        }

        if (job->currentIndex >= job->entries.size()) {
            finishArchiveJob(job);
            continue;
        }

        ArchiveEntry &entry = job->entries[job->currentIndex];
        if (!job->inputFile && job->currentEntryBytes == 0) {
            if (job->outputFile->pos() < 0
                || static_cast<quint64>(job->outputFile->pos()) > std::numeric_limits<quint32>::max()) {
                failArchiveJob(job, QStringLiteral("ZIP 檔案位置超過 4GB 限制。"));
                continue;
            }

            entry.localHeaderOffset = static_cast<quint32>(job->outputFile->pos());
            const QByteArray nameBytes = entry.archivePath.toUtf8();
            if (nameBytes.size() > std::numeric_limits<quint16>::max()) {
                failArchiveJob(job, QStringLiteral("資料夾內有過長的檔名，無法寫入 ZIP。"));
                continue;
            }

            QByteArray header;
            appendLe32(header, 0x04034b50u);
            appendLe16(header, 20);
            appendLe16(header, 0x0808);
            appendLe16(header, 0);
            appendLe16(header, entry.dosTime);
            appendLe16(header, entry.dosDate);
            appendLe32(header, 0);
            appendLe32(header, 0);
            appendLe32(header, 0);
            appendLe16(header, static_cast<quint16>(nameBytes.size()));
            appendLe16(header, 0);
            header += nameBytes;
            if (!writeAll(job->outputFile, header)) {
                failArchiveJob(job, QStringLiteral("寫入 ZIP 標頭失敗。"));
                continue;
            }

            job->currentCrc = 0xffffffffu;
            if (entry.directory) {
                finishArchiveEntry(job);
                continue;
            }

            job->inputFile = new QFile(entry.sourcePath, this);
            if (!job->inputFile->open(QIODevice::ReadOnly)) {
                failArchiveJob(job, QStringLiteral("無法讀取檔案：%1").arg(entry.archivePath));
                continue;
            }
        }

        if (!job->inputFile) {
            continue;
        }

        const QByteArray chunk = job->inputFile->read(512 * 1024);
        if (chunk.isEmpty()) {
            if (job->inputFile->error() != QFile::NoError || job->currentEntryBytes != entry.size) {
                failArchiveJob(job, QStringLiteral("讀取檔案失敗：%1").arg(entry.archivePath));
            } else {
                finishArchiveEntry(job);
            }
            continue;
        }

        if (!writeAll(job->outputFile, chunk)) {
            failArchiveJob(job, QStringLiteral("寫入 ZIP 內容失敗。"));
            continue;
        }
        job->currentCrc = updateZipCrc32(job->currentCrc, chunk);
        job->currentEntryBytes += chunk.size();
        job->processedBytes += chunk.size();

        if (job->currentEntryBytes >= entry.size) {
            finishArchiveEntry(job);
        }
    }
}

void HttpFileServer::finishArchiveEntry(ArchiveJob *job)
{
    if (!job || job->currentIndex < 0 || job->currentIndex >= job->entries.size()) {
        return;
    }

    ArchiveEntry &entry = job->entries[job->currentIndex];
    if (job->inputFile) {
        job->inputFile->close();
        job->inputFile->deleteLater();
        job->inputFile = nullptr;
    }

    if (job->currentEntryBytes != entry.size) {
        failArchiveJob(job, QStringLiteral("檔案大小在打包過程中發生變化：%1").arg(entry.archivePath));
        return;
    }

    entry.crc32 = job->currentCrc ^ 0xffffffffu;
    QByteArray descriptor;
    appendLe32(descriptor, 0x08074b50u);
    appendLe32(descriptor, entry.crc32);
    appendLe32(descriptor, static_cast<quint32>(entry.size));
    appendLe32(descriptor, static_cast<quint32>(entry.size));
    if (!writeAll(job->outputFile, descriptor)) {
        failArchiveJob(job, QStringLiteral("寫入 ZIP 檔案資訊失敗。"));
        return;
    }

    ++job->currentIndex;
    job->currentEntryBytes = 0;
    job->currentCrc = 0xffffffffu;
    if (job->currentIndex >= job->entries.size()) {
        finishArchiveJob(job);
    }
}

void HttpFileServer::finishArchiveJob(ArchiveJob *job)
{
    if (!job || job->state != QStringLiteral("packaging") || !job->outputFile) {
        return;
    }

    const qint64 centralOffset64 = job->outputFile->pos();
    if (centralOffset64 < 0 || static_cast<quint64>(centralOffset64) > std::numeric_limits<quint32>::max()) {
        failArchiveJob(job, QStringLiteral("ZIP 中央目錄位置超過 4GB 限制。"));
        return;
    }
    const quint32 centralOffset = static_cast<quint32>(centralOffset64);

    for (const ArchiveEntry &entry : job->entries) {
        const QByteArray nameBytes = entry.archivePath.toUtf8();
        QByteArray central;
        appendLe32(central, 0x02014b50u);
        appendLe16(central, 20);
        appendLe16(central, 20);
        appendLe16(central, 0x0808);
        appendLe16(central, 0);
        appendLe16(central, entry.dosTime);
        appendLe16(central, entry.dosDate);
        appendLe32(central, entry.crc32);
        appendLe32(central, static_cast<quint32>(entry.size));
        appendLe32(central, static_cast<quint32>(entry.size));
        appendLe16(central, static_cast<quint16>(nameBytes.size()));
        appendLe16(central, 0);
        appendLe16(central, 0);
        appendLe16(central, 0);
        appendLe16(central, 0);
        appendLe32(central, entry.directory ? 0x10u : 0u);
        appendLe32(central, entry.localHeaderOffset);
        central += nameBytes;
        if (!writeAll(job->outputFile, central)) {
            failArchiveJob(job, QStringLiteral("寫入 ZIP 中央目錄失敗。"));
            return;
        }
    }

    const qint64 centralEnd64 = job->outputFile->pos();
    const qint64 centralSize64 = centralEnd64 - centralOffset64;
    if (centralSize64 < 0 || static_cast<quint64>(centralSize64) > std::numeric_limits<quint32>::max()) {
        failArchiveJob(job, QStringLiteral("ZIP 中央目錄超過 4GB 限制。"));
        return;
    }

    QByteArray endRecord;
    appendLe32(endRecord, 0x06054b50u);
    appendLe16(endRecord, 0);
    appendLe16(endRecord, 0);
    appendLe16(endRecord, static_cast<quint16>(job->entries.size()));
    appendLe16(endRecord, static_cast<quint16>(job->entries.size()));
    appendLe32(endRecord, static_cast<quint32>(centralSize64));
    appendLe32(endRecord, centralOffset);
    appendLe16(endRecord, 0);
    if (!writeAll(job->outputFile, endRecord) || !job->outputFile->flush()) {
        failArchiveJob(job, QStringLiteral("完成 ZIP 檔案時寫入失敗。"));
        return;
    }

    job->outputFile->close();
    job->state = QStringLiteral("ready");
    job->processedBytes = job->totalBytes;
    job->finishedAt = QDateTime::currentDateTime();
    emit activityEvent(QStringLiteral("%1 打包完成：%2（%3）")
                           .arg(job->clientAddress, job->archiveName, humanReadableSize(QFileInfo(job->outputPath).size())));
    publishActiveTransfers();
}

void HttpFileServer::failArchiveJob(ArchiveJob *job, const QString &message)
{
    if (!job) {
        return;
    }
    if (job->inputFile) {
        job->inputFile->close();
        job->inputFile->deleteLater();
        job->inputFile = nullptr;
    }
    if (job->outputFile) {
        job->outputFile->close();
    }
    QFile::remove(job->outputPath);
    job->state = QStringLiteral("error");
    job->errorMessage = message;
    job->finishedAt = QDateTime::currentDateTime();
    emit activityEvent(QStringLiteral("%1 打包失敗：%2（%3）")
                           .arg(job->clientAddress, job->archiveName, message));
    publishActiveTransfers();
}

void HttpFileServer::cleanupArchiveJob(const QString &jobId, bool removeOutputFile)
{
    ArchiveJob *job = m_archiveJobs.take(jobId);
    if (!job) {
        return;
    }
    if (job->inputFile) {
        job->inputFile->close();
        delete job->inputFile;
        job->inputFile = nullptr;
    }
    if (job->outputFile) {
        job->outputFile->close();
        delete job->outputFile;
        job->outputFile = nullptr;
    }
    if (removeOutputFile && !job->outputPath.isEmpty()) {
        QFile::remove(job->outputPath);
        QDir(QFileInfo(job->outputPath).absolutePath()).removeRecursively();
    }
    delete job;
    publishActiveTransfers();
}

void HttpFileServer::cleanupExpiredArchiveJobs()
{
    const QDateTime now = QDateTime::currentDateTime();
    const QStringList jobIds = m_archiveJobs.keys();
    for (const QString &jobId : jobIds) {
        ArchiveJob *job = m_archiveJobs.value(jobId, nullptr);
        if (!job) {
            continue;
        }
        if (job->state == QStringLiteral("packaging") && job->createdAt.secsTo(now) > 30 * 60) {
            failArchiveJob(job, QStringLiteral("打包工作逾時。"));
        }
        if (job->state != QStringLiteral("packaging")
            && job->finishedAt.isValid()
            && job->finishedAt.secsTo(now) > 15 * 60) {
            cleanupArchiveJob(jobId, true);
        }
    }
}

QList<ActiveTransferInfo> HttpFileServer::activeTransferSnapshot() const
{
    QList<ActiveTransferInfo> result;
    const QList<FileTransfer *> transfers = m_transfers.values();
    for (const FileTransfer *transfer : transfers) {
        if (!transfer || !transfer->trackAsDownload) {
            continue;
        }
        ActiveTransferInfo info;
        info.id = transfer->socketKey;
        info.fileName = transfer->fileName;
        info.relativePath = transfer->relativePath;
        info.clientAddress = transfer->clientAddress;
        info.status = QStringLiteral("downloading");
        info.bytesProcessed = transfer->bytesQueued;
        info.totalBytes = transfer->totalBytes;
        result.append(info);
    }

    const QList<ArchiveJob *> jobs = m_archiveJobs.values();
    for (const ArchiveJob *job : jobs) {
        if (!job || job->state != QStringLiteral("packaging")) {
            continue;
        }
        ActiveTransferInfo info;
        info.id = job->id;
        info.fileName = job->archiveName;
        info.relativePath = job->relativePath;
        info.clientAddress = job->clientAddress;
        info.status = QStringLiteral("packaging");
        info.bytesProcessed = job->processedBytes;
        info.totalBytes = job->totalBytes;
        result.append(info);
    }
    return result;
}

void HttpFileServer::publishActiveTransfers()
{
    emit activeTransfersChanged(activeTransferSnapshot());
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
                              bool inlineDisposition,
                              const QString &cleanupArchiveJobId)
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
        contentLength,
        0,
        true,
        trackAsDownload,
        cleanupArchiveJobId,
    };
    m_transfers.insert(socket, transfer);

    const qint64 queuedHeaderBytes = socket->write(headers);
    if (queuedHeaderBytes != headers.size()) {
        finalizeTransfer(socket, false);
        return;
    }

    publishActiveTransfers();
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
    return localizeWebHtml(html).toUtf8();
#endif
    QString rows;
    QJsonArray galleryItems;
    for (const ShareItem &share : rootShares()) {
        const bool isFolder = share.type == ShareType::Directory || share.type == ShareType::VirtualDirectory;
        const bool isImageShare = share.type == ShareType::File && isImageFilePath(share.sourcePath);
        const bool isAudioShare = share.type == ShareType::File && isAudioFilePath(share.sourcePath);
        const bool isVideoShare = share.type == ShareType::File && isVideoFilePath(share.sourcePath);
        const QString infoText = isFolder ? webTx(QStringLiteral("可直接進入資料夾"), QStringLiteral("Open folder")) : humanReadableSize(share.pinnedSize);
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
                        "<div class='row-size'>%5</div>"
                        "<a class='row-download' href='%6' download>直接下載</a>"
                        "</div>")
                        .arg(escapeHtml(viewerHref),
                             escapeHtml(previewHref),
                             escapeHtml(share.name),
                             escapeHtml(share.name),
                             escapeHtml(humanReadableSize(share.pinnedSize)),
                             escapeHtml(href));
            continue;
        }

        if (isAudioShare || isVideoShare) {
            const QString href = routeForShare(share);
            const QString viewerHref = href + QStringLiteral("?__media=1");
            const QString iconClass = isVideoShare ? QStringLiteral("icon video") : QStringLiteral("icon audio");
            const QString iconText = isVideoShare ? QStringLiteral("V") : QStringLiteral("A");
            const QString noteText = isVideoShare ? webTx(QStringLiteral("線上播放"), QStringLiteral("Play online"))
                                                  : webTx(QStringLiteral("音樂播放"), QStringLiteral("Play audio"));

            rows += QStringLiteral(
                        "<div class='row media-row'>"
                        "<a class='row-main-link' href='%1'>"
                        "<div class='%2'>%3</div>"
                        "<div class='row-main'>"
                        "<div class='row-name'>%4</div>"
                        "<div class='row-note'>%5</div>"
                        "</div>"
                        "</a>"
                        "<div class='row-size'>%6</div>"
                        "<a class='row-download' href='%7' download>直接下載</a>"
                        "</div>")
                        .arg(escapeHtml(viewerHref),
                             iconClass,
                             iconText,
                             escapeHtml(share.name),
                             noteText,
                             escapeHtml(humanReadableSize(share.pinnedSize)),
                             escapeHtml(href));
            continue;
        }

        const QString iconClass = isFolder ? QStringLiteral("icon folder") : QStringLiteral("icon file");
        const QString iconText = isFolder ? QStringLiteral("D") : QStringLiteral("F");

        if (isFolder) {
            const QString folderHref = routeForShare(share);
            rows += QStringLiteral(
                        "<div class='row folder-row' data-open='%1'>"
                        "<a class='row-main-link' href='%1'>"
                        "<div class='%2'>%3</div>"
                        "<div class='row-main'>"
                        "<div class='row-name'>%4</div>"
                        "<div class='row-note'>點擊整列進入資料夾</div>"
                        "</div>"
                        "</a>"
                        "<div class='row-size'>%5</div>"
                        "<button type='button' class='archive-download' data-share='%6' data-path=''>打包下載</button>"
                        "</div>")
                        .arg(escapeHtml(folderHref),
                             iconClass,
                             iconText,
                             escapeHtml(share.name),
                             escapeHtml(infoText),
                             escapeHtml(share.routeSegment));
            continue;
        }

        const QString fileHref = routeForShare(share);
        rows += QStringLiteral(
                    "<div class='row file-row' data-download='%1'>"
                    "<a class='row-main-link' href='%1' download>"
                    "<div class='%2'>%3</div>"
                    "<div class='row-main'>"
                    "<div class='row-name'>%4</div>"
                    "<div class='row-note'>檔案</div>"
                    "</div>"
                    "</a>"
                    "<div class='row-size'>%5</div>"
                    "<a class='row-download' href='%1' download>直接下載</a>"
                    "</div>")
                    .arg(escapeHtml(fileHref),
                         iconClass,
                         iconText,
                         escapeHtml(share.name),
                         escapeHtml(infoText));
    }

    if (m_chatEnabled) {
        rows.prepend(QStringLiteral("<a class='row chat-row' href='/chat'><div class='icon chat-icon'>💬</div><div class='row-main'><div class='row-name'>聊天室</div><div class='row-note'>與訪客一起交流</div></div><div class='row-info'>進入聊天室 ›</div></a>"));
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

    QString html = QStringLiteral(
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
        ".icon{width:54px;height:54px;display:flex;align-items:center;justify-content:center;border:0!important;outline:0!important;box-shadow:none!important;border-radius:16px;font-size:22px;font-weight:800;flex:none;}"
        ".icon.folder{background:#fff2c9;color:#d79b00;}"
        ".icon.file{background:#dff1ff;color:#1f9df2;}"
        ".icon.audio{background:#eaf7ec;color:#2d9b52;}"
        ".icon.video{background:#ffe8e3;color:#d96445;}"
        ".thumb{width:54px;height:54px;object-fit:cover;border-radius:16px;flex:none;background:#dff1ff;border:1px solid #dbe6f5;}"
        ".image-row{cursor:default;justify-content:space-between;}.file-row{cursor:pointer;}"
        ".row-main-link{display:flex;align-items:center;gap:16px;min-width:0;flex:1;text-decoration:none;color:#163152;outline:0;box-shadow:none;}"
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
        ".file-row,.image-row,.media-row{flex-wrap:wrap;}"
        ".file-row .row-size,.image-row .row-size,.media-row .row-size{margin-left:auto;}"
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
    html.replace(QStringLiteral("</style>"), archiveUiStyles() + QStringLiteral("</style>"));
    html.replace(QStringLiteral("</body>"), archiveUiMarkup() + archiveUiScript() + QStringLiteral("</body>"));
    return localizeWebHtml(html).toUtf8();
}

QByteArray HttpFileServer::renderChatPage() const
{
    const QString html = QStringLiteral(R"HTML(<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'><title>%1 聊天室</title><style>
body{margin:0;background:#eef4ff;color:#17304f;font-family:'Microsoft JhengHei UI','Segoe UI',sans-serif}.wrap{max-width:980px;margin:auto;padding:24px}.hero,.panel{background:white;border-radius:22px;box-shadow:0 10px 28px rgba(37,75,140,.1)}.hero{display:flex;align-items:center;gap:14px;padding:20px 24px}.hero img{width:58px;height:58px;border-radius:16px}.hero h1{font-size:28px;margin:0}.sub{color:#7890af;margin-top:4px}.panel{margin-top:18px;padding:18px}.messages{height:52vh;min-height:340px;overflow:auto;border:1px solid #dce7f7;border-radius:16px;background:#f9fbff}.msg{padding:12px 16px;border-bottom:1px solid #e8eef8}.top{display:flex;gap:10px;align-items:center}.name{font-weight:800;color:#315fd4}.ip{margin-left:auto;color:#8b98ac;font-size:12px}.time{color:#9aa6b7;font-size:12px}.text{margin-top:5px;white-space:pre-wrap;word-break:break-word}.form{display:grid;grid-template-columns:180px 1fr auto;gap:10px;margin-top:14px}input,textarea,button{font:inherit;border-radius:12px}input,textarea{border:1px solid #cfdcf0;padding:11px 12px}textarea{resize:vertical;min-height:44px}button{border:0;padding:0 22px;background:linear-gradient(90deg,#7357e8,#4e6ef5);color:white;font-weight:800;cursor:pointer}.note{color:#7c8da7;font-size:13px;margin-top:9px}@media(max-width:680px){.form{grid-template-columns:1fr}.messages{height:55vh}button{height:46px}}
</style></head><body><div class='wrap'><div class='hero'><img src='/__logo'><div><h1>%1 聊天室</h1><div class='sub'>自由留言 · 訪客免登入 · 留言顯示 IP</div></div></div><div class='panel'><div id='messages' class='messages'></div><div class='form'><input id='name' maxlength='30' placeholder='名稱（可留空）'><textarea id='message' maxlength='1000' placeholder='輸入留言…'></textarea><button id='send'>送出</button></div><div class='note'>Enter 送出，Shift+Enter 換行。聊天室名稱會跟隨 HFS 名稱。</div></div></div><script>
const box=document.getElementById('messages'),nameEl=document.getElementById('name'),msgEl=document.getElementById('message');function esc(s){return String(s).replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]))}async function load(){try{const r=await fetch('/__chat/messages',{cache:'no-store'});if(!r.ok)return;const a=await r.json();box.innerHTML=a.map(x=>`<div class="msg"><div class="top"><span class="name">${esc(x.name)}</span><span class="time">${esc(x.time)}</span><span class="ip">IP: ${esc(x.ip)}</span></div><div class="text">${esc(x.message)}</div></div>`).join('');box.scrollTop=box.scrollHeight}catch(e){}}async function send(){const m=msgEl.value.trim();if(!m)return;const body=new URLSearchParams({name:nameEl.value,message:m});const r=await fetch('/__chat/send',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded;charset=UTF-8'},body});if(r.ok){msgEl.value='';await load();msgEl.focus()}}document.getElementById('send').onclick=send;msgEl.addEventListener('keydown',e=>{if(e.key==='Enter'&&!e.shiftKey){e.preventDefault();send()}});load();setInterval(load,2000);
</script></body></html>)HTML").arg(escapeHtml(m_siteName));
    return localizeWebHtml(html).toUtf8();
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
    return localizeWebHtml(html).toUtf8();
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
                             "<div class='row-size'>%6</div>"
                             "<a class='row-download' href='%2' download onclick='event.stopPropagation()'>直接下載</a>"
                             "</div>")
                             .arg(escapeHtml(childRelative),
                                  escapeHtml(href),
                                  escapeHtml(entry.fileName()),
                                  escapeHtml(viewerHref),
                                  escapeHtml(previewHref),
                                  escapeHtml(humanReadableSize(entry.size())));
            continue;
        }

        if (isAudioEntry || isVideoEntry) {
            const QString viewerHref = href + QStringLiteral("?__media=1&sort=%1&order=%2")
                                                 .arg(directorySortKeyValue(sortKey), directorySortOrderValue(sortDescending));
            const QString iconClass = isVideoEntry ? QStringLiteral("icon video") : QStringLiteral("icon audio");
            const QString iconText = isVideoEntry ? QStringLiteral("V") : QStringLiteral("A");
            const QString noteText = isVideoEntry ? webTx(QStringLiteral("線上播放"), QStringLiteral("Play online"))
                                                 : webTx(QStringLiteral("音樂播放"), QStringLiteral("Play audio"));

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
                             "<div class='row-size'>%8</div>"
                             "<a class='row-download' href='%2' download onclick='event.stopPropagation()'>直接下載</a>"
                             "</div>")
                             .arg(escapeHtml(childRelative),
                                  escapeHtml(href),
                                  escapeHtml(entry.fileName()),
                                  escapeHtml(viewerHref),
                                  iconClass,
                                  iconText,
                                  noteText,
                                  escapeHtml(humanReadableSize(entry.size())));
            continue;
        }

        if (isDirectory) {
            itemsHtml += QStringLiteral(
                             "<div class='row selectable-row folder-row' data-kind='directory' data-path='%1' data-open='%3'>"
                             "<div class='row-select'><input class='row-check' type='checkbox' aria-label='選取 %2'></div>"
                             "<a class='row-main-link' href='%3'>"
                             "<div class='icon folder'>D</div>"
                             "<div class='row-main'>"
                             "<div class='row-name'>%2</div>"
                             "<div class='row-note'>點擊整列進入資料夾</div>"
                             "</div>"
                             "</a>"
                             "<div class='row-size'>進入資料夾</div>"
                             "<button type='button' class='archive-download' data-share='%4' data-path='%1'>打包下載</button>"
                             "</div>")
                             .arg(escapeHtml(childRelative),
                                  escapeHtml(entry.fileName()),
                                  escapeHtml(navigableHref),
                                  escapeHtml(share.routeSegment));
            continue;
        }

        itemsHtml += QStringLiteral(
                         "<div class='row selectable-row file-row' data-kind='file' data-path='%1' data-download='%2'>"
                         "<div class='row-select'><input class='row-check' type='checkbox' aria-label='選取 %3'></div>"
                         "<a class='row-main-link' href='%2' download>"
                         "<div class='icon file'>F</div>"
                         "<div class='row-main'>"
                         "<div class='row-name'>%3</div>"
                         "<div class='row-note'>一般檔案</div>"
                         "</div>"
                         "</a>"
                         "<div class='row-size'>%4</div>"
                         "<a class='row-download' href='%2' download onclick='event.stopPropagation()'>直接下載</a>"
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
        QString uploadConfigJson = QString::fromUtf8(
            QJsonDocument(QJsonObject{
                              {QStringLiteral("share"), share.routeSegment},
                              {QStringLiteral("currentPath"), relativePath},
                          })
                .toJson(QJsonDocument::Compact));
        uploadConfigJson.replace(QStringLiteral("</"), QStringLiteral("<\\/"));

        const QString uploadMarkup = QStringLiteral(
            "<div class='upload-box'>"
            "<div class='upload-title'>上傳區</div>"
            "<div class='upload-help'>請選擇上傳的檔案或資料夾，或直接拖曳到下方區塊，支援同時上傳多個檔案與資料夾。</div>"
            "<div class='upload-pickers'>"
            "<input id='uploadFiles' class='upload-input' type='file' multiple>"
            "<input id='uploadFolders' class='upload-input' type='file' multiple webkitdirectory directory>"
            "<label class='upload-picker-btn files' for='uploadFiles'>請選擇上傳的檔案（可同時多檔）</label>"
            "<label class='upload-picker-btn folders' for='uploadFolders'>請選擇上傳的資料夾</label>"
            "</div>"
            "<div id='uploadDropZone' class='upload-dropzone' tabindex='0' role='button' aria-label='拖曳上傳區'>"
            "<div class='upload-drop-title'>請直接拖曳要上傳的檔案或資料夾到此處，<br>支援同時上傳多個檔案及資料夾</div>"
            "<div class='upload-drop-subtitle'>拖入後會直接上傳到目前目錄，若包含資料夾則會保留原本的子資料夾結構。</div>"
            "</div>"
            "<div id='uploadLog' class='upload-log'></div>"
            "</div>");

        QString uploadScript;
        uploadScript += QStringLiteral("<script>");
        uploadScript += QStringLiteral("(() => {");
        uploadScript += QStringLiteral("const uploadConfig=");
        uploadScript += uploadConfigJson;
        uploadScript += QStringLiteral(";");
        uploadScript += QStringLiteral(R"JS(
            const filePicker=document.getElementById('uploadFiles');
            const folderPicker=document.getElementById('uploadFolders');
            const dropZone=document.getElementById('uploadDropZone');
            const log=document.getElementById('uploadLog');
            const externalChunkThreshold=80*1024*1024;
            const externalChunkSize=64*1024*1024;
            const externalChunkConcurrency=2;
            const externalChunkMaxRetries=3;
            const externalChunkRetryDelayMs=1200;
            const isProxyExternalUpload=/^\/(?:PHONE|HFS)(?:\/|$)/i.test(location.pathname||'');
            let uploadChain=Promise.resolve();
            let pendingBatches=0;
            let reloadNeeded=false;
            let reloadScheduled=false;

            const makeUploadId=()=>{
                if(window.crypto&&typeof window.crypto.randomUUID==='function'){
                    return window.crypto.randomUUID();
                }
                return 'upload-'+Date.now().toString(36)+'-'+Math.random().toString(36).slice(2);
            };

            const normalizeRelativePath=(value)=>{
                return String(value||'')
                    .replace(/\\/g,'/')
                    .split('/')
                    .filter((segment)=>segment&&segment!=='.')
                    .join('/');
            };

            const joinRelativePath=(basePath,childPath)=>{
                const base=normalizeRelativePath(basePath);
                const child=normalizeRelativePath(childPath);
                return [base,child].filter(Boolean).join('/');
            };

            const currentDirectoryPath=normalizeRelativePath(uploadConfig.currentPath||'');
            const uploadBase='/__upload?share='+encodeURIComponent(uploadConfig.share||'');

            const buildUploadItem=(file,relativePath='')=>{
                const parentPath=normalizeRelativePath(relativePath);
                return {
                    id:makeUploadId(),
                    file,
                    relativePath:parentPath,
                    displayName:parentPath?`${parentPath}/${file.name}`:file.name
                };
            };

            const ensureRow=(item)=>{
                const rowId='upload-'+item.id;
                let row=document.getElementById(rowId);
                if(!row){
                    row=document.createElement('div');
                    row.id=rowId;
                    row.className='upload-row progress';
                    row.innerHTML="<div class='upload-row-title'></div><div class='upload-row-bar'><div class='upload-row-fill'></div></div>";
                    log.appendChild(row);
                }
                return row;
            };

            const updateRow=(item,percent,text,state)=>{
                const row=ensureRow(item);
                row.className='upload-row '+state;
                row.querySelector('.upload-row-title').textContent=text;
                row.querySelector('.upload-row-fill').style.width=Math.max(0,Math.min(100,percent))+'%';
            };

            const stripErrorDetail=(value,limit)=>{
                return String(value||'')
                    .replace(/<[^>]+>/g,' ')
                    .replace(/\s+/g,' ')
                    .trim()
                    .slice(0,limit);
            };

            const setProgressText=(item,loaded,total,prefix)=>{
                const percent=total>0?Math.min(100,Math.round((loaded/total)*100)):100;
                updateRow(item,percent,prefix+item.displayName+' ('+percent+'%)','progress');
            };

            const delay=(ms)=>new Promise((resolve)=>setTimeout(resolve,ms));
            const shouldUseChunkUpload=(file)=>isProxyExternalUpload&&file.size>externalChunkThreshold;

            const buildUploadUrl=(item,extraQuery='')=>{
                const targetPath=joinRelativePath(currentDirectoryPath,item.relativePath);
                return uploadBase
                    +'&path='+encodeURIComponent(targetPath)
                    +'&name='+encodeURIComponent(item.file.name)
                    +extraQuery;
            };
        )JS");
        uploadScript += QStringLiteral(R"JS(
            async function uploadChunk(item,uploadId,chunkIndex,totalChunks,chunkSize,progress){
                const file=item.file;
                const start=chunkIndex*chunkSize;
                const end=Math.min(file.size,start+chunkSize);
                const blob=file.slice(start,end);
                const uploadUrl=buildUploadUrl(
                    item,
                    '&__upload_chunk=1&id='+encodeURIComponent(uploadId)
                    +'&index='+chunkIndex
                    +'&total='+totalChunks
                    +'&size='+file.size
                    +'&chunkSize='+chunkSize
                );
                let lastError=null;
                for(let attempt=1;attempt<=externalChunkMaxRetries;attempt++){
                    try{
                        await new Promise((resolve,reject)=>{
                            const xhr=new XMLHttpRequest();
                            xhr.open('POST',uploadUrl,true);
                            xhr.upload.onprogress=(event)=>{
                                progress[chunkIndex]=Math.min(blob.size,Math.max(0,event.loaded||0));
                                const loaded=progress.reduce((sum,value)=>sum+value,0);
                                setProgressText(item,loaded,file.size,attempt>1?'分段重試中：':'分段上傳中：');
                            };
                            xhr.onload=()=>{
                                if(xhr.status>=200&&xhr.status<300){
                                    progress[chunkIndex]=blob.size;
                                    const loaded=progress.reduce((sum,value)=>sum+value,0);
                                    setProgressText(item,loaded,file.size,'分段上傳中：');
                                    resolve();
                                }else{
                                    const detail=stripErrorDetail(xhr.responseText,220);
                                    reject(new Error(detail?('HTTP '+xhr.status+' - '+detail):('HTTP '+xhr.status)));
                                }
                            };
                            xhr.onerror=()=>reject(new Error('network'));
                            xhr.send(blob);
                        });
                        return;
                    }catch(error){
                        lastError=error;
                        progress[chunkIndex]=0;
                        const loaded=progress.reduce((sum,value)=>sum+value,0);
                        setProgressText(item,loaded,file.size,attempt<externalChunkMaxRetries?'分段重試中：':'分段上傳中：');
                        if(attempt<externalChunkMaxRetries){
                            await delay(externalChunkRetryDelayMs*attempt);
                        }
                    }
                }
                throw lastError||new Error('chunk failed');
            }

            async function uploadChunkedItem(item){
                const file=item.file;
                const chunkSize=externalChunkSize;
                const totalChunks=Math.max(1,Math.ceil(file.size/chunkSize));
                const progress=new Array(totalChunks).fill(0);
                const uploadId=makeUploadId();
                let nextChunkIndex=0;
                let abortedError=null;
                setProgressText(item,0,file.size,'分段上傳中：');

                const worker=async()=>{
                    while(true){
                        if(abortedError){
                            return;
                        }
                        const currentIndex=nextChunkIndex++;
                        if(currentIndex>=totalChunks){
                            return;
                        }
                        try{
                            await uploadChunk(item,uploadId,currentIndex,totalChunks,chunkSize,progress);
                        }catch(error){
                            abortedError=error;
                            throw error;
                        }
                    }
                };

                const workerCount=Math.min(externalChunkConcurrency,totalChunks);
                await Promise.all(Array.from({length:workerCount},()=>worker()));
                if(abortedError){
                    throw abortedError;
                }
            }

            async function uploadSingleItem(item){
                const file=item.file;
                if(shouldUseChunkUpload(file)){
                    await uploadChunkedItem(item);
                    return;
                }
                const uploadUrl=buildUploadUrl(item);
                updateRow(item,0,'上傳中：'+item.displayName+' (0%)','progress');
                await new Promise((resolve,reject)=>{
                    const xhr=new XMLHttpRequest();
                    xhr.open('POST',uploadUrl,true);
                    xhr.upload.onprogress=(event)=>{
                        if(event.lengthComputable){
                            const percent=file.size>0?Math.min(100,Math.round((event.loaded/file.size)*100)):100;
                            updateRow(item,percent,'上傳中：'+item.displayName+' ('+percent+'%)','progress');
                        }
                    };
                    xhr.onload=()=>{
                        if(xhr.status>=200&&xhr.status<300){
                            resolve();
                        }else{
                            const detail=stripErrorDetail(xhr.responseText,220);
                            reject(new Error(detail?('HTTP '+xhr.status+' - '+detail):('HTTP '+xhr.status)));
                        }
                    };
                    xhr.onerror=()=>reject(new Error('network'));
                    xhr.send(file);
                });
            }
        )JS");
        uploadScript += QStringLiteral(R"JS(
            async function processUploadItems(items){
                let anySuccess=false;
                for(const item of items){
                    try{
                        await uploadSingleItem(item);
                        updateRow(item,100,'完成：'+item.displayName+' (100%)','done');
                        anySuccess=true;
                    }catch(err){
                        updateRow(item,100,'失敗：'+item.displayName+' ('+(err&&err.message?err.message:'unknown')+')','error');
                    }
                }
                return anySuccess;
            }

            const scheduleReload=()=>{
                if(reloadScheduled){
                    return;
                }
                reloadScheduled=true;
                setTimeout(()=>location.reload(),700);
            };

            const enqueueUploadItems=(items)=>{
                const validItems=(items||[]).filter((item)=>item&&item.file);
                if(!validItems.length){
                    return;
                }
                pendingBatches+=1;
                uploadChain=uploadChain
                    .then(async()=>{
                        try{
                            if(await processUploadItems(validItems)){
                                reloadNeeded=true;
                            }
                        }finally{
                            pendingBatches=Math.max(0,pendingBatches-1);
                            if(pendingBatches===0&&reloadNeeded){
                                reloadNeeded=false;
                                scheduleReload();
                            }
                        }
                    })
                    .catch((error)=>{
                        console.error(error);
                    });
            };

            const buildItemsFromFiles=(files,relativePathGetter)=>{
                return Array.from(files||[])
                    .filter((file)=>file&&file.name)
                    .map((file)=>buildUploadItem(file,relativePathGetter(file)));
            };

            const handleFilePick=(event)=>{
                enqueueUploadItems(buildItemsFromFiles(event.target.files,()=>'')); 
                event.target.value='';
            };

            const handleFolderPick=(event)=>{
                enqueueUploadItems(buildItemsFromFiles(event.target.files,(file)=>{
                    return file.webkitRelativePath
                        ? file.webkitRelativePath.split('/').slice(0,-1).join('/')
                        : '';
                }));
                event.target.value='';
            };
        )JS");
        uploadScript += QStringLiteral(R"JS(
            const readAllDirectoryEntries=(reader)=>{
                return new Promise((resolve,reject)=>{
                    const entries=[];
                    const readNextBatch=()=>{
                        reader.readEntries((batch)=>{
                            if(!batch||batch.length===0){
                                resolve(entries);
                                return;
                            }
                            entries.push(...batch);
                            readNextBatch();
                        },reject);
                    };
                    readNextBatch();
                });
            };

            const readDroppedEntry=(entry,parentPath='')=>{
                if(!entry){
                    return Promise.resolve([]);
                }
                if(entry.isFile){
                    return new Promise((resolve,reject)=>{
                        entry.file((file)=>{
                            resolve([buildUploadItem(file,parentPath)]);
                        },reject);
                    });
                }
                if(!entry.isDirectory){
                    return Promise.resolve([]);
                }
                const currentPath=parentPath?`${parentPath}/${entry.name}`:entry.name;
                const reader=entry.createReader();
                return readAllDirectoryEntries(reader).then((children)=>
                    Promise.all(children.map((child)=>readDroppedEntry(child,currentPath)))
                        .then((results)=>results.flat())
                );
            };

            const collectDroppedUploadItems=async(dataTransfer)=>{
                const items=Array.from(dataTransfer.items||[]);
                const canReadDirectories=items.some((item)=>typeof item.webkitGetAsEntry==='function');
                if(canReadDirectories){
                    const results=await Promise.all(
                        items
                            .map((item)=>item.webkitGetAsEntry())
                            .filter((entry)=>entry)
                            .map((entry)=>readDroppedEntry(entry))
                    );
                    return results.flat();
                }
                return buildItemsFromFiles(dataTransfer.files,()=> '');
            };

            const handleDrag=(event)=>{
                event.preventDefault();
                dropZone.classList.add('is-over');
            };

            const clearDragState=(event)=>{
                event.preventDefault();
                dropZone.classList.remove('is-over');
            };

            const handleDrop=async(event)=>{
                event.preventDefault();
                dropZone.classList.remove('is-over');
                if(!event.dataTransfer){
                    return;
                }
                try{
                    enqueueUploadItems(await collectDroppedUploadItems(event.dataTransfer));
                }catch(error){
                    console.error(error);
                }
            };
        )JS");
        uploadScript += QStringLiteral(R"JS(
            filePicker?.addEventListener('change',handleFilePick);
            folderPicker?.addEventListener('change',handleFolderPick);
            dropZone?.addEventListener('click',()=>filePicker?.click());
            dropZone?.addEventListener('keydown',(event)=>{
                if(event.key==='Enter'||event.key===' '){
                    event.preventDefault();
                    filePicker?.click();
                }
            });
            ['dragenter','dragover'].forEach((eventName)=>{
                dropZone?.addEventListener(eventName,handleDrag);
            });
            ['dragleave','dragend'].forEach((eventName)=>{
                dropZone?.addEventListener(eventName,clearDragState);
            });
            dropZone?.addEventListener('drop',handleDrop);
            })();
        )JS");
        uploadScript += QStringLiteral("</script>");
        uploadBlock = uploadMarkup + uploadScript;
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

    const QString currentLabel = relativePath.isEmpty() ? webTx(QStringLiteral("根目錄"), QStringLiteral("Root")) : relativePath;
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
    QString html = QStringLiteral(
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
        ".icon{width:48px;height:48px;display:flex;align-items:center;justify-content:center;border:0!important;outline:0!important;box-shadow:none!important;border-radius:14px;font-size:20px;font-weight:800;flex:none;}"
        ".icon.folder{background:#fff2c9;color:#d79b00;}"
        ".icon.file{background:#dff1ff;color:#1f9df2;}"
        ".icon.audio{background:#eaf7ec;color:#2d9b52;}"
        ".icon.video{background:#ffe8e3;color:#d96445;}"
        ".thumb{width:64px;height:64px;object-fit:cover;border-radius:16px;flex:none;background:#dff1ff;border:1px solid #dbe6f5;}"
        ".image-row{cursor:default;}.file-row{cursor:pointer;}"
        ".row-main-link{display:flex;align-items:center;gap:14px;min-width:0;flex:1;text-decoration:none;color:#163152;outline:0;box-shadow:none;}"
        ".row-main{min-width:0;flex:1;}"
        ".row-name{font-size:18px;font-weight:700;word-break:break-all;}"
        ".row-note{margin-top:6px;color:#6d83a6;font-size:14px;}"
        ".row-size{color:#5d7398;font-weight:700;white-space:nowrap;}"
        ".row-download{display:inline-flex;align-items:center;justify-content:center;min-width:112px;padding:10px 16px;border-radius:14px;background:#1f9df2;color:#fff;text-decoration:none;font-weight:800;white-space:nowrap;flex:none;}"
        ".selection-summary{padding-left:6px;}"
        ".upload-box{margin-top:26px;border-top:1px solid #e8eef8;padding-top:22px;}"
        ".upload-title{font-size:30px;font-weight:900;margin-bottom:8px;color:#163152;}"
        ".upload-help{color:#6d83a6;line-height:1.75;font-size:16px;}"
        ".upload-input{display:none;}"
        ".upload-pickers{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:14px;margin-top:18px;}"
        ".upload-picker-btn{display:flex;align-items:center;justify-content:center;min-height:90px;padding:18px 24px;border-radius:26px;text-align:center;font-size:19px;font-weight:900;line-height:1.45;cursor:pointer;user-select:none;transition:transform .18s ease,box-shadow .18s ease,filter .18s ease;}"
        ".upload-picker-btn:hover,.upload-picker-btn:focus-visible{transform:translateY(-2px);filter:saturate(1.03);}"
        ".upload-picker-btn.files{color:#fff;background:linear-gradient(135deg,#c45d1f,#ea9338);box-shadow:0 18px 34px rgba(187,77,31,.24);}"
        ".upload-picker-btn.folders{color:#7c5300;background:linear-gradient(135deg,#ffe4a0,#f6c64d);box-shadow:0 18px 34px rgba(210,160,47,.2);}"
        ".upload-dropzone{display:grid;gap:14px;place-items:center;text-align:center;min-height:240px;margin-top:20px;padding:30px;border:2px dashed rgba(67,157,150,.4);border-radius:28px;background:linear-gradient(180deg,rgba(240,248,251,.96),rgba(255,255,255,.94));transition:border-color .18s ease,transform .18s ease,background .18s ease,box-shadow .18s ease;}"
        ".upload-dropzone.is-over{transform:translateY(-2px) scale(1.01);border-color:#ea9338;background:linear-gradient(180deg,rgba(255,242,228,.96),rgba(247,252,255,.96));box-shadow:0 18px 36px rgba(80,141,164,.14);}"
        ".upload-drop-title{font-size:24px;line-height:1.55;font-weight:900;color:#223b5f;}"
        ".upload-drop-subtitle{max-width:680px;color:#6d83a6;line-height:1.8;font-size:16px;}"
        ".upload-log{display:grid;gap:10px;margin-top:16px;}"
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
        ".file-row,.image-row,.media-row{flex-wrap:wrap;}"
        ".file-row .row-size,.image-row .row-size,.media-row .row-size{margin-left:auto;}"
        ".upload-pickers{grid-template-columns:1fr;}"
        ".upload-picker-btn{min-height:78px;font-size:18px;padding:16px 18px;}"
        ".upload-dropzone{min-height:200px;padding:24px 18px;}"
        ".upload-drop-title{font-size:21px;}"
        ".upload-drop-subtitle{font-size:15px;line-height:1.75;}"
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
    html.replace(QStringLiteral("</style>"), archiveUiStyles() + QStringLiteral("</style>"));
    html.replace(QStringLiteral("</body>"), archiveUiMarkup() + archiveUiScript() + QStringLiteral("</body>"));
    return localizeWebHtml(html).toUtf8();
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
    return localizeWebHtml(html).toUtf8();
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
    return localizeWebHtml(html).toUtf8();
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
        metaText += webTx(QStringLiteral(" · 已載入字幕"), QStringLiteral(" · Subtitles loaded"));
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
        metaText += webTx(QStringLiteral(" · 已載入字幕"), QStringLiteral(" · Subtitles loaded"));
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
    return localizeWebHtml(html).toUtf8();
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

QString HttpFileServer::webTx(const QString &zh, const QString &en) const
{
    return m_webLanguage.compare(QStringLiteral("English"), Qt::CaseInsensitive) == 0 ? en : zh;
}

QString HttpFileServer::localizeWebHtml(QString html) const
{
    const bool english = m_webLanguage.compare(QStringLiteral("English"), Qt::CaseInsensitive) == 0;

    if (english) {
    static const QList<QPair<QString, QString>> replacements = {
        {QStringLiteral("<!doctype html><html><head>"), QStringLiteral("<!doctype html><html lang='en'><head>")},
        {QStringLiteral("目前還沒有任何分享項目，請回到桌面程式拖曳檔案或資料夾加入分享。"), QStringLiteral("No shared items yet. Add files or folders from the desktop app.")},
        {QStringLiteral("目前尚無可下載的分享項目。"), QStringLiteral("No shared items are available yet.")},
        {QStringLiteral("可直接瀏覽或下載分享內容"), QStringLiteral("Browse or download shared content")},
        {QStringLiteral("進入前需要輸入下載密碼"), QStringLiteral("Enter the download password to continue")},
        {QStringLiteral("此站台已啟用下載密碼保護"), QStringLiteral("This site is protected by a download password")},
        {QStringLiteral("<div class='row-note'>圖片預覽</div>"), QStringLiteral("<div class='row-note'>Image preview</div>")},
        {QStringLiteral(">直接下載</a>"), QStringLiteral(">Download</a>")},
        {QStringLiteral(">打包下載</button>"), QStringLiteral(">Package download</button>")},
        {QStringLiteral("點擊整列進入資料夾"), QStringLiteral("Click the row to open the folder")},
        {QStringLiteral("<div class='row-size'>進入資料夾</div>"), QStringLiteral("<div class='row-size'>Open folder</div>")},
        {QStringLiteral("正在準備打包…"), QStringLiteral("Preparing package...")},
        {QStringLiteral("正在讀取資料夾內容，請勿關閉此頁面。"), QStringLiteral("Reading the folder. Keep this page open.")},
        {QStringLiteral("正在打包 "), QStringLiteral("Packaging ")},
        {QStringLiteral("正在建立壓縮檔："), QStringLiteral("Creating archive: ")},
        {QStringLiteral("打包完成，開始下載…"), QStringLiteral("Package ready. Starting download...")},
        {QStringLiteral("壓縮檔已完成，瀏覽器將自動開始下載。"), QStringLiteral("The archive is ready and the browser will start downloading it automatically.")},
        {QStringLiteral("打包失敗"), QStringLiteral("Packaging failed")},
        {QStringLiteral("建立壓縮檔時發生錯誤。"), QStringLiteral("An error occurred while creating the archive.")},
        {QStringLiteral("無法建立打包工作。"), QStringLiteral("Unable to start the packaging job.")},
        {QStringLiteral("打包工作失敗。"), QStringLiteral("The packaging job failed.")},
        {QStringLiteral("目前打包工作較多，請稍後再試。"), QStringLiteral("Too many packaging jobs are running. Please try again later.")},
        {QStringLiteral(">關閉</button>"), QStringLiteral(">Close</button>")},
        {QStringLiteral("aria-label='關閉預覽'"), QStringLiteral("aria-label='Close preview'")},
        {QStringLiteral("aria-label='上一張'"), QStringLiteral("aria-label='Previous image'")},
        {QStringLiteral("aria-label='下一張'"), QStringLiteral("aria-label='Next image'")},
        {QStringLiteral(">下載原圖</a>"), QStringLiteral(">Download original</a>")},
        {QStringLiteral("密碼錯誤，請重新輸入。"), QStringLiteral("Incorrect password. Please try again.")},
        {QStringLiteral("此分享頁面已加上下載密碼。請輸入正確密碼後繼續。"), QStringLiteral("This share is password-protected. Enter the correct password to continue.")},
        {QStringLiteral("請輸入下載密碼"), QStringLiteral("Enter download password")},
        {QStringLiteral("進入下載頁面"), QStringLiteral("Continue")},
        {QStringLiteral("aria-label='選取 "), QStringLiteral("aria-label='Select ")},
        {QStringLiteral("<div class='row-note'>資料夾</div>"), QStringLiteral("<div class='row-note'>Folder</div>")},
        {QStringLiteral("<div class='row-size'>資料夾</div>"), QStringLiteral("<div class='row-size'>Folder</div>")},
        {QStringLiteral("<div class='row-note'>一般檔案</div>"), QStringLiteral("<div class='row-note'>File</div>")},
        {QStringLiteral("<div class='row-note'>檔案</div>"), QStringLiteral("<div class='row-note'>File</div>")},
        {QStringLiteral("這個資料夾目前沒有任何檔案。"), QStringLiteral("This folder is empty.")},
        {QStringLiteral("<div class='upload-title'>上傳檔案</div>"), QStringLiteral("<div class='upload-title'>Upload files</div>")},
        {QStringLiteral("可一次選擇多個檔案，完成後會自動重新整理清單。"), QStringLiteral("Select multiple files at once. The list refreshes automatically when uploads finish.")},
        {QStringLiteral("'分段重試中：'"), QStringLiteral("'Retrying chunk: '")},
        {QStringLiteral("'分段上傳中：'"), QStringLiteral("'Uploading chunks: '")},
        {QStringLiteral("'上傳中：'"), QStringLiteral("'Uploading: '")},
        {QStringLiteral("'完成：'"), QStringLiteral("'Completed: '")},
        {QStringLiteral("'失敗：'"), QStringLiteral("'Failed: '")},
        {QStringLiteral(">刪除所選<"), QStringLiteral(">Delete Selected<")},
        {QStringLiteral(">下載所選<"), QStringLiteral(">Download Selected<")},
        {QStringLiteral(">建立資料夾<"), QStringLiteral(">New Folder<")},
        {QStringLiteral(">未選取項目<"), QStringLiteral(">No items selected<")},
        {QStringLiteral("selectionDelete.textContent=count>0?'刪除所選 ('+count+')':'刪除所選';"), QStringLiteral("selectionDelete.textContent=count>0?'Delete Selected ('+count+')':'Delete Selected';")},
        {QStringLiteral("selectionDownload.textContent=count>0?'下載所選 ('+count+')':'下載所選';"), QStringLiteral("selectionDownload.textContent=count>0?'Download Selected ('+count+')':'Download Selected';")},
        {QStringLiteral("selectionSummary.textContent=count>0?'已選取 '+count+' 項':(hfsSelectionConfig.createDirectoryAllowed?'可建立資料夾':'未選取項目');"), QStringLiteral("selectionSummary.textContent=count>0?'Selected '+count+' items':(hfsSelectionConfig.createDirectoryAllowed?'You can create folders':'No items selected');")},
        {QStringLiteral("確定要刪除已選取的 "), QStringLiteral("Delete the selected ")},
        {QStringLiteral(" 個項目嗎？"), QStringLiteral(" items?")},
        {QStringLiteral("'刪除中...'"), QStringLiteral("'Deleting...'")},
        {QStringLiteral("'刪除失敗：'"), QStringLiteral("'Delete failed: '")},
        {QStringLiteral("'請輸入新資料夾名稱'"), QStringLiteral("'Enter a new folder name'")},
        {QStringLiteral("'資料夾名稱不可為空。'"), QStringLiteral("'Folder name cannot be empty.'")},
        {QStringLiteral("'建立中...'"), QStringLiteral("'Creating...'")},
        {QStringLiteral("'建立資料夾失敗：'"), QStringLiteral("'Failed to create folder: '")},
        {QStringLiteral("<span class='sort-label'>排序</span>"), QStringLiteral("<span class='sort-label'>Sort</span>")},
        {QStringLiteral(">檔名</a>"), QStringLiteral(">Name</a>")},
        {QStringLiteral(">日期</a>"), QStringLiteral(">Date</a>")},
        {QStringLiteral(">大小</a>"), QStringLiteral(">Size</a>")},
        {QStringLiteral(">升冪</a>"), QStringLiteral(">Ascending</a>")},
        {QStringLiteral(">降冪</a>"), QStringLiteral(">Descending</a>")},
        {QStringLiteral(">返回上一層</a>"), QStringLiteral(">Back</a>")},
        {QStringLiteral("<div class='meta'>目前路徑："), QStringLiteral("<div class='meta'>Current path: ")},
        {QStringLiteral("‹ 上一張"), QStringLiteral("‹ Previous")},
        {QStringLiteral("下一張 ›"), QStringLiteral("Next ›")},
        {QStringLiteral(">返回相簿</a>"), QStringLiteral(">Back to gallery</a>")},
        {QStringLiteral("‹ 上一個"), QStringLiteral("‹ Previous")},
        {QStringLiteral("下一個 ›"), QStringLiteral("Next ›")},
        {QStringLiteral("label='中文字幕'"), QStringLiteral("label='Subtitles'")},
        {QStringLiteral("PotPlayer 播放"), QStringLiteral("Play in PotPlayer")},
        {QStringLiteral("可直接線上播放影片；若想改用外部播放器，可使用 PotPlayer 播放。"), QStringLiteral("Play the video in your browser, or open it with PotPlayer.")},
        {QStringLiteral("可直接播放音樂，播放結束後會自動切換下一首，也可開啟亂數播放。"), QStringLiteral("Play audio in your browser. The next track starts automatically, with optional shuffle playback.")},
        {QStringLiteral(">返回清單</a>"), QStringLiteral(">Back to list</a>")},
        {QStringLiteral("亂數播放：關閉"), QStringLiteral("Shuffle: Off")},
        {QStringLiteral("亂數播放：開啟"), QStringLiteral("Shuffle: On")},
        {QStringLiteral("找不到內容"), QStringLiteral("Content not found")},
        {QStringLiteral("指定的分享項目或路徑不存在，請返回上一頁重新確認。"), QStringLiteral("The requested share or path does not exist. Go back and try again.")},
        {QStringLiteral("請求無效"), QStringLiteral("Invalid request")},
        {QStringLiteral("路徑錯誤"), QStringLiteral("Invalid path")},
        {QStringLiteral("請求的目錄超出分享範圍。"), QStringLiteral("The requested directory is outside the shared area.")},
        {QStringLiteral("資料夾不存在"), QStringLiteral("Folder not found")},
        {QStringLiteral("這個目錄目前已不存在，請返回上一頁。"), QStringLiteral("This directory no longer exists. Please go back.")},
        {QStringLiteral("找不到相片"), QStringLiteral("Image not found")},
        {QStringLiteral("目前無法建立這張相片的檢視頁。"), QStringLiteral("A viewer page cannot be created for this image right now.")},
        {QStringLiteral("這張相片目前不在可預覽清單內。"), QStringLiteral("This image is not currently in the preview list.")},
        {QStringLiteral("找不到媒體"), QStringLiteral("Media not found")},
        {QStringLiteral("目前無法建立這個檔案的播放頁面。"), QStringLiteral("A player page cannot be created for this file right now.")},
        {QStringLiteral("這個檔案目前不在可播放清單內。"), QStringLiteral("This file is not currently in the playable list.")},
        {QStringLiteral(" · 已載入字幕"), QStringLiteral(" · Subtitles loaded")},
        {QStringLiteral("收到空白的 HTTP 請求。"), QStringLiteral("Received an empty HTTP request.")},
        {QStringLiteral("HTTP 請求列格式不正確。"), QStringLiteral("The HTTP request line is invalid.")},
        {QStringLiteral("上傳工作初始化失敗，請重新整理頁面後再試一次。"), QStringLiteral("Failed to initialize the upload. Refresh the page and try again.")},
        {QStringLiteral("請求路徑超出分享範圍。"), QStringLiteral("The requested path is outside the shared area.")},
        {QStringLiteral("這個分享項目不允許上傳。"), QStringLiteral("Uploads are not allowed for this share.")},
        {QStringLiteral("上傳目錄無效。"), QStringLiteral("The upload directory is invalid.")},
        {QStringLiteral("上傳檔名不可為空。"), QStringLiteral("The upload file name cannot be empty.")},
        {QStringLiteral("上傳內容不可為空。"), QStringLiteral("The upload content cannot be empty.")},
        {QStringLiteral("無法寫入上傳檔案。"), QStringLiteral("Unable to write the uploaded file.")},
        {QStringLiteral("上傳工作尚未初始化。"), QStringLiteral("The upload has not been initialized.")},
        {QStringLiteral("寫入上傳檔案時發生錯誤。"), QStringLiteral("An error occurred while writing the uploaded file.")},
        {QStringLiteral("這個分享項目不允許建立資料夾。"), QStringLiteral("Creating folders is not allowed for this share.")},
        {QStringLiteral("資料夾名稱無效。"), QStringLiteral("The folder name is invalid.")},
        {QStringLiteral("目前目錄不存在或無效。"), QStringLiteral("The current directory does not exist or is invalid.")},
        {QStringLiteral("建立資料夾的目標路徑無效。"), QStringLiteral("The target path for the new folder is invalid.")},
        {QStringLiteral("同名資料夾或檔案已存在。"), QStringLiteral("A file or folder with the same name already exists.")},
        {QStringLiteral("建立資料夾失敗。"), QStringLiteral("Failed to create the folder.")},
        {QStringLiteral("這個分享項目不允許刪除。"), QStringLiteral("Deleting items is not allowed for this share.")},
        {QStringLiteral("刪除目標不可為空。"), QStringLiteral("The delete target cannot be empty.")},
        {QStringLiteral("刪除目標超出分享範圍。"), QStringLiteral("The delete target is outside the shared area.")},
        {QStringLiteral("不可刪除分享根目錄。"), QStringLiteral("The root of a share cannot be deleted.")},
        {QStringLiteral("要刪除的項目不存在。"), QStringLiteral("The item to delete does not exist.")},
        {QStringLiteral("刪除失敗，檔案可能正在使用中。"), QStringLiteral("Delete failed. The file may be in use.")},
        {QStringLiteral("上傳路徑不正確。"), QStringLiteral("The upload path is invalid.")},
        {QStringLiteral("找不到分享項目。"), QStringLiteral("Share not found.")},
        {QStringLiteral("上傳分塊參數不正確。"), QStringLiteral("The chunk upload parameters are invalid.")},
        {QStringLiteral("無法建立暫存上傳檔案。"), QStringLiteral("Unable to create the temporary upload file.")},
        {QStringLiteral("無法預先配置上傳暫存空間。"), QStringLiteral("Unable to allocate temporary upload space.")},
        {QStringLiteral("上傳分塊順序錯誤，請重新上傳檔案。"), QStringLiteral("The upload chunks are out of order. Please upload the file again.")},
        {QStringLiteral("無法寫入暫存上傳檔案。"), QStringLiteral("Unable to write the temporary upload file.")},
        {QStringLiteral("上傳分塊內容不正確。"), QStringLiteral("The upload chunk content is invalid.")},
        {QStringLiteral("無法定位上傳分塊位置。"), QStringLiteral("Unable to locate the upload chunk position.")},
        {QStringLiteral("寫入上傳分塊失敗。"), QStringLiteral("Failed to write the upload chunk.")},
        {QStringLiteral("上傳檔案大小不一致，請重新上傳。"), QStringLiteral("The uploaded file size does not match. Please upload it again.")},
        {QStringLiteral("完成上傳時無法建立最終檔案。"), QStringLiteral("Unable to create the final file when completing the upload.")}
    };

    for (const auto &replacement : replacements) {
        html.replace(replacement.first, replacement.second);
    }
    }

    // Mark the document language and inject one compact visitor-side language
    // switch. The switch preserves the current path/query, stores the choice
    // in a browser cookie through the server, and works independently for each
    // visitor.
    if (english) {
        html.replace(QStringLiteral("<!doctype html><html><head>"),
                     QStringLiteral("<!doctype html><html lang='en'><head>"));
    } else {
        html.replace(QStringLiteral("<!doctype html><html><head>"),
                     QStringLiteral("<!doctype html><html lang='zh-Hant'><head>"));
    }

    const QString zhClass = english ? QString() : QStringLiteral(" active");
    const QString enClass = english ? QStringLiteral(" active") : QString();
    const QString switchHtml = QStringLiteral(
        "<style id='hfs-lang-style'>"
        ".hfs-lang-switch{position:fixed;top:14px;right:14px;z-index:9999;display:flex;gap:3px;padding:4px;background:rgba(255,255,255,.92);border:1px solid rgba(132,146,171,.22);border-radius:999px;box-shadow:0 8px 24px rgba(30,42,68,.14);backdrop-filter:blur(10px);font-family:'Microsoft JhengHei UI','Segoe UI',sans-serif;}"
        ".hfs-lang-btn{appearance:none;border:0;background:transparent;color:#66758f;min-width:46px;height:32px;padding:0 11px;border-radius:999px;font-size:13px;font-weight:800;cursor:pointer;transition:.16s ease;}"
        ".hfs-lang-btn:hover{background:#f1efff;color:#6557e8;}"
        ".hfs-lang-btn.active{background:linear-gradient(135deg,#7c4dff,#4f6df5);color:#fff;box-shadow:0 4px 12px rgba(99,84,235,.24);}"
        "@media(max-width:640px){.hfs-lang-switch{top:auto;right:10px;bottom:10px}.hfs-lang-btn{height:30px;min-width:44px;padding:0 9px}}"
        "</style>"
        "<div class='hfs-lang-switch' role='group' aria-label='Language'>"
        "<button type='button' class='hfs-lang-btn%1' data-hfs-lang='zh-TW'>繁中</button>"
        "<button type='button' class='hfs-lang-btn%2' data-hfs-lang='en'>EN</button>"
        "</div>"
        "<script>(function(){"
        "function setLang(lang){var u=new URL(window.location.href);u.searchParams.set('lang',lang);window.location.href=u.toString();}"
        "var bs=document.querySelectorAll('[data-hfs-lang]');for(var i=0;i<bs.length;i++){bs[i].addEventListener('click',function(){setLang(this.getAttribute('data-hfs-lang'));});}"
        "})();</script>")
        .arg(zhClass, enClass);

    const qsizetype bodyEnd = html.lastIndexOf(QStringLiteral("</body>"), -1, Qt::CaseInsensitive);
    if (bodyEnd >= 0) {
        html.insert(bodyEnd, switchHtml);
    } else {
        html += switchHtml;
    }

    return html;
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

    // Keep a moderate amount queued in Qt.  Progress is based on bytes accepted
    // by QTcpSocket::write(), so it can lead the browser by at most this small
    // buffer, while avoiding thousands of 256 KB callbacks on fast LAN links.
    constexpr qint64 kMaxPendingSocketBytes = 8 * 1024 * 1024;
    constexpr qint64 kUnlimitedChunkBytes = 4 * 1024 * 1024;

    const qint64 totalLimit = totalLimitBytesPerSecond();
    const qint64 perIpLimit = perIpLimitBytesPerSecond();
    qint64 globalBudget = totalLimit > 0
                              ? qMax<qint64>(1, totalLimit / 20)
                              : kUnlimitedChunkBytes;
    QHash<QString, qint64> perIpRemaining;

    const QList<QTcpSocket *> keys = m_transfers.keys();
    for (QTcpSocket *socket : keys) {
        FileTransfer *transfer = m_transfers.value(socket, nullptr);
        if (!transfer || !transfer->socket || !transfer->file) {
            finalizeTransfer(socket, false);
            continue;
        }

        if (socket->state() == QAbstractSocket::UnconnectedState) {
            finalizeTransfer(socket, false);
            continue;
        }

        if (transfer->remaining <= 0) {
            if (socket->bytesToWrite() <= 0) {
                finalizeTransfer(socket, true);
            }
            continue;
        }

        const qint64 pendingRoom = kMaxPendingSocketBytes - socket->bytesToWrite();
        if (pendingRoom <= 0) {
            continue;
        }

        qint64 chunkBudget = qMin(kUnlimitedChunkBytes, pendingRoom);
        if (totalLimit > 0) {
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
            finalizeTransfer(socket, false);
            continue;
        }

        const qint64 queuedBytes = transfer->socket->write(chunk);
        if (queuedBytes <= 0) {
            finalizeTransfer(socket, false);
            continue;
        }

        if (queuedBytes < chunk.size()) {
            const qint64 rewindBytes = chunk.size() - queuedBytes;
            if (!transfer->file->seek(transfer->file->pos() - rewindBytes)) {
                finalizeTransfer(socket, false);
                continue;
            }
        }

        transfer->remaining -= queuedBytes;
        transfer->bytesQueued += queuedBytes;

        // Counting accepted payload bytes is stable on loopback/LAN downloads
        // and differs from actual browser receipt by no more than the bounded
        // socket queue above.  It is also cheap enough not to disturb transfer
        // performance.
        if (transfer->trackAsDownload) {
            m_totalBytes += static_cast<quint64>(queuedBytes);
            m_windowBytes += queuedBytes;
        }

        if (totalLimit > 0) {
            globalBudget -= queuedBytes;
        }
        if (perIpLimit > 0) {
            perIpRemaining[transfer->clientAddress] -= queuedBytes;
        }

        if (transfer->remaining <= 0 && socket->bytesToWrite() <= 0) {
            finalizeTransfer(socket, true);
        }
    }

    // Statistics are published by the one-second stats timer.  Emitting them
    // for every network write was the main source of dashboard lag.
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
        record.bytesTransferred = transfer->bytesQueued;
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

    const QString cleanupArchiveJobId = transfer->cleanupArchiveJobId;
    if (socket->state() == QAbstractSocket::ConnectedState) {
        socket->disconnectFromHost();
    }
    if (!cleanupArchiveJobId.isEmpty()) {
        cleanupArchiveJob(cleanupArchiveJobId, true);
    }
    publishActiveTransfers();
    updateStats();
}

void HttpFileServer::updateStats()
{
    emit statsChanged(m_totalDownloads, m_totalBytes, activeTransferCount(), m_lastBytesPerSecond);
}

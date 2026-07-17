#pragma once

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QtGlobal>

enum class ShareType {
    File,
    Directory,
    VirtualDirectory,
    Link
};

struct DownloadSettings {
    QString password;
    int totalLimitValue = 0;
    QString totalLimitUnit = QStringLiteral("KB/s");
    bool perIpLimitEnabled = false;
    int perIpLimitValue = 0;
    QString perIpLimitUnit = QStringLiteral("KB/s");
    bool resumeEnabled = true;
};

struct AppSettings {
    QString siteName = QStringLiteral("Easy Cloud HFS");
    quint16 port = 80;
    QString language = QStringLiteral("\u7e41\u9ad4\u4e2d\u6587");
    QString theme = QStringLiteral("\u8ddf\u96a8\u7cfb\u7d71");
    QString updateFrequency = QStringLiteral("每週");
    QString logoPath;
    bool externalLinkEnabled = false;
    bool chatEnabled = true;
    QString instanceId;
    int windowWidth = 1360;
    int windowHeight = 820;
    bool windowMaximized = false;
    bool minimizeToTrayOnClose = true;
    bool launchOnStartup = false;
    bool startServerOnLaunch = false;
    bool clearSharesOnExit = false;
    DownloadSettings download;
    QString cloudflaredPath;
    QString uploadsRoot;
};

struct ShareItem {
    QString id;
    ShareType type = ShareType::Directory;
    QString name;
    QString routeSegment;
    QString sourcePath;
    QString storagePath;
    bool enabled = true;
    bool allowUpload = false;
    bool allowDelete = false;
    bool allowCreateDirectory = false;
    bool visibleOnHome = true;
    qint64 pinnedSize = 0;
};

struct DownloadRecord {
    QString id;
    QString shareId;
    QString fileName;
    QString relativePath;
    QString clientAddress;
    qint64 bytesTransferred = 0;
    bool success = true;
    QDateTime timestamp;
};


struct ActiveTransferInfo {
    QString id;
    QString fileName;
    QString relativePath;
    QString clientAddress;
    QString status;
    qint64 bytesProcessed = 0;
    qint64 totalBytes = 0;
};

struct ServerStats {
    quint64 totalDownloads = 0;
    quint64 totalBytes = 0;
    int shareCount = 0;
    int activeConnections = 0;
    qint64 currentBytesPerSecond = 0;
};

QString shareTypeToString(ShareType type);
ShareType shareTypeFromString(const QString &value);

QJsonObject toJson(const DownloadSettings &settings);
QJsonObject toJson(const AppSettings &settings);
QJsonObject toJson(const ShareItem &item);
QJsonObject toJson(const DownloadRecord &record);
QJsonArray toJson(const QList<ShareItem> &items);
QJsonArray toJson(const QList<DownloadRecord> &records);

DownloadSettings downloadSettingsFromJson(const QJsonObject &object);
AppSettings appSettingsFromJson(const QJsonObject &object);
ShareItem shareItemFromJson(const QJsonObject &object);
DownloadRecord downloadRecordFromJson(const QJsonObject &object);
QList<ShareItem> shareListFromJson(const QJsonArray &array);
QList<DownloadRecord> downloadRecordListFromJson(const QJsonArray &array);

QString createId();
QString slugify(const QString &value);
QString ensureUniqueRoute(const QString &preferred, const QList<ShareItem> &items);
QString humanReadableSize(qint64 bytes);
qint64 limitToBytesPerSecond(int value, const QString &unit);

Q_DECLARE_METATYPE(ActiveTransferInfo)
Q_DECLARE_METATYPE(QList<ActiveTransferInfo>)

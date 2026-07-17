#include "app/Models.h"

#include <QJsonValue>
#include <QRegularExpression>
#include <QUuid>
#include <QtGlobal>

namespace {
QString normalizeUnit(const QString &unit)
{
    const QString trimmed = unit.trimmed().toUpper();
    if (trimmed == QStringLiteral("GB/S")) {
        return QStringLiteral("GB/s");
    }
    if (trimmed == QStringLiteral("MB/S")) {
        return QStringLiteral("MB/s");
    }
    return QStringLiteral("KB/s");
}

QString normalizeLanguageName(const QString &language)
{
    const QString trimmed = language.trimmed();
    if (trimmed.compare(QStringLiteral("English"), Qt::CaseInsensitive) == 0) {
        return QStringLiteral("English");
    }
    return QStringLiteral("\u7e41\u9ad4\u4e2d\u6587");
}

QString normalizeThemeName(const QString &theme)
{
    const QString trimmed = theme.trimmed();
    if (trimmed == QStringLiteral("\u6df1\u8272\u6a21\u5f0f")) {
        return QStringLiteral("\u6df1\u8272\u6a21\u5f0f");
    }
    if (trimmed == QStringLiteral("\u6dfa\u8272\u6a21\u5f0f")) {
        return QStringLiteral("\u6dfa\u8272\u6a21\u5f0f");
    }
    return QStringLiteral("\u8ddf\u96a8\u7cfb\u7d71");
}

QRegularExpression unicodeSeparatorPattern()
{
    QRegularExpression expression(QStringLiteral(R"([^\p{L}\p{N}]+)"));
    expression.setPatternOptions(QRegularExpression::UseUnicodePropertiesOption);
    return expression;
}

}

QString shareTypeToString(ShareType type)
{
    switch (type) {
    case ShareType::File:
        return QStringLiteral("file");
    case ShareType::Directory:
        return QStringLiteral("directory");
    case ShareType::VirtualDirectory:
        return QStringLiteral("virtual-directory");
    case ShareType::Link:
        return QStringLiteral("link");
    }
    return QStringLiteral("directory");
}

ShareType shareTypeFromString(const QString &value)
{
    if (value == QStringLiteral("file")) {
        return ShareType::File;
    }
    if (value == QStringLiteral("virtual-directory")) {
        return ShareType::VirtualDirectory;
    }
    if (value == QStringLiteral("link")) {
        return ShareType::Link;
    }
    return ShareType::Directory;
}

QJsonObject toJson(const DownloadSettings &settings)
{
    return {
        {QStringLiteral("password"), settings.password},
        {QStringLiteral("totalLimitValue"), settings.totalLimitValue},
        {QStringLiteral("totalLimitUnit"), settings.totalLimitUnit},
        {QStringLiteral("perIpLimitEnabled"), settings.perIpLimitEnabled},
        {QStringLiteral("perIpLimitValue"), settings.perIpLimitValue},
        {QStringLiteral("perIpLimitUnit"), settings.perIpLimitUnit},
        {QStringLiteral("resumeEnabled"), settings.resumeEnabled},
    };
}

QJsonObject toJson(const AppSettings &settings)
{
    return {
        {QStringLiteral("siteName"), settings.siteName},
        {QStringLiteral("port"), static_cast<int>(settings.port)},
        {QStringLiteral("language"), settings.language},
        {QStringLiteral("theme"), settings.theme},
        {QStringLiteral("updateFrequency"), settings.updateFrequency},
        {QStringLiteral("logoPath"), settings.logoPath},
        {QStringLiteral("externalLinkEnabled"), settings.externalLinkEnabled},
        {QStringLiteral("chatEnabled"), settings.chatEnabled},
        {QStringLiteral("instanceId"), settings.instanceId},
        {QStringLiteral("windowWidth"), settings.windowWidth},
        {QStringLiteral("windowHeight"), settings.windowHeight},
        {QStringLiteral("windowMaximized"), settings.windowMaximized},
        {QStringLiteral("minimizeToTrayOnClose"), settings.minimizeToTrayOnClose},
        {QStringLiteral("launchOnStartup"), settings.launchOnStartup},
        {QStringLiteral("startServerOnLaunch"), settings.startServerOnLaunch},
        {QStringLiteral("clearSharesOnExit"), settings.clearSharesOnExit},
        {QStringLiteral("cloudflaredPath"), settings.cloudflaredPath},
        {QStringLiteral("uploadsRoot"), settings.uploadsRoot},
        {QStringLiteral("download"), toJson(settings.download)},
    };
}

QJsonObject toJson(const ShareItem &item)
{
    return {
        {QStringLiteral("id"), item.id},
        {QStringLiteral("type"), shareTypeToString(item.type)},
        {QStringLiteral("name"), item.name},
        {QStringLiteral("routeSegment"), item.routeSegment},
        {QStringLiteral("sourcePath"), item.sourcePath},
        {QStringLiteral("storagePath"), item.storagePath},
        {QStringLiteral("enabled"), item.enabled},
        {QStringLiteral("allowUpload"), item.allowUpload},
        {QStringLiteral("allowDelete"), item.allowDelete},
        {QStringLiteral("allowCreateDirectory"), item.allowCreateDirectory},
        {QStringLiteral("visibleOnHome"), item.visibleOnHome},
        {QStringLiteral("pinnedSize"), QString::number(item.pinnedSize)},
    };
}

QJsonObject toJson(const DownloadRecord &record)
{
    return {
        {QStringLiteral("id"), record.id},
        {QStringLiteral("shareId"), record.shareId},
        {QStringLiteral("fileName"), record.fileName},
        {QStringLiteral("relativePath"), record.relativePath},
        {QStringLiteral("clientAddress"), record.clientAddress},
        {QStringLiteral("bytesTransferred"), QString::number(record.bytesTransferred)},
        {QStringLiteral("success"), record.success},
        {QStringLiteral("timestamp"), record.timestamp.toString(Qt::ISODate)},
    };
}

QJsonArray toJson(const QList<ShareItem> &items)
{
    QJsonArray array;
    for (const ShareItem &item : items) {
        array.append(toJson(item));
    }
    return array;
}

QJsonArray toJson(const QList<DownloadRecord> &records)
{
    QJsonArray array;
    for (const DownloadRecord &record : records) {
        array.append(toJson(record));
    }
    return array;
}

DownloadSettings downloadSettingsFromJson(const QJsonObject &object)
{
    DownloadSettings settings;
    settings.password = object.value(QStringLiteral("password")).toString();
    settings.totalLimitValue = object.value(QStringLiteral("totalLimitValue")).toInt();
    settings.totalLimitUnit = normalizeUnit(object.value(QStringLiteral("totalLimitUnit")).toString(QStringLiteral("KB/s")));
    settings.perIpLimitEnabled = object.value(QStringLiteral("perIpLimitEnabled")).toBool(false);
    settings.perIpLimitValue = object.value(QStringLiteral("perIpLimitValue")).toInt();
    settings.perIpLimitUnit = normalizeUnit(object.value(QStringLiteral("perIpLimitUnit")).toString(QStringLiteral("KB/s")));
    settings.resumeEnabled = object.value(QStringLiteral("resumeEnabled")).toBool(true);
    return settings;
}

AppSettings appSettingsFromJson(const QJsonObject &object)
{
    AppSettings settings;
    settings.siteName = object.value(QStringLiteral("siteName")).toString(settings.siteName);
    const int parsedPort = object.value(QStringLiteral("port")).toInt(settings.port);
    settings.port = static_cast<quint16>(qBound(1, parsedPort, 65535));
    settings.language = normalizeLanguageName(object.value(QStringLiteral("language")).toString(settings.language));
    settings.theme = normalizeThemeName(object.value(QStringLiteral("theme")).toString(settings.theme));
    settings.updateFrequency = object.value(QStringLiteral("updateFrequency")).toString(settings.updateFrequency);
    settings.logoPath = object.value(QStringLiteral("logoPath")).toString();
    settings.externalLinkEnabled = object.value(QStringLiteral("externalLinkEnabled")).toBool(false);
    settings.chatEnabled = object.value(QStringLiteral("chatEnabled")).toBool(true);
    settings.instanceId = object.value(QStringLiteral("instanceId")).toString();
    settings.windowWidth = qMax(780, object.value(QStringLiteral("windowWidth")).toInt(settings.windowWidth));
    settings.windowHeight = qMax(520, object.value(QStringLiteral("windowHeight")).toInt(settings.windowHeight));
    settings.windowMaximized = object.value(QStringLiteral("windowMaximized")).toBool(false);
    settings.minimizeToTrayOnClose = object.value(QStringLiteral("minimizeToTrayOnClose")).toBool(true);
    settings.launchOnStartup = object.value(QStringLiteral("launchOnStartup")).toBool(false);
    settings.startServerOnLaunch = object.value(QStringLiteral("startServerOnLaunch")).toBool(false);
    if (settings.launchOnStartup) {
        settings.startServerOnLaunch = true;
    }
    settings.clearSharesOnExit = false;
    settings.cloudflaredPath = object.value(QStringLiteral("cloudflaredPath")).toString();
    settings.uploadsRoot = object.value(QStringLiteral("uploadsRoot")).toString();
    if (object.value(QStringLiteral("download")).isObject()) {
        settings.download = downloadSettingsFromJson(object.value(QStringLiteral("download")).toObject());
    }
    return settings;
}

ShareItem shareItemFromJson(const QJsonObject &object)
{
    ShareItem item;
    item.id = object.value(QStringLiteral("id")).toString();
    item.type = shareTypeFromString(object.value(QStringLiteral("type")).toString());
    item.name = object.value(QStringLiteral("name")).toString();
    item.routeSegment = object.value(QStringLiteral("routeSegment")).toString();
    item.sourcePath = object.value(QStringLiteral("sourcePath")).toString();
    item.storagePath = object.value(QStringLiteral("storagePath")).toString();
    item.enabled = object.value(QStringLiteral("enabled")).toBool(true);
    item.allowUpload = object.value(QStringLiteral("allowUpload")).toBool(false);
    item.allowDelete = object.value(QStringLiteral("allowDelete")).toBool(false);
    item.allowCreateDirectory = object.value(QStringLiteral("allowCreateDirectory")).toBool(false);
    item.visibleOnHome = object.value(QStringLiteral("visibleOnHome")).toBool(true);
    item.pinnedSize = object.value(QStringLiteral("pinnedSize")).toString().toLongLong();
    return item;
}

DownloadRecord downloadRecordFromJson(const QJsonObject &object)
{
    DownloadRecord record;
    record.id = object.value(QStringLiteral("id")).toString();
    record.shareId = object.value(QStringLiteral("shareId")).toString();
    record.fileName = object.value(QStringLiteral("fileName")).toString();
    record.relativePath = object.value(QStringLiteral("relativePath")).toString();
    record.clientAddress = object.value(QStringLiteral("clientAddress")).toString();
    record.bytesTransferred = object.value(QStringLiteral("bytesTransferred")).toString().toLongLong();
    record.success = object.value(QStringLiteral("success")).toBool(true);
    record.timestamp = QDateTime::fromString(object.value(QStringLiteral("timestamp")).toString(), Qt::ISODate);
    if (!record.timestamp.isValid()) {
        record.timestamp = QDateTime::currentDateTime();
    }
    return record;
}

QList<ShareItem> shareListFromJson(const QJsonArray &array)
{
    QList<ShareItem> items;
    items.reserve(array.size());
    for (const QJsonValue &value : array) {
        if (value.isObject()) {
            items.append(shareItemFromJson(value.toObject()));
        }
    }
    return items;
}

QList<DownloadRecord> downloadRecordListFromJson(const QJsonArray &array)
{
    QList<DownloadRecord> records;
    records.reserve(array.size());
    for (const QJsonValue &value : array) {
        if (value.isObject()) {
            records.append(downloadRecordFromJson(value.toObject()));
        }
    }
    return records;
}

QString createId()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

QString slugify(const QString &value)
{
    QString candidate = value.trimmed().toLower();
    candidate.replace(unicodeSeparatorPattern(), QStringLiteral("-"));
    candidate.replace(QRegularExpression(QStringLiteral(R"(-+)")), QStringLiteral("-"));
    candidate.remove(QRegularExpression(QStringLiteral(R"(^-+|-+$)")));

    if (candidate.isEmpty()) {
        candidate = QStringLiteral("share-") + createId().left(8);
    }
    return candidate;
}

QString ensureUniqueRoute(const QString &preferred, const QList<ShareItem> &items)
{
    const QString base = preferred.trimmed().isEmpty() ? QStringLiteral("share") : preferred.trimmed();
    QString candidate = base;
    int suffix = 2;

    while (true) {
        bool exists = false;
        for (const ShareItem &item : items) {
            if (item.routeSegment.compare(candidate, Qt::CaseInsensitive) == 0) {
                exists = true;
                break;
            }
        }
        if (!exists) {
            return candidate;
        }
        candidate = QStringLiteral("%1-%2").arg(base).arg(suffix++);
    }
}

QString humanReadableSize(qint64 bytes)
{
    constexpr qint64 kilo = 1024;
    constexpr qint64 mega = kilo * 1024;
    constexpr qint64 giga = mega * 1024;

    if (bytes >= giga) {
        return QString::number(static_cast<double>(bytes) / static_cast<double>(giga), 'f', 2) + QStringLiteral(" GB");
    }
    if (bytes >= mega) {
        return QString::number(static_cast<double>(bytes) / static_cast<double>(mega), 'f', 2) + QStringLiteral(" MB");
    }
    if (bytes >= kilo) {
        return QString::number(static_cast<double>(bytes) / static_cast<double>(kilo), 'f', 2) + QStringLiteral(" KB");
    }
    return QString::number(bytes) + QStringLiteral(" B");
}

qint64 limitToBytesPerSecond(int value, const QString &unit)
{
    if (value <= 0) {
        return 0;
    }

    const QString normalized = normalizeUnit(unit);
    if (normalized == QStringLiteral("GB/s")) {
        return static_cast<qint64>(value) * 1024 * 1024 * 1024;
    }
    if (normalized == QStringLiteral("MB/s")) {
        return static_cast<qint64>(value) * 1024 * 1024;
    }
    return static_cast<qint64>(value) * 1024;
}

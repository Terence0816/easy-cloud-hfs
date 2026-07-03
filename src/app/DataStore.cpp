#include "app/DataStore.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QSettings>
#include <QStandardPaths>

namespace {
QJsonObject readObjectFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }

    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    return document.isObject() ? document.object() : QJsonObject{};
}

QJsonArray readArrayFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }

    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    return document.isArray() ? document.array() : QJsonArray{};
}
}

DataStore::DataStore(QObject *parent)
    : QObject(parent)
{
}

AppSettings DataStore::loadSettings() const
{
    const QJsonObject root = loadStoreObject();

    AppSettings settings;
    if (root.value(QStringLiteral("settings")).isObject()) {
        settings = appSettingsFromJson(root.value(QStringLiteral("settings")).toObject());
    } else if (!root.isEmpty()) {
        settings = appSettingsFromJson(root);
    }

    if (settings.uploadsRoot.isEmpty()) {
        settings.uploadsRoot = managedVirtualRoot();
    }
    return settings;
}

QList<ShareItem> DataStore::loadShares() const
{
    const bool iniExists = QFile::exists(sharesPath());
    QSettings ini(sharesPath(), QSettings::IniFormat);
    const int size = ini.beginReadArray(QStringLiteral("shares"));
    QList<ShareItem> items;
    items.reserve(size);
    for (int index = 0; index < size; ++index) {
        ini.setArrayIndex(index);

        ShareItem item;
        item.id = ini.value(QStringLiteral("id")).toString();
        item.type = shareTypeFromString(ini.value(QStringLiteral("type")).toString());
        item.name = ini.value(QStringLiteral("name")).toString();
        item.routeSegment = ini.value(QStringLiteral("routeSegment")).toString();
        item.sourcePath = ini.value(QStringLiteral("sourcePath")).toString();
        item.storagePath = ini.value(QStringLiteral("storagePath")).toString();
        item.enabled = ini.value(QStringLiteral("enabled"), true).toBool();
        item.allowUpload = ini.value(QStringLiteral("allowUpload"), false).toBool();
        item.allowDelete = ini.value(QStringLiteral("allowDelete"), false).toBool();
        item.allowCreateDirectory = ini.value(QStringLiteral("allowCreateDirectory"), false).toBool();
        item.visibleOnHome = ini.value(QStringLiteral("visibleOnHome"), true).toBool();
        item.pinnedSize = ini.value(QStringLiteral("pinnedSize"), 0).toLongLong();

        if (!item.id.isEmpty()) {
            items.append(item);
        }
    }
    ini.endArray();

    if (iniExists) {
        return items;
    }

    const QJsonObject root = loadStoreObject();
    if (root.value(QStringLiteral("shares")).isArray()) {
        return shareListFromJson(root.value(QStringLiteral("shares")).toArray());
    }
    return {};
}

QList<DownloadRecord> DataStore::loadDownloads() const
{
    const QJsonArray primaryArray = readArrayFile(downloadsPath());
    if (!primaryArray.isEmpty()) {
        return downloadRecordListFromJson(primaryArray);
    }

    const QJsonObject root = loadStoreObject();
    if (root.value(QStringLiteral("downloads")).isArray()) {
        return downloadRecordListFromJson(root.value(QStringLiteral("downloads")).toArray());
    }

    return {};
}

bool DataStore::saveSettings(const AppSettings &settings) const
{
    QJsonObject root = loadStoreObject();
    root.insert(QStringLiteral("settings"), toJson(settings));
    if (!root.contains(QStringLiteral("shares"))) {
        root.insert(QStringLiteral("shares"), QJsonArray{});
    }
    root.insert(QStringLiteral("schemaVersion"), 2);
    return saveStoreObject(root);
}

bool DataStore::saveShares(const QList<ShareItem> &shares) const
{
    QSettings ini(sharesPath(), QSettings::IniFormat);
    ini.clear();
    ini.beginWriteArray(QStringLiteral("shares"));
    for (int index = 0; index < shares.size(); ++index) {
        const ShareItem &item = shares.at(index);
        ini.setArrayIndex(index);
        ini.setValue(QStringLiteral("id"), item.id);
        ini.setValue(QStringLiteral("type"), shareTypeToString(item.type));
        ini.setValue(QStringLiteral("name"), item.name);
        ini.setValue(QStringLiteral("routeSegment"), item.routeSegment);
        ini.setValue(QStringLiteral("sourcePath"), item.sourcePath);
        ini.setValue(QStringLiteral("storagePath"), item.storagePath);
        ini.setValue(QStringLiteral("enabled"), item.enabled);
        ini.setValue(QStringLiteral("allowUpload"), item.allowUpload);
        ini.setValue(QStringLiteral("allowDelete"), item.allowDelete);
        ini.setValue(QStringLiteral("allowCreateDirectory"), item.allowCreateDirectory);
        ini.setValue(QStringLiteral("visibleOnHome"), item.visibleOnHome);
        ini.setValue(QStringLiteral("pinnedSize"), item.pinnedSize);
    }
    ini.endArray();
    ini.sync();
    return ini.status() == QSettings::NoError;
}

bool DataStore::saveDownloads(const QList<DownloadRecord> &downloads) const
{
    QFile file(downloadsPath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }

    file.write(QJsonDocument(toJson(downloads)).toJson(QJsonDocument::Indented));
    return true;
}

QString DataStore::appDataRoot() const
{
#ifdef Q_OS_ANDROID
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return ensureRoot(base.isEmpty() ? QDir::homePath() + "/EasyCloudHFS-Android" : base);
#else
    const QString appDir = QCoreApplication::applicationDirPath();
    return ensureRoot(appDir.isEmpty() ? QDir::currentPath() : appDir);
#endif
}

QString DataStore::runtimeRoot() const
{
    return ensureRoot(appDataRoot() + "/runtime");
}

QString DataStore::managedVirtualRoot() const
{
    return ensureRoot(appDataRoot() + "/virtual");
}

QString DataStore::managedBinRoot() const
{
    return ensureRoot(appDataRoot() + "/bin");
}

QJsonObject DataStore::loadStoreObject() const
{
    const QJsonObject primary = readObjectFile(storePath());
    if (!primary.isEmpty()) {
        return primary;
    }

    QJsonObject merged;

    const auto mergeLegacyRoot = [this, &merged](const QString &rootPath) {
        if (rootPath.isEmpty()) {
            return;
        }

        const QString configPath = rootPath + "/config.json";
        const QString sharesFilePath = rootPath + "/shares.json";
        if (!merged.contains(QStringLiteral("settings"))) {
            const QJsonObject settingsObject = readObjectFile(configPath);
            if (!settingsObject.isEmpty()) {
                merged.insert(QStringLiteral("settings"), settingsObject);
            }
        }

        if (!merged.contains(QStringLiteral("shares"))) {
            const QJsonArray sharesArray = readArrayFile(sharesFilePath);
            if (!sharesArray.isEmpty()) {
                merged.insert(QStringLiteral("shares"), sharesArray);
            }
        }

    };

    mergeLegacyRoot(appDataRoot());
    if (legacyAppDataRoot() != appDataRoot()) {
        mergeLegacyRoot(legacyAppDataRoot());
    }

    if (!merged.isEmpty()) {
        merged.insert(QStringLiteral("schemaVersion"), 2);
    }
    return merged;
}

bool DataStore::saveStoreObject(const QJsonObject &object) const
{
    QFile file(storePath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }

    file.write(QJsonDocument(object).toJson(QJsonDocument::Indented));
    return true;
}

QString DataStore::ensureRoot(const QString &path) const
{
    QDir dir;
    dir.mkpath(path);
    return path;
}

QString DataStore::storePath() const
{
    return appDataRoot() + "/easycloudhfs.json";
}

QString DataStore::settingsPath() const
{
    return appDataRoot() + "/config.json";
}

QString DataStore::sharesPath() const
{
    return appDataRoot() + "/shares.ini";
}

QString DataStore::downloadsPath() const
{
    return appDataRoot() + "/downloads.json";
}

QString DataStore::legacyAppDataRoot() const
{
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    return base.isEmpty() ? QDir::homePath() + "/EasyCloudHFS" : base;
}

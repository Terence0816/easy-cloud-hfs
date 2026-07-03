#pragma once

#include "app/Models.h"

#include <QObject>

class DataStore : public QObject {
    Q_OBJECT

public:
    explicit DataStore(QObject *parent = nullptr);

    [[nodiscard]] AppSettings loadSettings() const;
    [[nodiscard]] QList<ShareItem> loadShares() const;
    [[nodiscard]] QList<DownloadRecord> loadDownloads() const;

    bool saveSettings(const AppSettings &settings) const;
    bool saveShares(const QList<ShareItem> &shares) const;
    bool saveDownloads(const QList<DownloadRecord> &downloads) const;

    [[nodiscard]] QString appDataRoot() const;
    [[nodiscard]] QString runtimeRoot() const;
    [[nodiscard]] QString managedVirtualRoot() const;
    [[nodiscard]] QString managedBinRoot() const;

private:
    [[nodiscard]] QJsonObject loadStoreObject() const;
    bool saveStoreObject(const QJsonObject &object) const;
    [[nodiscard]] QString ensureRoot(const QString &path) const;
    [[nodiscard]] QString storePath() const;
    [[nodiscard]] QString settingsPath() const;
    [[nodiscard]] QString sharesPath() const;
    [[nodiscard]] QString downloadsPath() const;
    [[nodiscard]] QString legacyAppDataRoot() const;
};

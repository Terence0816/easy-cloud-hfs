#pragma once

#include <QObject>
#include <QPointer>
#include <QStringList>

class QNetworkAccessManager;
class QNetworkReply;
class QProcess;

class TunnelManager : public QObject {
    Q_OBJECT

public:
    explicit TunnelManager(const QString &binRoot, const QString &runtimeRoot, QObject *parent = nullptr);

    void setExecutableHint(const QString &path);

    [[nodiscard]] QString executablePath() const;
    [[nodiscard]] bool isInstalled() const;
    [[nodiscard]] bool isRunning() const;
    [[nodiscard]] QString publicUrl() const;
    [[nodiscard]] QString logs() const;

public slots:
    void downloadCloudflared();
    void startQuickTunnel(quint16 port);
    void stop();

signals:
    void statusChanged(const QString &status);
    void runningChanged(bool running);
    void publicUrlChanged(const QString &url);
    void logsChanged(const QString &logs);
    void downloadProgress(qint64 received, qint64 total);
    void executablePathChanged(const QString &path);
    void errorOccurred(const QString &message);

private:
    void appendLog(const QString &line);
    void refreshExecutablePath();
    void handleProcessText(const QString &text);

    QString m_binRoot;
    QString m_runtimeRoot;
    QString m_executableHint;
    QString m_executablePath;
    QString m_publicUrl;
    QStringList m_logs;
    QNetworkAccessManager *m_network = nullptr;
    QPointer<QNetworkReply> m_downloadReply;
    QProcess *m_process = nullptr;
};


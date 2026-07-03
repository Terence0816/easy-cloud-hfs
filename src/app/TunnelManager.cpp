#include "app/TunnelManager.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QSysInfo>
#include <QUrl>

namespace {
QString cloudflaredExecutableFileName()
{
#ifdef Q_OS_WIN
    return QStringLiteral("cloudflared.exe");
#else
    return QStringLiteral("cloudflared");
#endif
}

QString cloudflaredDownloadUrl()
{
#ifdef Q_OS_WIN
    return QStringLiteral("https://github.com/cloudflare/cloudflared/releases/latest/download/cloudflared-windows-amd64.exe");
#else
    const QString architecture = QSysInfo::currentCpuArchitecture().toLower();
    if (architecture.contains(QStringLiteral("aarch64"))
        || architecture.contains(QStringLiteral("arm64"))) {
        return QStringLiteral("https://github.com/cloudflare/cloudflared/releases/latest/download/cloudflared-linux-arm64");
    }
    if (architecture.contains(QStringLiteral("arm"))) {
        return QStringLiteral("https://github.com/cloudflare/cloudflared/releases/latest/download/cloudflared-linux-arm");
    }
    if (architecture.contains(QStringLiteral("x86_64"))
        || architecture.contains(QStringLiteral("amd64"))) {
        return QStringLiteral("https://github.com/cloudflare/cloudflared/releases/latest/download/cloudflared-linux-amd64");
    }
    return QStringLiteral("https://github.com/cloudflare/cloudflared/releases/latest/download/cloudflared-linux-amd64");
#endif
}
}

TunnelManager::TunnelManager(const QString &binRoot, const QString &runtimeRoot, QObject *parent)
    : QObject(parent)
    , m_binRoot(binRoot)
    , m_runtimeRoot(runtimeRoot)
    , m_network(new QNetworkAccessManager(this))
    , m_process(new QProcess(this))
{
    connect(m_process, &QProcess::readyReadStandardOutput, this, [this]() {
        handleProcessText(QString::fromUtf8(m_process->readAllStandardOutput()));
    });
    connect(m_process, &QProcess::readyReadStandardError, this, [this]() {
        handleProcessText(QString::fromUtf8(m_process->readAllStandardError()));
    });
    connect(m_process, &QProcess::stateChanged, this, [this](QProcess::ProcessState state) {
        emit runningChanged(state != QProcess::NotRunning);
    });
    connect(m_process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, [this](int code, QProcess::ExitStatus status) {
        Q_UNUSED(status)
        appendLog(QStringLiteral("cloudflared exited with code %1").arg(code));
        m_publicUrl.clear();
        emit publicUrlChanged(m_publicUrl);
        emit statusChanged(QStringLiteral("Cloudflare Tunnel is stopped"));
        emit runningChanged(false);
    });

    refreshExecutablePath();
}

void TunnelManager::setExecutableHint(const QString &path)
{
    m_executableHint = path;
    refreshExecutablePath();
}

QString TunnelManager::executablePath() const
{
    return m_executablePath;
}

bool TunnelManager::isInstalled() const
{
    return !m_executablePath.isEmpty() && QFileInfo::exists(m_executablePath);
}

bool TunnelManager::isRunning() const
{
    return m_process->state() != QProcess::NotRunning;
}

QString TunnelManager::publicUrl() const
{
    return m_publicUrl;
}

QString TunnelManager::logs() const
{
    return m_logs.join('\n');
}

void TunnelManager::downloadCloudflared()
{
    if (m_downloadReply) {
        return;
    }

    appendLog(QStringLiteral("Starting cloudflared download..."));
    emit statusChanged(QStringLiteral("Downloading cloudflared"));

    QNetworkRequest request{QUrl(cloudflaredDownloadUrl())};
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("EasyCloudHFS/1.0"));
    m_downloadReply = m_network->get(request);

    connect(m_downloadReply, &QNetworkReply::downloadProgress, this, &TunnelManager::downloadProgress);
    connect(m_downloadReply, &QNetworkReply::finished, this, [this]() {
        if (!m_downloadReply) {
            return;
        }

        const auto reply = m_downloadReply;
        m_downloadReply = nullptr;
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            const QString message = QStringLiteral("cloudflared download failed: %1").arg(reply->errorString());
            appendLog(message);
            emit errorOccurred(message);
            emit statusChanged(QStringLiteral("cloudflared download failed"));
            return;
        }

        const QByteArray payload = reply->readAll();
        if (payload.isEmpty()) {
            const QString message = QStringLiteral("cloudflared download failed: empty response");
            appendLog(message);
            emit errorOccurred(message);
            emit statusChanged(QStringLiteral("cloudflared download failed"));
            return;
        }

        QDir().mkpath(m_binRoot);
        const QString outputPath = QDir(m_binRoot).filePath(cloudflaredExecutableFileName());
        QFile file(outputPath);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            const QString message = QStringLiteral("Cannot write file: %1").arg(outputPath);
            appendLog(message);
            emit errorOccurred(message);
            emit statusChanged(QStringLiteral("cloudflared write failed"));
            return;
        }

        if (file.write(payload) != payload.size()) {
            const QString message = QStringLiteral("Cannot fully write file: %1").arg(outputPath);
            appendLog(message);
            emit errorOccurred(message);
            emit statusChanged(QStringLiteral("cloudflared write failed"));
            return;
        }
        file.close();

#ifndef Q_OS_WIN
        QFile::Permissions permissions = file.permissions();
        permissions |= QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner
                       | QFileDevice::ReadGroup | QFileDevice::ExeGroup
                       | QFileDevice::ReadOther | QFileDevice::ExeOther;
        QFile::setPermissions(outputPath, permissions);
#endif

        appendLog(QStringLiteral("Downloaded to %1").arg(outputPath));
        emit statusChanged(QStringLiteral("cloudflared installed"));
        refreshExecutablePath();
        emit executablePathChanged(m_executablePath);
    });
}

void TunnelManager::startQuickTunnel(quint16 port)
{
    refreshExecutablePath();
    if (!isInstalled()) {
        emit errorOccurred(QStringLiteral("cloudflared was not found"));
        return;
    }

    stop();

    QDir().mkpath(m_runtimeRoot);
    const QString workspace = QDir(m_runtimeRoot).filePath(QStringLiteral("cloudflare-home"));
    QDir().mkpath(workspace);

    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("HOME"), workspace);
    env.insert(QStringLiteral("USERPROFILE"), workspace);
    env.insert(QStringLiteral("HOMEDRIVE"), QFileInfo(workspace).absolutePath().left(2));
    env.insert(QStringLiteral("HOMEPATH"), workspace.mid(2));
    m_process->setProcessEnvironment(env);
    m_process->setWorkingDirectory(workspace);

    m_publicUrl.clear();
    emit publicUrlChanged(m_publicUrl);
    m_logs.clear();
    emit logsChanged(logs());

    const QString localUrl = QStringLiteral("http://127.0.0.1:%1").arg(port);
    appendLog(QStringLiteral("Starting Quick Tunnel for %1").arg(localUrl));
    emit statusChanged(QStringLiteral("Starting Cloudflare Tunnel"));

    const QStringList arguments{
        QStringLiteral("tunnel"),
        QStringLiteral("--url"),
        localUrl,
        QStringLiteral("--no-autoupdate"),
#if !defined(Q_OS_WIN)
        QStringLiteral("--protocol"),
        QStringLiteral("http2"),
#endif
    };
    m_process->start(m_executablePath, arguments);
    if (!m_process->waitForStarted(5000)) {
        const QString message = QStringLiteral("cloudflared start failed: %1").arg(m_process->errorString());
        appendLog(message);
        emit errorOccurred(message);
#ifdef Q_OS_ANDROID
        emit statusChanged(QStringLiteral("Cloudflare Tunnel start failed: Android may block external CLI execution"));
#else
        emit statusChanged(QStringLiteral("Cloudflare Tunnel start failed"));
#endif
        return;
    }

    emit runningChanged(true);
}

void TunnelManager::stop()
{
    if (!isRunning()) {
        return;
    }

    const qint64 processId = m_process->processId();
    appendLog(QStringLiteral("Stopping Cloudflare Tunnel"));
    m_process->terminate();
    if (!m_process->waitForFinished(3000)) {
#ifdef _WIN32
        if (processId > 0) {
            QProcess::execute(QStringLiteral("taskkill"),
                              QStringList{
                                  QStringLiteral("/PID"),
                                  QString::number(processId),
                                  QStringLiteral("/T"),
                                  QStringLiteral("/F"),
                              });
        }
#endif
        m_process->kill();
        m_process->waitForFinished(3000);
    }

    m_publicUrl.clear();
    emit publicUrlChanged(m_publicUrl);
    emit statusChanged(QStringLiteral("Cloudflare Tunnel is stopped"));
}

void TunnelManager::appendLog(const QString &line)
{
    const QString trimmed = line.trimmed();
    if (trimmed.isEmpty()) {
        return;
    }

    m_logs.append(trimmed);
    while (m_logs.size() > 400) {
        m_logs.removeFirst();
    }
    emit logsChanged(logs());
}

void TunnelManager::refreshExecutablePath()
{
    QString resolved;
    if (!m_executableHint.isEmpty() && QFileInfo::exists(m_executableHint)) {
        resolved = m_executableHint;
    }
    if (resolved.isEmpty()) {
        const QString managed = QDir(m_binRoot).filePath(cloudflaredExecutableFileName());
        if (QFileInfo::exists(managed)) {
            resolved = managed;
        }
    }
    if (resolved.isEmpty()) {
        resolved = QStandardPaths::findExecutable(QStringLiteral("cloudflared"));
    }

    if (resolved != m_executablePath) {
        m_executablePath = resolved;
        emit executablePathChanged(m_executablePath);
    }
}

void TunnelManager::handleProcessText(const QString &text)
{
    const QStringList lines = text.split('\n', Qt::SkipEmptyParts);
    const QRegularExpression urlPattern(R"(https://[-a-z0-9]+\.trycloudflare\.com)");

    for (const QString &line : lines) {
        const QString trimmed = line.trimmed();
        appendLog(trimmed);

        const auto match = urlPattern.match(trimmed);
        if (match.hasMatch()) {
            const QString url = match.captured(0);
            if (url != m_publicUrl) {
                m_publicUrl = url;
                emit publicUrlChanged(m_publicUrl);
                emit statusChanged(QStringLiteral("Cloudflare Tunnel is running"));
            }
        }
    }
}

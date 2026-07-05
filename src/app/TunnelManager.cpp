#include "app/TunnelManager.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QOperatingSystemVersion>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QSysInfo>
#include <QUrl>
#include <QVersionNumber>

namespace {
QString cloudflaredExecutableFileName()
{
#ifdef Q_OS_WIN
    return QStringLiteral("cloudflared.exe");
#else
    return QStringLiteral("cloudflared");
#endif
}

QString win7CloudflaredVersion()
{
    // Pinned for Windows 7. Do not auto-upgrade Win7 to current cloudflared builds.
    return QStringLiteral("2023.7.3");
}

bool isWindows7Runtime()
{
#ifdef Q_OS_WIN
    const QOperatingSystemVersion current = QOperatingSystemVersion::current();
    return current.type() == QOperatingSystemVersion::Windows
           && current.majorVersion() == 6
           && current.minorVersion() == 1;
#else
    return false;
#endif
}

QString cloudflaredDownloadUrl()
{
#ifdef Q_OS_WIN
    if (isWindows7Runtime()) {
        return QStringLiteral("https://github.com/cloudflare/cloudflared/releases/download/2023.7.3/cloudflared-windows-amd64.exe");
    }
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

QString cloudflaredLatestReleaseApiUrl()
{
    return QStringLiteral("https://api.github.com/repos/cloudflare/cloudflared/releases/latest");
}

QString extractVersionString(const QString &text)
{
    const QRegularExpression pattern(QStringLiteral(R"((\d+(?:\.\d+){1,3}))"));
    const QRegularExpressionMatch match = pattern.match(text);
    return match.hasMatch() ? match.captured(1) : QString();
}

bool looksLikeCloudflaredPayload(const QByteArray &payload)
{
    // Reject empty/HTML/error pages before replacing a working helper.
    if (payload.size() < 64 * 1024) {
        return false;
    }
#ifdef Q_OS_WIN
    return payload.size() >= 2 && payload.at(0) == 'M' && payload.at(1) == 'Z';
#else
    return true;
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
    connect(m_process,
            qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this,
            [this](int code, QProcess::ExitStatus status) {
                Q_UNUSED(status)
                if (!m_processTextBuffer.trimmed().isEmpty()) {
                    appendLog(m_processTextBuffer);
                }
                m_processTextBuffer.clear();
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

void TunnelManager::ensureLatestAndStart(quint16 port)
{
    m_pendingPort = port;
    m_startRequested = true;

    refreshExecutablePath();
    if (isRunning()) {
        return;
    }

    if (m_downloadReply || m_versionReply) {
        return;
    }

    if (!isInstalled()) {
        appendLog(QStringLiteral("cloudflared not found; downloading automatically"));
        downloadCloudflared();
        return;
    }

    if (m_updateCheckDone) {
        startPendingTunnel();
        return;
    }

    // Windows 7 must stay on the pinned compatible cloudflared build.
    // Never query/update Win7 to GitHub latest.
    if (isWindows7Runtime()) {
        const QString localVersion = localCloudflaredVersion();
        const QString pinnedVersion = win7CloudflaredVersion();

        if (localVersion == pinnedVersion) {
            m_updateCheckDone = true;
            appendLog(QStringLiteral("Windows 7 detected; using pinned cloudflared %1").arg(pinnedVersion));
            startPendingTunnel();
            return;
        }

        if (localVersion.isEmpty()) {
            appendLog(QStringLiteral("Windows 7 detected; cloudflared version is unavailable, installing pinned %1")
                          .arg(pinnedVersion));
        } else {
            appendLog(QStringLiteral("Windows 7 detected; replacing cloudflared %1 with pinned %2")
                          .arg(localVersion, pinnedVersion));
        }
        downloadCloudflared();
        return;
    }

    // Windows 10/11 and other supported platforms keep the current latest-version policy.
    checkLatestVersionAndContinue();
}

QString TunnelManager::localCloudflaredVersion() const
{
    if (!isInstalled()) {
        return {};
    }

    QProcess versionProcess;
    versionProcess.start(m_executablePath, QStringList{QStringLiteral("--version")});
    if (!versionProcess.waitForStarted(3000)) {
        return {};
    }
    if (!versionProcess.waitForFinished(5000)) {
        versionProcess.kill();
        versionProcess.waitForFinished(1000);
        return {};
    }

    const QString output = QString::fromUtf8(versionProcess.readAllStandardOutput())
                           + QString::fromUtf8(versionProcess.readAllStandardError());
    return extractVersionString(output);
}

void TunnelManager::checkLatestVersionAndContinue()
{
    if (m_versionReply || m_downloadReply) {
        return;
    }

    const QString localVersion = localCloudflaredVersion();
    if (localVersion.isEmpty()) {
        appendLog(QStringLiteral("Could not determine cloudflared version; refreshing automatically"));
        downloadCloudflared();
        return;
    }

    appendLog(QStringLiteral("Installed cloudflared version: %1").arg(localVersion));
    emit statusChanged(QStringLiteral("Checking cloudflared version"));

    QNetworkRequest request{QUrl(cloudflaredLatestReleaseApiUrl())};
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("EasyCloudHFS/1.0"));
    request.setRawHeader("Accept", "application/vnd.github+json");
    m_versionReply = m_network->get(request);

    connect(m_versionReply, &QNetworkReply::finished, this, [this, localVersion]() {
        if (!m_versionReply) {
            return;
        }

        const auto reply = m_versionReply;
        m_versionReply = nullptr;
        const QByteArray body = reply->readAll();
        const QNetworkReply::NetworkError networkError = reply->error();
        const QString networkErrorString = reply->errorString();
        reply->deleteLater();

        if (networkError != QNetworkReply::NoError) {
            m_updateCheckDone = true;
            finishWithExistingOrError(
                QStringLiteral("cloudflared version check failed: %1").arg(networkErrorString));
            return;
        }

        const QJsonDocument json = QJsonDocument::fromJson(body);
        const QString latestTag = json.isObject()
                                      ? json.object().value(QStringLiteral("tag_name")).toString()
                                      : QString();
        const QString latestVersion = extractVersionString(latestTag);
        if (latestVersion.isEmpty()) {
            m_updateCheckDone = true;
            finishWithExistingOrError(QStringLiteral("cloudflared version check failed: invalid latest release response"));
            return;
        }

        appendLog(QStringLiteral("Latest cloudflared version: %1").arg(latestVersion));

        const QVersionNumber local = QVersionNumber::fromString(localVersion);
        const QVersionNumber latest = QVersionNumber::fromString(latestVersion);
        if (local.isNull() || latest.isNull()) {
            appendLog(QStringLiteral("Version comparison was inconclusive; refreshing cloudflared automatically"));
            downloadCloudflared();
            return;
        }

        if (QVersionNumber::compare(local, latest) < 0) {
            appendLog(QStringLiteral("cloudflared is outdated; updating automatically"));
            downloadCloudflared();
            return;
        }

        m_updateCheckDone = true;
        appendLog(QStringLiteral("cloudflared is up to date"));
        startPendingTunnel();
    });
}

void TunnelManager::downloadCloudflared()
{
    if (m_downloadReply) {
        return;
    }

    if (isWindows7Runtime()) {
        appendLog(QStringLiteral("Windows 7: downloading pinned cloudflared %1")
                      .arg(win7CloudflaredVersion()));
        emit statusChanged(QStringLiteral("Downloading Windows 7 compatible cloudflared"));
    } else {
        appendLog(QStringLiteral("Starting latest cloudflared download..."));
        emit statusChanged(QStringLiteral("Downloading latest cloudflared"));
    }

    QNetworkRequest request{QUrl(cloudflaredDownloadUrl())};
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("EasyCloudHFS/1.0"));
    m_downloadReply = m_network->get(request);

    connect(m_downloadReply, &QNetworkReply::downloadProgress, this, &TunnelManager::downloadProgress);
    connect(m_downloadReply, &QNetworkReply::finished, this, [this]() {
        if (!m_downloadReply) {
            return;
        }

        const auto reply = m_downloadReply;
        m_downloadReply = nullptr;
        const QByteArray payload = reply->readAll();
        const QNetworkReply::NetworkError networkError = reply->error();
        const QString networkErrorString = reply->errorString();
        reply->deleteLater();

        if (networkError != QNetworkReply::NoError) {
            finishWithExistingOrError(
                QStringLiteral("cloudflared download failed: %1").arg(networkErrorString));
            return;
        }

        if (!looksLikeCloudflaredPayload(payload)) {
            finishWithExistingOrError(
                QStringLiteral("cloudflared download failed: invalid or incomplete executable response"));
            return;
        }

        QDir().mkpath(m_binRoot);
        const QString outputPath = QDir(m_binRoot).filePath(cloudflaredExecutableFileName());

        // Atomic replacement: a failed write must never destroy the existing helper.
        QSaveFile file(outputPath);
        if (!file.open(QIODevice::WriteOnly)) {
            finishWithExistingOrError(QStringLiteral("Cannot write file: %1").arg(outputPath));
            return;
        }

        if (file.write(payload) != payload.size()) {
            file.cancelWriting();
            finishWithExistingOrError(QStringLiteral("Cannot fully write file: %1").arg(outputPath));
            return;
        }

        if (!file.commit()) {
            finishWithExistingOrError(QStringLiteral("Cannot replace file: %1").arg(outputPath));
            return;
        }

#ifndef Q_OS_WIN
        QFile::Permissions permissions = QFile::permissions(outputPath);
        permissions |= QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner
                       | QFileDevice::ReadGroup | QFileDevice::ExeGroup
                       | QFileDevice::ReadOther | QFileDevice::ExeOther;
        QFile::setPermissions(outputPath, permissions);
#endif

        // A freshly managed binary must win over a stale absolute path saved by an older build.
        m_executableHint = outputPath;
        const QString oldPath = m_executablePath;
        m_executablePath = outputPath;
        m_updateCheckDone = true;

        if (isWindows7Runtime()) {
            appendLog(QStringLiteral("Installed pinned Windows 7 cloudflared %1 to %2")
                          .arg(win7CloudflaredVersion(), outputPath));
            emit statusChanged(QStringLiteral("Windows 7 compatible cloudflared installed"));
        } else {
            appendLog(QStringLiteral("Downloaded latest cloudflared to %1").arg(outputPath));
            emit statusChanged(QStringLiteral("Latest cloudflared installed"));
        }
        Q_UNUSED(oldPath)
        // Preserve the original behavior: listeners may use this as an install-complete event.
        emit executablePathChanged(m_executablePath);

        startPendingTunnel();
    });
}

void TunnelManager::startPendingTunnel()
{
    if (!m_startRequested) {
        return;
    }

    const quint16 port = m_pendingPort;
    m_startRequested = false;
    startQuickTunnel(port);
}

void TunnelManager::finishWithExistingOrError(const QString &message)
{
    appendLog(message);
    refreshExecutablePath();

    if (m_startRequested && isInstalled()) {
        appendLog(QStringLiteral("Using existing cloudflared because automatic update was unavailable"));
        m_updateCheckDone = true;
        startPendingTunnel();
        return;
    }

    emit errorOccurred(message);
    if (message.contains(QStringLiteral("write"), Qt::CaseInsensitive)
        || message.contains(QStringLiteral("replace"), Qt::CaseInsensitive)) {
        emit statusChanged(QStringLiteral("cloudflared write failed"));
    } else {
        emit statusChanged(QStringLiteral("cloudflared download failed"));
    }
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
    m_processTextBuffer.clear();
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
    if (!text.isEmpty()) {
        m_processTextBuffer += text;
    }

    // Search the accumulated stream before splitting lines. QProcess may deliver a URL
    // across multiple readyRead chunks, especially with the Qt 5 build.
    const QRegularExpression urlPattern(QStringLiteral(R"(https://[-a-z0-9]+\.trycloudflare\.com)"));
    const QRegularExpressionMatch match = urlPattern.match(m_processTextBuffer);
    if (match.hasMatch()) {
        const QString url = match.captured(0);
        if (url != m_publicUrl) {
            m_publicUrl = url;
            emit publicUrlChanged(m_publicUrl);
            emit statusChanged(QStringLiteral("Cloudflare Tunnel is running"));
        }
    }

    int newlineIndex = -1;
    while ((newlineIndex = m_processTextBuffer.indexOf('\n')) >= 0) {
        const QString line = m_processTextBuffer.left(newlineIndex);
        m_processTextBuffer.remove(0, newlineIndex + 1);
        appendLog(line);
    }

    // Bound an abnormal no-newline stream without losing URL detection entirely.
    if (m_processTextBuffer.size() > 64 * 1024) {
        appendLog(m_processTextBuffer.left(32 * 1024));
        m_processTextBuffer.remove(0, 32 * 1024);
    }
}

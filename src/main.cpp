#include "app/Controller.h"
#include "ui/MainWindow.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QFont>
#include <QIcon>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>
#include <QStringList>
#include <QUrl>
#include <QUrlQuery>

namespace {
QString findPotPlayerExecutable()
{
    const QStringList candidates = {
        QStringLiteral("C:/Program Files/DAUM/PotPlayer/PotPlayerMini64.exe"),
        QStringLiteral("C:/Program Files/DAUM/PotPlayer/PotPlayerMini.exe"),
        QStringLiteral("C:/Program Files (x86)/DAUM/PotPlayer/PotPlayerMini.exe"),
        QStringLiteral("C:/Program Files (x86)/DAUM/PotPlayer/PotPlayerMini64.exe"),
    };

    for (const QString &candidate : candidates) {
        if (QFileInfo::exists(candidate)) {
            return QDir::toNativeSeparators(candidate);
        }
    }

    const QString discovered64 = QStandardPaths::findExecutable(QStringLiteral("PotPlayerMini64.exe"));
    if (!discovered64.isEmpty()) {
        return QDir::toNativeSeparators(discovered64);
    }

    const QString discovered32 = QStandardPaths::findExecutable(QStringLiteral("PotPlayerMini.exe"));
    if (!discovered32.isEmpty()) {
        return QDir::toNativeSeparators(discovered32);
    }

    return {};
}

void registerProtocol(const QString &scheme, const QString &displayName)
{
#ifdef Q_OS_WIN
    const QString exePath = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
    if (exePath.isEmpty()) {
        return;
    }

    const QString rootKey = QStringLiteral(R"(HKCU\Software\Classes\)") + scheme;
    const QString commandValue = QStringLiteral("\"%1\" \"%2\"").arg(exePath, QStringLiteral("%1"));

    QProcess::execute(QStringLiteral("reg"),
                      {QStringLiteral("add"),
                       rootKey,
                       QStringLiteral("/ve"),
                       QStringLiteral("/d"),
                       displayName,
                       QStringLiteral("/f")});
    QProcess::execute(QStringLiteral("reg"),
                      {QStringLiteral("add"),
                       rootKey,
                       QStringLiteral("/v"),
                       QStringLiteral("URL Protocol"),
                       QStringLiteral("/d"),
                       QString(),
                       QStringLiteral("/f")});
    QProcess::execute(QStringLiteral("reg"),
                      {QStringLiteral("add"),
                       rootKey + QStringLiteral(R"(\DefaultIcon)"),
                       QStringLiteral("/ve"),
                       QStringLiteral("/d"),
                       exePath + QStringLiteral(",0"),
                       QStringLiteral("/f")});
    QProcess::execute(QStringLiteral("reg"),
                      {QStringLiteral("add"),
                       rootKey + QStringLiteral(R"(\shell\open\command)"),
                       QStringLiteral("/ve"),
                       QStringLiteral("/d"),
                       commandValue,
                       QStringLiteral("/f")});
#endif
}

void cleanupLegacyPotplayerBridge()
{
#ifdef Q_OS_WIN
    const QString exePath = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
    if (exePath.isEmpty()) {
        return;
    }

    QProcess query;
    query.start(QStringLiteral("reg"),
                {QStringLiteral("query"),
                 QStringLiteral(R"(HKCU\Software\Classes\potplayer\shell\open\command)"),
                 QStringLiteral("/ve")});
    if (!query.waitForFinished(2000)) {
        query.kill();
        return;
    }

    const QString output = QString::fromLocal8Bit(query.readAllStandardOutput())
                               + QString::fromLocal8Bit(query.readAllStandardError());
    if (!output.contains(exePath, Qt::CaseInsensitive)) {
        return;
    }

    QProcess::execute(QStringLiteral("reg"),
                      {QStringLiteral("delete"),
                       QStringLiteral(R"(HKCU\Software\Classes\potplayer)"),
                       QStringLiteral("/f")});
#endif
}

void registerPotPlayerProtocol()
{
    cleanupLegacyPotplayerBridge();
    registerProtocol(QStringLiteral("easycloudhfs-pot"), QStringLiteral("URL:Easy Cloud HFS PotPlayer Launcher"));
}

bool decodeLaunchToken(const QString &token, QString *targetUrl, QString *subtitleUrl, QString *refererUrl)
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

    const QJsonObject object = document.object();
    if (targetUrl) {
        *targetUrl = object.value(QStringLiteral("u")).toString().trimmed();
    }
    if (subtitleUrl) {
        *subtitleUrl = object.value(QStringLiteral("sub")).toString().trimmed();
    }
    if (refererUrl) {
        *refererUrl = object.value(QStringLiteral("referer")).toString().trimmed();
    }
    return true;
}

bool handlePotPlayerProtocolLaunch(const QStringList &arguments)
{
    if (arguments.size() < 2) {
        return false;
    }

    const QString protocolArgument = arguments.at(1).trimmed();
    QString targetUrl;
    QString subtitleUrl;
    QString refererUrl;

    if (protocolArgument.startsWith(QStringLiteral("easycloudhfs-pot://"), Qt::CaseInsensitive)) {
        const QUrl protocolUrl = QUrl::fromEncoded(protocolArgument.toUtf8());
        const QStringList pathSegments = protocolUrl.path(QUrl::FullyDecoded).split(QLatin1Char('/'), Qt::SkipEmptyParts);
        if (protocolUrl.host().compare(QStringLiteral("open"), Qt::CaseInsensitive) == 0 && !pathSegments.isEmpty()) {
            decodeLaunchToken(pathSegments.first(), &targetUrl, &subtitleUrl, &refererUrl);
        } else {
            const QUrlQuery query(protocolUrl);
            targetUrl = query.queryItemValue(QStringLiteral("u"), QUrl::FullyDecoded).trimmed();
            subtitleUrl = query.queryItemValue(QStringLiteral("sub"), QUrl::FullyDecoded).trimmed();
            refererUrl = query.queryItemValue(QStringLiteral("referer"), QUrl::FullyDecoded).trimmed();
        }
    } else if (protocolArgument.startsWith(QStringLiteral("easycloudhfs-pot:"), Qt::CaseInsensitive)) {
        const QString payload = protocolArgument.mid(QStringLiteral("easycloudhfs-pot:").size()).trimmed();
        if (payload.startsWith(QStringLiteral("open?"), Qt::CaseInsensitive)) {
            const QUrlQuery query(payload.mid(QStringLiteral("open?").size()));
            targetUrl = query.queryItemValue(QStringLiteral("u"), QUrl::FullyDecoded).trimmed();
            subtitleUrl = query.queryItemValue(QStringLiteral("sub"), QUrl::FullyDecoded).trimmed();
            refererUrl = query.queryItemValue(QStringLiteral("referer"), QUrl::FullyDecoded).trimmed();
        } else {
            targetUrl = payload;
        }
    } else if (protocolArgument.startsWith(QStringLiteral("potplayer://"), Qt::CaseInsensitive)) {
        targetUrl = protocolArgument.mid(QStringLiteral("potplayer://").size()).trimmed();
        if (targetUrl.contains(QLatin1Char('%'))) {
            targetUrl = QUrl::fromPercentEncoding(targetUrl.toUtf8()).trimmed();
        }
    } else {
        return false;
    }

    if (targetUrl.startsWith(QStringLiteral("/http:://"))) {
        targetUrl.remove(0, 1);
    } else if (targetUrl.startsWith(QStringLiteral("/https:://"))) {
        targetUrl.remove(0, 1);
    } else if (targetUrl.startsWith(QStringLiteral("/file:://"))) {
        targetUrl.remove(0, 1);
    } else if (targetUrl.startsWith(QStringLiteral("/http://"))) {
        targetUrl.remove(0, 1);
    } else if (targetUrl.startsWith(QStringLiteral("/https://"))) {
        targetUrl.remove(0, 1);
    } else if (targetUrl.startsWith(QStringLiteral("/file://"))) {
        targetUrl.remove(0, 1);
    }

    if (targetUrl.startsWith(QStringLiteral("http:://"), Qt::CaseInsensitive)) {
        targetUrl.remove(4, 1);
    } else if (targetUrl.startsWith(QStringLiteral("https:://"), Qt::CaseInsensitive)) {
        targetUrl.remove(5, 1);
    } else if (targetUrl.startsWith(QStringLiteral("file:://"), Qt::CaseInsensitive)) {
        targetUrl.remove(4, 1);
    } else if (targetUrl.startsWith(QStringLiteral("http//"), Qt::CaseInsensitive)) {
        targetUrl.insert(4, QLatin1Char(':'));
    } else if (targetUrl.startsWith(QStringLiteral("https//"), Qt::CaseInsensitive)) {
        targetUrl.insert(5, QLatin1Char(':'));
    } else if (targetUrl.startsWith(QStringLiteral("file//"), Qt::CaseInsensitive)) {
        targetUrl.insert(4, QLatin1Char(':'));
    }

    if (targetUrl.isEmpty()) {
        return true;
    }

    const QString potPlayerExe = findPotPlayerExecutable();
    if (potPlayerExe.isEmpty()) {
        return true;
    }

    QStringList potArgs;
    potArgs.append(targetUrl);
    if (!subtitleUrl.isEmpty()) {
        potArgs.append(QStringLiteral("/sub=%1").arg(subtitleUrl));
    }
    if (!refererUrl.isEmpty()) {
        potArgs.append(QStringLiteral("/referer=%1").arg(refererUrl));
    }

    QProcess::startDetached(potPlayerExe, potArgs);

    return true;
}
} // namespace

int main(int argc, char *argv[])
{
    QStringList rawArguments;
    rawArguments.reserve(argc);
    for (int index = 0; index < argc; ++index) {
        rawArguments.append(QString::fromLocal8Bit(argv[index]));
    }

    if (handlePotPlayerProtocolLaunch(rawArguments)) {
        return 0;
    }

    QApplication app(argc, argv);
    app.setApplicationName("Easy Cloud HFS");
    app.setApplicationVersion(QStringLiteral("1.2.0.0"));
    app.setOrganizationName("EasyCloudHFS");
    app.setWindowIcon(QIcon(":/desktop_logo.png"));
    app.setFont(QFont("Microsoft JhengHei UI", 10));

    registerPotPlayerProtocol();

    Controller controller;
    MainWindow window(&controller);

    const bool startupTrayLaunch = app.arguments().contains(QStringLiteral("--startup-tray"))
                                   && controller.settings().launchOnStartup;

    if (startupTrayLaunch) {
        const bool started = controller.startServer();
        if (!window.trayAvailable() || !started) {
            window.showMainWindow();
        }
    } else {
        window.showMainWindow();
    }

    return app.exec();
}

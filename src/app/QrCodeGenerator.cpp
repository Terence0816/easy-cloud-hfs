#include "app/QrCodeGenerator.h"

#include <QEventLoop>
#include <QFont>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QUrl>

QrCodeGenerator::QrCodeGenerator(QObject *parent)
    : QObject(parent)
    , m_network(new QNetworkAccessManager(this))
{
}

QImage QrCodeGenerator::buildCode(const QString &text, int size)
{
    const QUrl url(QStringLiteral("https://api.qrserver.com/v1/create-qr-code/?size=%1x%1&data=%2")
                       .arg(size)
                       .arg(QString::fromUtf8(QUrl::toPercentEncoding(text))));
    QNetworkRequest request{url};
    auto *reply = m_network->get(request);

    QEventLoop loop;
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    QImage image;
    if (reply->error() == QNetworkReply::NoError) {
        image.loadFromData(reply->readAll());
    }
    reply->deleteLater();

    if (image.isNull()) {
        return buildFallback(text, size);
    }
    return image;
}

QImage QrCodeGenerator::buildFallback(const QString &text, int size) const
{
    QImage image(size, size, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::white);

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.fillRect(image.rect(), QColor("#ffffff"));
    painter.setPen(QColor("#123054"));
    painter.drawRect(0, 0, size - 1, size - 1);
    painter.setFont(QFont("Microsoft JhengHei UI", 11, QFont::Bold));
    painter.drawText(QRect(18, 18, size - 36, 50), Qt::AlignCenter | Qt::TextWordWrap, QStringLiteral("QR preview unavailable"));
    painter.setFont(QFont("Microsoft JhengHei UI", 9));
    painter.drawText(QRect(18, 78, size - 36, size - 96), Qt::AlignCenter | Qt::TextWordWrap, text);
    return image;
}

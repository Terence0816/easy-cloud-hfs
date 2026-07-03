#pragma once

#include <QObject>
#include <QImage>

class QNetworkAccessManager;

class QrCodeGenerator : public QObject {
    Q_OBJECT

public:
    explicit QrCodeGenerator(QObject *parent = nullptr);

    [[nodiscard]] QImage buildCode(const QString &text, int size = 220);

private:
    [[nodiscard]] QImage buildFallback(const QString &text, int size) const;

    QNetworkAccessManager *m_network = nullptr;
};


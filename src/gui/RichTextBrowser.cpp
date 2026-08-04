#include "RichTextBrowser.h"

#include <QDesktopServices>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTextDocument>

RichTextBrowser::RichTextBrowser(QWidget* parent) : QTextBrowser(parent) {
    setOpenLinks(false);
    setOpenExternalLinks(false);
    m_network = new QNetworkAccessManager(this);
    connect(this, &QTextBrowser::anchorClicked, this, [](const QUrl& url) {
        if (url.scheme() == QStringLiteral("http") ||
            url.scheme() == QStringLiteral("https")) {
            QDesktopServices::openUrl(url);
        }
    });
}

QVariant RichTextBrowser::loadResource(int type, const QUrl& name) {
    if (type != QTextDocument::ImageResource ||
        (name.scheme() != QStringLiteral("http") &&
         name.scheme() != QStringLiteral("https"))) {
        return QTextBrowser::loadResource(type, name);
    }

    if (m_images.contains(name)) return QVariant::fromValue(m_images.value(name));
    if (!m_pending.contains(name)) {
        m_pending.insert(name);
        QNetworkReply* reply = m_network->get(QNetworkRequest(name));
        connect(reply, &QNetworkReply::finished, this, [this, reply, name] {
            m_pending.remove(name);
            const QByteArray bytes = reply->error() == QNetworkReply::NoError
                ? reply->readAll() : QByteArray();
            reply->deleteLater();
            QImage image = QImage::fromData(bytes);
            if (image.isNull()) return;
            if (image.width() > 900 || image.height() > 650) {
                image = image.scaled(900, 650, Qt::KeepAspectRatio,
                                     Qt::SmoothTransformation);
            }
            m_images.insert(name, image);
            document()->addResource(QTextDocument::ImageResource, name, image);
            document()->markContentsDirty(0, document()->characterCount());
            viewport()->update();
        });
    }
    return QVariant();
}

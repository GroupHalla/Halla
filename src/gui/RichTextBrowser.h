#pragma once

#include <QHash>
#include <QImage>
#include <QList>
#include <QSet>
#include <QTextBrowser>
#include <QUrl>

class QNetworkAccessManager;

// QTextBrowser used for descriptions and chat. Besides preserving line breaks, it
// loads HTTP(S) images without blocking the interface and opens links in the
// system default browser.
class RichTextBrowser : public QTextBrowser {
public:
    explicit RichTextBrowser(QWidget* parent = nullptr);

protected:
    QVariant loadResource(int type, const QUrl& name) override;

private:
    QNetworkAccessManager* m_network = nullptr;
    QHash<QUrl, QImage> m_images;
    // v1.1.35: insertion order of the cached images -- without it the cache grows
    // forever in long sessions with many images in the chat.
    QList<QUrl> m_imageOrder;
    QSet<QUrl> m_pending;
};

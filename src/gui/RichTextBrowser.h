#pragma once

#include <QHash>
#include <QImage>
#include <QSet>
#include <QTextBrowser>
#include <QUrl>

class QNetworkAccessManager;

// QTextBrowser usado em descrições e chat. Além de preservar quebras de
// linha, carrega imagens HTTP(S) sem bloquear a interface e abre links no
// navegador padrão do sistema.
class RichTextBrowser : public QTextBrowser {
public:
    explicit RichTextBrowser(QWidget* parent = nullptr);

protected:
    QVariant loadResource(int type, const QUrl& name) override;

private:
    QNetworkAccessManager* m_network = nullptr;
    QHash<QUrl, QImage> m_images;
    QSet<QUrl> m_pending;
};

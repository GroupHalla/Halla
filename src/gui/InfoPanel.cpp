#include "InfoPanel.h"
#include "Icons.h"
#include "ChatPanel.h"
#include "RichTextBrowser.h"
#include "app/Theme.h"
#include "core/GroupIconCache.h"

#include <QTextDocument>
#include <QUrl>
#include <QVBoxLayout>
#include <QPainter>
#include <QImage>
#include <QFrame>

static QPixmap serverBannerPixmap(const ServerData* data) {
    // O refresh roda a cada segundo enquanto o servidor está selecionado —
    // decodificar o banner (JPEG/PNG de até 512 KiB) a cada tick é CPU pura
    // desperdiçada. O QByteArray é compartilhado (refcounted), guardar a
    // última cópia custa nada; comparação de 512 KiB é memcpy rápido.
    static QByteArray lastBytes(1, '\x01'); // nunca igual no primeiro uso
    static QPixmap lastPixmap;
    const QByteArray b = data ? data->serverBanner : QByteArray();
    if (b == lastBytes && !lastPixmap.isNull()) return lastPixmap;
    lastBytes = b;
    if (!b.isEmpty()) {
        const QImage image = QImage::fromData(b);
        if (!image.isNull()) {
            lastPixmap = QPixmap::fromImage(image);
            return lastPixmap;
        }
    }
    lastPixmap = HIcons::banner(820, 210);
    return lastPixmap;
}

class InfoView : public RichTextBrowser {
public:
    explicit InfoView(QWidget* parent = nullptr) : RichTextBrowser(parent) {}

protected:
    void paintEvent(QPaintEvent* e) override {
        QTextBrowser::paintEvent(e);
        const QPixmap wm = HIcons::appIcon(112);
        if (wm.isNull()) return;
        QPainter p(viewport());
        p.setOpacity(HTheme::isDark() ? 0.12 : 0.075);
        p.drawPixmap(viewport()->width() - wm.width() - 22, 18, wm);
    }
};

InfoPanel::InfoPanel(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("infoPanel"));
    QVBoxLayout* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);

    m_banner = new QLabel(this);
    m_banner->setObjectName(QStringLiteral("infoBanner"));
    m_banner->setPixmap(serverBannerPixmap(nullptr));
    m_banner->setScaledContents(true);
    m_banner->setMinimumHeight(210);
    m_banner->setMaximumHeight(210);
    // QLabel com scaledContents usa o TAMANHO DO PIXMAP como minimumSizeHint:
    // com banner personalizado (até 1600px de largura) o cartão de informações
    // ficava com largura mínima de 1600px — o QSplitter recusava o arraste, o
    // painel cobria a lista de canais e só o banner padrão (820px) "funcionava"
    // (e ainda assim limitado). Largura mínima explícita pequena devolve o
    // controle total do splitter; o pixmap é esticado para o espaço disponível.
    m_banner->setMinimumWidth(1);
    lay->addWidget(m_banner);

    m_view = new InfoView(this);
    m_view->setObjectName(QStringLiteral("infoView"));
    m_view->setOpenLinks(false);
    m_view->setFrameShape(QFrame::NoFrame);
    lay->addWidget(m_view, 1);

    m_timer = new QTimer(this);
    m_timer->setInterval(1000);
    connect(m_timer, &QTimer::timeout, this, [this] {
        if (m_data && m_kind == 0) refresh();
    });
    m_timer->start();
}

QString InfoPanel::uptime(const QDateTime& since) {
    qint64 secs = since.secsTo(QDateTime::currentDateTime());
    qint64 d = secs / 86400; secs %= 86400;
    qint64 h = secs / 3600;  secs %= 3600;
    qint64 m = secs / 60;    secs %= 60;
    if (d > 0) return QStringLiteral("%1 dias %2:%3:%4")
                    .arg(d).arg(h, 2, 10, QChar('0'))
                    .arg(m, 2, 10, QChar('0')).arg(secs, 2, 10, QChar('0'));
    return QStringLiteral("%1:%2:%3")
        .arg(h, 2, 10, QChar('0')).arg(m, 2, 10, QChar('0')).arg(secs, 2, 10, QChar('0'));
}

static QString row(const QString& key, const QString& value) {
    const QString muted = HTheme::isDark() ? QStringLiteral("#8D899F") : QStringLiteral("#6E7D8B");
    return QStringLiteral("<tr><td style=\"color:%1; padding:5px 20px 5px 0; white-space:nowrap;\">%2</td>"
                          "<td style=\"padding:5px 0; font-weight:600;\">%3</td></tr>")
        .arg(muted, key, value);
}

static QString heading(const QString& icon, const QString& title, const QString& badge = QString()) {
    const QString line = HTheme::isDark() ? QStringLiteral("#2A2840") : QStringLiteral("#DDE3E9");
    const QString accent = HTheme::isDark() ? QStringLiteral("#8B5CF6") : QStringLiteral("#2E6FAE");
    QString h = QStringLiteral("<table width=\"100%\" cellspacing=\"0\" cellpadding=\"0\"><tr>");
    h += QStringLiteral("<td style=\"font-size:15px; padding:3px 0 10px 0;\"><span style=\"color:%1;\">%2</span> <b>%3</b></td>")
             .arg(accent, icon, title);
    if (!badge.isEmpty()) {
        h += QStringLiteral("<td align=\"right\" style=\"padding:2px 0 10px 0; color:%1; font-weight:700;\">%2</td>")
                 .arg(accent, badge);
    }
    h += QStringLiteral("</tr></table><hr style=\"border:0; border-top:1px solid %1; margin:0 0 10px 0;\">").arg(line);
    return h;
}

QString InfoPanel::serverHtml() const {
    const QString name = m_data->name.toHtmlEscaped();
    const QString line = HTheme::isDark() ? QStringLiteral("#2A2840") : QStringLiteral("#E8E5F0");
    QString h = heading(QStringLiteral("◉"), tr("Servidor: %1").arg(name));
    h += QStringLiteral("<table cellspacing=\"0\" cellpadding=\"0\">");
    h += row(tr("Endereço:"), m_data->address.toHtmlEscaped());
    h += row(tr("Versão:"), QStringLiteral("%1 no %2").arg(m_data->version, m_data->platform));
    h += row(tr("Tempo ativo:"), uptime(m_data->connectedAt));
    h += row(tr("Clientes conectados:"), QStringLiteral("%1 / %2").arg(m_data->totalClients()).arg(m_data->maxClients));
    h += row(tr("Canais:"), QString::number(m_data->channels.size()));
    h += row(tr("Perda de pacotes:"), QStringLiteral("0,00%"));
    h += QStringLiteral("</table><hr style=\"border:0; border-top:1px solid %1; margin:15px 0 12px 0;\">").arg(line);
    h += QStringLiteral("<div style=\"line-height:1.5;\">%1</div>").arg(m_data->motd.toHtmlEscaped());
    return h;
}

QString InfoPanel::channelHtml(const Channel& c) const {
    static const char* tnames[] = { "Temporário", "Semi-permanente", "Permanente" };
    const QString badge = QStringLiteral("♟ %1").arg(c.users.size());
    QString h = heading(QStringLiteral("◖"), tr("Canal: %1").arg(c.name.toHtmlEscaped()), badge);
    h += QStringLiteral("<table cellspacing=\"0\" cellpadding=\"0\">");
    h += row(tr("Tipo:"), QString::fromUtf8(tnames[qBound(0, c.type, 2)]));
    h += row(tr("Codec:"), codecShortNames().value(c.codec));
    h += row(tr("Qualidade do codec:"), QString::number(c.codecQuality));
    h += row(tr("Clientes no canal:"), c.maxClients >= 0
             ? QStringLiteral("%1 / %2").arg(c.users.size()).arg(c.maxClients)
             : QString::number(c.users.size()));
    h += row(tr("Protegido por senha:"), c.hasPassword ? tr("Sim") : tr("Não"));
    h += row(tr("Moderado:"), c.moderated ? tr("Sim") : tr("Não"));
    if (c.isDefault) h += row(tr("Canal padrão:"), tr("Sim"));
    h += QStringLiteral("</table>");
    if (!c.topic.isEmpty())
        h += QStringLiteral("<p style=\"margin-top:18px;\"><b>%1</b></p>").arg(c.topic.toHtmlEscaped());
    if (!c.description.isEmpty())
        h += QStringLiteral("<p style=\"line-height:1.5;\">%1</p>").arg(ChatPanel::bbToHtml(c.description));
    return h;
}

QString InfoPanel::userHtml(const User& u) {
    QString h = heading(QStringLiteral("●"), tr("Cliente: %1").arg(u.name.toHtmlEscaped()));
    h += QStringLiteral("<table cellspacing=\"0\" cellpadding=\"0\">");
    if (u.id == m_data->selfId) h += row(tr("Tipo:"), tr("Você (este cliente)"));
    h += row(tr("Versão:"), QStringLiteral("%1 no %2").arg(u.version, u.platform));
    h += row(tr("Tempo online:"), uptime(u.connectedAt));
    h += row(tr("Volume:"), QStringLiteral("%1 dB").arg(u.volumeDb));

    // O servidor envia cada cargo como "<icone> <nome>" quando o cargo tem
    // ícone (ex.: "rota.png ROTA") — cargo sem ícone vem só com o nome.
    // Antes este campo era impresso como texto puro, e o painel mostrava
    // literalmente "rota.png ROTA" em vez da imagem do ícone. Agora o
    // MESMO GroupIconCache da árvore fornece o pixmap, que é embutido no
    // HTML como recurso do documento (escopo por servidor, atualização na
    // hora quando o admin troca a imagem).
    const QString serverKey = GroupIconCache::serverKey(m_data->address);
    QStringList roles = u.serverGroups.split(QStringLiteral("\n"));
    int printedIndex = 0;
    for (const QString& r : roles) {
        if (r.trimmed().isEmpty()) continue;
        if (printedIndex == 0) {
            h += row(tr("Cargos:"), roleHtml(r.trimmed(), serverKey));
        } else {
            h += row(QString(), roleHtml(r.trimmed(), serverKey));
        }
        printedIndex++;
    }
    
    QStringList flags;
    if (u.away)        flags << tr("Ausente");
    if (u.inputMuted)  flags << tr("Microfone mudo");
    if (u.outputMuted) flags << tr("Alto-falantes mudos");
    if (u.locallyMuted) flags << tr("Silenciado localmente");
    if (u.recording)   flags << tr("Gravando");
    if (u.commander)   flags << tr("Comandante do canal");
    if (!flags.isEmpty()) h += row(tr("Estado:"), flags.join(QStringLiteral(", ")));
    h += QStringLiteral("</table>");
    if (!u.description.isEmpty())
        h += QStringLiteral("<p style=\"line-height:1.5;\"><i>%1</i></p>")
                 .arg(u.description.toHtmlEscaped());
    return h;
}

QString InfoPanel::roleHtml(const QString& roleLine, const QString& serverKey) {
    // O servidor concatena "<icone> <nome>" (applyGroup) sem separador
    // explícito. A separação ícone/nome vive no GroupIconCache (compartilhada
    // com o tooltip da árvore): ícone de IMAGEM termina em extensão conhecida;
    // emoji/letra/sigla e cargo sem ícone continuam como texto (o emoji
    // renderiza nativamente no HTML).
    QString iconName, label;
    GroupIconCache::splitRoleLine(roleLine, &iconName, &label);
    if (!iconName.isEmpty() && !label.isEmpty()) {
        const QPixmap pm = GroupIconCache::instance().pixmap(serverKey, iconName);
        if (!pm.isNull()) {
            // Recurso embutido no documento: URL interna do painel, sem
            // espaços (espaço no src atravessa a resolução de QUrl do rich
            // text de forma dependente de formato — '_' remove a dúvida).
            // Mesma chave em cada refresh: quando o admin troca a imagem,
            // iconDataReceived refaz o refresh e addResource sobrescreve o
            // pixmap antigo pelo novo.
            const QUrl url(QStringLiteral("halla-role-icon:///")
                               + GroupIconCache::safeName(iconName)
                                     .replace(QLatin1Char(' '), QLatin1Char('_')));
            m_view->document()->addResource(QTextDocument::ImageResource, url, pm.toImage());
            // Tamanho real do pixmap do cache (KeepAspectRatio): o ícone não
            // estica — quadrado fica quadrado.
            return QStringLiteral(
                       "<img src=\"%1\" width=\"%2\" height=\"%3\" "
                       "style=\"vertical-align:middle\"/> %4")
                .arg(url.toString())
                .arg(pm.width()).arg(pm.height())
                .arg(label.toHtmlEscaped());
        }
        // Ainda não temos os bytes (ex.: primeira exibição antes do
        // icon_data chegar): pede ao servidor e mostra só o nome do
        // cargo — o nome do ARQUIVO é detalhe interno e não deve vazar
        // para a interface. Quando o dado chega, o ServerTab chama
        // refresh() e a linha ganha a imagem.
        if (GroupIconCache::shouldRequest(serverKey + QLatin1Char('|') + iconName, false))
            emit iconRequested(iconName);
        return label.toHtmlEscaped();
    }
    return roleLine.toHtmlEscaped();
}

void InfoPanel::refresh() {
    m_banner->setPixmap(serverBannerPixmap(m_data));
    if (!m_data) {
        m_view->setHtml(QString());
        return;
    }
    if (m_kind == 1 && m_data->channels.contains(m_id)) {
        m_view->setHtml(channelHtml(m_data->channels[m_id]));
    } else if (m_kind == 2 && m_data->users.contains(m_id)) {
        m_view->setHtml(userHtml(m_data->users[m_id]));
    } else {
        m_view->setHtml(serverHtml());
    }
}

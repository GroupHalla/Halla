#include "InfoPanel.h"
#include "Icons.h"
#include "ChatPanel.h"
#include "RichTextBrowser.h"
#include "app/Theme.h"

#include <QVBoxLayout>
#include <QPainter>
#include <QImage>
#include <QFrame>

static QPixmap serverBannerPixmap(const ServerData* data) {
    if (data && !data->serverBanner.isEmpty()) {
        const QImage image = QImage::fromData(data->serverBanner);
        if (!image.isNull())
            return QPixmap::fromImage(image);
    }
    return HIcons::banner(820, 210);
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

QString InfoPanel::userHtml(const User& u) const {
    QString h = heading(QStringLiteral("●"), tr("Cliente: %1").arg(u.name.toHtmlEscaped()));
    h += QStringLiteral("<table cellspacing=\"0\" cellpadding=\"0\">");
    if (u.id == m_data->selfId) h += row(tr("Tipo:"), tr("Você (este cliente)"));
    h += row(tr("Versão:"), QStringLiteral("%1 no %2").arg(u.version, u.platform));
    h += row(tr("Tempo online:"), uptime(u.connectedAt));
    h += row(tr("Volume:"), QStringLiteral("%1 dB").arg(u.volumeDb));
    
    QStringList roles = u.serverGroups.split(QStringLiteral("\n"));
    int printedIndex = 0;
    for (const QString& r : roles) {
        if (r.trimmed().isEmpty()) continue;
        if (printedIndex == 0) {
            h += row(tr("Cargos:"), r.toHtmlEscaped());
        } else {
            h += row(QString(), r.toHtmlEscaped());
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

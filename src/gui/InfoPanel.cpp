#include "InfoPanel.h"
#include "Icons.h"
#include "app/Theme.h"

#include <QVBoxLayout>

InfoPanel::InfoPanel(QWidget* parent) : QWidget(parent) {
    QVBoxLayout* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);

    m_banner = new QLabel(this);
    m_banner->setPixmap(HIcons::banner(560, 58));
    m_banner->setScaledContents(true);
    m_banner->setMinimumHeight(58);
    m_banner->setMaximumHeight(58);
    lay->addWidget(m_banner);

    m_view = new QTextBrowser(this);
    m_view->setObjectName(QStringLiteral("infoView"));
    m_view->setOpenLinks(false);
    lay->addWidget(m_view, 1);

    m_timer = new QTimer(this);
    m_timer->setInterval(1000);
    connect(m_timer, &QTimer::timeout, this, [this] {
        if (m_data && m_kind == 0) refresh(); // atualiza o tempo ativo do servidor
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

static QString row(const QString& k, const QString& v) {
    return QStringLiteral("<tr><td style=\"color:#666666; padding-right:14px; "
                          "vertical-align:top\">%1</td><td>%2</td></tr>")
        .arg(k, v);
}

QString InfoPanel::serverHtml() const {
    QString h = QStringLiteral("<h3 style=\"margin:6px 0 8px 0\">%1</h3>")
                    .arg(m_data->name.toHtmlEscaped());
    h += QStringLiteral("<table cellspacing=\"0\" cellpadding=\"1\">");
    h += row(tr("Endereço:"), m_data->address.toHtmlEscaped());
    h += row(tr("Versão:"),
             QStringLiteral("%1 no %2").arg(m_data->version, m_data->platform));
    h += row(tr("Tempo ativo:"), uptime(m_data->connectedAt));
    h += row(tr("Clientes conectados:"),
             QStringLiteral("%1 / %2").arg(m_data->totalClients()).arg(32));
    h += row(tr("Canais:"), QString::number(m_data->channels.size()));
    h += row(tr("Perda de pacotes:"), QStringLiteral("0,00%"));
    h += QStringLiteral("</table>");
    const QString hrColor = HTheme::isDark() ? QStringLiteral("#4A4F56")
                                             : QStringLiteral("#DDDDDD");
    h += QStringLiteral("<hr style=\"border:none; border-top:1px solid ")
         + hrColor + QStringLiteral("\">");
    // MOTD: sem cor fixa — herda a cor do documento (funciona claro/escuro)
    h += QStringLiteral("<div>%1</div>").arg(m_data->motd.toHtmlEscaped());
    return h;
}

QString InfoPanel::channelHtml(const Channel& c) const {
    static const char* tnames[] = { "Temporário", "Semi-permanente", "Permanente" };
    QString h = QStringLiteral("<h3 style=\"margin:6px 0 8px 0\">Canal: %1</h3>")
                    .arg(c.name.toHtmlEscaped());
    h += QStringLiteral("<table cellspacing=\"0\" cellpadding=\"1\">");
    h += row(tr("Tipo:"), QString::fromUtf8(tnames[c.type]));
    h += row(tr("Codec:"), codecShortNames()[c.codec]);
    h += row(tr("Qualidade do codec:"), QString::number(c.codecQuality));
    h += row(tr("Clientes no canal:"),
             c.maxClients >= 0
                 ? QStringLiteral("%1 / %2").arg(c.users.size()).arg(c.maxClients)
                 : QString::number(c.users.size()));
    h += row(tr("Protegido por senha:"), c.hasPassword ? tr("Sim") : tr("Não"));
    h += row(tr("Moderado:"), c.moderated ? tr("Sim") : tr("Não"));
    if (c.isDefault) h += row(tr("Canal padrão:"), tr("Sim"));
    h += QStringLiteral("</table>");
    if (!c.topic.isEmpty())
        h += QStringLiteral("<p><b>%1</b></p>").arg(c.topic.toHtmlEscaped());
    if (!c.description.isEmpty())
        h += QStringLiteral("<p>%1</p>").arg(c.description.toHtmlEscaped());
    return h;
}

QString InfoPanel::userHtml(const User& u) const {
    QString h = QStringLiteral("<h3 style=\"margin:6px 0 8px 0\">Cliente: %1</h3>")
                    .arg(u.name.toHtmlEscaped());
    h += QStringLiteral("<table cellspacing=\"0\" cellpadding=\"1\">");
    if (u.id == m_data->selfId) h += row(tr("Tipo:"), tr("Você (este cliente)"));
    h += row(tr("ID único:"), QStringLiteral("<code>%1</code>").arg(u.uniqueId.toHtmlEscaped()));
    h += row(tr("Versão:"),
             QStringLiteral("%1 no %2").arg(u.version, u.platform));
    h += row(tr("Tempo online:"), uptime(u.connectedAt));
    h += row(tr("Grupos de servidor:"), u.serverGroups.toHtmlEscaped());
    h += row(tr("Volume:"), QStringLiteral("%1 dB").arg(u.volumeDb));
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
        h += QStringLiteral("<p><i>%1</i></p>")
                 .arg(u.description.toHtmlEscaped());
    return h;
}

void InfoPanel::refresh() {
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

#include "ChatPanel.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QToolBar>
#include <QMenu>
#include <QDateTime>
#include <QRegularExpression>
#include <QTabBar>

ChatPanel::ChatPanel(QWidget* parent) : QWidget(parent) {
    QVBoxLayout* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);

    m_tabs = new QTabWidget(this);
    m_tabs->setObjectName(QStringLiteral("chatTabs"));
    m_tabs->setDocumentMode(true);
    m_tabs->setTabsClosable(true);
    m_tabs->setMovable(false);
    m_tabs->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    m_serverChat  = makeBrowser();
    m_channelChat = makeBrowser();
    m_tabs->addTab(m_serverChat, tr("Chat do servidor"));
    m_tabs->addTab(m_channelChat, tr("Chat do canal"));
    m_tabs->tabBar()->setTabButton(0, QTabBar::RightSide, nullptr);
    m_tabs->tabBar()->setTabButton(1, QTabBar::RightSide, nullptr);
    lay->addWidget(m_tabs, 1);

    // A barra de edição reproduz a faixa de ícones da referência.
    QToolBar* fmt = new QToolBar(this);
    fmt->setObjectName(QStringLiteral("chatFormatBar"));
    fmt->setIconSize(QSize(16, 16));
    fmt->setFloatable(false);
    fmt->setMovable(false);

    auto btn = [&](const QString& text, const QString& tip, bool checkable = false) {
        QToolButton* b = new QToolButton(fmt);
        b->setText(text);
        b->setToolTip(tip);
        b->setCheckable(checkable);
        b->setAutoRaise(true);
        b->setMinimumWidth(28);
        fmt->addWidget(b);
        return b;
    };

    m_bold = btn(QStringLiteral("B"), tr("Negrito [b]"), true);
    m_italic = btn(QStringLiteral("I"), tr("Itálico [i]"), true);
    m_underline = btn(QStringLiteral("U"), tr("Sublinhado [u]"), true);
    QToolButton* strike = btn(QStringLiteral("S"), tr("Tachado [s]"), true);
    QToolButton* code = btn(QStringLiteral("</>"), tr("Código"));
    QToolButton* emoji = btn(QStringLiteral("☺"), tr("Inserir emoticon"));
    QToolButton* attachment = btn(QStringLiteral("📎"), tr("Anexar arquivo"));
    Q_UNUSED(attachment);

    QFont bf = m_bold->font(); bf.setBold(true); m_bold->setFont(bf);
    QFont itf = m_italic->font(); itf.setItalic(true); m_italic->setFont(itf);
    QFont uf = m_underline->font(); uf.setUnderline(true); m_underline->setFont(uf);
    QFont sf = strike->font(); sf.setStrikeOut(true); strike->setFont(sf);
    fmt->addSeparator();
    lay->addWidget(fmt);

    QHBoxLayout* inputRow = new QHBoxLayout;
    inputRow->setContentsMargins(8, 2, 2, 0);
    inputRow->setSpacing(8);
    m_input = new QLineEdit(this);
    m_input->setObjectName(QStringLiteral("chatInput"));
    m_input->setPlaceholderText(tr("Digite a mensagem..."));
    inputRow->addWidget(m_input, 1);
    QToolButton* send = new QToolButton(this);
    send->setObjectName(QStringLiteral("chatSendButton"));
    send->setText(QStringLiteral("➤"));
    send->setToolTip(tr("Enviar mensagem"));
    inputRow->addWidget(send);
    lay->addLayout(inputRow);

    connect(m_input, &QLineEdit::returnPressed, this, &ChatPanel::sendCurrent);
    connect(send, &QToolButton::clicked, this, &ChatPanel::sendCurrent);

    auto wrap = [this](const QString& open, const QString& close) {
        const int pos = m_input->cursorPosition();
        const QString sel = m_input->selectedText();
        if (sel.isEmpty()) {
            m_input->insert(open + close);
            m_input->setCursorPosition(pos + open.size());
        } else {
            m_input->insert(open + sel + close);
        }
    };
    connect(m_bold, &QToolButton::clicked, this, [=] { wrap("[b]", "[/b]"); });
    connect(m_italic, &QToolButton::clicked, this, [=] { wrap("[i]", "[/i]"); });
    connect(m_underline, &QToolButton::clicked, this, [=] { wrap("[u]", "[/u]"); });
    connect(strike, &QToolButton::clicked, this, [=] { wrap("[s]", "[/s]"); });
    connect(code, &QToolButton::clicked, this, [=] { wrap("[code]", "[/code]"); });

    QMenu* emenu = new QMenu(this);
    const QStringList emojis = { "😀", "😂", "😉", "😍", "😎", "🤔", "😢", "😡",
                                 "👍", "👎", "👏", "🎉", "❤️", "🔥", "✅", "☕" };
    for (const QString& emo : emojis)
        emenu->addAction(emo, this, [this, emo] { m_input->insert(emo); });
    emoji->setMenu(emenu);
    emoji->setPopupMode(QToolButton::InstantPopup);

    connect(m_tabs, &QTabWidget::tabCloseRequested, this, [this](int idx) {
        if (idx <= 1) return;
        QTextBrowser* w = qobject_cast<QTextBrowser*>(m_tabs->widget(idx));
        m_tabs->removeTab(idx);
        if (w) {
            m_private.remove(m_privateIds.value(w));
            m_privateIds.remove(w);
            w->deleteLater();
        }
    });
    connect(m_tabs, &QTabWidget::currentChanged, this, [this](int) { updateSendTarget(); });
    updateSendTarget();
}

QTextBrowser* ChatPanel::makeBrowser() {
    QTextBrowser* b = new QTextBrowser(m_tabs);
    b->setObjectName(QStringLiteral("chatBrowser"));
    b->setOpenLinks(false);
    b->setReadOnly(true);
    return b;
}

void ChatPanel::updateSendTarget() {}

void ChatPanel::addLine(QTextBrowser* browser, const QString& html) {
    browser->append(html);
    QTextCursor c = browser->textCursor();
    c.movePosition(QTextCursor::End);
    browser->setTextCursor(c);
}

QString ChatPanel::bbToHtml(const QString& text) {
    QString h = text.toHtmlEscaped();
    static const QRegularExpression reColorOpen("\\[color=(#[0-9a-fA-F]{3,8}|[a-zA-Z]+)\\]");
    h.replace(reColorOpen, QStringLiteral("<span style=\"color:\\1\">"));
    h.replace(QStringLiteral("[/color]"), QStringLiteral("</span>"));
    static const QRegularExpression reSizeOpen("\\[size=(\\d{1,2})\\]");
    h.replace(reSizeOpen, QStringLiteral("<span style=\"font-size:\\1px\">"));
    h.replace(QStringLiteral("[/size]"), QStringLiteral("</span>"));
    static const QRegularExpression reUrl("\\[url=([^\\]]+)\\](.*?)\\[/url\\]");
    h.replace(reUrl, QStringLiteral("<a href=\"\\1\" style=\"color:#8B5CF6\">\\2</a>"));
    static const QRegularExpression reUrl2("\\[url\\](.*?)\\[/url\\]");
    h.replace(reUrl2, QStringLiteral("<a href=\"\\1\" style=\"color:#8B5CF6\">\\1</a>"));
    static const QRegularExpression reB("\\[b\\](.*?)\\[/b\\]");
    h.replace(reB, QStringLiteral("<b>\\1</b>"));
    static const QRegularExpression reI("\\[i\\](.*?)\\[/i\\]");
    h.replace(reI, QStringLiteral("<i>\\1</i>"));
    static const QRegularExpression reU("\\[u\\](.*?)\\[/u\\]");
    h.replace(reU, QStringLiteral("<u>\\1</u>"));
    static const QRegularExpression reS("\\[s\\](.*?)\\[/s\\]");
    h.replace(reS, QStringLiteral("<s>\\1</s>"));
    static const QRegularExpression reCode("\\[code\\](.*?)\\[/code\\]");
    h.replace(reCode, QStringLiteral("<code>\\1</code>"));
    return h;
}

static QString stamp() {
    return QStringLiteral("<span style=\"color:#8D899F\">[%1]</span>")
        .arg(QDateTime::currentDateTime().toString("HH:mm:ss"));
}

void ChatPanel::addServerChat(const QString& user, const QString& text) {
    addLine(m_serverChat,
            QStringLiteral("%1 <b style=\"color:#8B5CF6\">%2</b>: %3")
                .arg(stamp(), user.toHtmlEscaped(), bbToHtml(text)));
}

void ChatPanel::addChannelChat(const QString& user, const QString& text) {
    addLine(m_channelChat,
            QStringLiteral("%1 <b style=\"color:#8B5CF6\">%2</b>: %3")
                .arg(stamp(), user.toHtmlEscaped(), bbToHtml(text)));
}

void ChatPanel::addServerSystem(const QString& text) {
    addLine(m_serverChat,
            QStringLiteral("%1 <i style=\"color:#8D899F\">* %2</i>")
                .arg(stamp(), text.toHtmlEscaped()));
}

void ChatPanel::addChannelSystem(const QString& text) {
    addLine(m_channelChat,
            QStringLiteral("%1 <i style=\"color:#8D899F\">* %2</i>")
                .arg(stamp(), text.toHtmlEscaped()));
}

void ChatPanel::addPrivateTab(int userId, const QString& name) {
    if (m_private.contains(userId)) {
        m_tabs->setCurrentWidget(m_private[userId]);
        return;
    }
    QTextBrowser* b = makeBrowser();
    m_private[userId] = b;
    m_privateIds[b] = userId;
    const int idx = m_tabs->addTab(b, name);
    m_tabs->setCurrentIndex(idx);
}

void ChatPanel::addPrivateChat(int userId, const QString& user, const QString& text) {
    if (!m_private.contains(userId)) return;
    addLine(m_private[userId],
            QStringLiteral("%1 <b style=\"color:#8B5CF6\">%2</b>: %3")
                .arg(stamp(), user.toHtmlEscaped(), bbToHtml(text)));
}

void ChatPanel::sendCurrent() {
    const QString text = m_input->text();
    if (text.trimmed().isEmpty()) return;
    m_input->clear();

    const int idx = m_tabs->currentIndex();
    QString target = QStringLiteral("channel");
    int targetId = 0;
    if (idx == 0) {
        target = QStringLiteral("server");
    } else if (idx > 1) {
        QTextBrowser* w = qobject_cast<QTextBrowser*>(m_tabs->currentWidget());
        target = QStringLiteral("private");
        targetId = m_privateIds.value(w, 0);
    }
    emit messageSent(target, targetId, text);
}

#pragma once

#include <QWidget>
#include <QTabWidget>
#include <QTextBrowser>
#include <QLineEdit>
#include <QToolButton>
#include <QMap>

// Painel de chat no estilo Halla: abas de chat do servidor/canal,
// mensagens com horário, BBCode ([b][i][u][color=][size=][url=]) e emojis.
class ChatPanel : public QWidget {
    Q_OBJECT
public:
    explicit ChatPanel(QWidget* parent = nullptr);

    void setSelfName(const QString& name) { m_selfName = name; }

    void addServerChat(const QString& user, const QString& text);
    void addChannelChat(const QString& user, const QString& text);
    void addServerSystem(const QString& text);
    void addChannelSystem(const QString& text);
    void addPrivateTab(int userId, const QString& name);
    void addPrivateChat(int userId, const QString& user, const QString& text);

    static QString bbToHtml(const QString& text);

signals:
    void messageSent(const QString& target, int targetId, const QString& text);

private:
    void sendCurrent();
    void addLine(QTextBrowser* browser, const QString& html);
    QTextBrowser* makeBrowser();
    void updateSendTarget();

    QTabWidget* m_tabs = nullptr;
    QLineEdit* m_input = nullptr;
    QTextBrowser* m_serverChat = nullptr;
    QTextBrowser* m_channelChat = nullptr;
    QMap<int, QTextBrowser*> m_private;
    QMap<QTextBrowser*, int> m_privateIds;
    QString m_selfName;
    QToolButton* m_bold = nullptr;
    QToolButton* m_italic = nullptr;
    QToolButton* m_underline = nullptr;
};

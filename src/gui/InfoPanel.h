#pragma once

#include <QWidget>
#include <QLabel>
#include <QTextBrowser>
#include <QTimer>
#include "core/Models.h"

// Painel de informações do lado direito (banner + detalhes), como no Halla.
// Mostra informações do servidor, do canal selecionado ou do usuário selecionado.
class InfoPanel : public QWidget {
    Q_OBJECT
public:
    explicit InfoPanel(QWidget* parent = nullptr);

    void setData(const ServerData* d) { m_data = d; refresh(); }
    void setSelection(int kind, int id) { m_kind = kind; m_id = id; refresh(); }
    void refresh();

private:
    QString serverHtml() const;
    QString channelHtml(const Channel& c) const;
    QString userHtml(const User& u) const;
    static QString uptime(const QDateTime& since);

    QLabel* m_banner = nullptr;
    QTextBrowser* m_view = nullptr;
    QTimer* m_timer = nullptr;
    const ServerData* m_data = nullptr;
    int m_kind = 0; // 0 servidor, 1 canal, 2 usuário
    int m_id = 0;
};

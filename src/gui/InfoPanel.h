#pragma once

#include <QWidget>
#include <QLabel>
#include <QTextBrowser>
#include <QTimer>
#include "core/Models.h"

// Information panel on the right (banner + details), as in Halla. Shows details of
// the server, of the selected channel or of the selected user.
class InfoPanel : public QWidget {
    Q_OBJECT
public:
    explicit InfoPanel(QWidget* parent = nullptr);

    void setData(const ServerData* d) { m_data = d; refresh(); }
    void setSelection(int kind, int id) { m_kind = kind; m_id = id; refresh(); }
    void refresh();

signals:
    // Role icon (image) is not in the local cache yet -- ServerTab connects this
    // signal to icon_get, exactly as it does for the channel tree.
    void iconRequested(const QString& name);

private:
    QString serverHtml() const;
    QString channelHtml(const Channel& c) const;
    QString userHtml(const User& u);
    // One user role line: "<icon> <name>" becomes image + name.
    QString roleHtml(const QString& roleLine, const QString& serverKey);
    static QString uptime(const QDateTime& since);

    QLabel* m_banner = nullptr;
    QTextBrowser* m_view = nullptr;
    QTimer* m_timer = nullptr;
    const ServerData* m_data = nullptr;
    int m_kind = 0; // 0 server, 1 channel, 2 user
    int m_id = 0;
};

#pragma once

#include <QTreeWidget>
#include <QStyledItemDelegate>
#include "core/Models.h"

// Papel dos nós da árvore
enum NodeKind { NodeServer = 0, NodeChannel = 1, NodeUser = 2 };

enum TreeRoles {
    RoleKind = Qt::UserRole + 1,
    RoleId,
};

// Delegado que pinta os mini-ícones de estado (mic mudo, fones mudos, ausente,
// gravando, comandante) logo após o nome do usuário — exatamente como o TS3.
class ServerRowDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void setServerData(const ServerData* d) { m_data = d; }
    void setShowMinis(bool show) { m_showMinis = show; }

protected:
    void paint(QPainter* p, const QStyleOptionViewItem& opt,
               const QModelIndex& index) const override;

private:
    const ServerData* m_data = nullptr;
    bool m_showMinis = true;
};

// Árvore de canais/usuários de uma conexão (visual e comportamento do TS3)
class ServerTreeWidget : public QTreeWidget {
    Q_OBJECT
public:
    explicit ServerTreeWidget(QWidget* parent = nullptr);

    void setServerData(ServerData* d);
    void rebuild();
    void setShowCounts(bool on)  { m_showCounts = on; }
    void setShowMinis(bool on);

    // seleção atual
    int  currentKind() const;
    int  currentId() const;

signals:
    void selectionChanged(int kind, int id);
    void joinChannelRequested(int channelId);
    void createChannelRequested(int parentChannelId);
    void editChannelRequested(int channelId);
    void deleteChannelRequested(int channelId);
    void renameRequested();
    void setDescriptionRequested();
    void viewAvatarRequested();
    void pokeRequested(int userId);
    void volumeRequested(int userId);
    void localMuteToggled(int userId, bool muted);
    void commanderToggled();
    void kickRequested(int userId, bool fromServer);
    void banRequested(int userId);
    void moveToMyChannelRequested(int userId);
    void privateMessageRequested(int userId);
    void disconnectRequested();
    void addBookmarkRequested();
    void editVirtualServerRequested();

protected:
    void contextMenuEvent(QContextMenuEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;
    QStringList mimeTypes() const override;
    QMimeData* mimeData(const QList<QTreeWidgetItem*>& items) const override;
    bool dropMimeData(QTreeWidgetItem* parent, int index, const QMimeData* data,
                      Qt::DropAction action) override;

private:
    QTreeWidgetItem* buildChannelItem(const Channel& c, QTreeWidgetItem* parentItem);
    void addUserItem(QTreeWidgetItem* chanItem, const User& u);
    QString userTooltip(const User& u) const;
    QString channelTooltip(const Channel& c) const;

    ServerData* m_data = nullptr;
    ServerRowDelegate* m_delegate = nullptr;
    bool m_showCounts = true;
};

#pragma once

#include <QTreeWidget>
#include <QStyledItemDelegate>
#include <QElapsedTimer>
#include <QSet>
#include "core/Models.h"

class QDropEvent;
class QMouseEvent;
class QEvent;

// Papel dos nós da árvore
enum NodeKind { NodeServer = 0, NodeChannel = 1, NodeUser = 2 };

enum TreeRoles {
    RoleKind = Qt::UserRole + 1,
    RoleId,
};

// Delegado que pinta os ícones de grupo/status; os indicadores de microfone,
// fones, ausência e fala ficam no bloco antes do nome — exatamente como o Halla.
class ServerRowDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void setServerData(const ServerData* d) { m_data = d; }
    void setShowMinis(bool show) { m_showMinis = show; }
    bool showMinis() const { return m_showMinis; }

protected:
    void paint(QPainter* p, const QStyleOptionViewItem& opt,
               const QModelIndex& index) const override;
    QSize sizeHint(const QStyleOptionViewItem& option,
                   const QModelIndex& index) const override;

private:
    const ServerData* m_data = nullptr;
    bool m_showMinis = true;
};

// Árvore de canais/usuários de uma conexão (visual e comportamento do Halla)
class ServerTreeWidget : public QTreeWidget {
    Q_OBJECT
public:
    explicit ServerTreeWidget(QWidget* parent = nullptr);

    void setServerData(ServerData* d);
    void rebuild();
    void setShowCounts(bool on)  { m_showCounts = on; }
    void setShowMinis(bool on);
    void setSortClientsBelow(bool on) { m_sortClientsBelow = on; }
    void setCanMoveOthers(bool on) { m_canMoveOthers = on; }
    void setChannelManagementPermissions(bool edit, bool remove) {
        m_canEditChannels = edit;
        m_canDeleteChannels = remove;
    }
    void setCommanderPermissions(bool self, bool others) {
        m_canSetSelfCommander = self;
        m_canSetOtherCommander = others;
    }

    // modos de expansão (Opções → Aparência → Árvore do canal)
    void expandChannelsToLevel(int level);     // recolhe tudo e expande até o nível
    void expandOwnChannelOnly(int channelId);  // recolhe tudo, expande só o próprio canal

    // seleção atual
    int  currentKind() const;
    int  currentId() const;
    QList<int> selectedChannelIds() const;
    void selectNode(int kind, int id);

signals:
    void selectionChanged(int kind, int id);
    void joinChannelRequested(int channelId);
    void createChannelRequested(int parentChannelId);
    void editChannelRequested(int channelId);
    void deleteChannelRequested(int channelId);
    void channelDescriptionRequested(int channelId);
    void renameRequested();
    void setDescriptionRequested();
    void viewAvatarRequested(int userId);
    void userInfoRequested(int userId);
    void complaintRequested(int userId);
    void pokeRequested(int userId);
    void volumeRequested(int userId);
    void localMuteToggled(int userId, bool muted);
    void commanderToggled();
    void kickRequested(int userId, bool fromServer);
    void banRequested(int userId);
    void moveToMyChannelRequested(int userId);
    void moveUserRequested(int userId, int channelId);
    void channelMoveRequested(int channelId, int parentId, int order);
    void channelLinkRequested(const QList<int>& channelIds, bool link);
    void commanderRequested(int userId, bool on);
    void privateMessageRequested(int userId);
    void screenshareHovered(int userId, int channelId, const QPoint& pos);
    void screenshareHoverLeft();
    void disconnectRequested();
    void addBookmarkRequested();
    void editVirtualServerRequested();
    void iconRequested(const QString& name);

protected:
    void contextMenuEvent(QContextMenuEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void leaveEvent(QEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;
    QStringList mimeTypes() const override;
    QMimeData* mimeData(const QList<QTreeWidgetItem*>& items) const override;
    void dropEvent(QDropEvent* event) override;

private slots:
    void onItemEntered(QTreeWidgetItem* item, int column);

private:
    QTreeWidgetItem* buildChannelItem(const Channel& c, QTreeWidgetItem* parentItem,
                                      QSet<int>& path, QSet<int>& built);
    bool wouldCreateChannelCycle(int channelId, int parentId) const;
    bool isDuplicateChannelMove(int channelId, int parentId, int order);
    void addUserItem(QTreeWidgetItem* chanItem, const User& u);
    QString userTooltip(const User& u) const;
    QString channelTooltip(const Channel& c) const;

    ServerData* m_data = nullptr;
    ServerRowDelegate* m_delegate = nullptr;
    bool m_showCounts = true;
    bool m_sortClientsBelow = false;
    bool m_canMoveOthers = false;
    bool m_canEditChannels = false;
    bool m_canDeleteChannels = false;
    bool m_canSetSelfCommander = false;
    bool m_canSetOtherCommander = false;
    QElapsedTimer m_lastChannelMoveClock;
    int m_lastMovedChannel = 0;
    int m_lastMoveParent = 0;
    int m_lastMoveOrder = -1;
};

#pragma once

#include <QTreeWidget>
#include <QStyledItemDelegate>
#include <QElapsedTimer>
#include <QSet>
#include "core/Models.h"

class QDropEvent;
class QMouseEvent;
class QEvent;

// Role of the tree nodes
enum NodeKind { NodeServer = 0, NodeChannel = 1, NodeUser = 2 };

enum TreeRoles {
    RoleKind = Qt::UserRole + 1,
    RoleId,
    // v1.1.35: visual key of the user (talking/whisper/mutes/away/...) -- allows
    // updating the row icon WITHOUT recreating the whole tree.
    RoleVisualKey,
};

// Delegate that paints the group/status icons; the microphone, headphone, away and
// speech indicators sit in the block before the name -- exactly like Halla.
class ServerRowDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void setServerData(const ServerData* d) { m_data = d; }
    void setShowMinis(bool show) { m_showMinis = show; }
    bool showMinis() const { return m_showMinis; }
    void setShowBadges(bool show) { m_showBadges = show; }

protected:
    void paint(QPainter* p, const QStyleOptionViewItem& opt,
               const QModelIndex& index) const override;
    QSize sizeHint(const QStyleOptionViewItem& option,
                   const QModelIndex& index) const override;

private:
    const ServerData* m_data = nullptr;
    bool m_showMinis = true;
    bool m_showBadges = true;   // v1.1.31: "Show badge icons in clients"
};

// Channel/user tree of a connection (Halla visuals and behavior)
class ServerTreeWidget : public QTreeWidget {
    Q_OBJECT
public:
    explicit ServerTreeWidget(QWidget* parent = nullptr);

    void setServerData(ServerData* d);
    void rebuild();
    // v1.1.35: LIGHT update -- repaints only the icons/colors of the users whose
    // visual state changed ("speaking" ring, mutes, away), without destroying and
    // recreating items. This is the path of speech transitions: a full rebuild() per
    // speech transition froze the GUI in full rooms -- and the audio pipeline shares
    // the SAME GUI thread.
    void updateUserVisuals();
    void setShowCounts(bool on)  { m_showCounts = on; }
    void setShowMinis(bool on);
    void setShowBadges(bool on);          // v1.1.31: global badges in the tree
    bool showBadges() const { return m_showBadges; }
    void setSortClientsBelow(bool on) { m_sortClientsBelow = on; }
    void setCanMoveOthers(bool on) { m_canMoveOthers = on; }
    void setCanRenameOthers(bool on) { m_canRenameOthers = on; }
    void setChannelManagementPermissions(bool edit, bool remove) {
        m_canEditChannels = edit;
        m_canDeleteChannels = remove;
    }
    void setCommanderPermissions(bool self, bool others) {
        m_canSetSelfCommander = self;
        m_canSetOtherCommander = others;
    }

    // expansion modes (Options > Appearance > Channel tree)
    void expandChannelsToLevel(int level);     // collapses everything and expands down to the level
    void expandOwnChannelOnly(int channelId);  // collapses everything, expands only the own channel

    // current selection
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
    void renameUserRequested(int userId);
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
    void startDrag(Qt::DropActions actions) override;
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
    bool m_showBadges = true;   // v1.1.31: default ON (design/showBadgeIcons)
    bool m_sortClientsBelow = false;
    bool m_canMoveOthers = false;
    bool m_canRenameOthers = false;
    bool m_canEditChannels = false;
    bool m_canDeleteChannels = false;
    bool m_canSetSelfCommander = false;
    bool m_canSetOtherCommander = false;
    QElapsedTimer m_lastChannelMoveClock;
    int m_lastMovedChannel = 0;
    int m_lastMoveParent = 0;
    int m_lastMoveOrder = -1;
};

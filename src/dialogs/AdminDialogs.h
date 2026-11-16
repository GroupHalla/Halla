#pragma once

#include <QDialog>
#include <QJsonArray>
#include <QJsonObject>
#include <QVector>
#include <QStringList>

class QTableWidget;
class QTreeWidget;
class QComboBox;
class QCheckBox;
class QSpinBox;
class QLineEdit;
class QLabel;
class NetSession;
struct ServerData;

// ============================================================================
// Halla administration dialogs (protocol v3) -- Halla style
// ============================================================================

// "Ban list" -- shows active bans and allows removing them
class BanListDialog : public QDialog {
    Q_OBJECT
public:
    explicit BanListDialog(NetSession* net, QWidget* parent = nullptr);
private:
    void fill(const QJsonArray& bans);
    NetSession* m_net;
    QTableWidget* m_table;
};

// "Complaints" -- complaints filed by users (requires banList)
class ComplaintsDialog : public QDialog {
    Q_OBJECT
public:
    explicit ComplaintsDialog(NetSession* net, QWidget* parent = nullptr);
private:
    void fill(const QJsonArray& complaints);
    NetSession* m_net;
    QTreeWidget* m_tree;
};

// "Server groups" -- full editor: create/delete groups, edit permissions
// (checkbox + talk power) and assign a group to a user
class ServerGroupsDialog : public QDialog {
    Q_OBJECT
public:
    explicit ServerGroupsDialog(NetSession* net, ServerData* data,
                                QWidget* parent = nullptr);
private:
    void fillGroups(const QJsonArray& groups);
    void applyConfirmedGroup(const QJsonObject& group);
    void loadPerms(const QJsonObject& perms);
    QJsonObject collectPerms() const;
    void refreshUsers();
    void refreshMembers(const QJsonArray& members);

    NetSession* m_net;
    ServerData* m_data;
    QTreeWidget* m_groups;
    QTreeWidget* m_members = nullptr;
    QWidget* m_editor = nullptr;
    QLabel* m_groupLabel = nullptr;
    QJsonObject m_cur;         // group being edited (id, name, perms)
    QJsonObject m_pendingGroup; // values awaiting server confirmation
    QList<QPair<QString, QCheckBox*>> m_checks; // boolean permissions
    QSpinBox* m_talkPower = nullptr;
    QLineEdit* m_sigla = nullptr;
    QComboBox* m_siglaPlacement = nullptr;
    QSpinBox* m_order = nullptr;
    QCheckBox* m_orderEnabled = nullptr;
    QSpinBox* m_position = nullptr;  // Pillar 1: hierarchical position of the roles
    QLineEdit* m_icon = nullptr;
    QComboBox* m_userCombo = nullptr;
};

// "Show user permissions" -- read-only view of my own permissions
class PermissionsOverviewDialog : public QDialog {
    Q_OBJECT
public:
    explicit PermissionsOverviewDialog(const QJsonObject& myPerms,
                                       const QString& groupName,
                                       QWidget* parent = nullptr);
};

// Offline message item received in this session
struct OfflineMsgItem { QString fromName; QString text; QString ts; };

// "Offline messages" -- inbox + sending new messages
class OfflineMessagesDialog : public QDialog {
    Q_OBJECT
public:
    explicit OfflineMessagesDialog(NetSession* net, ServerData* data,
                                   const QVector<OfflineMsgItem>& inbox,
                                   QWidget* parent = nullptr);
private:
    void addItem(const QString& fromName, const QString& text, const QString& ts);
    NetSession* m_net;
    ServerData* m_data;
    QTreeWidget* m_tree;
};

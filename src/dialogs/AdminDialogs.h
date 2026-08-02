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
class QLabel;
class NetSession;
struct ServerData;

// ============================================================================
// Diálogos de administração do Halla (protocolo v3) — estilo TeamSpeak 3
// ============================================================================

// "Lista de banidos" — mostra banimentos ativos e permite removê-los
class BanListDialog : public QDialog {
    Q_OBJECT
public:
    explicit BanListDialog(NetSession* net, QWidget* parent = nullptr);
private:
    void fill(const QJsonArray& bans);
    NetSession* m_net;
    QTableWidget* m_table;
};

// "Reclamações" — reclamações registradas pelos usuários (requer banList)
class ComplaintsDialog : public QDialog {
    Q_OBJECT
public:
    explicit ComplaintsDialog(NetSession* net, QWidget* parent = nullptr);
private:
    void fill(const QJsonArray& complaints);
    NetSession* m_net;
    QTreeWidget* m_tree;
};

// "Grupos de servidores" — editor completo: criar/excluir grupos,
// editar permissões (checkbox + poder de fala) e atribuir grupo a usuário
class ServerGroupsDialog : public QDialog {
    Q_OBJECT
public:
    explicit ServerGroupsDialog(NetSession* net, ServerData* data,
                                QWidget* parent = nullptr);
private:
    void fillGroups(const QJsonArray& groups);
    void loadPerms(const QJsonObject& perms);
    QJsonObject collectPerms() const;
    void refreshUsers();

    NetSession* m_net;
    ServerData* m_data;
    QTreeWidget* m_groups;
    QWidget* m_editor = nullptr;
    QLabel* m_groupLabel = nullptr;
    QJsonObject m_cur;         // grupo em edição (id, name, perms)
    QList<QPair<QString, QCheckBox*>> m_checks; // permissões booleanas
    QSpinBox* m_talkPower = nullptr;
    QComboBox* m_userCombo = nullptr;
};

// "Mostrar permissões do usuário" — visão somente-leitura das minhas permissões
class PermissionsOverviewDialog : public QDialog {
    Q_OBJECT
public:
    explicit PermissionsOverviewDialog(const QJsonObject& myPerms,
                                       const QString& groupName,
                                       QWidget* parent = nullptr);
};

// Item de mensagem offline recebida nesta sessão
struct OfflineMsgItem { QString fromName; QString text; QString ts; };

// "Mensagens offline" — caixa de entrada + envio de novas mensagens
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

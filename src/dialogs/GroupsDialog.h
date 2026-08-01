#pragma once

#include <QDialog>
#include <QListWidget>
#include <QTableWidget>
#include <QLineEdit>
#include "core/Models.h"

// Janela "Grupos de servidores" — editor de permissões no formato do TS3,
// com grade de permissões, filtro e coluna "Conceder" no modo avançado.
class GroupsDialog : public QDialog {
    Q_OBJECT
public:
    explicit GroupsDialog(ServerData* data, QWidget* parent = nullptr);

private:
    void reloadGroups();
    void reloadPermissions();
    void storePermissions();

    ServerData* m_data;
    QListWidget* m_users = nullptr;
    QTableWidget* m_groups = nullptr;
    QLineEdit* m_filter = nullptr;
    QTableWidget* m_perms = nullptr;

    static const QStringList& permissionDefaults();
};

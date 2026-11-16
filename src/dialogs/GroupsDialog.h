#pragma once

#include <QDialog>
#include <QListWidget>
#include <QTableWidget>
#include <QLineEdit>
#include "core/Models.h"

// "Server groups" window -- permission editor in Halla's format, with a permission
// grid, a filter and a "Grant" column in advanced mode.
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

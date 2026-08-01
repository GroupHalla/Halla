#include "GroupsDialog.h"
#include "TsBanner.h"
#include "Icons.h"
#include "Settings.h"
#include "AppLog.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QSplitter>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>

// Permissões reais do TeamSpeak 3 exibidas na grade
const QStringList& GroupsDialog::permissionDefaults() {
    static const QStringList list = {
        "b_virtualserver_select",
        "b_virtualserver_info_view",
        "b_virtualserver_channel_list",
        "b_virtualserver_client_list",
        "b_channel_info_view",
        "b_client_info_view",
        "b_channel_create_permanent",
        "b_channel_create_semi_permanent",
        "b_channel_create_temporary",
        "b_channel_create_private",
        "i_channel_modify_power",
        "i_channel_delete_power",
        "b_channel_modify_name",
        "b_channel_modify_password",
        "b_channel_modify_description",
        "b_channel_modify_topic",
        "b_channel_modify_codec",
        "b_channel_join_permanent",
        "b_channel_join_semi_permanent",
        "b_channel_join_temporary",
        "i_client_talk_power",
        "i_client_poke_power",
        "i_client_move_power",
        "i_client_kick_from_channel_power",
        "i_client_kick_from_server_power",
        "i_client_ban_power",
        "i_client_max_clones_uid",
        "b_client_is_channel_commander",
        "b_client_ignore_antiflood",
        "i_group_show_name_in_tree",
        "b_client_modify_description",
        "b_client_set_flag_avatar",
        "b_ft_file_download",
        "b_ft_file_upload",
        "i_ft_quota_mb_download_per_client",
        "i_ft_quota_mb_upload_per_client",
    };
    return list;
}

GroupsDialog::GroupsDialog(ServerData* data, QWidget* parent)
    : QDialog(parent), m_data(data) {
    setWindowTitle(tr("Grupos de servidores"));
    resize(860, 520);

    QVBoxLayout* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 8);
    root->setSpacing(0);
    root->addWidget(new TsBanner(tr("Grupos de servidores"),
                                 tr("Atribua permissões a grupos e clientes"),
                                 HIcons::groups().pixmap(24, 24), this));
    root->addSpacing(8);

    QSplitter* split = new QSplitter(this);

    // ---- coluna 1: clientes
    QWidget* colUsers = new QWidget(split);
    QVBoxLayout* ulay = new QVBoxLayout(colUsers);
    ulay->setContentsMargins(8, 0, 4, 0);
    ulay->addWidget(new QLabel(tr("Clientes:"), colUsers));
    m_users = new QListWidget(colUsers);
    ulay->addWidget(m_users, 1);

    // ---- coluna 2: grupos
    QWidget* colGroups = new QWidget(split);
    QVBoxLayout* glay = new QVBoxLayout(colGroups);
    glay->setContentsMargins(4, 0, 4, 0);
    glay->addWidget(new QLabel(tr("Grupos de servidores:"), colGroups));
    m_groups = new QTableWidget(0, 2, colGroups);
    m_groups->setHorizontalHeaderLabels({ tr("Grupo"), tr("Atribuído") });
    m_groups->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_groups->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_groups->setEditTriggers(QAbstractItemView::NoEditTriggers);
    glay->addWidget(m_groups, 1);

    // ---- coluna 3: permissões
    QWidget* colPerms = new QWidget(split);
    QVBoxLayout* play = new QVBoxLayout(colPerms);
    play->setContentsMargins(4, 0, 8, 0);
    m_filter = new QLineEdit(colPerms);
    m_filter->setPlaceholderText(tr("Filtro"));
    play->addWidget(m_filter);
    m_perms = new QTableWidget(0, 3, colPerms);
    play->addWidget(m_perms, 1);

    split->addWidget(colUsers);
    split->addWidget(colGroups);
    split->addWidget(colPerms);
    split->setStretchFactor(0, 1);
    split->setStretchFactor(1, 1);
    split->setStretchFactor(2, 2);
    root->addWidget(split, 1);

    QHBoxLayout* bottom = new QHBoxLayout;
    bottom->setContentsMargins(10, 6, 10, 0);
    bottom->addStretch(1);
    QPushButton* close = new QPushButton(tr("Fechar"), this);
    bottom->addWidget(close);
    root->addLayout(bottom);
    connect(close, &QPushButton::clicked, this, [this] { storePermissions(); accept(); });

    // permissões padrão se vazio
    if (m_data->permissions.isEmpty()) {
        for (const QString& p : permissionDefaults()) {
            PermValue pv;
            pv.active = (p.startsWith("b_virtualserver") || p.startsWith("b_channel_join") ||
                         p.startsWith("b_channel_info") || p.startsWith("b_client_info"));
            pv.value = pv.active ? 1 : 0;
            pv.grant = 75;
            m_data->permissions[p] = pv;
        }
        m_data->permissions["i_client_talk_power"].active = true;
        m_data->permissions["i_client_talk_power"].value = 50;
    }

    reloadGroups();
    reloadPermissions();

    connect(m_filter, &QLineEdit::textChanged, this,
            [this](const QString&) { reloadPermissions(); });

    connect(m_groups, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* item) {
        if (item->column() != 1) return;
        storePermissions();
        AppLog::info(tr("Grupos de servidores atualizados"));
    });

    connect(m_perms, &QTableWidget::cellDoubleClicked, this, [this](int row, int col) {
        if (col < 1) return;
        QTableWidgetItem* nameItem = m_perms->item(row, 0);
        if (!nameItem) return;
        const QString pname = nameItem->data(Qt::UserRole).toString();
        if (pname.isEmpty()) return;
        PermValue& pv = m_data->permissions[pname];
        bool ok = false;
        int v = QInputDialog::getInt(this, tr("Editar permissão"), pname,
                                     col == 1 ? pv.value : pv.grant, -9999, 1000000, 1, &ok);
        if (!ok) return;
        if (col == 1) { pv.value = v; pv.active = true; }
        else pv.grant = v;
        reloadPermissions();
    });
}

void GroupsDialog::reloadGroups() {
    const QString self = m_data->users.value(m_data->selfId).name +
                         tr(" (você)");
    m_groups->setRowCount(3);
    const QStringList names = { tr("Convidado"), tr("Normal"), tr("Admin do servidor") };
    const QString& assigned = m_data->users[m_data->selfId].serverGroups;
    for (int i = 0; i < 3; ++i) {
        m_groups->setItem(i, 0, new QTableWidgetItem(names[i]));
        QTableWidgetItem* chk = new QTableWidgetItem;
        chk->setFlags(chk->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsEnabled);
        chk->setCheckState((assigned == names[i] || (i == 1 && assigned == "Normal"))
                               ? Qt::Checked : Qt::Unchecked);
        m_groups->setItem(i, 1, chk);
    }
    m_users->clear();
    m_users->addItem(new QListWidgetItem(HIcons::user(false, false), self));
}

void GroupsDialog::storePermissions() {
    // atribuição de grupo selecionado
    static const QStringList names = { "Convidado", "Normal", "Admin do servidor" };
    for (int i = 0; i < m_groups->rowCount() && i < names.size(); ++i) {
        QTableWidgetItem* chk = m_groups->item(i, 1);
        if (chk && chk->checkState() == Qt::Checked) {
            m_data->users[m_data->selfId].serverGroups = names[i];
        }
    }
}

void GroupsDialog::reloadPermissions() {
    const QString filter = m_filter->text().trimmed().toLower();
    const bool advanced = S::flag("app/advancedPerms", false);

    m_perms->clear();
    m_perms->setSortingEnabled(false);
    QStringList headers = { tr("Permissão"), tr("Valor") };
    if (advanced) headers << tr("Conceder");
    m_perms->setColumnCount(headers.size());
    m_perms->setHorizontalHeaderLabels(headers);
    m_perms->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_perms->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_perms->setSelectionBehavior(QAbstractItemView::SelectRows);

    m_perms->setRowCount(0);
    for (auto it = m_data->permissions.begin(); it != m_data->permissions.end(); ++it) {
        if (!filter.isEmpty() && !it.key().toLower().contains(filter)) continue;
        const int r = m_perms->rowCount();
        m_perms->insertRow(r);

        // nome "amigável" + nome técnico no tooltip, como no TS3
        QTableWidgetItem* name = new QTableWidgetItem(it.key());
        name->setData(Qt::UserRole, it.key());
        name->setToolTip(it.key());
        if (it->active) name->setIcon(HIcons::check());
        m_perms->setItem(r, 0, name);

        QTableWidgetItem* val = new QTableWidgetItem(QString::number(it->value));
        val->setTextAlignment(Qt::AlignCenter);
        m_perms->setItem(r, 1, val);

        if (advanced) {
            QTableWidgetItem* grant = new QTableWidgetItem(QString::number(it->grant));
            grant->setTextAlignment(Qt::AlignCenter);
            m_perms->setItem(r, 2, grant);
        }
    }
}

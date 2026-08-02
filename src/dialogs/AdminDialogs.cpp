#include "AdminDialogs.h"
#include "TsBanner.h"
#include "Icons.h"
#include "Settings.h"
#include "net/NetSession.h"
#include "core/Models.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QSplitter>
#include <QPushButton>
#include <QLabel>
#include <QHeaderView>
#include <QTableWidget>
#include <QTreeWidget>
#include <QComboBox>
#include <QCheckBox>
#include <QSpinBox>
#include <QLineEdit>
#include <QInputDialog>
#include <QMessageBox>
#include <QDateTime>
#include <QGroupBox>
#include <QScrollArea>
#include <QJsonDocument>

static QString fmtTs(const QString& iso) {
    const QDateTime dt = QDateTime::fromString(iso, Qt::ISODate);
    return dt.isValid() ? dt.toLocalTime().toString(QStringLiteral("dd/MM/yyyy HH:mm"))
                        : iso;
}

// ============================================================== Banidos
BanListDialog::BanListDialog(NetSession* net, QWidget* parent)
    : QDialog(parent), m_net(net) {
    setWindowTitle(tr("Lista de banidos"));
    resize(640, 340);

    QVBoxLayout* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 8);
    root->setSpacing(0);
    root->addWidget(new TsBanner(tr("Lista de banidos"),
                                 tr("Banimentos ativos neste servidor"),
                                 HIcons::key().pixmap(24, 24), this));
    root->addSpacing(8);

    QHBoxLayout* mid = new QHBoxLayout;
    mid->setContentsMargins(10, 0, 10, 0);
    m_table = new QTableWidget(0, 5, this);
    m_table->setHorizontalHeaderLabels({ tr("Apelido"), tr("ID único"), tr("IP"),
                                         tr("Motivo"), tr("Expira em") });
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    mid->addWidget(m_table, 1);
    root->addLayout(mid);

    QHBoxLayout* btns = new QHBoxLayout;
    btns->setContentsMargins(10, 6, 10, 0);
    QPushButton* refresh = new QPushButton(tr("Atualizar"), this);
    QPushButton* unban   = new QPushButton(tr("Remover banimento"), this);
    QPushButton* close   = new QPushButton(tr("Fechar"), this);
    btns->addWidget(refresh);
    btns->addWidget(unban);
    btns->addStretch(1);
    btns->addWidget(close);
    root->addLayout(btns);

    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    connect(refresh, &QPushButton::clicked, this, [this] { m_net->requestBanList(); });
    connect(unban, &QPushButton::clicked, this, [this] {
        const int row = m_table->currentRow();
        if (row < 0) return;
        const QString uid = m_table->item(row, 1)->text();
        if (QMessageBox::question(this, tr("Remover banimento"),
                tr("Remover o banimento de \\\"%1\\\" (%2)?")
                    .arg(m_table->item(row, 0)->text(), uid.left(20)))
            == QMessageBox::Yes) {
            m_net->unban(uid);
            m_net->requestBanList(); // recarrega a lista
        }
    });

    connect(m_net, &NetSession::banListReceived, this, &BanListDialog::fill);
    m_net->requestBanList();
}

void BanListDialog::fill(const QJsonArray& bans) {
    m_table->setRowCount(bans.size());
    for (int i = 0; i < bans.size(); ++i) {
        const QJsonObject b = bans[i].toObject();
        m_table->setItem(i, 0, new QTableWidgetItem(b["name"].toString()));
        m_table->setItem(i, 1, new QTableWidgetItem(b["uid"].toString()));
        m_table->setItem(i, 2, new QTableWidgetItem(b["ip"].toString()));
        m_table->setItem(i, 3, new QTableWidgetItem(b["reason"].toString()));
        const QString ex = b["expires"].toString();
        m_table->setItem(i, 4, new QTableWidgetItem(ex.isEmpty() ? tr("Permanente")
                                                                 : fmtTs(ex)));
    }
}

// ============================================================== Reclamações
ComplaintsDialog::ComplaintsDialog(NetSession* net, QWidget* parent)
    : QDialog(parent), m_net(net) {
    setWindowTitle(tr("Reclamações"));
    resize(620, 360);

    QVBoxLayout* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 8);
    root->setSpacing(0);
    root->addWidget(new TsBanner(tr("Reclamações"),
                                 tr("Reclamações registradas pelos usuários"),
                                 HIcons::logPage().pixmap(24, 24), this));
    root->addSpacing(8);

    QHBoxLayout* mid = new QHBoxLayout;
    mid->setContentsMargins(10, 0, 10, 0);
    m_tree = new QTreeWidget(this);
    m_tree->setHeaderLabels({ tr("Sobre"), tr("Por"), tr("Data"), tr("Reclamação") });
    m_tree->setRootIsDecorated(false);
    m_tree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_tree->header()->setStretchLastSection(true);
    mid->addWidget(m_tree, 1);
    root->addLayout(mid);

    QHBoxLayout* btns = new QHBoxLayout;
    btns->setContentsMargins(10, 6, 10, 0);
    QPushButton* refresh   = new QPushButton(tr("Atualizar"), this);
    QPushButton* clearUser = new QPushButton(tr("Remover do usuário selecionado"), this);
    QPushButton* clearAll  = new QPushButton(tr("Limpar todas"), this);
    QPushButton* close     = new QPushButton(tr("Fechar"), this);
    btns->addWidget(refresh);
    btns->addWidget(clearUser);
    btns->addWidget(clearAll);
    btns->addStretch(1);
    btns->addWidget(close);
    root->addLayout(btns);

    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    connect(refresh, &QPushButton::clicked, this, [this] { m_net->complaintList(); });
    connect(clearUser, &QPushButton::clicked, this, [this] {
        QTreeWidgetItem* it = m_tree->currentItem();
        if (!it) return;
        m_net->complaintClear(it->data(0, Qt::UserRole).toString());
        m_net->complaintList();
    });
    connect(clearAll, &QPushButton::clicked, this, [this] {
        if (QMessageBox::question(this, tr("Limpar reclamações"),
                tr("Excluir TODAS as reclamações deste servidor?"))
            == QMessageBox::Yes) {
            m_net->complaintClear();
            m_net->complaintList();
        }
    });

    connect(m_net, &NetSession::complaintListReceived, this, &ComplaintsDialog::fill);
    m_net->complaintList();
}

void ComplaintsDialog::fill(const QJsonArray& complaints) {
    m_tree->clear();
    for (const QJsonValue& v : complaints) {
        const QJsonObject c = v.toObject();
        QTreeWidgetItem* it = new QTreeWidgetItem(m_tree);
        it->setText(0, c["name"].toString());
        it->setText(1, c["byName"].toString());
        it->setText(2, fmtTs(c["ts"].toString()));
        it->setText(3, c["text"].toString());
        it->setData(0, Qt::UserRole, c["uid"].toString());
        it->setToolTip(3, c["text"].toString());
    }
}

// ============================================================== Grupos
// chave de permissão -> rótulo
static const QList<QPair<QString, QString>>& permDefs() {
    static const QList<QPair<QString, QString>> defs = {
        { QStringLiteral("*"),                QStringLiteral("Todas as permissões (administrador total)") },
        { QStringLiteral("kick"),             QStringLiteral("Expulsar clientes") },
        { QStringLiteral("ban"),              QStringLiteral("Banir clientes") },
        { QStringLiteral("banList"),          QStringLiteral("Ver lista de banidos e reclamações") },
        { QStringLiteral("move"),             QStringLiteral("Mover clientes entre canais") },
        { QStringLiteral("poke"),             QStringLiteral("Cutucar clientes") },
        { QStringLiteral("privmsg"),          QStringLiteral("Enviar mensagens privadas") },
        { QStringLiteral("chanCreateTemp"),   QStringLiteral("Criar canal temporário") },
        { QStringLiteral("chanCreateSemi"),   QStringLiteral("Criar canal semi-permanente") },
        { QStringLiteral("chanCreatePerm"),   QStringLiteral("Criar canal permanente") },
        { QStringLiteral("chanEdit"),         QStringLiteral("Editar canais") },
        { QStringLiteral("chanDelete"),       QStringLiteral("Excluir canais") },
        { QStringLiteral("serverEdit"),       QStringLiteral("Editar servidor virtual") },
        { QStringLiteral("groupEdit"),        QStringLiteral("Editar grupos e atribuições") },
        { QStringLiteral("ignoreChanPass"),   QStringLiteral("Ignorar senha de canal") },
        { QStringLiteral("ignoreTalkPower"),  QStringLiteral("Falar em canais moderados") },
    };
    return defs;
}

ServerGroupsDialog::ServerGroupsDialog(NetSession* net, ServerData* data, QWidget* parent)
    : QDialog(parent), m_net(net), m_data(data) {
    setWindowTitle(tr("Grupos de servidores"));
    resize(760, 470);

    QVBoxLayout* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 8);
    root->setSpacing(0);
    root->addWidget(new TsBanner(tr("Grupos de servidores"),
                                 tr("Crie grupos, edite permissões e atribua usuários"),
                                 HIcons::groups().pixmap(24, 24), this));
    root->addSpacing(8);

    QHBoxLayout* mid = new QHBoxLayout;
    mid->setContentsMargins(10, 0, 10, 0);

    QSplitter* split = new QSplitter(this);

    // ---- esquerda: lista de grupos
    QWidget* left = new QWidget(split);
    QVBoxLayout* ll = new QVBoxLayout(left);
    ll->setContentsMargins(0, 0, 0, 0);
    ll->addWidget(new QLabel(tr("Grupos:"), left));
    m_groups = new QTreeWidget(left);
    m_groups->setHeaderLabels({ tr("ID"), tr("Nome") });
    m_groups->setRootIsDecorated(false);
    m_groups->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_groups->header()->setStretchLastSection(true);
    m_groups->setSortingEnabled(true);
    ll->addWidget(m_groups, 1);
    QHBoxLayout* gbtns = new QHBoxLayout;
    QPushButton* add = new QPushButton(tr("Adicionar"), left);
    QPushButton* del = new QPushButton(tr("Excluir"), left);
    gbtns->addWidget(add);
    gbtns->addWidget(del);
    gbtns->addStretch(1);
    ll->addLayout(gbtns);
    split->addWidget(left);

    // ---- direita: editor de permissões + atribuição
    QWidget* right = new QWidget(split);
    QVBoxLayout* rl = new QVBoxLayout(right);
    rl->setContentsMargins(8, 0, 0, 0);
    m_groupLabel = new QLabel(tr("Selecione um grupo à esquerda."), right);
    rl->addWidget(m_groupLabel);

    QScrollArea* scroll = new QScrollArea(right);
    scroll->setWidgetResizable(true);
    m_editor = new QWidget(scroll);
    QVBoxLayout* el = new QVBoxLayout(m_editor);
    for (const auto& def : permDefs()) {
        QCheckBox* cb = new QCheckBox(def.second, m_editor);
        el->addWidget(cb);
        m_checks << qMakePair(def.first, cb);
    }
    QHBoxLayout* tp = new QHBoxLayout;
    tp->addWidget(new QLabel(tr("Poder de fala:"), m_editor));
    m_talkPower = new QSpinBox(m_editor);
    m_talkPower->setRange(0, 100);
    tp->addWidget(m_talkPower);
    tp->addStretch(1);
    el->addLayout(tp);
    el->addStretch(1);
    scroll->setWidget(m_editor);
    rl->addWidget(scroll, 1);

    QHBoxLayout* ebtns = new QHBoxLayout;
    QPushButton* rename = new QPushButton(tr("Renomear"), right);
    QPushButton* apply  = new QPushButton(tr("Aplicar permissões"), right);
    ebtns->addWidget(rename);
    ebtns->addStretch(1);
    ebtns->addWidget(apply);
    rl->addLayout(ebtns);

    // atribuição de usuário
    QGroupBox* assignBox = new QGroupBox(tr("Atribuir grupo a cliente conectado"), right);
    QHBoxLayout* al = new QHBoxLayout(assignBox);
    m_userCombo = new QComboBox(assignBox);
    QPushButton* assign = new QPushButton(tr("Atribuir"), assignBox);
    QPushButton* reloadU = new QPushButton(tr("Atualizar"), assignBox);
    al->addWidget(m_userCombo, 1);
    al->addWidget(assign);
    al->addWidget(reloadU);
    rl->addWidget(assignBox);

    split->addWidget(right);
    split->setStretchFactor(0, 1);
    split->setStretchFactor(1, 2);
    mid->addWidget(split, 1);
    root->addLayout(mid);

    QHBoxLayout* btns = new QHBoxLayout;
    btns->setContentsMargins(10, 6, 10, 0);
    QPushButton* refresh = new QPushButton(tr("Atualizar lista"), this);
    QPushButton* close   = new QPushButton(tr("Fechar"), this);
    btns->addWidget(refresh);
    btns->addStretch(1);
    btns->addWidget(close);
    root->addLayout(btns);

    // ---------------------------------------------------------- ações
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    connect(refresh, &QPushButton::clicked, this, [this] { m_net->requestGroupList(); });
    connect(reloadU, &QPushButton::clicked, this, &ServerGroupsDialog::refreshUsers);

    connect(m_groups, &QTreeWidget::currentItemChanged, this,
            [this](QTreeWidgetItem* cur, QTreeWidgetItem*) {
                if (!cur) return;
                m_cur = QJsonObject();
                m_cur["id"]   = cur->data(0, Qt::UserRole).toInt();
                m_cur["name"] = cur->text(1);
                m_cur["perms"] = QJsonDocument::fromJson(
                    cur->data(0, Qt::UserRole + 1).toString().toUtf8()).object();
                loadPerms(m_cur["perms"].toObject());
            });

    connect(add, &QPushButton::clicked, this, [this] {
        bool ok = false;
        const QString name = QInputDialog::getText(this, tr("Novo grupo"),
                                                   tr("Nome do grupo:"), QLineEdit::Normal,
                                                   QString(), &ok).trimmed();
        if (!ok || name.isEmpty()) return;
        QJsonObject perms;
        perms["poke"] = true;
        perms["privmsg"] = true;
        perms["talkPower"] = 25;
        m_net->groupSet(0, name, perms); // id 0 = criar
        m_net->requestGroupList();
    });

    connect(del, &QPushButton::clicked, this, [this] {
        QTreeWidgetItem* cur = m_groups->currentItem();
        if (!cur) return;
        const int id = cur->data(0, Qt::UserRole).toInt();
        if (id < 100) {
            QMessageBox::information(this, tr("Grupo interno"),
                tr("Grupos internos (convidado, normal, admin) não podem ser excluídos."));
            return;
        }
        if (QMessageBox::question(this, tr("Excluir grupo"),
                tr("Excluir o grupo \\\"%1\\\"? Usuários voltarão ao grupo padrão.")
                    .arg(cur->text(1))) == QMessageBox::Yes) {
            m_net->groupDelete(id);
            m_net->requestGroupList();
        }
    });

    connect(rename, &QPushButton::clicked, this, [this] {
        if (m_cur.isEmpty()) return;
        const int id = m_cur["id"].toInt();
        if (id < 100) {
            QMessageBox::information(this, tr("Grupo interno"),
                tr("Grupos internos não podem ser renomeados."));
            return;
        }
        bool ok = false;
        const QString name = QInputDialog::getText(this, tr("Renomear grupo"),
                                                   tr("Nome do grupo:"), QLineEdit::Normal,
                                                   m_cur["name"].toString(), &ok).trimmed();
        if (!ok || name.isEmpty()) return;
        m_net->groupSet(id, name, m_cur["perms"].toObject());
        m_net->requestGroupList();
    });

    connect(apply, &QPushButton::clicked, this, [this] {
        if (m_cur.isEmpty()) return;
        m_net->groupSet(m_cur["id"].toInt(), m_cur["name"].toString(), collectPerms());
        m_net->requestGroupList();
    });

    connect(assign, &QPushButton::clicked, this, [this] {
        if (m_cur.isEmpty() || m_userCombo->currentIndex() < 0) return;
        const int userId = m_userCombo->currentData().toInt();
        m_net->clientSetGroup(userId, m_cur["id"].toInt());
        QMessageBox::information(this, tr("Atribuição"),
            tr("Grupo \\\"%1\\\" atribuído a \\\"%2\\\".\n"
               "A atribuição fica salva no servidor (por ID único).")
                .arg(m_cur["name"].toString(), m_userCombo->currentText()));
    });

    connect(m_net, &NetSession::groupListReceived, this, &ServerGroupsDialog::fillGroups);

    // pré-carrega com o que veio no welcome e pede a lista atualizada
    fillGroups(m_net->serverGroups());
    refreshUsers();
    m_net->requestGroupList();
}

void ServerGroupsDialog::fillGroups(const QJsonArray& groups) {
    m_groups->clear();
    for (const QJsonValue& v : groups) {
        const QJsonObject g = v.toObject();
        QTreeWidgetItem* it = new QTreeWidgetItem(m_groups);
        it->setText(0, QString::number(g["id"].toInt()));
        it->setText(1, g["name"].toString());
        it->setData(0, Qt::UserRole, g["id"].toInt());
        it->setData(0, Qt::UserRole + 1,
                    QString::fromUtf8(QJsonDocument(g["perms"].toObject())
                                          .toJson(QJsonDocument::Compact)));
    }
    if (m_groups->topLevelItemCount() > 0 && !m_groups->currentItem())
        m_groups->setCurrentItem(m_groups->topLevelItem(0));
}

void ServerGroupsDialog::loadPerms(const QJsonObject& perms) {
    m_groupLabel->setText(tr("Permissões de \\\"%1\\\":").arg(m_cur["name"].toString()));
    for (const auto& p : m_checks)
        p.second->setChecked(perms.value(p.first).toBool());
    m_talkPower->setValue(perms["talkPower"].toInt());
}

QJsonObject ServerGroupsDialog::collectPerms() const {
    QJsonObject perms;
    for (const auto& p : m_checks)
        if (p.second->isChecked()) perms[p.first] = true;
    perms["talkPower"] = m_talkPower->value();
    return perms;
}

void ServerGroupsDialog::refreshUsers() {
    m_userCombo->clear();
    if (!m_data) return;
    for (const User& u : m_data->users)
        m_userCombo->addItem(QStringLiteral("%1  [%2]").arg(u.name, u.serverGroups), u.id);
}

// ============================================================== Minhas permissões
PermissionsOverviewDialog::PermissionsOverviewDialog(const QJsonObject& myPerms,
                                                     const QString& groupName,
                                                     QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(tr("Permissões do usuário"));
    resize(460, 430);

    QVBoxLayout* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 8);
    root->setSpacing(0);
    root->addWidget(new TsBanner(tr("Permissões do usuário"),
                                 tr("Suas permissões neste servidor (grupo: %1)").arg(groupName),
                                 HIcons::key().pixmap(24, 24), this));
    root->addSpacing(8);

    QTreeWidget* tree = new QTreeWidget(this);
    tree->setHeaderLabels({ tr("Permissão"), tr("Valor") });
    tree->setRootIsDecorated(false);
    tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    QHBoxLayout* mid = new QHBoxLayout;
    mid->setContentsMargins(10, 0, 10, 0);
    mid->addWidget(tree, 1);
    root->addLayout(mid);

    int row = 0;
    for (const auto& def : permDefs()) {
        QTreeWidgetItem* it = new QTreeWidgetItem(tree);
        it->setText(0, def.second + QStringLiteral("   (") + def.first + QStringLiteral(")"));
        const bool on = myPerms.value(def.first).toBool();
        it->setText(1, on ? tr("concedida") : tr("negada"));
        it->setForeground(1, on ? QColor(QStringLiteral("#2c8a2c"))
                                : QColor(QStringLiteral("#a04040")));
        ++row;
    }
    {
        QTreeWidgetItem* it = new QTreeWidgetItem(tree);
        it->setText(0, tr("Poder de fala   (talkPower)"));
        it->setText(1, QString::number(myPerms["talkPower"].toInt(10)));
    }

    QHBoxLayout* btns = new QHBoxLayout;
    btns->setContentsMargins(10, 6, 10, 0);
    btns->addStretch(1);
    QPushButton* close = new QPushButton(tr("Fechar"), this);
    btns->addWidget(close);
    root->addLayout(btns);
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
}

// ============================================================== Mensagens offline
OfflineMessagesDialog::OfflineMessagesDialog(NetSession* net, ServerData* data,
                                             const QVector<OfflineMsgItem>& inbox,
                                             QWidget* parent)
    : QDialog(parent), m_net(net), m_data(data) {
    setWindowTitle(tr("Mensagens offline"));
    resize(600, 380);

    QVBoxLayout* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 8);
    root->setSpacing(0);
    root->addWidget(new TsBanner(tr("Mensagens offline"),
                                 tr("Mensagens deixadas para você enquanto estava ausente"),
                                 HIcons::contacts().pixmap(24, 24), this));
    root->addSpacing(8);

    QHBoxLayout* mid = new QHBoxLayout;
    mid->setContentsMargins(10, 0, 10, 0);
    m_tree = new QTreeWidget(this);
    m_tree->setHeaderLabels({ tr("De"), tr("Data"), tr("Mensagem") });
    m_tree->setRootIsDecorated(false);
    m_tree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_tree->header()->setStretchLastSection(true);
    mid->addWidget(m_tree, 1);
    root->addLayout(mid);

    for (const OfflineMsgItem& m : inbox) addItem(m.fromName, m.text, m.ts);

    QHBoxLayout* btns = new QHBoxLayout;
    btns->setContentsMargins(10, 6, 10, 0);
    QPushButton* compose = new QPushButton(tr("Nova mensagem..."), this);
    QPushButton* close   = new QPushButton(tr("Fechar"), this);
    btns->addWidget(compose);
    btns->addStretch(1);
    btns->addWidget(close);
    root->addLayout(btns);

    connect(close, &QPushButton::clicked, this, &QDialog::accept);

    connect(m_net, &NetSession::offlineMsgReceived, this,
            [this](const QString& fromName, const QString& text, const QString& ts) {
                addItem(fromName, text, ts);
            });

    connect(compose, &QPushButton::clicked, this, [this] {
        if (!m_data) return;
        QStringList names;
        QStringList uids;
        for (const User& u : m_data->users) {
            if (u.id == m_data->selfId) continue;
            names << QStringLiteral("%1  (online)").arg(u.name);
            uids << u.uniqueId;
        }
        if (names.isEmpty()) {
            QMessageBox::information(this, tr("Sem destinatários"),
                tr("Não há outros usuários conectados.\n"
                   "Mensagens offline podem ser enviadas apenas a usuários "
                   "registrados neste servidor."));
            return;
        }
        bool ok = false;
        const QString who = QInputDialog::getItem(this, tr("Nova mensagem offline"),
                                                  tr("Destinatário:"), names, 0, false, &ok);
        if (!ok) return;
        const QString msg = QInputDialog::getMultiLineText(this, tr("Nova mensagem offline"),
                                                           tr("Mensagem:"), QString(), &ok);
        if (!ok || msg.trimmed().isEmpty()) return;
        m_net->offlineSend(uids.value(names.indexOf(who)), msg.trimmed());
    });

    connect(m_net, &NetSession::offlineSendConfirmed, this, [this](const QString&) {
        QMessageBox::information(this, tr("Mensagem offline"),
            tr("Mensagem entregue. Será mostrada quando o usuário se conectar."));
    });
}

void OfflineMessagesDialog::addItem(const QString& fromName, const QString& text,
                                    const QString& ts) {
    QTreeWidgetItem* it = new QTreeWidgetItem(m_tree);
    it->setText(0, fromName);
    it->setText(1, fmtTs(ts));
    it->setText(2, text);
    it->setToolTip(2, text);
    m_tree->scrollToBottom();
}

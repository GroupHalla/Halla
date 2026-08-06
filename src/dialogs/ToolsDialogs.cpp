#include "ToolsDialogs.h"
#include "TsBanner.h"
#include "Icons.h"
#include "Settings.h"
#include "AppLog.h"
#include "net/NetSession.h"
#include "core/Models.h"
#include "HotkeyEdit.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QPushButton>
#include <QLabel>
#include <QHeaderView>
#include <QInputDialog>
#include <QKeySequenceEdit>
#include <QCheckBox>
#include <QComboBox>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QFileDialog>
#include <QFileInfo>
#include <QDateTime>
#include <QDir>
#include <QMessageBox>

static QList<QJsonObject> loadList(const char* key) {
    QList<QJsonObject> out;
    QJsonDocument doc = QJsonDocument::fromJson(S::str(key).toUtf8());
    if (doc.isArray())
        for (const QJsonValue& v : doc.array()) out << v.toObject();
    return out;
}

static void saveList(const char* key, const QList<QJsonObject>& list) {
    QJsonArray arr;
    for (const QJsonObject& o : list) arr << o;
    S::set(key, QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)));
}

// ================================================================== Whisper
#include <QSplitter>
#include <QListWidget>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QComboBox>
#include <QLineEdit>
#include <functional>

QStringList WhisperDialog::activeWhisperUids() {
    const QString uid = S::str("whisper/activeList");
    if (uid.isEmpty()) return {};
    for (const QJsonObject& o : loadList("whispers"))
        if (o["name"].toString() == uid) {
            QStringList out;
            for (const QJsonValue& v : o["uids"].toArray()) out << v.toString();
            return out;
        }
    return {};
}

WhisperDialog::WhisperDialog(const ServerData* data, QWidget* parent)
    : QDialog(parent), m_data(data) {
    setWindowTitle(tr("Listas de sussurro"));
    resize(950, 550);

    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(10, 10, 10, 10);
    mainLayout->setSpacing(8);

    QHBoxLayout* columnsLayout = new QHBoxLayout;
    columnsLayout->setSpacing(10);

    // ========================================== COLUMN 1: LEFT (Gerenciamento de Listas)
    QVBoxLayout* colLeft = new QVBoxLayout;
    
    colLeft->addWidget(new QLabel(tr("Listas de sussurros sincronizadas"), this));
    m_syncList = new QListWidget(this);
    m_syncList->setMinimumHeight(150);
    colLeft->addWidget(m_syncList, 3);

    colLeft->addWidget(new QLabel(tr("Listas de sussurros locais"), this));
    m_localList = new QListWidget(this);
    m_localList->setMinimumHeight(150);
    colLeft->addWidget(m_localList, 2);

    QHBoxLayout* leftButtons = new QHBoxLayout;
    m_btnNew = new QPushButton(tr("Novo"), this);
    m_btnRemove = new QPushButton(tr("Remover"), this);
    m_btnRename = new QPushButton(tr("Renomear"), this);
    leftButtons->addWidget(m_btnNew);
    leftButtons->addWidget(m_btnRemove);
    leftButtons->addWidget(m_btnRename);
    colLeft->addLayout(leftButtons);

    columnsLayout->addLayout(colLeft, 1);

    // ========================================== COLUMN 2: CENTER (Detalhes e Árvore de Alvos)
    QVBoxLayout* colCenter = new QVBoxLayout;
    
    QFormLayout* centerForm = new QFormLayout;
    centerForm->setSpacing(6);
    centerForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    m_hotkeyEdit = new HotkeyEdit(this);
    m_hotkeyEdit->setMinimumWidth(180);
    centerForm->addRow(tr("Tecla de atalho:"), m_hotkeyEdit);

    m_replyHotkeyEdit = new HotkeyEdit(this);
    m_replyHotkeyEdit->setMinimumWidth(180);
    centerForm->addRow(tr("Tecla de atalho para resposta:"), m_replyHotkeyEdit);

    m_scopeCombo = new QComboBox(this);
    m_scopeCombo->addItems({ tr("Clientes & canais"), tr("Grupos de servidores"), tr("Grupos de canais") });
    centerForm->addRow(tr("Enviar sussurro para:"), m_scopeCombo);

    colCenter->addLayout(centerForm);
    colCenter->addSpacing(4);

    colCenter->addWidget(new QLabel(tr("Árvore de Alvos"), this));
    m_targetsTree = new QTreeWidget(this);
    m_targetsTree->setHeaderHidden(true);
    m_targetsTree->setFrameShape(QFrame::StyledPanel);
    colCenter->addWidget(m_targetsTree, 1);

    columnsLayout->addLayout(colCenter, 1);

    // ========================================== COLUMN 3: RIGHT (Navegador/Seletor de Canais e Usuários)
    QVBoxLayout* colRight = new QVBoxLayout;

    QHBoxLayout* rightTop = new QHBoxLayout;
    rightTop->addWidget(new QLabel(tr("Filtro:"), this));
    m_filterCombo = new QComboBox(this);
    m_filterCombo->addItems({ tr("Ver tudo"), tr("Canais"), tr("Clientes") });
    rightTop->addWidget(m_filterCombo, 1);
    colRight->addLayout(rightTop);

    colRight->addWidget(new QLabel(tr("Árvore do Servidor"), this));
    m_serverTree = new QTreeWidget(this);
    m_serverTree->setHeaderHidden(true);
    m_serverTree->setFrameShape(QFrame::StyledPanel);
    colRight->addWidget(m_serverTree, 1);

    QHBoxLayout* searchLayout = new QHBoxLayout;
    m_searchEdit = new QLineEdit(this);
    m_searchEdit->setPlaceholderText(tr("Pesquisar..."));
    searchLayout->addWidget(m_searchEdit, 1);
    colRight->addLayout(searchLayout);

    columnsLayout->addLayout(colRight, 1);

    mainLayout->addLayout(columnsLayout, 1);

    // ========================================== FOOTER
    QHBoxLayout* footerLayout = new QHBoxLayout;
    m_btnReload = new QPushButton(tr("Recarregar"), this);
    footerLayout->addWidget(m_btnReload);
    footerLayout->addStretch(1);

    QPushButton* btnOk = new QPushButton(tr("OK"), this);
    QPushButton* btnCancel = new QPushButton(tr("Cancelar"), this);
    QPushButton* btnApply = new QPushButton(tr("Aplicar"), this);
    footerLayout->addWidget(btnOk);
    footerLayout->addWidget(btnCancel);
    footerLayout->addWidget(btnApply);
    mainLayout->addLayout(footerLayout);

    // Connections
    connect(m_btnNew, &QPushButton::clicked, this, &WhisperDialog::onNewList);
    connect(m_btnRemove, &QPushButton::clicked, this, &WhisperDialog::onRemoveList);
    connect(m_btnRename, &QPushButton::clicked, this, &WhisperDialog::onRenameList);
    connect(m_btnReload, &QPushButton::clicked, this, &WhisperDialog::onReload);
    connect(m_syncList, &QListWidget::currentItemChanged, this, &WhisperDialog::onSelectedListChanged);
    connect(m_searchEdit, &QLineEdit::textChanged, this, &WhisperDialog::onSearchTextChanged);
    connect(m_filterCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &WhisperDialog::onFilterChanged);
    connect(m_serverTree, &QTreeWidget::itemDoubleClicked, this, &WhisperDialog::onServerTreeDoubleClicked);
    connect(m_targetsTree, &QTreeWidget::itemChanged, this, &WhisperDialog::onTargetItemChanged);

    connect(m_hotkeyEdit, &HotkeyEdit::specChanged, this, [this](const QString& spec) {
        if (m_isLoading) return;
        QListWidgetItem* item = m_syncList->currentItem();
        if (!item) return;
        int index = m_syncList->row(item);
        if (index >= 0 && index < m_whispers.size()) {
            m_whispers[index]["key"] = spec;
            QString name = m_whispers[index]["name"].toString();
            QString displayKey = spec.isEmpty() ? tr("Nenhuma tecla de atalho atribuída") : spec;
            item->setText(name + " (" + displayKey + ")");
        }
    });

    connect(m_replyHotkeyEdit, &HotkeyEdit::specChanged, this, [this](const QString& spec) {
        if (m_isLoading) return;
        QListWidgetItem* item = m_syncList->currentItem();
        if (!item) return;
        int index = m_syncList->row(item);
        if (index >= 0 && index < m_whispers.size()) {
            m_whispers[index]["replyKey"] = spec;
        }
    });

    connect(m_scopeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int idx) {
        if (m_isLoading) return;
        QListWidgetItem* item = m_syncList->currentItem();
        if (!item) return;
        int index = m_syncList->row(item);
        if (index >= 0 && index < m_whispers.size()) {
            m_whispers[index]["scope"] = idx;
        }
    });

    connect(btnOk, &QPushButton::clicked, this, &WhisperDialog::onAccept);
    connect(btnCancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(btnApply, &QPushButton::clicked, this, &WhisperDialog::onApply);

    // Initial load
    loadSettings();
    populateServerTree();
}

void WhisperDialog::loadSettings() {
    m_isLoading = true;
    m_whispers = loadList("whispers");
    m_syncList->clear();
    
    for (const QJsonObject& o : m_whispers) {
        QString name = o["name"].toString();
        QString key = o["key"].toString();
        if (key.isEmpty()) {
            key = tr("Nenhuma tecla de atalho atribuída");
        }
        m_syncList->addItem(name + " (" + key + ")");
    }
    
    m_localList->clear();
    // Local lists are empty, show standard placeholder text "Nenhum item neste painel..."
    QListWidgetItem* placeholder = new QListWidgetItem(tr("Nenhum item neste painel..."), m_localList);
    placeholder->setFlags(placeholder->flags() & ~Qt::ItemIsEnabled);
    m_localList->addItem(placeholder);
    
    m_isLoading = false;
    
    if (m_syncList->count() > 0) {
        m_syncList->setCurrentRow(0);
    } else {
        onSelectedListChanged();
    }
}

void WhisperDialog::saveSettings() {
    saveList("whispers", m_whispers);
    QListWidgetItem* item = m_syncList->currentItem();
    if (item) {
        int index = m_syncList->row(item);
        if (index >= 0 && index < m_whispers.size()) {
            S::set("whisper/activeList", m_whispers[index]["name"].toString());
        }
    }
}

void WhisperDialog::onNewList() {
    bool ok = false;
    QString name = QInputDialog::getText(this, tr("Nova lista de sussurro"),
                                         tr("Nome da lista:"), QLineEdit::Normal, QString(), &ok);
    if (!ok || name.trimmed().isEmpty()) return;
    
    QJsonObject o;
    o["name"] = name.trimmed();
    o["key"] = "";
    o["replyKey"] = "";
    o["scope"] = 0;
    o["uids"] = QJsonArray();
    o["targetNames"] = "";
    
    m_whispers << o;
    saveSettings();
    loadSettings();
    
    for (int i = 0; i < m_syncList->count(); ++i) {
        if (m_syncList->item(i)->text().startsWith(name.trimmed())) {
            m_syncList->setCurrentRow(i);
            break;
        }
    }
}

void WhisperDialog::onRemoveList() {
    QListWidgetItem* item = m_syncList->currentItem();
    if (!item) return;
    int index = m_syncList->row(item);
    if (index >= 0 && index < m_whispers.size()) {
        const QString name = m_whispers[index]["name"].toString();
        m_whispers.removeAt(index);
        saveSettings();
        if (S::str("whisper/activeList") == name) S::set("whisper/activeList", QString());
        loadSettings();
    }
}

void WhisperDialog::onRenameList() {
    QListWidgetItem* item = m_syncList->currentItem();
    if (!item) return;
    int index = m_syncList->row(item);
    if (index >= 0 && index < m_whispers.size()) {
        const QString oldName = m_whispers[index]["name"].toString();
        bool ok = false;
        QString name = QInputDialog::getText(this, tr("Renomear lista de sussurro"),
                                             tr("Novo nome:"), QLineEdit::Normal, oldName, &ok);
        if (!ok || name.trimmed().isEmpty()) return;
        
        m_whispers[index]["name"] = name.trimmed();
        saveSettings();
        if (S::str("whisper/activeList") == oldName) S::set("whisper/activeList", name.trimmed());
        loadSettings();
    }
}

void WhisperDialog::onReload() {
    loadSettings();
    populateServerTree();
}

void WhisperDialog::onSelectedListChanged() {
    m_isLoading = true;
    
    QListWidgetItem* currentItem = m_syncList->currentItem();
    if (!currentItem) {
        m_hotkeyEdit->setSpec("");
        m_replyHotkeyEdit->setSpec("");
        m_scopeCombo->setCurrentIndex(0);
        m_targetsTree->clear();
        m_isLoading = false;
        return;
    }
    
    int index = m_syncList->row(currentItem);
    if (index >= 0 && index < m_whispers.size()) {
        const QJsonObject& o = m_whispers[index];
        m_hotkeyEdit->setSpec(o["key"].toString());
        m_replyHotkeyEdit->setSpec(o["replyKey"].toString());
        m_scopeCombo->setCurrentIndex(o["scope"].toInt(0));
        populateTargetsTreeForSelected();
    }
    
    m_isLoading = false;
}

void WhisperDialog::onTargetsChanged() {
}

void WhisperDialog::onSearchTextChanged(const QString& text) {
    if (text.isEmpty()) {
        for (int i = 0; i < m_serverTree->topLevelItemCount(); ++i) {
            QTreeWidgetItem* top = m_serverTree->topLevelItem(i);
            top->setHidden(false);
            std::function<void(QTreeWidgetItem*)> showAll = [&](QTreeWidgetItem* item) {
                item->setHidden(false);
                for (int j = 0; j < item->childCount(); ++j) showAll(item->child(j));
            };
            showAll(top);
        }
        return;
    }
    
    for (int i = 0; i < m_serverTree->topLevelItemCount(); ++i) {
        QTreeWidgetItem* top = m_serverTree->topLevelItem(i);
        filterTree(top, text);
    }
}

void WhisperDialog::filterTree(QTreeWidgetItem* item, const QString& text) {
    bool match = item->text(0).contains(text, Qt::CaseInsensitive);
    bool anyChildMatch = false;
    for (int i = 0; i < item->childCount(); ++i) {
        filterTree(item->child(i), text);
        if (!item->child(i)->isHidden()) {
            anyChildMatch = true;
        }
    }
    item->setHidden(!match && !anyChildMatch);
}

void WhisperDialog::onFilterChanged(int index) {
    std::function<void(QTreeWidgetItem*)> applyFilter = [&](QTreeWidgetItem* item) {
        QString type = item->data(0, Qt::UserRole + 1).toString();
        bool show = true;
        if (index == 1) { // Canais
            show = (type == "channel" || type == "server");
        } else if (index == 2) { // Clientes
            show = (type == "user" || type == "server" || type.isEmpty());
        }
        
        item->setHidden(!show);
        
        for (int i = 0; i < item->childCount(); ++i) {
            applyFilter(item->child(i));
        }
    };
    
    for (int i = 0; i < m_serverTree->topLevelItemCount(); ++i) {
        applyFilter(m_serverTree->topLevelItem(i));
    }
}

void WhisperDialog::populateServerTree() {
    m_serverTree->clear();
    AppLog::info(QStringLiteral("Sussurro: Populando árvore do servidor. m_data=%1").arg(m_data ? "Sim" : "Não"));
    
    // Top-level 1: Contactos
    QTreeWidgetItem* contactsRoot = new QTreeWidgetItem(m_serverTree);
    contactsRoot->setText(0, tr("Contactos"));
    contactsRoot->setIcon(0, HIcons::contacts());
    
    QList<QJsonObject> contacts = loadList("contacts");
    for (const QJsonObject& c : contacts) {
        QTreeWidgetItem* cItem = new QTreeWidgetItem(contactsRoot);
        cItem->setText(0, c["name"].toString());
        cItem->setIcon(0, HIcons::contacts());
        cItem->setData(0, Qt::UserRole, c["name"].toString());
        cItem->setData(0, Qt::UserRole + 1, "user");
        cItem->setData(0, Qt::UserRole + 2, c["uid"].toString());
    }
    
    // Top-level 2: Servidores
    QTreeWidgetItem* serverRoot = new QTreeWidgetItem(m_serverTree);
    serverRoot->setText(0, m_data ? m_data->name : tr("Servidores"));
    serverRoot->setIcon(0, HIcons::server());
    serverRoot->setData(0, Qt::UserRole + 1, "server");
    
    if (m_data) {
        QList<int> rootChans = m_data->childChannels(0);
        
        std::function<void(QTreeWidgetItem*, int)> addServerChan = [&](QTreeWidgetItem* parentItem, int chanId) {
            const Channel& chan = m_data->channels[chanId];
            QTreeWidgetItem* chanItem = new QTreeWidgetItem(parentItem);
            chanItem->setText(0, chan.name);
            chanItem->setIcon(0, HIcons::channel(false, false, false, false));
            chanItem->setData(0, Qt::UserRole, chan.name);
            chanItem->setData(0, Qt::UserRole + 1, "channel");
            chanItem->setData(0, Qt::UserRole + 2, QString::number(chan.id));
            
            for (int userId : chan.users) {
                if (m_data->users.contains(userId)) {
                    const User& u = m_data->users[userId];
                    QTreeWidgetItem* userItem = new QTreeWidgetItem(chanItem);
                    userItem->setText(0, u.name);
                    userItem->setIcon(0, HIcons::user(false, false));
                    userItem->setData(0, Qt::UserRole, u.name);
                    userItem->setData(0, Qt::UserRole + 1, "user");
                    userItem->setData(0, Qt::UserRole + 2, u.uniqueId);
                }
            }
            
            for (int childId : m_data->childChannels(chanId)) {
                addServerChan(chanItem, childId);
            }
        };
        
        for (int rId : rootChans) {
            addServerChan(serverRoot, rId);
        }
    }
    
    m_serverTree->expandAll();
}

void WhisperDialog::populateTargetsTreeForSelected() {
    m_isLoading = true;
    m_targetsTree->clear();
    AppLog::info(QStringLiteral("Sussurro: Populando árvore de alvos. m_data=%1").arg(m_data ? "Sim" : "Não"));
    
    QListWidgetItem* currentItem = m_syncList->currentItem();
    if (!currentItem) {
        m_isLoading = false;
        return;
    }
    
    int index = m_syncList->row(currentItem);
    if (index < 0 || index >= m_whispers.size()) {
        m_isLoading = false;
        return;
    }
    
    const QJsonObject& o = m_whispers[index];
    QJsonArray uidsArr = o["uids"].toArray();
    QStringList uids;
    for (const QJsonValue& val : uidsArr) {
        uids << val.toString();
    }
    
    if (!m_data) {
        for (const QString& uid : uids) {
            QTreeWidgetItem* item = new QTreeWidgetItem(m_targetsTree);
            item->setText(0, uid);
            item->setIcon(0, HIcons::user(false, false));
            item->setCheckState(0, Qt::Checked);
            item->setData(0, Qt::UserRole, uid);
            item->setData(0, Qt::UserRole + 1, "user");
            item->setData(0, Qt::UserRole + 2, uid);
        }
        m_isLoading = false;
        return;
    }
    
    QList<int> rootChans = m_data->childChannels(0);
    
    std::function<void(QTreeWidgetItem*, int)> addTargetsChan = [&](QTreeWidgetItem* parentItem, int chanId) {
        const Channel& chan = m_data->channels[chanId];
        QTreeWidgetItem* chanItem = nullptr;
        if (parentItem) {
            chanItem = new QTreeWidgetItem(parentItem);
        } else {
            chanItem = new QTreeWidgetItem(m_targetsTree);
        }
        chanItem->setText(0, chan.name);
        chanItem->setIcon(0, HIcons::channel(false, false, false, false));
        chanItem->setData(0, Qt::UserRole, chan.name);
        chanItem->setData(0, Qt::UserRole + 1, "channel");
        chanItem->setData(0, Qt::UserRole + 2, QString::number(chan.id));
        
        bool isChanChecked = uids.contains(QString::number(chan.id));
        chanItem->setCheckState(0, isChanChecked ? Qt::Checked : Qt::Unchecked);
        
        for (int userId : chan.users) {
            if (m_data->users.contains(userId)) {
                const User& u = m_data->users[userId];
                QTreeWidgetItem* userItem = new QTreeWidgetItem(chanItem);
                userItem->setText(0, u.name);
                userItem->setIcon(0, HIcons::user(false, false));
                userItem->setData(0, Qt::UserRole, u.name);
                userItem->setData(0, Qt::UserRole + 1, "user");
                userItem->setData(0, Qt::UserRole + 2, u.uniqueId);
                
                bool isUserChecked = uids.contains(u.uniqueId);
                userItem->setCheckState(0, isUserChecked ? Qt::Checked : Qt::Unchecked);
            }
        }
        
        for (int childId : m_data->childChannels(chanId)) {
            addTargetsChan(chanItem, childId);
        }
    };
    
    for (int rId : rootChans) {
        addTargetsChan(nullptr, rId);
    }
    
    m_targetsTree->expandAll();
    m_isLoading = false;
}

void WhisperDialog::onServerTreeDoubleClicked(QTreeWidgetItem* item, int column) {
    Q_UNUSED(column);
    QString type = item->data(0, Qt::UserRole + 1).toString();
    if (type.isEmpty() || type == "server") return;
    QString name = item->text(0);
    QString uid = item->data(0, Qt::UserRole + 2).toString();
    
    QListWidgetItem* currentList = m_syncList->currentItem();
    if (!currentList) {
        QMessageBox::warning(this, tr("Nenhuma lista selecionada"),
                             tr("Selecione ou crie uma lista de sussurros à esquerda antes de adicionar destinatários."));
        return;
    }
    
    QList<QTreeWidgetItem*> found = m_targetsTree->findItems(name, Qt::MatchRecursive);
    bool marked = false;
    for (QTreeWidgetItem* tItem : found) {
        if (tItem->data(0, Qt::UserRole + 2).toString() == uid) {
            tItem->setCheckState(0, Qt::Checked);
            marked = true;
            onTargetItemChanged(tItem, 0);
        }
    }
    
    if (!marked) {
        QTreeWidgetItem* newItem = new QTreeWidgetItem(m_targetsTree);
        newItem->setText(0, name);
        newItem->setIcon(0, (type == "channel") ? HIcons::channel(false, false, false, false) : HIcons::user(false, false));
        newItem->setData(0, Qt::UserRole, name);
        newItem->setData(0, Qt::UserRole + 1, type);
        newItem->setData(0, Qt::UserRole + 2, uid);
        newItem->setCheckState(0, Qt::Checked);
        onTargetItemChanged(newItem, 0);
    }
}

void WhisperDialog::onTargetItemChanged(QTreeWidgetItem* item, int column) {
    Q_UNUSED(column);
    if (m_isLoading) return;
    
    QListWidgetItem* listRow = m_syncList->currentItem();
    if (!listRow) return;
    int index = m_syncList->row(listRow);
    if (index < 0 || index >= m_whispers.size()) return;
    
    m_isLoading = true;
    
    Qt::CheckState state = item->checkState(0);
    std::function<void(QTreeWidgetItem*, Qt::CheckState)> checkChildren = [&](QTreeWidgetItem* parent, Qt::CheckState s) {
        for (int i = 0; i < parent->childCount(); ++i) {
            QTreeWidgetItem* child = parent->child(i);
            child->setCheckState(0, s);
            checkChildren(child, s);
        }
    };
    checkChildren(item, state);
    
    QJsonArray checkedUids;
    QStringList checkedNames;
    
    std::function<void(QTreeWidgetItem*)> collectChecked = [&](QTreeWidgetItem* parent) {
        for (int i = 0; i < parent->childCount(); ++i) {
            QTreeWidgetItem* child = parent->child(i);
            if (child->checkState(0) == Qt::Checked) {
                QString uid = child->data(0, Qt::UserRole + 2).toString();
                if (!uid.isEmpty()) {
                    checkedUids.append(uid);
                    checkedNames.append(child->text(0));
                }
            }
            collectChecked(child);
        }
    };
    
    for (int i = 0; i < m_targetsTree->topLevelItemCount(); ++i) {
        QTreeWidgetItem* top = m_targetsTree->topLevelItem(i);
        if (top->checkState(0) == Qt::Checked) {
            QString uid = top->data(0, Qt::UserRole + 2).toString();
            if (!uid.isEmpty()) {
                checkedUids.append(uid);
                checkedNames.append(top->text(0));
            }
        }
        collectChecked(top);
    }
    
    m_whispers[index]["uids"] = checkedUids;
    m_whispers[index]["targetNames"] = checkedNames.join(QStringLiteral(", "));
    
    m_isLoading = false;
}

void WhisperDialog::onApply() {
    saveSettings();
    emit settingsSaved();
}

void WhisperDialog::onAccept() {
    saveSettings();
    emit settingsSaved();
    QDialog::accept();
}

void WhisperDialog::addTargetToSelected(const QString& name, const QString& uid, bool isChannel) {
    Q_UNUSED(name);
    Q_UNUSED(uid);
    Q_UNUSED(isChannel);
}

// ================================================================== Contatos
ContactsDialog::ContactsDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Contatos"));
    resize(460, 300);

    QVBoxLayout* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 8);
    root->setSpacing(0);
    root->addWidget(new TsBanner(tr("Contatos"),
                                 tr("Seus amigos do Halla (armazenados localmente)"),
                                 HIcons::contacts().pixmap(24, 24), this));
    root->addSpacing(8);

    QHBoxLayout* mid = new QHBoxLayout;
    mid->setContentsMargins(10, 0, 10, 0);
    m_table = new QTableWidget(0, 2, this);
    m_table->setHorizontalHeaderLabels({ tr("Nome"), tr("ID único") });
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    mid->addWidget(m_table, 1);
    root->addLayout(mid);

    QHBoxLayout* btns = new QHBoxLayout;
    btns->setContentsMargins(10, 6, 10, 0);
    QPushButton* add = new QPushButton(tr("Adicionar"), this);
    QPushButton* del = new QPushButton(tr("Excluir"), this);
    QPushButton* close = new QPushButton(tr("Fechar"), this);
    btns->addWidget(add);
    btns->addWidget(del);
    btns->addStretch(1);
    btns->addWidget(close);
    root->addLayout(btns);

    connect(close, &QPushButton::clicked, this, &QDialog::accept);

    connect(add, &QPushButton::clicked, this, [this] {
        bool ok = false;
        QString name = QInputDialog::getText(this, tr("Adicionar contato"),
                                             tr("Nome:"), QLineEdit::Normal, QString(), &ok);
        if (!ok || name.trimmed().isEmpty()) return;
        QString uid = QInputDialog::getText(this, tr("Adicionar contato"),
                                            tr("ID único do contato:"), QLineEdit::Normal,
                                            QString(), &ok);
        if (!ok) return;
        QList<QJsonObject> list = loadList("contacts");
        QJsonObject o;
        o["name"] = name.trimmed();
        o["uid"] = uid.trimmed();
        list << o;
        saveList("contacts", list);
        reload();
    });

    connect(del, &QPushButton::clicked, this, [this] {
        auto items = m_table->selectedItems();
        if (items.isEmpty()) return;
        QList<QJsonObject> list = loadList("contacts");
        list.removeAt(items.first()->row());
        saveList("contacts", list);
        reload();
    });

    reload();
}

void ContactsDialog::reload() {
    QList<QJsonObject> list = loadList("contacts");
    m_table->setRowCount(list.size());
    for (int i = 0; i < list.size(); ++i) {
        m_table->setItem(i, 0, new QTableWidgetItem(list[i]["name"].toString()));
        m_table->setItem(i, 1, new QTableWidgetItem(list[i]["uid"].toString()));
    }
}

// ================================================================== Transferências
static QString fmtSize(const QString& bytesStr) {
    const qint64 b = bytesStr.toLongLong();
    if (b >= 1024 * 1024) return QStringLiteral("%1 MB").arg(b / 1048576.0, 0, 'f', 1);
    if (b >= 1024)        return QStringLiteral("%1 KB").arg(b / 1024.0, 0, 'f', 1);
    return QStringLiteral("%1 B").arg(b);
}

FileTransferDialog::FileTransferDialog(NetSession* net, ServerData* data, QWidget* parent)
    : QDialog(parent), m_net(net), m_data(data) {
    setWindowTitle(tr("Transferência de arquivos"));
    resize(620, 360);

    QVBoxLayout* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 8);
    root->setSpacing(0);
    root->addWidget(new TsBanner(tr("Transferência de arquivos"),
                                 tr("Arquivos compartilhados por canal neste servidor"),
                                 HIcons::transfer().pixmap(24, 24), this));
    root->addSpacing(8);

    QHBoxLayout* chrow = new QHBoxLayout;
    chrow->setContentsMargins(10, 0, 10, 0);
    chrow->addWidget(new QLabel(tr("Canal:"), this));
    m_channels = new QComboBox(this);
    if (m_data) {
        const int myChan = m_data->channelOfUser(m_data->selfId);
        int sel = 0;
        for (const Channel& c : m_data->channels) {
            m_channels->addItem(c.name, c.id);
            if (c.id == myChan) sel = m_channels->count() - 1;
        }
        m_channels->setCurrentIndex(qMax(0, sel));
    }
    chrow->addWidget(m_channels, 1);
    QPushButton* refresh = new QPushButton(tr("Atualizar"), this);
    chrow->addWidget(refresh);
    root->addLayout(chrow);
    root->addSpacing(6);

    QHBoxLayout* mid = new QHBoxLayout;
    mid->setContentsMargins(10, 0, 10, 0);
    m_table = new QTableWidget(0, 4, this);
    m_table->setHorizontalHeaderLabels({ tr("Arquivo"), tr("Tamanho"), tr("Enviado por"),
                                         tr("Data") });
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    mid->addWidget(m_table, 1);
    root->addLayout(mid);

    QHBoxLayout* btns = new QHBoxLayout;
    btns->setContentsMargins(10, 6, 10, 0);
    QPushButton* up  = new QPushButton(tr("Enviar arquivo..."), this);
    QPushButton* dn  = new QPushButton(tr("Baixar..."), this);
    QPushButton* del = new QPushButton(tr("Excluir"), this);
    QPushButton* close = new QPushButton(tr("Fechar"), this);
    btns->addWidget(up);
    btns->addWidget(dn);
    btns->addWidget(del);
    btns->addStretch(1);
    btns->addWidget(close);
    root->addLayout(btns);

    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    connect(refresh, &QPushButton::clicked, this, &FileTransferDialog::refresh);
    connect(m_channels, &QComboBox::currentIndexChanged, this,
            [this](int) { this->refresh(); });

    // lista recebida do servidor
    connect(m_net, &NetSession::ftListReceived, this,
            [this](int channel, const QJsonArray& files) {
                if (channel != currentChannel()) return;
                m_table->setRowCount(files.size());
                for (int i = 0; i < files.size(); ++i) {
                    const QJsonObject f = files[i].toObject();
                    m_table->setItem(i, 0, new QTableWidgetItem(f["name"].toString()));
                    m_table->setItem(i, 1, new QTableWidgetItem(fmtSize(f["size"].toString())));
                    m_table->setItem(i, 2, new QTableWidgetItem(f["by"].toString()));
                    const QDateTime ts = QDateTime::fromString(f["ts"].toString(), Qt::ISODate);
                    m_table->setItem(i, 3, new QTableWidgetItem(
                        ts.isValid() ? ts.toLocalTime().toString(QStringLiteral("dd/MM/yyyy HH:mm"))
                                     : f["ts"].toString()));
                }
            });

    // conteúdo de download recebido -> salvar
    connect(m_net, &NetSession::ftDataReceived, this,
            [this](int channel, const QString& name, const QByteArray& bytes) {
                Q_UNUSED(channel);
                const QString path = QFileDialog::getSaveFileName(
                    this, tr("Salvar arquivo"), QDir::homePath() + QLatin1Char('/') + name);
                if (path.isEmpty()) return;
                QFile out(path);
                if (out.open(QIODevice::WriteOnly)) {
                    out.write(bytes);
                    QMessageBox::information(this, tr("Download concluído"),
                        tr("Arquivo \\\"%1\\\" salvo em:\n%2").arg(name, path));
                }
            });

    connect(m_net, &NetSession::ftUploadConfirmed, this,
            [this](int channel, const QString&) {
                if (channel == currentChannel()) this->refresh();
            });
    connect(m_net, &NetSession::ftDeleteConfirmed, this,
            [this](int channel, const QString&) {
                if (channel == currentChannel()) this->refresh();
            });

    connect(up, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(this, tr("Enviar arquivo"));
        if (path.isEmpty()) return;
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) return;
        const QByteArray data = f.readAll();
        if (data.size() > 1024 * 512) { // base64 incha ~33% -> limite efetivo no servidor: 1 MiB
            QMessageBox::warning(this, tr("Arquivo grande"),
                tr("O arquivo excede 512 KB, o limite por arquivo deste servidor."));
            return;
        }
        m_net->ftUpload(currentChannel(), QFileInfo(path).fileName(), data);
    });

    connect(dn, &QPushButton::clicked, this, [this] {
        const int row = m_table->currentRow();
        if (row < 0) return;
        m_net->ftDownload(currentChannel(), m_table->item(row, 0)->text());
    });

    connect(del, &QPushButton::clicked, this, [this] {
        const int row = m_table->currentRow();
        if (row < 0) return;
        const QString name = m_table->item(row, 0)->text();
        if (QMessageBox::question(this, tr("Excluir arquivo"),
                tr("Excluir \\\"%1\\\" deste canal?").arg(name)) == QMessageBox::Yes)
            m_net->ftDelete(currentChannel(), name);
    });

    this->refresh();
}

int FileTransferDialog::currentChannel() const {
    return m_channels->currentData().toInt();
}

void FileTransferDialog::refresh() {
    m_table->setRowCount(0);
    if (m_channels->count() > 0) m_net->ftList(currentChannel());
}

#include "ChannelDialog.h"
#include "TsBanner.h"
#include "Icons.h"
#include "net/NetSession.h"

#include <QVBoxLayout>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QDialogButtonBox>
#include <QLabel>
#include <QPushButton>
#include <QTabWidget>
#include <QGroupBox>
#include <QListWidget>
#include <QTableWidget>
#include <QHeaderView>
#include <QPushButton>

ChannelDialog::ChannelDialog(const QString& title, const ServerData* server, NetSession* net, QWidget* parent)
    : QDialog(parent), m_server(server), m_net(net) {
    setWindowTitle(title);
    setMinimumWidth(440);

    QVBoxLayout* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 8);
    root->setSpacing(0);
    root->addWidget(new TsBanner(title, tr("Configure as propriedades do canal"),
                                 HIcons::channel(false, false, false, false).pixmap(24, 24), this));
    root->addSpacing(10);

    QTabWidget* tabs = new QTabWidget(this);
    QWidget* propPage = new QWidget(tabs);
    QVBoxLayout* propLayout = new QVBoxLayout(propPage);
    propLayout->setContentsMargins(4, 4, 4, 4);

    QFormLayout* form = new QFormLayout;
    form->setContentsMargins(12, 4, 12, 4);
    form->setSpacing(7);

    m_name = new QLineEdit(this);
    form->addRow(tr("Nome do canal:"), m_name);

    m_topic = new QLineEdit(this);
    m_topic->setMaxLength(80);
    form->addRow(tr("Tópico:"), m_topic);

    m_desc = new QTextEdit(this);
    m_desc->setAcceptRichText(false);
    m_desc->setMinimumHeight(120);
    m_desc->setMaximumHeight(160);
    form->addRow(tr("Descrição:"), m_desc);
    QLabel* descHint = new QLabel(
        tr("Aceita linhas em branco, [br], [img]URL[/img], "
           "[url=URL]texto[/url] e links Markdown."), this);
    descHint->setWordWrap(true);
    descHint->setObjectName(QStringLiteral("captionLabel"));
    form->addRow(QString(), descHint);

    m_password = new QLineEdit(this);
    m_password->setEchoMode(QLineEdit::Password);
    form->addRow(tr("Senha:"), m_password);

    m_codec = new QComboBox(this);
    m_codec->addItems(codecNames());
    m_codec->setCurrentIndex(4); // Opus Voice (padrão do Halla)
    form->addRow(tr("Codec:"), m_codec);

    QHBoxLayout* qrow = new QHBoxLayout;
    m_quality = new QSlider(Qt::Horizontal, this);
    m_quality->setRange(0, 10);
    m_quality->setValue(6);
    m_qualityLabel = new QLabel(QStringLiteral("6"), this);
    m_qualityLabel->setMinimumWidth(18);
    qrow->addWidget(m_quality, 1);
    qrow->addWidget(m_qualityLabel);
    QWidget* qw = new QWidget(this);
    qw->setLayout(qrow);
    form->addRow(tr("Qualidade do codec:"), qw);
    connect(m_quality, &QSlider::valueChanged, this,
            [this](int v) { m_qualityLabel->setText(QString::number(v)); });

    m_bitrate = new QSpinBox(this);
    m_bitrate->setRange(16, 384);
    m_bitrate->setValue(96);
    m_bitrate->setSuffix(tr(" kbps"));
    form->addRow(tr("Bitrate do codec:"), m_bitrate);

    m_sortAfter = new QComboBox(this);
    m_sortAfter->addItem(tr("- ordenado -"), 0);
    if (server) {
        for (const Channel& c : server->channels)
            if (c.parentId == 0) m_sortAfter->addItem(c.name, c.id);
    }
    form->addRow(tr("Classificar abaixo de:"), m_sortAfter);

    m_maxClients = new QSpinBox(this);
    m_maxClients->setRange(-1, 999);
    m_maxClients->setValue(-1);
    m_maxClients->setSpecialValueText(tr("ilimitado"));
    form->addRow(tr("Máx. de clientes:"), m_maxClients);

    // tipo do canal — radio buttons como no Halla
    QHBoxLayout* typerow = new QHBoxLayout;
    m_temp = new QRadioButton(tr("Temporário"), this);
    m_semi = new QRadioButton(tr("Semi-permanente"), this);
    m_perm = new QRadioButton(tr("Permanente"), this);
    m_perm->setChecked(true);
    typerow->addWidget(m_temp);
    typerow->addWidget(m_semi);
    typerow->addWidget(m_perm);
    typerow->addStretch(1);
    QWidget* tw = new QWidget(this);
    tw->setLayout(typerow);
    form->addRow(tr("Tipo do canal:"), tw);

    m_tempChannelParent = new QCheckBox(
        tr("Receber canais temporários como subcanais"), this);
    m_tempChannelParent->setToolTip(tr(
        "Quando qualquer usuário criar um canal temporário, o servidor o colocará automaticamente dentro deste canal."));
    // Não bloqueie esta opção usando o snapshot local de myPerms: chaves de
    // privilégio concedem poder individual no servidor sem trocar o cargo do
    // usuário. O servidor continua sendo a autoridade e rejeita alterações de
    // quem realmente não possui chanEdit.
    m_tempChannelParent->setEnabled(true);
    connect(m_temp, &QRadioButton::toggled, this,
            [this](bool temporary) {
                if (temporary) m_tempChannelParent->setChecked(false);
                m_tempChannelParent->setEnabled(!temporary);
            });
    form->addRow(QString(), m_tempChannelParent);

    m_default = new QCheckBox(tr("Canal padrão"), this);
    m_moderated = new QCheckBox(tr("Moderado (precisa de poder de fala)"), this);
    m_hideSymbol = new QCheckBox(tr("Ocultar símbolo do canal"), this);
    QHBoxLayout* chkrow = new QHBoxLayout;
    chkrow->addWidget(m_default);
    chkrow->addWidget(m_moderated);
    chkrow->addWidget(m_hideSymbol);
    chkrow->addStretch(1);
    QWidget* cw = new QWidget(this);
    cw->setLayout(chkrow);
    form->addRow(QString(), cw);

    propLayout->addLayout(form);
    propPage->setLayout(propLayout);
    tabs->addTab(propPage, tr("Propriedades"));

    // ---- Tab 2: Permissões — regras de acesso no estilo LCA do Halla
    QWidget* permPage = new QWidget(tabs);
    QHBoxLayout* pLayout = new QHBoxLayout(permPage);
    pLayout->setContentsMargins(8, 8, 8, 8);
    pLayout->setSpacing(8);

    // Coluna esquerda: grupos que possuem uma regra específica neste canal.
    QVBoxLayout* left = new QVBoxLayout;
    left->setSpacing(6);
    left->addWidget(new QLabel(tr("Grupos ativos"), permPage));
    m_lcaList = new QListWidget(permPage);
    m_lcaList->setMinimumWidth(250);
    left->addWidget(m_lcaList, 1);

    QHBoxLayout* lcaButtons = new QHBoxLayout;
    lcaButtons->addStretch(1);
    m_lcaAdd = new QPushButton(tr("Adicionar"), permPage);
    m_lcaDelete = new QPushButton(tr("Excluir"), permPage);
    lcaButtons->addWidget(m_lcaAdd);
    lcaButtons->addWidget(m_lcaDelete);
    left->addLayout(lcaButtons);

    QGroupBox* subject = new QGroupBox(tr("Grupo"), permPage);
    QFormLayout* subjectForm = new QFormLayout(subject);
    m_permGroupCombo = new QComboBox(subject);
    subjectForm->addRow(tr("Grupo"), m_permGroupCombo);
    left->addWidget(subject);
    pLayout->addLayout(left, 1);

    // Coluna direita: matriz Negar/Permitir para as permissões existentes.
    QVBoxLayout* right = new QVBoxLayout;
    right->setSpacing(6);
    right->addWidget(new QLabel(tr("Permissões"), permPage));
    m_permTable = new QTableWidget(0, 3, permPage);
    m_permTable->setHorizontalHeaderLabels({ QString(), tr("Negar"), tr("Permitir") });
    m_permTable->verticalHeader()->setVisible(false);
    m_permTable->setSelectionMode(QAbstractItemView::NoSelection);
    m_permTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_permTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_permTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_permTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);

    const QList<QPair<QString, QString>> permissions = {
        { QStringLiteral("view"), tr("Ver canal") },
        { QStringLiteral("join"), tr("Entrar no canal") },
        { QStringLiteral("talk"), tr("Falar no canal") },
        { QStringLiteral("whisper"), tr("Sussurrar neste canal") },
        { QStringLiteral("text_chat"), tr("Mensagem de texto") },
        { QStringLiteral("pluginData"), tr("Dados de complementos") },
        { QStringLiteral("file_upload"), tr("Enviar arquivos") },
        { QStringLiteral("file_download"), tr("Baixar arquivos") }
    };
    for (const auto& permission : permissions) {
        const int row = m_permTable->rowCount();
        m_permTable->insertRow(row);
        QTableWidgetItem* label = new QTableWidgetItem(permission.second);
        label->setData(Qt::UserRole, permission.first);
        m_permTable->setItem(row, 0, label);
        for (int col = 1; col <= 2; ++col) {
            QTableWidgetItem* check = new QTableWidgetItem;
            check->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
            check->setCheckState(Qt::Unchecked);
            m_permTable->setItem(row, col, check);
        }
    }
    right->addWidget(m_permTable, 1);
    pLayout->addLayout(right, 1);
    tabs->addTab(permPage, tr("Permissões"));

    if (m_net) {
        for (const QJsonValue& v : m_net->serverGroups()) {
            QJsonObject g = v.toObject();
            m_permGroupCombo->addItem(g["name"].toString(), g["id"].toInt());
        }
    } else {
        m_permGroupCombo->addItem(tr("guest"), 1);
        m_permGroupCombo->addItem(tr("normal"), 2);
        m_permGroupCombo->addItem(tr("admin"), 3);
    }

    connect(m_permGroupCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
        if (index < 0 || m_isUpdatingPerms) return;
        saveCurrentGroupPerms();
        loadGroupPerms(m_permGroupCombo->itemData(index).toInt());
    });
    connect(m_lcaList, &QListWidget::currentRowChanged, this, [this](int row) {
        if (row < 0 || m_isUpdatingPerms) return;
        saveCurrentGroupPerms();
        const int gid = m_lcaList->item(row)->data(Qt::UserRole).toInt();
        const int index = m_permGroupCombo->findData(gid);
        if (index >= 0) m_permGroupCombo->setCurrentIndex(index);
    });
    connect(m_permTable, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* item) {
        if (m_isUpdatingPerms || !item || item->column() < 1) return;
        m_isUpdatingPerms = true;
        const int otherColumn = item->column() == 1 ? 2 : 1;
        if (item->checkState() == Qt::Checked)
            m_permTable->item(item->row(), otherColumn)->setCheckState(Qt::Unchecked);
        m_isUpdatingPerms = false;
        saveCurrentGroupPerms();
    });
    connect(m_lcaAdd, &QPushButton::clicked, this, [this] {
        const int gid = selectedGroupId();
        if (gid < 0) return;
        if (!m_localGroupPerms.contains(QString::number(gid)))
            m_localGroupPerms[QString::number(gid)] = QJsonObject();
        rebuildLcaList();
        loadGroupPerms(gid);
    });
    connect(m_lcaDelete, &QPushButton::clicked, this, [this] {
        const int gid = selectedGroupId();
        if (gid < 0) return;
        m_localGroupPerms.remove(QString::number(gid));
        m_lastGid = -1;
        rebuildLcaList();
        loadGroupPerms(-1);
    });
    rebuildLcaList();
    if (m_permGroupCombo->count() > 0) loadGroupPerms(m_permGroupCombo->itemData(0).toInt());

    root->addWidget(tabs);

    QDialogButtonBox* bb = new QDialogButtonBox(QDialogButtonBox::Ok |
                                                QDialogButtonBox::Cancel, this);
    bb->button(QDialogButtonBox::Ok)->setText(tr("OK"));
    bb->button(QDialogButtonBox::Cancel)->setText(tr("Cancelar"));
    connect(bb, &QDialogButtonBox::accepted, this, [this] {
        if (m_name->text().trimmed().isEmpty()) {
            m_name->setFocus();
            m_name->setStyleSheet(QStringLiteral("border:1px solid #D9534F"));
            return;
        }
        accept();
    });
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    QHBoxLayout* brow = new QHBoxLayout;
    brow->setContentsMargins(12, 0, 12, 0);
    brow->addStretch(1);
    brow->addWidget(bb);
    root->addLayout(brow);
}

void ChannelDialog::setChannel(const Channel& c) {
    m_name->setText(c.name);
    m_topic->setText(c.topic);
    m_desc->setPlainText(c.description);
    m_password->setText(c.passwordHash);
    m_codec->setCurrentIndex(c.codec);
    m_quality->setValue(c.codecQuality);
    m_bitrate->setValue(c.bitrate > 0 ? qBound(16, c.bitrate, 384) : 96);
    m_maxClients->setValue(c.maxClients);
    m_temp->setChecked(c.type == 0);
    m_semi->setChecked(c.type == 1);
    m_perm->setChecked(c.type == 2);
    m_default->setChecked(c.isDefault);
    m_moderated->setChecked(c.moderated);
    m_hideSymbol->setChecked(c.noSymbol);
    m_tempChannelParent->setChecked(c.tempChannelParent && c.type != 0);
    const int idx = m_sortAfter->findData(c.id);
    if (idx >= 0) m_sortAfter->removeItem(idx); // não pode classificar abaixo de si mesmo

    m_localGroupPerms = c.groupPerms;
    m_lastGid = -1;
    rebuildLcaList();
    if (m_permGroupCombo->count() > 0) {
        m_permGroupCombo->setCurrentIndex(-1);
        m_permGroupCombo->setCurrentIndex(0);
    }
}

Channel ChannelDialog::resultChannel() const {
    Channel c;
    c.name = m_name->text(); // preserva espaços iniciais para canais decorativos
    c.topic = m_topic->text().trimmed();
    c.description = m_desc->toPlainText();
    c.passwordHash = m_password->text();
    c.hasPassword = !m_password->text().isEmpty();
    c.codec = m_codec->currentIndex();
    c.codecQuality = m_quality->value();
    c.bitrate = m_bitrate->value();
    c.maxClients = m_maxClients->value();
    c.type = m_temp->isChecked() ? 0 : (m_semi->isChecked() ? 1 : 2);
    c.isDefault = m_default->isChecked();
    c.moderated = m_moderated->isChecked();
    c.noSymbol = m_hideSymbol->isChecked();
    c.tempChannelParent = m_tempChannelParent->isChecked() && c.type != 0;
    
    const_cast<ChannelDialog*>(this)->saveCurrentGroupPerms();
    c.groupPerms = m_localGroupPerms;
    return c;
}

int ChannelDialog::selectedGroupId() const {
    if (!m_permGroupCombo || m_permGroupCombo->currentIndex() < 0) return -1;
    return m_permGroupCombo->currentData().toInt();
}

void ChannelDialog::rebuildLcaList() {
    if (!m_lcaList) return;
    m_isUpdatingPerms = true;
    const int current = selectedGroupId();
    m_lcaList->clear();
    for (auto it = m_localGroupPerms.constBegin(); it != m_localGroupPerms.constEnd(); ++it) {
        bool ok = false;
        const int gid = it.key().toInt(&ok);
        if (!ok) continue;
        const int comboIndex = m_permGroupCombo->findData(gid);
        const QString name = comboIndex >= 0
            ? m_permGroupCombo->itemText(comboIndex)
            : QStringLiteral("Grupo %1").arg(gid);
        QListWidgetItem* item = new QListWidgetItem(QStringLiteral("@%1").arg(name), m_lcaList);
        item->setData(Qt::UserRole, gid);
    }
    for (int i = 0; i < m_lcaList->count(); ++i) {
        if (m_lcaList->item(i)->data(Qt::UserRole).toInt() == current) {
            m_lcaList->setCurrentRow(i);
            break;
        }
    }
    m_isUpdatingPerms = false;
}

void ChannelDialog::saveCurrentGroupPerms() {
    if (m_lastGid < 0 || !m_permTable) return;

    QJsonObject gPerms;
    for (int row = 0; row < m_permTable->rowCount(); ++row) {
        const QString key = m_permTable->item(row, 0)->data(Qt::UserRole).toString();
        const bool deny = m_permTable->item(row, 1)->checkState() == Qt::Checked;
        const bool allow = m_permTable->item(row, 2)->checkState() == Qt::Checked;
        if (deny) gPerms[key] = false;
        else if (allow) gPerms[key] = true;
    }
    const QString gid = QString::number(m_lastGid);
    if (gPerms.isEmpty()) m_localGroupPerms.remove(gid);
    else m_localGroupPerms[gid] = gPerms;
}

void ChannelDialog::loadGroupPerms(int gid) {
    m_isUpdatingPerms = true;
    m_lastGid = gid;
    const QJsonObject gPerms = m_localGroupPerms.value(QString::number(gid)).toObject();
    if (m_permTable) {
        for (int row = 0; row < m_permTable->rowCount(); ++row) {
            const QString key = m_permTable->item(row, 0)->data(Qt::UserRole).toString();
            const QJsonValue value = gPerms.value(key);
            m_permTable->item(row, 1)->setCheckState(
                value.isBool() && !value.toBool() ? Qt::Checked : Qt::Unchecked);
            m_permTable->item(row, 2)->setCheckState(
                value.isBool() && value.toBool() ? Qt::Checked : Qt::Unchecked);
        }
    }
    m_isUpdatingPerms = false;
}

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

    m_default = new QCheckBox(tr("Canal padrão"), this);
    m_moderated = new QCheckBox(tr("Moderado (precisa de poder de fala)"), this);
    QHBoxLayout* chkrow = new QHBoxLayout;
    chkrow->addWidget(m_default);
    chkrow->addWidget(m_moderated);
    chkrow->addStretch(1);
    QWidget* cw = new QWidget(this);
    cw->setLayout(chkrow);
    form->addRow(QString(), cw);

    propLayout->addLayout(form);
    propPage->setLayout(propLayout);
    tabs->addTab(propPage, tr("Propriedades"));

    // ---- Tab 2: Permissões
    QWidget* permPage = new QWidget(tabs);
    QVBoxLayout* pLayout = new QVBoxLayout(permPage);
    pLayout->setContentsMargins(12, 12, 12, 12);
    pLayout->setSpacing(8);

    QHBoxLayout* gRow = new QHBoxLayout;
    gRow->addWidget(new QLabel(tr("Cargo/Grupo:"), permPage));
    m_permGroupCombo = new QComboBox(permPage);
    gRow->addWidget(m_permGroupCombo, 1);
    pLayout->addLayout(gRow);

    pLayout->addWidget(new QLabel(tr("Definir permissões específicas deste canal:"), permPage));

    m_chkJoin = new QCheckBox(tr("Permitir entrar no canal"), permPage);
    m_chkTalk = new QCheckBox(tr("Permitir falar no canal"), permPage);
    m_chkWhisper = new QCheckBox(tr("Permitir sussurrar neste canal"), permPage);
    m_chkUpload = new QCheckBox(tr("Permitir enviar arquivos no canal"), permPage);
    m_chkDownload = new QCheckBox(tr("Permitir baixar arquivos no canal"), permPage);
    m_chkChat = new QCheckBox(tr("Permitir enviar chat de texto no canal"), permPage);

    pLayout->addWidget(m_chkJoin);
    pLayout->addWidget(m_chkTalk);
    pLayout->addWidget(m_chkWhisper);
    pLayout->addWidget(m_chkUpload);
    pLayout->addWidget(m_chkDownload);
    pLayout->addWidget(m_chkChat);
    pLayout->addStretch(1);

    permPage->setLayout(pLayout);
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
        int gid = m_permGroupCombo->itemData(index).toInt();
        loadGroupPerms(gid);
    });

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
    const int idx = m_sortAfter->findData(c.id);
    if (idx >= 0) m_sortAfter->removeItem(idx); // não pode classificar abaixo de si mesmo

    m_localGroupPerms = c.groupPerms;
    m_lastGid = -1;
    if (m_permGroupCombo->count() > 0) {
        m_permGroupCombo->setCurrentIndex(-1);
        m_permGroupCombo->setCurrentIndex(0);
    }
}

Channel ChannelDialog::resultChannel() const {
    Channel c;
    c.name = m_name->text().trimmed();
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
    
    const_cast<ChannelDialog*>(this)->saveCurrentGroupPerms();
    c.groupPerms = m_localGroupPerms;
    return c;
}

void ChannelDialog::saveCurrentGroupPerms() {
    if (m_lastGid < 0) return;
    
    QJsonObject gPerms;
    gPerms["join"] = m_chkJoin->isChecked();
    gPerms["talk"] = m_chkTalk->isChecked();
    gPerms["whisper"] = m_chkWhisper->isChecked();
    gPerms["file_upload"] = m_chkUpload->isChecked();
    gPerms["file_download"] = m_chkDownload->isChecked();
    gPerms["text_chat"] = m_chkChat->isChecked();
    
    m_localGroupPerms[QString::number(m_lastGid)] = gPerms;
}

void ChannelDialog::loadGroupPerms(int gid) {
    m_isUpdatingPerms = true;
    m_lastGid = gid;
    
    QString gidStr = QString::number(gid);
    QJsonObject gPerms;
    if (m_localGroupPerms.contains(gidStr)) {
        gPerms = m_localGroupPerms[gidStr].toObject();
    }
    
    m_chkJoin->setChecked(gPerms.value("join").toBool(true));
    m_chkTalk->setChecked(gPerms.value("talk").toBool(true));
    m_chkWhisper->setChecked(gPerms.value("whisper").toBool(true));
    m_chkUpload->setChecked(gPerms.value("file_upload").toBool(true));
    m_chkDownload->setChecked(gPerms.value("file_download").toBool(true));
    m_chkChat->setChecked(gPerms.value("text_chat").toBool(true));
    
    m_isUpdatingPerms = false;
}

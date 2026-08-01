#include "ConnectDialog.h"
#include "Settings.h"
#include "AppLog.h"

#include <QVBoxLayout>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QIntValidator>
#include <QMessageBox>

ConnectDialog::ConnectDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Conectar"));
    setMinimumWidth(380);
    setSizeGripEnabled(false);

    QVBoxLayout* root = new QVBoxLayout(this);

    QLabel* intro = new QLabel(
        tr("Digite um endereço de servidor, apelido e, se necessário, a senha do servidor."),
        this);
    intro->setWordWrap(true);
    root->addWidget(intro);

    QFormLayout* form = new QFormLayout;
    form->setSpacing(7);

    m_nick = new QLineEdit(S::str("connect/nickname", tr("HallaUser")), this);
    form->addRow(tr("Apelido:"), m_nick);

    QHBoxLayout* hostRow = new QHBoxLayout;
    m_host = new QLineEdit(this);
    m_host->setPlaceholderText(tr("Endereço do servidor"));
    m_port = new QLineEdit(QStringLiteral("9987"), this);
    m_port->setValidator(new QIntValidator(1, 65535, this));
    m_port->setMaximumWidth(64);
    hostRow->addWidget(m_host, 1);
    QLabel* colon = new QLabel(QStringLiteral(":"), this);
    hostRow->addWidget(colon);
    hostRow->addWidget(m_port);
    QWidget* hw = new QWidget(this);
    hw->setLayout(hostRow);
    form->addRow(tr("Endereço do servidor:"), hw);

    m_password = new QLineEdit(this);
    m_password->setEchoMode(QLineEdit::Password);
    form->addRow(tr("Senha do servidor:"), m_password);

    root->addLayout(form);

    // --------- seção "Mais >>" ---------
    m_moreBtn = new QPushButton(tr("Mais >>"), this);
    m_moreBtn->setFlat(false);
    m_moreBtn->setMaximumWidth(90);
    root->addWidget(m_moreBtn, 0, Qt::AlignLeft);

    m_moreArea = new QWidget(this);
    QFormLayout* more = new QFormLayout(m_moreArea);
    more->setContentsMargins(0, 0, 0, 0);
    more->setSpacing(7);

    m_phonetic = new QLineEdit(m_moreArea);
    more->addRow(tr("Apelido fonético:"), m_phonetic);

    m_captureProfile = new QComboBox(m_moreArea);
    m_captureProfile->addItem(S::str("capture/profile", tr("Padrão")));
    m_captureProfile->setEditable(true);
    more->addRow(tr("Perfil de captura:"), m_captureProfile);

    m_playbackProfile = new QComboBox(m_moreArea);
    m_playbackProfile->addItem(S::str("playback/profile", tr("Padrão")));
    m_playbackProfile->setEditable(true);
    more->addRow(tr("Perfil de reprodução:"), m_playbackProfile);

    m_moreArea->setVisible(false);
    root->addWidget(m_moreArea);

    connect(m_moreBtn, &QPushButton::clicked, this, [this] {
        const bool show = !m_moreArea->isVisible();
        m_moreArea->setVisible(show);
        m_moreBtn->setText(show ? tr("Menos <<") : tr("Mais >>"));
        adjustSize();
    });

    root->addSpacing(8);

    // --------- botões ---------
    QHBoxLayout* brow = new QHBoxLayout;
    QPushButton* cancel = new QPushButton(tr("Cancelar"), this);
    QPushButton* connectBtn = new QPushButton(tr("Conectar"), this);
    connectBtn->setDefault(true);
    brow->addStretch(1);
    brow->addWidget(cancel);
    brow->addWidget(connectBtn);
    root->addLayout(brow);

    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(connectBtn, &QPushButton::clicked, this, [this] {
        if (m_host->text().trimmed().isEmpty()) {
            QMessageBox::warning(this, tr("Conectar"),
                                 tr("Digite o endereço do servidor."));
            m_host->setFocus();
            return;
        }
        if (m_nick->text().trimmed().isEmpty()) {
            QMessageBox::warning(this, tr("Conectar"), tr("Digite um apelido."));
            m_nick->setFocus();
            return;
        }
        accept();
    });
}

QString ConnectDialog::nickname() const { return m_nick->text().trimmed(); }
QString ConnectDialog::address() const  { return m_host->text().trimmed(); }
quint16 ConnectDialog::port() const     { return m_port->text().toUShort(); }
QString ConnectDialog::password() const { return m_password->text(); }
QString ConnectDialog::phoneticNickname() const { return m_phonetic->text(); }

void ConnectDialog::setNickname(const QString& n) { m_nick->setText(n); }
void ConnectDialog::setAddress(const QString& a, quint16 port) {
    m_host->setText(a);
    m_port->setText(QString::number(port));
}

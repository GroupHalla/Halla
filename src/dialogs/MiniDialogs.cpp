#include "MiniDialogs.h"
#include "TsBanner.h"
#include "Icons.h"

#include <QVBoxLayout>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QDialogButtonBox>

// ------------------------------------------------------------------ chave de privilégio
PrivilegeKeyDialog::PrivilegeKeyDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Usar chave de privilégio"));
    setMinimumWidth(360);
    QVBoxLayout* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 8);
    root->setSpacing(0);
    root->addWidget(new TsBanner(tr("Chave de privilégio"),
                                 tr("Digite a chave recebida do administrador"),
                                 HIcons::key().pixmap(24, 24), this));
    root->addSpacing(10);
    QFormLayout* form = new QFormLayout;
    form->setContentsMargins(12, 4, 12, 4);
    m_key = new QLineEdit(this);
    m_key->setPlaceholderText(tr("Ex.: AbCdEfGhIjKlMnOpQrStUvWxYz12"));
    form->addRow(tr("Chave:"), m_key);
    root->addLayout(form);

    QDialogButtonBox* bb = new QDialogButtonBox(QDialogButtonBox::Ok |
                                                QDialogButtonBox::Cancel, this);
    connect(bb, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    QHBoxLayout* brow = new QHBoxLayout;
    brow->setContentsMargins(12, 0, 12, 0);
    brow->addStretch(1);
    brow->addWidget(bb);
    root->addLayout(brow);
}

QString PrivilegeKeyDialog::key() const { return m_key->text().trimmed(); }

// ------------------------------------------------------------------ cutucar
PokeDialog::PokeDialog(const QString& target, QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Cutucar"));
    setMinimumWidth(340);
    QVBoxLayout* root = new QVBoxLayout(this);
    QLabel* l = new QLabel(tr("Enviar uma cutucada para <b>%1</b>:").arg(target.toHtmlEscaped()),
                           this);
    l->setWordWrap(true);
    root->addWidget(l);
    m_msg = new QLineEdit(tr("Ei!"), this);
    m_msg->setMaxLength(100);
    root->addWidget(m_msg);
    QDialogButtonBox* bb = new QDialogButtonBox(QDialogButtonBox::Ok |
                                                QDialogButtonBox::Cancel, this);
    connect(bb, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(bb);
}

QString PokeDialog::message() const { return m_msg->text(); }

// ------------------------------------------------------------------ expulsar / banir
KickBanDialog::KickBanDialog(const QString& target, Mode mode, QWidget* parent)
    : QDialog(parent) {
    QString title = mode == Ban ? tr("Banir cliente")
                    : (mode == KickServer ? tr("Expulsar do servidor")
                                          : tr("Expulsar do canal"));
    setWindowTitle(title);
    setMinimumWidth(380);

    QVBoxLayout* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 8);
    root->setSpacing(0);
    root->addWidget(new TsBanner(title, target, QPixmap(), this));
    root->addSpacing(10);

    QFormLayout* form = new QFormLayout;
    form->setContentsMargins(12, 4, 12, 4);
    m_reason = new QTextEdit(this);
    m_reason->setAcceptRichText(false);
    m_reason->setMinimumHeight(60);
    m_reason->setPlaceholderText(tr("Motivo (opcional)"));
    form->addRow(tr("Motivo:"), m_reason);
    if (mode == Ban) {
        m_duration = new QSpinBox(this);
        m_duration->setRange(0, 525600);
        m_duration->setValue(0);
        m_duration->setSpecialValueText(tr("permanente"));
        m_duration->setSuffix(QStringLiteral(" min"));
        form->addRow(tr("Duração do banimento:"), m_duration);
    }
    root->addLayout(form);

    QDialogButtonBox* bb = new QDialogButtonBox(QDialogButtonBox::Ok |
                                                QDialogButtonBox::Cancel, this);
    connect(bb, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    QHBoxLayout* brow = new QHBoxLayout;
    brow->setContentsMargins(12, 0, 12, 0);
    brow->addStretch(1);
    brow->addWidget(bb);
    root->addLayout(brow);
}

QString KickBanDialog::reason() const { return m_reason->toPlainText(); }
int KickBanDialog::banMinutes() const { return m_duration ? m_duration->value() : 0; }

// ------------------------------------------------------------------ volume
VolumeDialog::VolumeDialog(const QString& target, int currentDb, QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(tr("Volume"));
    setMinimumWidth(340);
    QVBoxLayout* root = new QVBoxLayout(this);
    root->addWidget(new QLabel(tr("Volume de reprodução de <b>%1</b>:")
                                   .arg(target.toHtmlEscaped()), this));

    QHBoxLayout* row = new QHBoxLayout;
    m_slider = new QSlider(Qt::Horizontal, this);
    m_slider->setRange(-40, 12);
    m_slider->setValue(currentDb);
    m_label = new QLabel(QStringLiteral("%1 dB").arg(currentDb), this);
    row->addWidget(m_slider, 1);
    row->addWidget(m_label);
    root->addLayout(row);

    connect(m_slider, &QSlider::valueChanged, this,
            [this](int v) { m_label->setText(QStringLiteral("%1 dB").arg(v)); });

    QDialogButtonBox* bb = new QDialogButtonBox(QDialogButtonBox::Ok |
                                                QDialogButtonBox::Cancel, this);
    connect(bb, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(bb);
}

int VolumeDialog::volume() const { return m_slider->value(); }

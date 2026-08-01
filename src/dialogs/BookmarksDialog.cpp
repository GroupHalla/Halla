#include "BookmarksDialog.h"
#include "IdentityDialog.h"
#include "TsBanner.h"
#include "Icons.h"
#include "Settings.h"
#include "AppLog.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QHeaderView>
#include <QPushButton>
#include <QSplitter>
#include <QLabel>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>

QList<Bookmark> BookmarksDialog::loadAll() {
    QList<Bookmark> out;
    QJsonDocument doc = QJsonDocument::fromJson(S::str("bookmarks").toUtf8());
    if (!doc.isArray()) return out;
    for (const QJsonValue& v : doc.array()) {
        QJsonObject o = v.toObject();
        Bookmark b;
        b.label = o["label"].toString();
        b.address = o["addr"].toString();
        b.port = static_cast<quint16>(o["port"].toInt(9987));
        b.nickname = o["nick"].toString();
        b.password = o["pass"].toString();
        b.autoconnect = o["auto"].toBool();
        out << b;
    }
    return out;
}

void BookmarksDialog::saveAll(const QList<Bookmark>& list) {
    QJsonArray arr;
    for (const Bookmark& b : list) {
        QJsonObject o;
        o["label"] = b.label;
        o["addr"] = b.address;
        o["port"] = static_cast<int>(b.port);
        o["nick"] = b.nickname;
        o["pass"] = b.password;
        o["auto"] = b.autoconnect;
        arr << o;
    }
    S::set("bookmarks", QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)));
}

BookmarksDialog::BookmarksDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Gerenciar favoritos"));
    resize(640, 380);

    QVBoxLayout* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 8);
    root->setSpacing(0);
    root->addWidget(new TsBanner(tr("Gerenciar favoritos"),
                                 tr("Adicione, edite e conecte-se aos seus servidores favoritos"),
                                 HIcons::bookmarkStar().pixmap(24, 24), this));
    root->addSpacing(8);

    QSplitter* split = new QSplitter(this);

    // esquerda: tabela
    QWidget* left = new QWidget(split);
    QVBoxLayout* llay = new QVBoxLayout(left);
    llay->setContentsMargins(8, 0, 4, 0);
    m_table = new QTableWidget(0, 3, left);
    m_table->setHorizontalHeaderLabels({ tr("Favorito"), tr("Endereço"), tr("Auto") });
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    llay->addWidget(m_table, 1);
    QHBoxLayout* lbtns = new QHBoxLayout;
    QPushButton* add = new QPushButton(tr("Novo"), left);
    QPushButton* del = new QPushButton(tr("Excluir"), left);
    lbtns->addWidget(add);
    lbtns->addWidget(del);
    lbtns->addStretch(1);
    llay->addLayout(lbtns);

    // direita: formulário
    QWidget* right = new QWidget(split);
    QVBoxLayout* rlay = new QVBoxLayout(right);
    rlay->setContentsMargins(4, 0, 10, 0);
    QFormLayout* form = new QFormLayout;
    form->setSpacing(6);
    m_label = new QLineEdit(right);
    form->addRow(tr("Rótulo:"), m_label);
    m_address = new QLineEdit(right);
    form->addRow(tr("Endereço:"), m_address);
    m_port = new QLineEdit(QStringLiteral("9987"), right);
    form->addRow(tr("Porta:"), m_port);
    m_nickname = new QLineEdit(right);
    form->addRow(tr("Apelido:"), m_nickname);
    m_password = new QLineEdit(right);
    m_password->setEchoMode(QLineEdit::Password);
    form->addRow(tr("Senha do servidor:"), m_password);
    m_autoconnect = new QCheckBox(tr("Conectar automaticamente ao iniciar"), right);
    form->addRow(QString(), m_autoconnect);
    rlay->addLayout(form);
    rlay->addStretch(1);

    split->addWidget(left);
    split->addWidget(right);
    split->setStretchFactor(0, 2);
    split->setStretchFactor(1, 1);
    root->addWidget(split, 1);

    QHBoxLayout* bottom = new QHBoxLayout;
    bottom->setContentsMargins(10, 6, 10, 0);
    QPushButton* connectBtn = new QPushButton(tr("Conectar"), this);
    QPushButton* closeBtn = new QPushButton(tr("Fechar"), this);
    bottom->addStretch(1);
    bottom->addWidget(connectBtn);
    bottom->addWidget(closeBtn);
    root->addLayout(bottom);

    // ---- lógica
    connect(add, &QPushButton::clicked, this, [this] {
        storeForm();
        Bookmark b;
        b.label = tr("Novo favorito");
        b.nickname = IdentityDialog::defaultNickname();
        m_list << b;
        saveAll(m_list);
        reload();
        m_table->selectRow(m_list.size() - 1);
        m_label->setFocus();
        m_label->selectAll();
    });

    connect(del, &QPushButton::clicked, this, [this] {
        int r = currentRow();
        if (r < 0) return;
        m_list.removeAt(r);
        saveAll(m_list);
        reload();
        emit changed();
    });

    connect(m_table, &QTableWidget::currentCellChanged, this,
            [this](int row, int, int prev, int) {
                if (prev >= 0 && prev < m_list.size()) storeForm();
                if (row >= 0 && row < m_list.size()) {
                    const Bookmark& b = m_list[row];
                    m_label->setText(b.label);
                    m_address->setText(b.address);
                    m_port->setText(QString::number(b.port));
                    m_nickname->setText(b.nickname);
                    m_password->setText(b.password);
                    m_autoconnect->setChecked(b.autoconnect);
                }
            });

    auto applyForm = [this] { storeForm(); reload(); emit changed(); };
    for (QLineEdit* le : { m_label, m_address, m_port, m_nickname, m_password })
        connect(le, &QLineEdit::editingFinished, this, applyForm);
    connect(m_autoconnect, &QCheckBox::toggled, this,
            [this, applyForm](bool) { applyForm(); });

    connect(connectBtn, &QPushButton::clicked, this, [this] {
        int r = currentRow();
        if (r < 0 || m_list[r].address.trimmed().isEmpty()) return;
        emit connectRequested(m_list[r].address.trimmed(), m_list[r].port,
                              m_list[r].nickname, m_list[r].password);
        accept();
    });

    connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);

    m_list = loadAll();
    reload();
    if (!m_list.isEmpty()) m_table->selectRow(0);
}

int BookmarksDialog::currentRow() const {
    auto items = m_table->selectedItems();
    return items.isEmpty() ? -1 : items.first()->row();
}

void BookmarksDialog::reload() {
    m_table->blockSignals(true);
    m_table->setRowCount(m_list.size());
    for (int i = 0; i < m_list.size(); ++i) {
        const Bookmark& b = m_list[i];
        m_table->setItem(i, 0, new QTableWidgetItem(b.label));
        m_table->setItem(i, 1, new QTableWidgetItem(
            b.port == 9987 ? b.address : b.address + ":" + QString::number(b.port)));
        m_table->setItem(i, 2, new QTableWidgetItem(b.autoconnect ? tr("Sim") : QString()));
    }
    m_table->blockSignals(false);
}

void BookmarksDialog::storeForm() {
    int r = currentRow();
    if (r < 0 || r >= m_list.size()) return;
    Bookmark& b = m_list[r];
    b.label = m_label->text().trimmed();
    b.address = m_address->text().trimmed();
    b.port = static_cast<quint16>(m_port->text().toUShort());
    b.nickname = m_nickname->text().trimmed();
    b.password = m_password->text();
    b.autoconnect = m_autoconnect->isChecked();
    saveAll(m_list);
}

void BookmarksDialog::prefill(const QString& label, const QString& address, quint16 port,
                              const QString& nickname) {
    Bookmark b;
    b.label = label;
    b.address = address;
    b.port = port;
    b.nickname = nickname;
    m_list << b;
    saveAll(m_list);
    reload();
    m_table->selectRow(m_list.size() - 1);
    m_label->setFocus();
    m_label->selectAll();
}

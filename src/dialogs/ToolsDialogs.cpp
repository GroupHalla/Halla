#include "ToolsDialogs.h"
#include "TsBanner.h"
#include "Icons.h"
#include "Settings.h"
#include "AppLog.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QPushButton>
#include <QLabel>
#include <QHeaderView>
#include <QInputDialog>
#include <QKeySequenceEdit>
#include <QComboBox>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>

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
WhisperDialog::WhisperDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Listas de sussurro"));
    resize(460, 300);

    QVBoxLayout* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 8);
    root->setSpacing(0);
    root->addWidget(new TsBanner(tr("Listas de sussurro"),
                                 tr("Configure teclas e destinos para sussurrar"),
                                 HIcons::captureMic().pixmap(24, 24), this));
    root->addSpacing(8);

    QHBoxLayout* mid = new QHBoxLayout;
    mid->setContentsMargins(10, 0, 10, 0);
    m_table = new QTableWidget(0, 2, this);
    m_table->setHorizontalHeaderLabels({ tr("Nome"), tr("Tecla") });
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
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
        QDialog d(this);
        d.setWindowTitle(tr("Nova lista de sussurro"));
        QFormLayout* f = new QFormLayout(&d);
        QLineEdit* name = new QLineEdit(&d);
        QKeySequenceEdit* key = new QKeySequenceEdit(&d);
        QComboBox* target = new QComboBox(&d);
        target->addItems({ tr("Canal atual"), tr("Todos os canais") });
        f->addRow(tr("Nome:"), name);
        f->addRow(tr("Tecla:"), key);
        f->addRow(tr("Destino:"), target);
        QHBoxLayout* rb = new QHBoxLayout;
        QPushButton* ok = new QPushButton(tr("OK"), &d);
        QPushButton* cancel = new QPushButton(tr("Cancelar"), &d);
        rb->addStretch(1);
        rb->addWidget(ok);
        rb->addWidget(cancel);
        f->addRow(rb);
        QObject::connect(ok, &QPushButton::clicked, &d, &QDialog::accept);
        QObject::connect(cancel, &QPushButton::clicked, &d, &QDialog::reject);
        if (d.exec() != QDialog::Accepted || name->text().trimmed().isEmpty()) return;
        QList<QJsonObject> list = loadList("whispers");
        QJsonObject o;
        o["name"] = name->text().trimmed();
        o["key"] = key->keySequence().toString();
        o["target"] = target->currentIndex();
        list << o;
        saveList("whispers", list);
        reload();
    });

    connect(del, &QPushButton::clicked, this, [this] {
        auto items = m_table->selectedItems();
        if (items.isEmpty()) return;
        QList<QJsonObject> list = loadList("whispers");
        list.removeAt(items.first()->row());
        saveList("whispers", list);
        reload();
    });

    reload();
}

void WhisperDialog::reload() {
    QList<QJsonObject> list = loadList("whispers");
    m_table->setRowCount(list.size());
    for (int i = 0; i < list.size(); ++i) {
        m_table->setItem(i, 0, new QTableWidgetItem(list[i]["name"].toString()));
        m_table->setItem(i, 1, new QTableWidgetItem(list[i]["key"].toString()));
    }
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
FileTransferDialog::FileTransferDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Transferência de arquivos"));
    resize(560, 300);

    QVBoxLayout* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 8);
    root->setSpacing(0);
    root->addWidget(new TsBanner(tr("Transferência de arquivos"),
                                 tr("Uploads e downloads em andamento"),
                                 HIcons::transfer().pixmap(24, 24), this));
    root->addSpacing(8);

    QHBoxLayout* mid = new QHBoxLayout;
    mid->setContentsMargins(10, 0, 10, 0);
    m_table = new QTableWidget(0, 5, this);
    m_table->setHorizontalHeaderLabels({ tr("Arquivo"), tr("Tamanho"), tr("Progresso"),
                                         tr("Velocidade"), tr("Estado") });
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    mid->addWidget(m_table, 1);
    root->addLayout(mid);

    QLabel* note = new QLabel(tr("Nenhuma transferência ativa. Envie arquivos pelo menu de "
                                 "contexto do canal ao conectar-se a um servidor."), this);
    note->setStyleSheet(QStringLiteral("color:#888888"));
    note->setWordWrap(true);
    note->setContentsMargins(10, 4, 10, 0);
    root->addWidget(note);

    QHBoxLayout* btns = new QHBoxLayout;
    btns->setContentsMargins(10, 6, 10, 0);
    btns->addStretch(1);
    QPushButton* close = new QPushButton(tr("Fechar"), this);
    btns->addWidget(close);
    root->addLayout(btns);
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
}

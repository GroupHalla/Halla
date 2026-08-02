#include "ToolsDialogs.h"
#include "TsBanner.h"
#include "Icons.h"
#include "Settings.h"
#include "AppLog.h"
#include "net/NetSession.h"
#include "core/Models.h"

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
    resize(560, 320);

    QVBoxLayout* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 8);
    root->setSpacing(0);
    root->addWidget(new TsBanner(tr("Listas de sussurro"),
                                 tr("Escolha para quem sua voz vai ao sussurrar"),
                                 HIcons::captureMic().pixmap(24, 24), this));
    root->addSpacing(8);

    QHBoxLayout* mid = new QHBoxLayout;
    mid->setContentsMargins(10, 0, 10, 0);
    m_table = new QTableWidget(0, 2, this);
    m_table->setHorizontalHeaderLabels({ tr("Nome"), tr("Destinatários") });
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
    QPushButton* use = new QPushButton(tr("Usar esta lista"), this);
    QPushButton* close = new QPushButton(tr("Fechar"), this);
    btns->addWidget(add);
    btns->addWidget(del);
    btns->addStretch(1);
    btns->addWidget(use);
    btns->addWidget(close);
    root->addLayout(btns);

    connect(close, &QPushButton::clicked, this, &QDialog::accept);

    connect(add, &QPushButton::clicked, this, [this] {
        QDialog d(this);
        d.setWindowTitle(tr("Nova lista de sussurro"));
        QVBoxLayout* dv = new QVBoxLayout(&d);
        QFormLayout* f = new QFormLayout;
        QLineEdit* name = new QLineEdit(&d);
        f->addRow(tr("Nome:"), name);
        dv->addLayout(f);
        dv->addWidget(new QLabel(
            m_data ? tr("Marque os destinatários (identificados pelo ID único):")
                   : tr("Conecte-se a um servidor para escolher destinatários "
                        "na lista abaixo."), &d));
        QList<QPair<QCheckBox*, QString>> picks;
        if (m_data) {
            for (const User& u : m_data->users) {
                if (u.id == m_data->selfId) continue;
                QCheckBox* cb = new QCheckBox(u.name, &d);
                picks << qMakePair(cb, u.uniqueId);
                dv->addWidget(cb);
            }
        }
        QLineEdit* manual = new QLineEdit(&d);
        manual->setPlaceholderText(tr("IDs únicos extras, separados por vírgula"));
        dv->addWidget(manual);
        QHBoxLayout* rb = new QHBoxLayout;
        QPushButton* ok = new QPushButton(tr("OK"), &d);
        QPushButton* cancel = new QPushButton(tr("Cancelar"), &d);
        rb->addStretch(1);
        rb->addWidget(ok);
        rb->addWidget(cancel);
        dv->addLayout(rb);
        QObject::connect(ok, &QPushButton::clicked, &d, &QDialog::accept);
        QObject::connect(cancel, &QPushButton::clicked, &d, &QDialog::reject);
        if (d.exec() != QDialog::Accepted || name->text().trimmed().isEmpty()) return;

        QJsonArray uids;
        QStringList names;
        for (const auto& pr : picks)
            if (pr.first->isChecked()) { uids << pr.second; names << pr.first->text(); }
        for (const QString& extra : manual->text().split(QLatin1Char(','),
                                                         Qt::SkipEmptyParts)) {
            const QString e = extra.trimmed();
            if (!e.isEmpty()) uids << e;
        }
        QList<QJsonObject> list = loadList("whispers");
        QJsonObject o;
        o["name"] = name->text().trimmed();
        o["uids"] = uids;
        o["targetNames"] = names.join(QStringLiteral(", "));
        list << o;
        saveList("whispers", list);
        reload();
    });

    connect(del, &QPushButton::clicked, this, [this] {
        auto items = m_table->selectedItems();
        if (items.isEmpty()) return;
        QList<QJsonObject> list = loadList("whispers");
        const QString name = list.value(items.first()->row())["name"].toString();
        list.removeAt(items.first()->row());
        saveList("whispers", list);
        if (S::str("whisper/activeList") == name) S::set("whisper/activeList", QString());
        reload();
    });

    connect(use, &QPushButton::clicked, this, [this] {
        auto items = m_table->selectedItems();
        if (items.isEmpty()) return;
        const QString name = loadList("whispers").value(items.first()->row())
                                                   ["name"].toString();
        S::set("whisper/activeList", name);
        reload();
    });

    reload();
}

void WhisperDialog::reload() {
    QList<QJsonObject> list = loadList("whispers");
    const QString active = S::str("whisper/activeList");
    m_table->setRowCount(list.size());
    for (int i = 0; i < list.size(); ++i) {
        const QString name = list[i]["name"].toString();
        QTableWidgetItem* n = new QTableWidgetItem(
            name == active ? QStringLiteral("%1  (ativa)").arg(name) : name);
        m_table->setItem(i, 0, n);
        QString targets = list[i]["targetNames"].toString();
        if (targets.isEmpty())
            targets = QString::number(list[i]["uids"].toArray().size()) + tr(" destino(s)");
        m_table->setItem(i, 1, new QTableWidgetItem(targets));
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

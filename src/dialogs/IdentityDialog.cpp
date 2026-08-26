#include "IdentityDialog.h"
#include "TsBanner.h"
#include "Icons.h"
#include "Settings.h"
#include "core/SecureStore.h"
#include "AppLog.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QToolButton>
#include <QHeaderView>
#include <QInputDialog>
#include <QMessageBox>
#include <QPushButton>
#include <QLabel>
#include <QClipboard>
#include <QGuiApplication>
#include <QRandomGenerator>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QCryptographicHash>
#include <openssl/evp.h>
#include <openssl/x509.h>
#include <openssl/pem.h>

static QString keyBase(const QString& uid, const QString& field) {
    return QStringLiteral("identityKeys/%1/%2").arg(uid, field);
}

static QString storeIdentityKey(EVP_PKEY* key) {
    // Chave pública em SPKI DER (i2d_PUBKEY) — suportado por OpenSSL e
    // BoringSSL para todos os tipos de chave.
    const int pubLen = i2d_PUBKEY(key, nullptr);
    if (pubLen <= 0) return QString();
    // Chave privada NO FORMATO CRU (seed Ed25519 de 32 bytes). O
    // i2d_PrivateKey clássico só serializa RSA/EC/DSA: no BoringSSL
    // embutido no SDK WebRTC (que o build Windows linka no lugar do
    // OpenSSL) ele devolve -1 para Ed25519 — toda identidade nascia com
    // ID único vazio e todo login caía em bad_identity. O formato cru
    // existe e funciona igual nos dois (EVP_PKEY_get_raw_private_key),
    // e a seed basta para reconstruir a chave ao assinar.
    QByteArray priv(64, 0);
    size_t rawLen = priv.size();
    if (EVP_PKEY_get_raw_private_key(key, reinterpret_cast<unsigned char*>(priv.data()), &rawLen) != 1
        || rawLen == 0 || rawLen > 64)
        return QString();
    priv.resize(int(rawLen));
    QByteArray pub(pubLen, 0);
    unsigned char* p = reinterpret_cast<unsigned char*>(pub.data());
    i2d_PUBKEY(key, &p);
    const QString uid = QString::fromLatin1(QCryptographicHash::hash(pub, QCryptographicHash::Sha256).toBase64());
    QString secureError;
    if (!SecureStore::write(keyBase(uid, QStringLiteral("privateDer")), priv, &secureError)) {
        // Sem cofre do sistema (keyring ausente/travado, Credential Manager
        // bloqueado por política etc.) a identidade ainda assim precisa ser
        // criável: cai no armazenamento local — o mesmo local legado que as
        // instalações antigas usavam. signNonce() lê o SecureStore primeiro
        // e migra de volta para o cofre assim que ele volta a funcionar.
        AppLog::error(QObject::tr(
                          "Não foi possível salvar a identidade no cofre do sistema: %1 — "
                          "a chave privada será guardada apenas no perfil local (menos seguro).")
                          .arg(secureError));
        S::set(keyBase(uid, QStringLiteral("privateDer")), QString::fromLatin1(priv.toBase64()));
    } else {
        // Cofre ok: remove material legado em texto simples.
        S::store().remove(keyBase(uid, QStringLiteral("privateDer")));
    }
    S::set(keyBase(uid, QStringLiteral("publicDer")), QString::fromLatin1(pub.toBase64()));
    S::store().sync();
    return uid;
}

QString IdentityDialog::generateUniqueId() {
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr);
    EVP_PKEY* key = nullptr;
    QString uid;
    if (ctx && EVP_PKEY_keygen_init(ctx) == 1 && EVP_PKEY_keygen(ctx, &key) == 1)
        uid = storeIdentityKey(key);
    EVP_PKEY_free(key);
    EVP_PKEY_CTX_free(ctx);
    return uid;
}

QByteArray IdentityDialog::publicKeyForUid(const QString& uid) {
    return QByteArray::fromBase64(S::str(keyBase(uid, QStringLiteral("publicDer"))).toLatin1());
}

QByteArray IdentityDialog::signNonce(const QString& uid, const QByteArray& nonce) {
    const QString privateKeyName = keyBase(uid, QStringLiteral("privateDer"));
    QByteArray priv = SecureStore::read(privateKeyName);
    if (priv.isEmpty()) {
        // Migração de instalações legadas (PKCS#8 em QSettings) e de
        // identidades gravadas pelo fallback de storeIdentityKey() quando o
        // cofre do sistema estava indisponível: usa o material local MESMO
        // QUE a re-gravação no cofre falhe de novo. Sem isso, em máquinas
        // com o Credential Manager bloqueado, a identidade era criada pelo
        // fallback mas o desafio de login falhava — a chave estava no
        // perfil local e o signNonce se recusava a usá-la.
        const QByteArray legacy = QByteArray::fromBase64(S::str(privateKeyName).toLatin1());
        if (!legacy.isEmpty()) {
            priv = legacy;
            if (SecureStore::write(privateKeyName, legacy)) {
                S::store().remove(privateKeyName);
                S::store().sync();
            }
        }
    }
    if (priv.isEmpty() || nonce.isEmpty()) return QByteArray();
    const unsigned char* p = reinterpret_cast<const unsigned char*>(priv.constData());
    EVP_PKEY* key = d2i_AutoPrivateKey(nullptr, &p, priv.size());
    if (!key) {
        // Formato cru (seed Ed25519 de 32 bytes) gravado por
        // storeIdentityKey(): no BoringSSL do SDK WebRTC o i2d_PrivateKey
        // não suporta Ed25519, então a chave é persistida como seed crua.
        // EVP_PKEY_new_raw_private_key existe em OpenSSL e BoringSSL.
        if (priv.size() == 32)
            key = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr,
                                               reinterpret_cast<const unsigned char*>(priv.constData()), 32);
        if (!key) return QByteArray();
    }
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    QByteArray sig(64, 0);
    size_t sigLen = sig.size();
    bool ok = ctx && EVP_DigestSignInit(ctx, nullptr, nullptr, nullptr, key) == 1
           && EVP_DigestSign(ctx, reinterpret_cast<unsigned char*>(sig.data()), &sigLen,
                             reinterpret_cast<const unsigned char*>(nonce.constData()), nonce.size()) == 1;
    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(key);
    if (!ok) return QByteArray();
    sig.resize(int(sigLen));
    return sig;
}

QList<QStringList> IdentityDialog::loadAll() {
    QList<QStringList> rows;
    QJsonDocument doc = QJsonDocument::fromJson(S::str("identities").toUtf8());
    bool migrated = false;
    if (doc.isArray()) {
        for (const QJsonValue& v : doc.array()) {
            QJsonObject o = v.toObject();
            QString uid = o["uid"].toString();
            if (uid.isEmpty() || publicKeyForUid(uid).isEmpty()) { uid = generateUniqueId(); migrated = true; }
            rows << QStringList{ o["def"].toBool() ? "1" : "0",
                                 o["nick"].toString(), o["phon"].toString(),
                                 uid };
        }
    }
    if (migrated && !rows.isEmpty()) saveAll(rows);
    if (rows.isEmpty()) {
        // identidade inicial: gerar UMA vez e persistir — o ID único precisa
        // ser estável entre execuções (o servidor o usa para bans, grupos e
        // chaves de privilégio)
        rows << QStringList{ "1", tr("HallaUser"), QString(), generateUniqueId() };
        saveAll(rows);
    }
    return rows;
}

void IdentityDialog::saveAll(const QList<QStringList>& rows) {
    QJsonArray arr;
    for (const QStringList& r : rows) {
        QJsonObject o;
        o["def"] = r.value(0) == "1";
        o["nick"] = r.value(1);
        o["phon"] = r.value(2);
        o["uid"] = r.value(3);
        arr << o;
    }
    S::set("identities", QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)));
}

QString IdentityDialog::defaultNickname() {
    for (const QStringList& r : loadAll())
        if (r.value(0) == "1") return r.value(1);
    return tr("HallaUser");
}

IdentityDialog::IdentityDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Identidades"));
    resize(560, 340);

    QVBoxLayout* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 8);
    root->setSpacing(0);
    root->addWidget(new TsBanner(tr("Identidades"),
                                 tr("Gerencie suas identidades (chaves exclusivas)"),
                                 HIcons::identity().pixmap(24, 24), this));
    root->addSpacing(8);

    QHBoxLayout* mid = new QHBoxLayout;
    mid->setContentsMargins(10, 0, 10, 0);

    m_table = new QTableWidget(0, 3, this);
    m_table->setHorizontalHeaderLabels({ tr("Apelido"), tr("Apelido fonético"),
                                         tr("ID único") });
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    mid->addWidget(m_table, 1);

    QVBoxLayout* btns = new QVBoxLayout;
    btns->setSpacing(4);
    auto mkBtn = [&](const QString& text, const QString& tip = QString()) {
        QPushButton* b = new QPushButton(text, this);
        b->setToolTip(tip);
        b->setMinimumWidth(130);
        btns->addWidget(b);
        return b;
    };
    QPushButton* add = mkBtn(tr("Adicionar"));
    QPushButton* ren = mkBtn(tr("Renomear"));
    QPushButton* del = mkBtn(tr("Excluir"));
    QPushButton* def = mkBtn(tr("Definir como padrão"));
    QPushButton* copy = mkBtn(tr("Copiar ID único"));
    btns->addStretch(1);
    mid->addLayout(btns);
    root->addLayout(mid, 1);

    QHBoxLayout* bottom = new QHBoxLayout;
    bottom->setContentsMargins(10, 6, 10, 0);
    QLabel* note = new QLabel(tr("O ID único identifica você perante os servidores. "
                                 "Guarde-o com segurança."), this);
    note->setStyleSheet(QStringLiteral("color:#666666"));
    bottom->addWidget(note, 1);
    QPushButton* close = new QPushButton(tr("Fechar"), this);
    bottom->addWidget(close);
    root->addLayout(bottom);

    connect(close, &QPushButton::clicked, this, &QDialog::accept);

    connect(add, &QPushButton::clicked, this, [this] {
        bool ok = false;
        QString name = QInputDialog::getText(this, tr("Nova identidade"),
                                             tr("Apelido:"), QLineEdit::Normal,
                                             QString(), &ok);
        if (!ok || name.trimmed().isEmpty()) return;
        const QString uid = generateUniqueId();
        if (uid.isEmpty() || publicKeyForUid(uid).isEmpty()) {
            QMessageBox::critical(
                this, tr("Identidades"),
                tr("Não foi possível criar a identidade: a chave Ed25519 não pôde ser "
                   "gerada ou salva.\n\nVerifique o cofre de senhas do sistema "
                   "(Keychain, Keyring ou Credential Manager) e tente novamente."));
            return;
        }
        QList<QStringList> rows = loadAll();
        rows << QStringList{ "0", name.trimmed(), QString(), uid };
        saveAll(rows);
        reload();
        AppLog::info(tr("Identidade \"%1\" criada").arg(name.trimmed()));
    });

    connect(ren, &QPushButton::clicked, this, [this] {
        int r = selectedRow();
        if (r < 0) return;
        QList<QStringList> rows = loadAll();
        bool ok = false;
        QString name = QInputDialog::getText(this, tr("Renomear identidade"),
                                             tr("Apelido:"), QLineEdit::Normal,
                                             rows[r].value(1), &ok);
        if (!ok || name.trimmed().isEmpty()) return;
        rows[r][1] = name.trimmed();
        saveAll(rows);
        reload();
    });

    connect(del, &QPushButton::clicked, this, [this] {
        int r = selectedRow();
        if (r < 0) return;
        QList<QStringList> rows = loadAll();
        if (rows.size() <= 1) {
            QMessageBox::warning(this, tr("Identidades"),
                                 tr("É necessário manter ao menos uma identidade."));
            return;
        }
        if (QMessageBox::question(this, tr("Identidades"),
                                  tr("Excluir a identidade \"%1\"?").arg(rows[r].value(1)))
            != QMessageBox::Yes)
            return;
        rows.removeAt(r);
        if (!std::any_of(rows.begin(), rows.end(),
                         [](const QStringList& x) { return x.value(0) == "1"; }))
            rows.first()[0] = "1";
        saveAll(rows);
        reload();
    });

    connect(def, &QPushButton::clicked, this, [this] {
        int r = selectedRow();
        if (r < 0) return;
        QList<QStringList> rows = loadAll();
        for (QStringList& x : rows) x[0] = "0";
        rows[r][0] = "1";
        saveAll(rows);
        reload();
    });

    connect(copy, &QPushButton::clicked, this, [this] {
        int r = selectedRow();
        if (r < 0) return;
        QGuiApplication::clipboard()->setText(loadAll()[r].value(3));
    });

    reload();
}

int IdentityDialog::selectedRow() const {
    auto items = m_table->selectedItems();
    return items.isEmpty() ? -1 : items.first()->row();
}

void IdentityDialog::reload() {
    const QList<QStringList> rows = loadAll();
    m_table->setRowCount(rows.size());
    for (int i = 0; i < rows.size(); ++i) {
        QString nick = rows[i].value(1);
        if (rows[i].value(0) == "1") nick += tr("  (padrão)");
        m_table->setItem(i, 0, new QTableWidgetItem(nick));
        m_table->setItem(i, 1, new QTableWidgetItem(rows[i].value(2)));
        QTableWidgetItem* uid = new QTableWidgetItem(
            rows[i].value(3).left(13) + QStringLiteral("..."));
        uid->setToolTip(rows[i].value(3));
        m_table->setItem(i, 2, uid);
    }
    if (rows.size()) m_table->selectRow(0);
}

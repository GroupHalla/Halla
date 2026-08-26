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
#include "version.h"
#include <cstring>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/x509.h>
#include <openssl/pem.h>

static QString keyBase(const QString& uid, const QString& field) {
    return QStringLiteral("identityKeys/%1/%2").arg(uid, field);
}

// Cabeçalho PKCS#8 fixo (RFC 8410) para uma chave Ed25519: basta concatenar
// a seed de 32 bytes. TODA a cripto de identidade desta janela usa caminhos
// identificados por OID — este cabeçalho é parseado por d2i_AutoPrivateKey
// tanto no OpenSSL quanto no BoringSSL. Nenhum NID numérico entra em cena:
// o build Windows compila com os headers do OpenSSL 3 do vcpkg mas LINKA o
// BoringSSL do webrtc.lib, e os dois discordam do número de Ed25519
// (OpenSSL NID_ED25519=1087, BoringSSL NID_ED25519=949) — com o valor do
// header, EVP_PKEY_CTX_new_id/EVP_PKEY_new_raw_private_key devolviam
// UNSUPPORTED_ALGORITHM e NENHUMA identidade foi criada em nenhum build
// WebRTC (era a causa raiz do ID único vazio e do bad_identity, confirmada
// em runtime pelo smoke do CI).
static const unsigned char kPkcs8SeedHeader[] = {
    0x30, 0x2E, 0x02, 0x01, 0x00, 0x30, 0x05, 0x06, 0x03, 0x2B, 0x65, 0x70,
    0x04, 0x22, 0x04, 0x20,
};

// Reconstrói a chave Ed25519 a partir da seed crua de 32 bytes, sem NID:
// embrulha no PKCS#8 mínimo e deixa o parser identificar pelo OID.
static EVP_PKEY* keyFromSeed(const unsigned char* seed) {
    unsigned char der[48];
    std::memcpy(der, kPkcs8SeedHeader, sizeof(kPkcs8SeedHeader));
    std::memcpy(der + sizeof(kPkcs8SeedHeader), seed, 32);
    const unsigned char* p = der;
    return d2i_AutoPrivateKey(nullptr, &p, sizeof(der));
}

// Último motivo pelo qual generateUniqueId()/storeIdentityKey() devolveram
// vazio. Antes o diálogo só dizia "não pôde ser gerada ou salva" e era
// impossível distinguir keygen quebrado, serialização quebrada ou cofre
// bloqueado — cada relato de usuário virava um interrogatório. Guardado em
// texto pronto para exibir/logar (a geração roda só na thread da GUI).
static QString g_lastIdentityError;

static QString sslErrorText() {
    const unsigned long e = ERR_get_error();
    if (!e) return QStringLiteral("sem detalhe");
    char buf[256] = {0};
    ERR_error_string_n(e, buf, sizeof(buf));
    return QString::fromLatin1(buf);
}

static QString storeIdentityKey(EVP_PKEY* key) {
    // Chave pública em SPKI DER (i2d_PUBKEY) — suportado por OpenSSL e
    // BoringSSL para todos os tipos de chave.
    const int pubLen = i2d_PUBKEY(key, nullptr);
    if (pubLen <= 0) {
        g_lastIdentityError = QStringLiteral("i2d_PUBKEY (chave pública): %1").arg(sslErrorText());
        return QString();
    }
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
        || rawLen == 0 || rawLen > 64) {
        g_lastIdentityError =
            QStringLiteral("EVP_PKEY_get_raw_private_key (chave privada): %1").arg(sslErrorText());
        return QString();
    }
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
    // Geração sem NID e sem keygen da EVP: a seed nasce do CSPRNG da própria
    // biblioteca linkada (RAND_bytes existe e é seguro em OpenSSL e
    // BoringSSL) e a chave é reconstruída pelo parser OID (keyFromSeed).
    unsigned char seed[32];
    QString uid;
    EVP_PKEY* key = nullptr;
    if (RAND_bytes(seed, sizeof(seed)) != 1) {
        g_lastIdentityError = QStringLiteral("RAND_bytes (geração da seed): %1").arg(sslErrorText());
    } else if (!(key = keyFromSeed(seed))) {
        g_lastIdentityError = QStringLiteral("d2i_AutoPrivateKey (chave Ed25519 da seed): %1").arg(sslErrorText());
    } else {
        uid = storeIdentityKey(key);
    }
    EVP_PKEY_free(key);
    if (uid.isEmpty()) {
        if (g_lastIdentityError.isEmpty())
            g_lastIdentityError = QStringLiteral("armazenamento da chave");
        AppLog::error(QStringLiteral("Identidade não gerada — %1").arg(g_lastIdentityError));
    }
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
    EVP_PKEY* key = nullptr;
    if (priv.size() == 32) {
        // Formato cru (seed Ed25519 de 32 bytes) gravado por
        // storeIdentityKey(): no BoringSSL do SDK WebRTC o i2d_PrivateKey
        // não suporta Ed25519, então a chave é persistida como seed crua.
        // keyFromSeed() embrulha no PKCS#8 mínimo e reconstrói por OID —
        // sem NID (o do header OpenSSL difere do BoringSSL linkado).
        key = keyFromSeed(reinterpret_cast<const unsigned char*>(priv.constData()));
    } else {
        // Material legado (PKCS#8 completo gravado por builds com OpenSSL
        // de verdade): parse direto.
        const unsigned char* p = reinterpret_cast<const unsigned char*>(priv.constData());
        key = d2i_AutoPrivateKey(nullptr, &p, priv.size());
    }
    if (!key) return QByteArray();
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
    m_note = new QLabel(tr("O ID único identifica você perante os servidores. "
                           "Guarde-o com segurança."), this);
    m_note->setStyleSheet(QStringLiteral("color:#666666"));
    bottom->addWidget(m_note, 1);
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
                   "gerada ou salva.\n\nEtapa com problema: %1\n\n"
                   "Verifique o cofre de senhas do sistema (Keychain, Keyring ou "
                   "Credential Manager) e tente novamente.\n\n"
                   "Halla %2")
                    .arg(g_lastIdentityError.isEmpty() ? tr("desconhecida") : g_lastIdentityError,
                         QString::fromUtf8(halla::kAppVersion)));
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
    bool broken = false;
    for (int i = 0; i < rows.size(); ++i) {
        QString nick = rows[i].value(1);
        if (rows[i].value(0) == "1") nick += tr("  (padrão)");
        m_table->setItem(i, 0, new QTableWidgetItem(nick));
        m_table->setItem(i, 1, new QTableWidgetItem(rows[i].value(2)));
        const QString uidText = rows[i].value(3);
        QTableWidgetItem* uid = new QTableWidgetItem(
            uidText.isEmpty() ? tr("(vazia)")
                              : uidText.left(13) + QStringLiteral("..."));
        uid->setToolTip(uidText.isEmpty()
            ? tr("O ID desta identidade está vazio — a geração de chave falhou "
                 "neste computador na versão que a criou. Use Adicionar para "
                 "criar uma identidade nova.")
            : uidText);
        if (uidText.isEmpty()) broken = true;
        m_table->setItem(i, 2, uid);
    }
    if (rows.size()) m_table->selectRow(0);
    // Nota de rodapé vira alerta quando há identidade quebrada na lista: o
    // usuário precisa saber POR QUE o ID está vazio, não apenas vê-lo vazio.
    if (m_note) {
        if (broken) {
            m_note->setText(tr("Atenção: há identidade com ID vazio — ao conectar "
                               "ela é rejeitada pelo servidor. Crie uma nova com "
                               "Adicionar e defina-a como padrão."));
            m_note->setStyleSheet(QStringLiteral("color:#b3261e"));
        } else {
            m_note->setText(tr("O ID único identifica você perante os servidores. "
                               "Guarde-o com segurança."));
            m_note->setStyleSheet(QStringLiteral("color:#666666"));
        }
    }
}

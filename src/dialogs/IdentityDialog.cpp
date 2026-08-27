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
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFile>
#include <QLineEdit>
#include "version.h"
#include <cstring>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/x509.h>
#include <openssl/pem.h>
#ifdef Q_OS_WIN
// Somente para CredEnumerateW (localizar identidades no cofre do Windows quando
// o usuário não tem mais o ID completo). Leitura/escrita continuam via qtkeychain.
#define NOMINMAX
#include <windows.h>
#include <wincred.h>
#endif

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

// ============================================================================
// Restauração e backup portátil de identidade
// ----------------------------------------------------------------------------
// O ID único é base64(SHA-256(chave pública SPKI DER)) — o MESMO cálculo do
// servidor (uidForIdentityPublicKey) — então TODAS as permissões, cargos,
// bans e apelidos por identidade vivem presos ao par de chaves. Apagar o
// registro do Windows (regedit) destrói apenas o perfil QSettings (lista
// "identities" + "identityKeys/<uid>/publicDer"): a chave privada, gravada
// pelo qtkeychain no Credential Manager, SOBREVIVE — a identidade pode ser
// recuperada a partir dela, e o ID volta a ser o mesmo sem tocar no servidor.
// ============================================================================
static QString uidForPublicDer(const QByteArray& pub) {
    return QString::fromLatin1(QCryptographicHash::hash(pub, QCryptographicHash::Sha256).toBase64());
}

// Backup portátil no MESMO formato do Halla Mobile (HallaCore.kt):
// "halla-identity-backup" v1, PBKDF2-HMAC-SHA256 + AES-256-GCM — arquivos
// cruzam Desktop <-> Mobile em qualquer direção.
static constexpr int kBackupIterations = 310000;
static constexpr int kBackupMaxBytes = 128 * 1024;
static constexpr int kBackupMinPassword = 10;

static QByteArray privateMaterialForUid(const QString& uid) {
    const QString privateKeyName = keyBase(uid, QStringLiteral("privateDer"));
    QByteArray priv = SecureStore::read(privateKeyName);
    if (!priv.isEmpty()) return priv;
    // Perfil local legado (PKCS#8 em QSettings, ou fallback de quando o cofre
    // estava indisponível): usa o material MESMO QUE a re-migração ao cofre
    // volte a falhar — a restauração não pode depender do cofre duas vezes.
    const QByteArray legacy = QByteArray::fromBase64(S::str(privateKeyName).toLatin1());
    if (legacy.isEmpty()) return legacy;
    if (SecureStore::write(privateKeyName, legacy)) {
        S::store().remove(privateKeyName);
        S::store().sync();
    }
    return legacy;
}

// Material pode ser a seed crua de 32 bytes (builds atuais) ou um PKCS#8
// DER completo (instalações antigas com OpenSSL de verdade no cofre).
static EVP_PKEY* keyFromMaterial(const QByteArray& priv) {
    if (priv.isEmpty()) return nullptr;
    if (priv.size() == 32)
        return keyFromSeed(reinterpret_cast<const unsigned char*>(priv.constData()));
    const unsigned char* p = reinterpret_cast<const unsigned char*>(priv.constData());
    return d2i_AutoPrivateKey(nullptr, &p, priv.size());
}

static QByteArray backupAad(const QString& alias, const QString& algorithm,
                            const QString& publicKeyBase64) {
    return QStringLiteral("halla-identity-backup|1|%1|%2|%3")
               .arg(alias, algorithm, publicKeyBase64)
               .toUtf8();
}

static bool deriveBackupKey(const QByteArray& password, const QByteArray& salt,
                            int iterations, QByteArray* out) {
    if (password.size() < kBackupMinPassword || salt.size() != 16
            || iterations < 100000 || iterations > 2000000)
        return false;
    unsigned char key[32];
    const int ok = PKCS5_PBKDF2_HMAC(password.constData(), int(password.size()),
                                     reinterpret_cast<const unsigned char*>(salt.constData()),
                                     int(salt.size()), iterations,
                                     EVP_sha256(), int(sizeof(key)), key);
    if (ok != 1) return false;
    *out = QByteArray(reinterpret_cast<char*>(key), int(sizeof(key)));
    OPENSSL_cleanse(key, sizeof(key));
    return true;
}

// GCM: saída = texto cifrado || tag de 16 bytes (o mesmo layout do Java/Android).
static bool aesGcmEncrypt(const QByteArray& key, const QByteArray& iv,
                          const QByteArray& aad, const QByteArray& plain,
                          QByteArray* out) {
    if (key.size() != 32 || iv.size() != 12 || plain.isEmpty()) return false;
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return false;
    QByteArray ct(plain.size() + 16, Qt::Uninitialized);
    unsigned char* dst = reinterpret_cast<unsigned char*>(ct.data());
    int len = 0;
    int total = 0;
    bool ok = EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1
        && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, int(iv.size()), nullptr) == 1
        && EVP_EncryptInit_ex(ctx, nullptr, nullptr,
                              reinterpret_cast<const unsigned char*>(key.constData()),
                              reinterpret_cast<const unsigned char*>(iv.constData())) == 1
        && EVP_EncryptUpdate(ctx, nullptr, &len,
                             reinterpret_cast<const unsigned char*>(aad.constData()),
                             int(aad.size())) == 1
        && EVP_EncryptUpdate(ctx, dst, &len,
                             reinterpret_cast<const unsigned char*>(plain.constData()),
                             int(plain.size())) == 1;
    if (ok) {
        total = len;
        ok = EVP_EncryptFinal_ex(ctx, dst + total, &len) == 1;
    }
    if (ok) {
        total += len;
        ok = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, dst + total) == 1;
    }
    EVP_CIPHER_CTX_free(ctx);
    if (!ok) return false;
    ct.resize(total + 16);
    *out = ct;
    return true;
}

static bool aesGcmDecrypt(const QByteArray& key, const QByteArray& iv,
                          const QByteArray& aad, const QByteArray& ctAndTag,
                          QByteArray* out) {
    if (key.size() != 32 || iv.size() != 12 || ctAndTag.size() <= 16) return false;
    const int ctLen = ctAndTag.size() - 16;
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return false;
    QByteArray pt(ctLen, Qt::Uninitialized);
    unsigned char* dst = reinterpret_cast<unsigned char*>(pt.data());
    int len = 0;
    int total = 0;
    unsigned char tag[16];
    std::memcpy(tag, ctAndTag.constData() + ctLen, sizeof(tag));
    bool ok = EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1
        && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, int(iv.size()), nullptr) == 1
        && EVP_DecryptInit_ex(ctx, nullptr, nullptr,
                              reinterpret_cast<const unsigned char*>(key.constData()),
                              reinterpret_cast<const unsigned char*>(iv.constData())) == 1
        && EVP_DecryptUpdate(ctx, nullptr, &len,
                             reinterpret_cast<const unsigned char*>(aad.constData()),
                             int(aad.size())) == 1
        && EVP_DecryptUpdate(ctx, dst, &len,
                             reinterpret_cast<const unsigned char*>(ctAndTag.constData()),
                             ctLen) == 1;
    if (ok) {
        total = len;
        ok = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, int(sizeof(tag)), tag) == 1
             && EVP_DecryptFinal_ex(ctx, dst + total, &len) == 1;
    }
    EVP_CIPHER_CTX_free(ctx);
    if (!ok) return false;
    pt.resize(total + len);
    *out = pt;
    return true;
}

#ifdef Q_OS_WIN
// UIDs com chave privada ainda presente no Credential Manager — usado quando
// o usuário não tem mais o ID completo para colar. Filtro por prefixo
// "identityKeys/" é o mesmo espaçamento de nome do SecureStore/qtkeychain.
static QStringList vaultIdentityUids() {
    QStringList uids;
    PCREDENTIALW* creds = nullptr;
    DWORD count = 0;
    if (!CredEnumerateW(L"identityKeys/*", 0, &count, &creds)) return uids;
    for (DWORD i = 0; i < count; ++i) {
        const QString target = QString::fromWCharArray(creds[i]->TargetName);
        if (!target.startsWith(QStringLiteral("identityKeys/"))) continue;
        const int cut = target.lastIndexOf(QLatin1Char('/'));
        const QString uid = target.mid(13, cut - 13);
        if (cut > 13 && !uid.isEmpty()) uids << uid;
    }
    CredFree(creds);
    uids.removeDuplicates();
    uids.sort();
    return uids;
}
#endif

// Registra no perfil uma identidade cuja chave privada já existe no cofre.
// A pública é re-derivada da privada e o hash é CONFERIDO contra o UID
// colado — nunca se registra uma identidade com chave trocada.
static bool restoreIdentityFromVault(QWidget* parent, const QString& uid) {
    QByteArray priv = privateMaterialForUid(uid);
    if (priv.isEmpty()) {
        QMessageBox::critical(
            parent, QObject::tr("Identidades"),
            QObject::tr("Nenhuma chave privada foi encontrada para este ID no cofre do "
                        "sistema nem no perfil local.\n\nIsso acontece quando o registro do "
                        "Windows foi limpo junto com a identidade antiga, ou quando o cofre "
                        "também foi apagado.\n\nAlternativas: importe um arquivo de backup "
                        "de identidade (Halla Desktop ou Mobile) ou peça a um administrador "
                        "do servidor para reconceder suas permissões ao seu novo ID."));
        return false;
    }
    EVP_PKEY* key = keyFromMaterial(priv);
    priv.fill(0);
    if (!key) {
        QMessageBox::critical(parent, QObject::tr("Identidades"),
                              QObject::tr("A chave privada encontrada para este ID não pôde "
                                          "ser interpretada."));
        return false;
    }
    const int pubLen = i2d_PUBKEY(key, nullptr);
    QByteArray pub(pubLen > 0 ? pubLen : 0, 0);
    unsigned char* p = reinterpret_cast<unsigned char*>(pub.data());
    const bool okPub = pubLen > 0 && i2d_PUBKEY(key, &p) == pubLen;
    EVP_PKEY_free(key);
    if (!okPub || uidForPublicDer(pub) != uid) {
        QMessageBox::critical(parent, QObject::tr("Identidades"),
                              QObject::tr("O material encontrado não gera este ID único."));
        return false;
    }
    S::set(keyBase(uid, QStringLiteral("publicDer")), QString::fromLatin1(pub.toBase64()));
    return true;
}

static bool exportIdentityBackupFile(QWidget* parent, const QString& uid,
                                     const QString& nick) {
    if (uid.isEmpty()) {
        QMessageBox::warning(parent, QObject::tr("Identidades"),
                             QObject::tr("A identidade selecionada está com ID vazio e não "
                                         "pode ser exportada."));
        return false;
    }
    QByteArray priv = privateMaterialForUid(uid);
    EVP_PKEY* key = keyFromMaterial(priv);
    priv.fill(0);
    if (!key) {
        QMessageBox::critical(parent, QObject::tr("Identidades"),
                              QObject::tr("Não foi possível ler a chave privada desta "
                                          "identidade para exportar."));
        return false;
    }
    const int pubLen = i2d_PUBKEY(key, nullptr);
    QByteArray pub(pubLen > 0 ? pubLen : 0, 0);
    unsigned char* p = reinterpret_cast<unsigned char*>(pub.data());
    const bool okPub = pubLen > 0 && i2d_PUBKEY(key, &p) == pubLen;
    unsigned char seed[32];
    size_t seedLen = sizeof(seed);
    const bool okSeed = okPub
        && EVP_PKEY_get_raw_private_key(key, seed, &seedLen) == 1 && seedLen == 32;
    EVP_PKEY_free(key);
    if (!okSeed || uidForPublicDer(pub) != uid) {
        OPENSSL_cleanse(seed, sizeof(seed));
        QMessageBox::critical(parent, QObject::tr("Identidades"),
                              QObject::tr("O material encontrado não gera este ID único."));
        return false;
    }
    // PKCS#8 mínimo (RFC 8410) — o MESMO formato que Java/Android grava, então
    // o Mobile importa backups gerados aqui (e vice-versa).
    QByteArray pkcs8(48, 0);
    std::memcpy(pkcs8.data(), kPkcs8SeedHeader, sizeof(kPkcs8SeedHeader));
    std::memcpy(pkcs8.data() + sizeof(kPkcs8SeedHeader), seed, 32);
    OPENSSL_cleanse(seed, sizeof(seed));

    bool ok = false;
    const QString password = QInputDialog::getText(
        parent, QObject::tr("Exportar backup de identidade"),
        QObject::tr("Crie uma senha para proteger o arquivo (mínimo de 10 caracteres):"),
        QLineEdit::Password, QString(), &ok);
    if (!ok || password.isEmpty()) { pkcs8.fill(0); return false; }
    QString confirm;
    if (password.size() >= kBackupMinPassword) {
        confirm = QInputDialog::getText(
            parent, QObject::tr("Exportar backup de identidade"),
            QObject::tr("Repita a senha:"), QLineEdit::Password, QString(), &ok);
    }
    if (password.size() < kBackupMinPassword || !ok || confirm != password) {
        pkcs8.fill(0);
        QMessageBox::critical(parent, QObject::tr("Identidades"),
                              QObject::tr("As senhas não conferem ou são curtas demais "
                                          "(mínimo de 10 caracteres)."));
        return false;
    }

    const QString publicB64 = QString::fromLatin1(pub.toBase64());
    const QString alias = QStringLiteral("desktop");
    const QString algorithm = QStringLiteral("Ed25519");
    QByteArray salt(16, 0), iv(12, 0);
    QByteArray derived, ciphertext;
    const bool cryptoOk = RAND_bytes(reinterpret_cast<unsigned char*>(salt.data()), int(salt.size())) == 1
        && RAND_bytes(reinterpret_cast<unsigned char*>(iv.data()), int(iv.size())) == 1
        && deriveBackupKey(password.toUtf8(), salt, kBackupIterations, &derived)
        && aesGcmEncrypt(derived, iv, backupAad(alias, algorithm, publicB64),
                         pkcs8, &ciphertext);
    pkcs8.fill(0);
    if (!cryptoOk) {
        derived.fill(0);
        QMessageBox::critical(parent, QObject::tr("Identidades"),
                              QObject::tr("Não foi possível cifrar o backup: %1")
                                  .arg(sslErrorText()));
        return false;
    }

    QJsonObject o;
    o[QStringLiteral("format")] = QStringLiteral("halla-identity-backup");
    o[QStringLiteral("version")] = 1;
    o[QStringLiteral("name")] = nick.left(80);
    o[QStringLiteral("alias")] = alias;
    o[QStringLiteral("uid")] = uid;
    o[QStringLiteral("algorithm")] = algorithm;
    o[QStringLiteral("public")] = publicB64;
    o[QStringLiteral("kdf")] = QStringLiteral("PBKDF2-HMAC-SHA256");
    o[QStringLiteral("iterations")] = kBackupIterations;
    o[QStringLiteral("salt")] = QString::fromLatin1(salt.toBase64());
    o[QStringLiteral("iv")] = QString::fromLatin1(iv.toBase64());
    o[QStringLiteral("private")] = QString::fromLatin1(ciphertext.toBase64());
    derived.fill(0);

    const QString path = QFileDialog::getSaveFileName(
        parent, QObject::tr("Exportar backup de identidade"),
        QStringLiteral("halla-identidade-%1.json").arg(uid.left(8)),
        QObject::tr("Arquivos de backup (*.json)"));
    if (path.isEmpty()) return false;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QMessageBox::critical(parent, QObject::tr("Identidades"),
                              QObject::tr("Não foi possível salvar o arquivo de backup."));
        return false;
    }
    const QByteArray payload = QJsonDocument(o).toJson(QJsonDocument::Compact);
    const bool wrote = file.write(payload) == payload.size();
    file.close();
    if (!wrote) {
        QMessageBox::critical(parent, QObject::tr("Identidades"),
                              QObject::tr("Não foi possível salvar o arquivo de backup."));
        return false;
    }
    QMessageBox::information(
        parent, QObject::tr("Identidades"),
        QObject::tr("Backup de identidade salvo em:\n%1\n\nGuarde o arquivo e a senha em "
                    "lugares seguros: com os dois, qualquer pessoa pode se passar por "
                    "você nos servidores.").arg(QDir::toNativeSeparators(path)));
    return true;
}

static bool importIdentityBackupFile(QWidget* parent, QString* outUid,
                                     QString* outNick) {
    const QString path = QFileDialog::getOpenFileName(
        parent, QObject::tr("Importar backup de identidade"), QString(),
        QObject::tr("Arquivos de backup (*.json)"));
    if (path.isEmpty()) return false;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > kBackupMaxBytes) {
        QMessageBox::critical(parent, QObject::tr("Identidades"),
                              QObject::tr("Arquivo de backup inválido ou corrompido."));
        return false;
    }
    const QByteArray raw = file.readAll();
    file.close();
    const QJsonDocument doc = QJsonDocument::fromJson(raw);
    if (!doc.isObject()) {
        QMessageBox::critical(parent, QObject::tr("Identidades"),
                              QObject::tr("Arquivo de backup inválido ou corrompido."));
        return false;
    }
    const QJsonObject o = doc.object();
    const QString alias = o[QStringLiteral("alias")].toString().trimmed();
    const QString algorithm = o[QStringLiteral("algorithm")].toString();
    const QString publicB64 = o[QStringLiteral("public")].toString();
    const QByteArray pub = QByteArray::fromBase64(publicB64.toLatin1());
    const QByteArray salt = QByteArray::fromBase64(o[QStringLiteral("salt")].toString().toLatin1());
    const QByteArray iv = QByteArray::fromBase64(o[QStringLiteral("iv")].toString().toLatin1());
    const QByteArray ct = QByteArray::fromBase64(o[QStringLiteral("private")].toString().toLatin1());
    const int iterations = o[QStringLiteral("iterations")].toInt(0);
    const bool schemaOk = o[QStringLiteral("format")].toString() == QStringLiteral("halla-identity-backup")
        && o[QStringLiteral("version")].toInt(0) == 1
        && o[QStringLiteral("kdf")].toString() == QStringLiteral("PBKDF2-HMAC-SHA256")
        && (algorithm == QStringLiteral("Ed25519") || algorithm == QStringLiteral("BC-Ed25519")
            || algorithm == QStringLiteral("EdDSA"))
        && !alias.isEmpty() && alias.size() <= 128
        && !alias.contains(QLatin1Char('\n')) && !alias.contains(QLatin1Char('\r'))
        && pub.size() >= 1 && pub.size() <= 512 && salt.size() == 16
        && iv.size() == 12 && ct.size() > 16 && ct.size() <= 2048
        && iterations >= 100000 && iterations <= 2000000;
    if (!schemaOk) {
        QMessageBox::critical(parent, QObject::tr("Identidades"),
                              QObject::tr("Arquivo de backup inválido ou corrompido."));
        return false;
    }
    bool ok = false;
    const QString password = QInputDialog::getText(
        parent, QObject::tr("Importar backup de identidade"),
        QObject::tr("Senha do arquivo de backup:"), QLineEdit::Password, QString(), &ok);
    if (!ok) return false;

    QByteArray derived, pkcs8;
    if (!deriveBackupKey(password.toUtf8(), salt, iterations, &derived)
        || !aesGcmDecrypt(derived, iv, backupAad(alias, algorithm, publicB64), ct, &pkcs8)) {
        derived.fill(0);
        QMessageBox::critical(parent, QObject::tr("Identidades"),
                              QObject::tr("Senha incorreta."));
        return false;
    }
    derived.fill(0);

    EVP_PKEY* key = keyFromMaterial(pkcs8);
    pkcs8.fill(0);
    if (!key) {
        QMessageBox::critical(parent, QObject::tr("Identidades"),
                              QObject::tr("Arquivo de backup inválido ou corrompido."));
        return false;
    }
    const int pubLen = i2d_PUBKEY(key, nullptr);
    QByteArray derivedPub(pubLen > 0 ? pubLen : 0, 0);
    unsigned char* p = reinterpret_cast<unsigned char*>(derivedPub.data());
    const bool okPub = pubLen > 0 && i2d_PUBKEY(key, &p) == pubLen;
    unsigned char seed[32];
    size_t seedLen = sizeof(seed);
    const bool okSeed = okPub
        && EVP_PKEY_get_raw_private_key(key, seed, &seedLen) == 1 && seedLen == 32;
    EVP_PKEY_free(key);
    if (!okSeed || derivedPub != pub) {
        OPENSSL_cleanse(seed, sizeof(seed));
        QMessageBox::critical(parent, QObject::tr("Identidades"),
                              QObject::tr("A chave privada do backup não corresponde à chave "
                                          "pública declarada."));
        return false;
    }
    const QString uid = uidForPublicDer(pub);
    const QString declaredUid = o[QStringLiteral("uid")].toString();
    if (!declaredUid.isEmpty() && declaredUid != uid) {
        OPENSSL_cleanse(seed, sizeof(seed));
        QMessageBox::critical(parent, QObject::tr("Identidades"),
                              QObject::tr("O ID do backup não confere com a chave pública."));
        return false;
    }

    const QString privateKeyName = keyBase(uid, QStringLiteral("privateDer"));
    QByteArray seedBytes(reinterpret_cast<char*>(seed), 32);
    OPENSSL_cleanse(seed, sizeof(seed));
    QString secureError;
    if (!SecureStore::write(privateKeyName, seedBytes, &secureError)) {
        AppLog::error(QObject::tr("Não foi possível salvar a identidade no cofre do sistema: %1 — "
                                  "a chave privada será guardada apenas no perfil local (menos seguro).")
                          .arg(secureError));
        S::set(privateKeyName, QString::fromLatin1(seedBytes.toBase64()));
    } else {
        S::store().remove(privateKeyName);
    }
    seedBytes.fill(0);
    S::set(keyBase(uid, QStringLiteral("publicDer")), publicB64);
    S::store().sync();
    *outUid = uid;
    *outNick = o[QStringLiteral("name")].toString().left(80);
    return true;
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
    resize(560, 420);

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
    // Recuperação de identidade: o registro do Windows limpo apaga a lista e
    // a chave pública, mas a chave privada sobrevive no cofre do sistema —
    // "Restaurar..." recupera a identidade pelo ID colado (ou procurando no
    // cofre) sem gerar um ID novo, devolvendo cargos e permissões.
    QPushButton* restore = mkBtn(tr("Restaurar..."),
                                 tr("Recuperar uma identidade deste computador pelo ID único"));
    QPushButton* exportB = mkBtn(tr("Exportar..."),
                                  tr("Salvar a identidade selecionada em um arquivo protegido por senha"));
    QPushButton* importB = mkBtn(tr("Importar..."),
                                 tr("Carregar uma identidade de um arquivo de backup do Halla Desktop ou Mobile"));
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

    connect(restore, &QPushButton::clicked, this, [this] {
        QDialog dlg(this);
        dlg.setWindowTitle(tr("Restaurar identidade"));
        QVBoxLayout* lay = new QVBoxLayout(&dlg);
        QLabel* info = new QLabel(tr("Cole abaixo o ID único completo da identidade "
                                     "que deseja recuperar:"), &dlg);
        info->setWordWrap(true);
        QLineEdit* edit = new QLineEdit(&dlg);
        edit->setPlaceholderText(tr("ID único (colar)"));
        lay->addWidget(info);
        lay->addWidget(edit);
#ifdef Q_OS_WIN
        QPushButton* scan = new QPushButton(tr("Procurar no cofre do Windows"), &dlg);
        lay->addWidget(scan);
        connect(scan, &QPushButton::clicked, this, [this, edit] {
            QStringList known;
            for (const QStringList& r : loadAll()) known << r.value(3);
            QStringList found;
            for (const QString& uid : vaultIdentityUids())
                if (!uid.isEmpty() && !known.contains(uid)) found << uid;
            if (found.isEmpty()) {
                QMessageBox::information(this, tr("Identidades"),
                                         tr("Nenhuma identidade adicional encontrada no "
                                            "cofre do Windows."));
                return;
            }
            QStringList display;
            for (const QString& uid : found)
                display << uid.left(13) + QStringLiteral("...");
            bool ok = false;
            const QString pick = QInputDialog::getItem(
                this, tr("Restaurar identidade"),
                tr("Identidades encontradas no cofre do Windows:"),
                display, 0, false, &ok);
            if (!ok) return;
            const int idx = display.indexOf(pick);
            if (idx >= 0 && idx < found.size()) edit->setText(found.at(idx));
        });
#endif
        QDialogButtonBox* bb = new QDialogButtonBox(QDialogButtonBox::Ok
                                                        | QDialogButtonBox::Cancel,
                                                    &dlg);
        lay->addWidget(bb);
        connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
        connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
        if (dlg.exec() != QDialog::Accepted) return;
        const QString uid = edit->text().trimmed();
        if (uid.isEmpty()) return;

        QList<QStringList> rows = loadAll();
        for (const QStringList& r : rows) {
            if (r.value(3) == uid) {
                QMessageBox::information(this, tr("Identidades"),
                                         tr("Identidade já presente na lista."));
                return;
            }
        }
        bool ok = false;
        const QString name = QInputDialog::getText(
            this, tr("Restaurar identidade"),
            tr("Apelido da identidade restaurada:"), QLineEdit::Normal,
            tr("HallaUser"), &ok);
        if (!ok || name.trimmed().isEmpty()) return;
        if (!restoreIdentityFromVault(this, uid)) return;

        rows << QStringList{ "0", name.trimmed(), QString(), uid };
        saveAll(rows);
        reload();
        for (int i = 0; i < rows.size(); ++i)
            if (rows.value(i).value(3) == uid) m_table->selectRow(i);
        if (QMessageBox::question(this, tr("Identidades"),
                                  tr("Definir a identidade restaurada como padrão?"))
            == QMessageBox::Yes) {
            rows = loadAll();
            for (QStringList& x : rows) x[0] = "0";
            for (QStringList& x : rows)
                if (x.value(3) == uid) x[0] = "1";
            saveAll(rows);
            reload();
        }
        QMessageBox::information(
            this, tr("Identidades"),
            tr("Identidade \"%1\" restaurada com sucesso!\n\nAo conectar, o servidor "
               "reconhece o mesmo ID: cargos, permissões e histórico voltam a valer "
               "para ela.").arg(name.trimmed()));
        AppLog::info(tr("Identidade \"%1\" restaurada do cofre local").arg(name.trimmed()));
    });

    connect(exportB, &QPushButton::clicked, this, [this] {
        int r = selectedRow();
        if (r < 0) return;
        const QList<QStringList> rows = loadAll();
        if (r >= rows.size()) return;
        if (exportIdentityBackupFile(this, rows.value(r).value(3),
                                     rows.value(r).value(1)))
            AppLog::info(tr("Backup de identidade exportado"));
    });

    connect(importB, &QPushButton::clicked, this, [this] {
        QString uid, nick;
        if (!importIdentityBackupFile(this, &uid, &nick)) return;
        QList<QStringList> rows = loadAll();
        int existing = -1;
        for (int i = 0; i < rows.size(); ++i)
            if (rows.value(i).value(3) == uid) existing = i;
        const QString name = nick.trimmed().isEmpty() ? tr("HallaUser") : nick.trimmed();
        if (existing < 0) {
            rows << QStringList{ "0", name, QString(), uid };
            saveAll(rows);
        }
        reload();
        rows = loadAll();
        for (int i = 0; i < rows.size(); ++i)
            if (rows.value(i).value(3) == uid) m_table->selectRow(i);
        if (QMessageBox::question(this, tr("Identidades"),
                                  tr("Definir a identidade importada como padrão?"))
            == QMessageBox::Yes) {
            for (QStringList& x : rows) x[0] = "0";
            for (QStringList& x : rows)
                if (x.value(3) == uid) x[0] = "1";
            saveAll(rows);
            reload();
        }
        QMessageBox::information(
            this, tr("Identidades"),
            tr("Identidade \"%1\" importada!\n\nConecte-se aos servidores com ela para "
               "usar os cargos e permissões dessa identidade.").arg(name));
        AppLog::info(tr("Identidade \"%1\" importada de um backup").arg(name));
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

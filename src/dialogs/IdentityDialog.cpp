#include "IdentityDialog.h"
#include "TsBanner.h"
#include "Icons.h"
#include "Settings.h"
#include "core/SecureStore.h"
#include "core/E2eeCrypto.h"
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
// Only for CredEnumerateW (locating identities in the Windows vault when the user no
// longer has the full ID). Reading/writing still go through qtkeychain.
#define NOMINMAX
#include <windows.h>
#include <wincred.h>
#endif

static QString keyBase(const QString& uid, const QString& field) {
    return QStringLiteral("identityKeys/%1/%2").arg(uid, field);
}

// Fixed PKCS#8 header (RFC 8410) for an Ed25519 key: appending the 32-byte seed is
// enough. ALL identity cryptography in this window uses OID-identified paths -- this
// header is parsed by d2i_AutoPrivateKey on both OpenSSL and BoringSSL. No numeric NID
// is ever involved: the Windows build compiles against the OpenSSL 3 headers from vcpkg
// but LINKS the BoringSSL from webrtc.lib, and the two disagree on the Ed25519 number
// (OpenSSL NID_ED25519=1087, BoringSSL NID_ED25519=949) -- with the header value,
// EVP_PKEY_CTX_new_id/EVP_PKEY_new_raw_private_key returned UNSUPPORTED_ALGORITHM and
// NO identity was ever created on any WebRTC build (that was the root cause of the empty
// unique ID and of bad_identity, confirmed at runtime by the CI smoke test).
static const unsigned char kPkcs8SeedHeader[] = {
    0x30, 0x2E, 0x02, 0x01, 0x00, 0x30, 0x05, 0x06, 0x03, 0x2B, 0x65, 0x70,
    0x04, 0x22, 0x04, 0x20,
};

// Rebuilds the Ed25519 key from the raw 32-byte seed, without a NID: wraps it in the
// minimal PKCS#8 and lets the parser identify it by OID.
static EVP_PKEY* keyFromSeed(const unsigned char* seed) {
    unsigned char der[48];
    std::memcpy(der, kPkcs8SeedHeader, sizeof(kPkcs8SeedHeader));
    std::memcpy(der + sizeof(kPkcs8SeedHeader), seed, 32);
    const unsigned char* p = der;
    return d2i_AutoPrivateKey(nullptr, &p, sizeof(der));
}

// Last reason why generateUniqueId()/storeIdentityKey() returned empty. The dialog used
// to say only "could not be generated or saved", making it impossible to tell a broken
// keygen from broken serialization or a locked vault -- every user report became an
// interrogation. Stored as ready-to-show/log text (generation only runs on the GUI
// thread).
static QString g_lastIdentityError;

static QString sslErrorText() {
    const unsigned long e = ERR_get_error();
    if (!e) return QStringLiteral("sem detalhe");
    char buf[256] = {0};
    ERR_error_string_n(e, buf, sizeof(buf));
    return QString::fromLatin1(buf);
}

// ============================================================================
// Portable identity restore and backup
// ----------------------------------------------------------------------------
// The unique ID is base64(SHA-256(SPKI DER public key)) -- the SAME computation as the
// server (uidForIdentityPublicKey) -- so ALL permissions, roles, bans and per-identity
// nicknames are bound to the key pair. Erasing the Windows registry entry (regedit)
// destroys only the QSettings profile ("identities" list + "identityKeys/<uid>/publicDer"):
// the private key, written by qtkeychain into the Credential Manager, SURVIVES -- the
// identity can be recovered from it and the ID becomes the same again without touching
// the server.
// ============================================================================
static QString uidForPublicDer(const QByteArray& pub) {
    return QString::fromLatin1(QCryptographicHash::hash(pub, QCryptographicHash::Sha256).toBase64());
}

// Portable backup in the SAME format as Halla Mobile (HallaCore.kt):
// "halla-identity-backup" v1, PBKDF2-HMAC-SHA256 + AES-256-GCM -- files cross Desktop <->
// Mobile in either direction.
static constexpr int kBackupIterations = 310000;
static constexpr int kBackupMaxBytes = 128 * 1024;
static constexpr int kBackupMinPassword = 10;

static QByteArray privateMaterialForUid(const QString& uid) {
    const QString privateKeyName = keyBase(uid, QStringLiteral("privateDer"));
    QByteArray priv = SecureStore::read(privateKeyName);
    if (!priv.isEmpty()) return priv;
    // Legacy local profile (PKCS#8 in QSettings, or the fallback used when the vault was
    // unavailable): uses the material EVEN IF the re-migration to the vault fails again --
    // restoring must not depend on the vault twice.
    const QByteArray legacy = QByteArray::fromBase64(S::str(privateKeyName).toLatin1());
    if (legacy.isEmpty()) return legacy;
    if (SecureStore::write(privateKeyName, legacy)) {
        S::store().remove(privateKeyName);
        S::store().sync();
    }
    return legacy;
}

// The material may be the raw 32-byte seed (current builds) or a full PKCS#8 DER (old
// installations with real OpenSSL in the vault).
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

// ---------------------------------------------------------------------------
// PBKDF2-HMAC-SHA256 WITHOUT PKCS5_PBKDF2_HMAC.
//
// The signature of that function DIVERGES between the libraries Halla uses: OpenSSL 3
// declares (const char*, int passlen, ..., int saltlen, int iter, ..., int keylen, ...)
// and the BoringSSL from webrtc.lib implements it with (const char*, size_t password_len,
// ..., size_t salt_len, uint32_t iterations, ..., size_t key_len, ...). The Windows build
// compiles against the OpenSSL headers and LINKS BoringSSL -- a passlen = -1 (the OpenSSL
// "count the password" convention) became 4294967295 in BoringSSL's size_t and the
// process died with an access violation (caught by the CI smoke test, run 33026636018).
// The HMAC()/HMAC_Init_ex family has the same divergence in key_len.
//
// The output below uses ONLY primitives whose ABI is identical in both libraries:
// EVP_MD_CTX_new/free, EVP_DigestInit_ex, EVP_DigestUpdate (size_t in both),
// EVP_DigestFinal_ex and EVP_sha256. The algorithm is RFC 8018 -- byte for byte the same
// PBKDF2 as PKCS5_PBKDF2_HMAC and as Java's PBKDF2WithHmacSHA256 (Halla Mobile), so
// backups still cross platforms as before.
// ---------------------------------------------------------------------------
static bool hmacSha256(const unsigned char* key, int keyLen,
                       const unsigned char* msg1, int msg1Len,
                       const unsigned char* msg2, int msg2Len,
                       unsigned char out[32]) {
    unsigned char block[64];
    std::memset(block, 0, sizeof(block));
    if (keyLen > int(sizeof(block))) {
        // chave maior que o bloco: reduz primeiro (RFC 2104)
        EVP_MD_CTX* ctx = EVP_MD_CTX_new();
        if (!ctx) return false;
        unsigned int kdLen = 0;
        const bool ok = EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) == 1
            && EVP_DigestUpdate(ctx, key, size_t(keyLen)) == 1
            && EVP_DigestFinal_ex(ctx, block, &kdLen) == 1;
        EVP_MD_CTX_free(ctx);
        if (!ok) return false;
    } else {
        std::memcpy(block, key, size_t(keyLen));
    }

    unsigned char ipad[64];
    unsigned char opad[64];
    for (int i = 0; i < 64; ++i) {
        ipad[i] = block[i] ^ 0x36;
        opad[i] = block[i] ^ 0x5c;
    }

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) return false;
    unsigned char inner[32];
    unsigned int innerLen = 0;
    bool ok = EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) == 1
        && EVP_DigestUpdate(ctx, ipad, sizeof(ipad)) == 1
        && (msg1Len <= 0 || EVP_DigestUpdate(ctx, msg1, size_t(msg1Len)) == 1)
        && (msg2Len <= 0 || EVP_DigestUpdate(ctx, msg2, size_t(msg2Len)) == 1)
        && EVP_DigestFinal_ex(ctx, inner, &innerLen) == 1;
    if (ok) {
        ok = EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) == 1
            && EVP_DigestUpdate(ctx, opad, sizeof(opad)) == 1
            && EVP_DigestUpdate(ctx, inner, sizeof(inner)) == 1
            && EVP_DigestFinal_ex(ctx, out, &innerLen) == 1;
    }
    EVP_MD_CTX_free(ctx);
    OPENSSL_cleanse(ipad, sizeof(ipad));
    OPENSSL_cleanse(opad, sizeof(opad));
    OPENSSL_cleanse(inner, sizeof(inner));
    return ok;
}

// PBKDF2 (RFC 8018) with HMAC-SHA256: dkLen is always 32 here (AES-256-GCM key), a
// single output block.
static bool pbkdf2HmacSha256(const QByteArray& password, const QByteArray& salt,
                             int iterations, unsigned char out[32]) {
    if (iterations < 1) return false;
    const unsigned char beOne[4] = {0, 0, 0, 1};  // INT(1) big-endian
    unsigned char u[32];
    if (!hmacSha256(reinterpret_cast<const unsigned char*>(password.constData()),
                    password.size(),
                    reinterpret_cast<const unsigned char*>(salt.constData()),
                    salt.size(), beOne, sizeof(beOne), u))
        return false;
    unsigned char t[32];
    std::memcpy(t, u, sizeof(t));
    for (int i = 1; i < iterations; ++i) {
        if (!hmacSha256(reinterpret_cast<const unsigned char*>(password.constData()),
                        password.size(), u, sizeof(u), nullptr, 0, u))
            return false;
        for (int b = 0; b < 32; ++b) t[b] ^= u[b];
    }
    std::memcpy(out, t, 32);
    OPENSSL_cleanse(u, sizeof(u));
    OPENSSL_cleanse(t, sizeof(t));
    return true;
}

static bool deriveBackupKey(const QByteArray& password, const QByteArray& salt,
                            int iterations, QByteArray* out) {
    if (password.size() < kBackupMinPassword || salt.size() != 16
            || iterations < 100000 || iterations > 2000000)
        return false;
    unsigned char key[32];
    if (!pbkdf2HmacSha256(password, salt, iterations, key)) return false;
    *out = QByteArray(reinterpret_cast<char*>(key), int(sizeof(key)));
    OPENSSL_cleanse(key, sizeof(key));
    return true;
}

// GCM: output = ciphertext || 16-byte tag (the same layout as Java/Android).
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
// UIDs whose private key is still present in the Credential Manager -- used when the user
// no longer has the full ID to paste. The "identityKeys/" prefix filter is the same name
// spacing as SecureStore/qtkeychain.
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

// Registers in the profile an identity whose private key already exists in the vault. The
// public key is re-derived from the private one and the hash is CHECKED against the pasted
// UID -- an identity with a swapped key is never registered.
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
    // Minimal PKCS#8 (RFC 8410) -- the SAME format Java/Android writes, so Mobile imports
    // backups generated here (and vice versa).
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

    // v6 E2EE: the (static) X25519 pair travels with the backup in its own ENCRYPTED
    // blob -- in the clear it would hand over the key that decrypts private chat, poke and
    // offline messages to whoever restores the file. The same password and KDF parameters
    // as the identity blob protect both.
    QJsonObject dhField;
    if (IdentityDialog::ensureDhKeyPair(uid)) {
        const QByteArray dhPriv = IdentityDialog::dhPrivateKeyForUid(uid);
        const QByteArray dhPub = IdentityDialog::dhPublicKeyForUid(uid);
        if (dhPriv.size() == 32 && dhPub.size() == 32) {
            QByteArray dhPlain;
            dhPlain.append(dhPriv);
            dhPlain.append(dhPub);
            QByteArray dhIv(12, 0), dhCt;
            const bool dhOk = RAND_bytes(reinterpret_cast<unsigned char*>(dhIv.data()), int(dhIv.size())) == 1
                && aesGcmEncrypt(derived, dhIv,
                                 (backupAad(alias, algorithm, publicB64)
                                  + QStringLiteral("|dh")).toUtf8(),
                                 dhPlain, &dhCt);
            dhPlain.fill(0);
            if (dhOk) {
                dhField[QStringLiteral("iv")] = QString::fromLatin1(dhIv.toBase64());
                dhField[QStringLiteral("ct")] = QString::fromLatin1(dhCt.toBase64());
            }
        }
    }
    derived.fill(0);

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
    if (!dhField.isEmpty()) o[QStringLiteral("dh")] = dhField;
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
    // v6 E2EE: decrypts the X25519 pair WHILE the derived key is still valid -- the
    // optional "dh" blob travels with the backup under the same password. Backups without
    // the field (generated before v6) leave dhPlain empty and remain valid.
    QByteArray dhPlain;
    const QJsonObject dhField = o[QStringLiteral("dh")].toObject();
    if (!dhField.isEmpty()) {
        const QByteArray dhIv = QByteArray::fromBase64(dhField[QStringLiteral("iv")].toString().toLatin1());
        const QByteArray dhCt = QByteArray::fromBase64(dhField[QStringLiteral("ct")].toString().toLatin1());
        if (dhIv.size() == 12 && dhCt.size() == 64 + 16)
            aesGcmDecrypt(derived, dhIv,
                          (backupAad(alias, algorithm, publicB64)
                           + QStringLiteral("|dh")).toUtf8(),
                          dhCt, &dhPlain);
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

    // v6 E2EE: stores the restored X25519 pair. The pair is only accepted if the declared
    // public key derives exactly from the restored private key -- a tampered blob never
    // enters the vault.
    if (dhPlain.size() == 64) {
        const QByteArray dhPriv = dhPlain.left(32);
        const QByteArray dhPub = dhPlain.right(32);
        if (E2ee::dhPublicFromPrivate(dhPriv) == dhPub) {
            const QString dhPrivName = keyBase(uid, QStringLiteral("dhPrivate"));
            if (!SecureStore::write(dhPrivName, dhPriv, &secureError)) {
                AppLog::error(QObject::tr(
                                  "Não foi possível salvar a chave E2EE no cofre do sistema: %1 — "
                                  "será guardada apenas no perfil local (menos seguro).")
                                  .arg(secureError));
                S::set(dhPrivName, QString::fromLatin1(dhPriv.toBase64()));
            } else {
                S::store().remove(dhPrivName);
            }
            S::set(keyBase(uid, QStringLiteral("dhPublic")),
                   QString::fromLatin1(dhPub.toBase64()));
            S::store().sync();
        }
    }
    dhPlain.fill(0);
    *outUid = uid;
    *outNick = o[QStringLiteral("name")].toString().left(80);
    return true;
}

static QString storeIdentityKey(EVP_PKEY* key) {
    // Public key in SPKI DER (i2d_PUBKEY) -- supported by OpenSSL and BoringSSL for every
    // key type.
    const int pubLen = i2d_PUBKEY(key, nullptr);
    if (pubLen <= 0) {
        g_lastIdentityError = QStringLiteral("i2d_PUBKEY (chave pública): %1").arg(sslErrorText());
        return QString();
    }
    // Private key in the RAW FORMAT (32-byte Ed25519 seed). The classic i2d_PrivateKey only
    // serializes RSA/EC/DSA: on the BoringSSL embedded in the WebRTC SDK (which the Windows
    // build links instead of OpenSSL) it returns -1 for Ed25519 -- every identity was born
    // with an empty unique ID and every login fell into bad_identity. The raw format exists
    // and behaves the same in both (EVP_PKEY_get_raw_private_key), and the seed is enough
    // to rebuild the key when signing.
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
        // Without a system vault (keyring missing/locked, Credential Manager blocked by
        // policy etc.) the identity still must be creatable: it falls back to local storage
        // -- the same legacy location old installations used. signNonce() reads SecureStore
        // first and migrates back to the vault as soon as it works again.
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
    // Generation without a NID and without EVP keygen: the seed comes from the CSPRNG of
    // the linked library itself (RAND_bytes exists and is safe on OpenSSL and BoringSSL)
    // and the key is rebuilt by the OID parser (keyFromSeed).
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

// ================================================================== v6 E2EE
// Persistent X25519 pair of the identity -- the static key behind the pair-to-pair
// agreements (private chat, poke, offline) and the receipt of channel key envelopes.
// Private in the system vault (same policy and fallback as Ed25519); public in the
// profile. Without a valid dhPub the v6 login is refused by the server -- that is why
// ensureDhKeyPair exists: identities created before v6 get the pair automatically on the
// first listing.
QByteArray IdentityDialog::dhPrivateKeyForUid(const QString& uid) {
    const QString privateKeyName = keyBase(uid, QStringLiteral("dhPrivate"));
    QByteArray priv = SecureStore::read(privateKeyName);
    if (priv.isEmpty()) {
        // Legacy/local fallback -- same as Ed25519, with migration back to the vault once it
        // accepts writes again.
        const QByteArray legacy = QByteArray::fromBase64(S::str(privateKeyName).toLatin1());
        if (!legacy.isEmpty()) {
            priv = legacy;
            if (SecureStore::write(privateKeyName, legacy)) {
                S::store().remove(privateKeyName);
                S::store().sync();
            }
        }
    }
    return priv;
}

QByteArray IdentityDialog::dhPublicKeyForUid(const QString& uid) {
    return QByteArray::fromBase64(S::str(keyBase(uid, QStringLiteral("dhPublic"))).toLatin1());
}

bool IdentityDialog::ensureDhKeyPair(const QString& uid) {
    if (uid.isEmpty()) return false;
    const QByteArray priv = dhPrivateKeyForUid(uid);
    const QByteArray storedPub = dhPublicKeyForUid(uid);
    // An existing pair is only accepted if the public key derives exactly from the private
    // one -- a truncated/corrupted pair from the vault is regenerated, never reused.
    if (priv.size() == 32 && storedPub == E2ee::dhPublicFromPrivate(priv))
        return true;
    E2ee::DhKeyPair kp;
    if (!E2ee::generateDhKeyPair(kp)) {
        AppLog::error(QObject::tr("Não foi possível gerar a chave E2EE da identidade (X25519)."));
        return false;
    }
    const QString privateKeyName = keyBase(uid, QStringLiteral("dhPrivate"));
    QString secureError;
    if (!SecureStore::write(privateKeyName, kp.priv, &secureError)) {
        AppLog::error(QObject::tr(
                          "Não foi possível salvar a chave E2EE no cofre do sistema: %1 — "
                          "será guardada apenas no perfil local (menos seguro).")
                          .arg(secureError));
        S::set(privateKeyName, QString::fromLatin1(kp.priv.toBase64()));
    } else {
        S::store().remove(privateKeyName);
    }
    S::set(keyBase(uid, QStringLiteral("dhPublic")), QString::fromLatin1(kp.pub.toBase64()));
    S::store().sync();
    return true;
}

QByteArray IdentityDialog::dhSignatureForUid(const QString& uid) {
    const QByteArray dhPub = dhPublicKeyForUid(uid);
    if (dhPub.size() != 32) return QByteArray();
    QByteArray idPriv = privateMaterialForUid(uid);
    const QByteArray sig = E2ee::ed25519Sign(idPriv, E2ee::dhBindingMessage(dhPub));
    idPriv.fill(0); // identity material does not stay in a local copy
    return sig;
}

QByteArray IdentityDialog::signNonce(const QString& uid, const QByteArray& nonce) {
    const QString privateKeyName = keyBase(uid, QStringLiteral("privateDer"));
    QByteArray priv = SecureStore::read(privateKeyName);
    if (priv.isEmpty()) {
        // Migration of legacy installations (PKCS#8 in QSettings) and of identities written by
        // the storeIdentityKey() fallback when the system vault was unavailable: uses the
        // local material EVEN IF the re-write to the vault fails again. Without this, on
        // machines with a blocked Credential Manager the identity was created by the
        // fallback but the login challenge failed -- the key was in the local profile and
        // signNonce refused to use it.
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
    // Key rebuild and the (deterministic) Ed25519 signature now live in E2ee::ed25519Sign --
    // the same path as the E2EE binding, so there are never two implementations of identity
    // signing.
    const QByteArray sig = E2ee::ed25519Sign(priv, nonce);
    priv.fill(0);
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
            // v6: identities created before E2EE get the X25519 pair here
            // — o login v6 recusa identidade sem chave de criptografia.
            if (!uid.isEmpty()) ensureDhKeyPair(uid);
            rows << QStringList{ o["def"].toBool() ? "1" : "0",
                                 o["nick"].toString(), o["phon"].toString(),
                                 uid };
        }
    }
    if (migrated && !rows.isEmpty()) saveAll(rows);
    if (rows.isEmpty()) {
        // initial identity: generate ONCE and persist -- the unique ID must be stable across
        // runs (the server uses it for bans, groups and privilege keys)
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
    // Identity recovery: a wiped Windows registry erases the list and the public key, but
    // the private key survives in the system vault -- "Restore..." recovers the identity
    // from the pasted ID (or by searching the vault) without generating a new ID, bringing
    // roles and permissions back.
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
    // The footnote becomes an alert when there is a broken identity in the list: the user
    // must know WHY the ID is empty, not merely see it empty.
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

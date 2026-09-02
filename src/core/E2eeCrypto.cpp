#include "E2eeCrypto.h"

#include <QCryptographicHash>
#include <QtGlobal>

#include <cstring>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/x509.h> // d2i_PUBKEY/i2d_PUBKEY/d2i_AutoPrivateKey (caminho livre de NID)

// Compatibilidade de backends (LEIA ANTES DE MEXAR EM EVP):
//
// O Windows compila contra os headers do OpenSSL 3 (vcpkg) mas LINKA o
// BoringSSL embutido no webrtc.lib do SDK. As ASSINATURAS das funções EVP
// abaixo são idênticas nas duas bibliotecas — mas as CONSTANTES de tipo
// NÃO são: os headers do OpenSSL declaram NID_X25519=1034/NID_ED25519=1087
// enquanto o BoringSSL espera 948/949. Por isso EVP_PKEY_new_raw_private_key
// e EVP_PKEY_new_raw_public_key (que recebem o NID por parâmetro) DEVOLVEM
// NULL em runtime no app Windows — o gate e2ee_crypto_smoke do release pega
// exatamente isso (o IdentityDialog teve o mesmo problema com Ed25519; ver
// o cabeçalho do tests/ed25519_identity_smoke.cpp).
//
// A solução é o caminho DER/OID, livre de NID: a chave crua de 32 bytes é
// embrulhada em PKCS#8/SPKI mínimos do RFC 8410 (cabeçalho fixo + chave) e
// interpretada por d2i_AutoPrivateKey/d2i_PUBKEY/i2d_PUBKEY — o tipo correto
// fica DENTRO do EVP_PKEY, e tanto o OpenSSL real quanto o BoringSSL aceitam
// os mesmos bytes (é o formato canônico do `openssl genpkey -algorithm
// X25519`/Ed25519). EVP_PKEY_CTX_new/derive, EVP_DigestSign/Verify, HMAC,
// RAND_bytes e a camada EVP_CIPHER (AES-GCM) têm ABI idêntica nas duas.

namespace E2ee {

const char kDomainKeyWrap[]   = "HALLA-E2EKEY-V1";
const char kDomainChat[]      = "HALLA-CHAT-V1";
const char kDomainPoke[]      = "HALLA-POKE-V1";
const char kDomainOffline[]   = "HALLA-OFFLINE-V1";
const char kDhBindingDomain[] = "HALLA-DH-V1";

namespace {

// Aparição de -Wold-style-cast em EVP_CIPHER_CTX_ctrl é padrão da API OpenSSL;
// o elenco aqui é o uso documentado da biblioteca.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"

QByteArray hmacSha256(const QByteArray& key, const QByteArray& data) {
    QByteArray out(32, 0);
    unsigned int outLen = 0;
    if (HMAC(EVP_sha256(),
             key.constData(), int(key.size()),
             reinterpret_cast<const unsigned char*>(data.constData()), size_t(data.size()),
             reinterpret_cast<unsigned char*>(out.data()), &outLen) == nullptr)
        return QByteArray();
    out.resize(int(outLen));
    return out;
}

// ------------------------------------------------------------ NID-free keys
//
// Conversões entre bytes crus de 32 bytes e EVP_PKEY pelos envelopes mínimos
// do RFC 8410 — NENHUMA constante EVP_PKEY_* do compilador entra no binário.

// PKCS#8 v2 (PrivateKeyInfo): 30 2E 02 01 00 30 05 06 03 <OID> 04 22 04 20 || key
const unsigned char kX25519Pkcs8Prefix[] = {
    0x30, 0x2E, 0x02, 0x01, 0x00, 0x30, 0x05, 0x06, 0x03, 0x2B, 0x65, 0x6E,
    0x04, 0x22, 0x04, 0x20,
};
const unsigned char kEd25519Pkcs8Prefix[] = {
    0x30, 0x2E, 0x02, 0x01, 0x00, 0x30, 0x05, 0x06, 0x03, 0x2B, 0x65, 0x70,
    0x04, 0x22, 0x04, 0x20,
};
// SPKI (SubjectPublicKeyInfo): 30 2A 30 05 06 03 <OID> 03 21 00 || key
const unsigned char kX25519SpkiPrefix[] = {
    0x30, 0x2A, 0x30, 0x05, 0x06, 0x03, 0x2B, 0x65, 0x6E, 0x03, 0x21, 0x00,
};
const unsigned char kEd25519SpkiPrefix[] = {
    0x30, 0x2A, 0x30, 0x05, 0x06, 0x03, 0x2B, 0x65, 0x70, 0x03, 0x21, 0x00,
};

QByteArray wrapRfc8410(const unsigned char* prefix, int prefixLen, const QByteArray& raw32) {
    QByteArray der(prefixLen + 32, 0);
    std::memcpy(der.data(), prefix, size_t(prefixLen));
    std::memcpy(der.data() + prefixLen, raw32.constData(), 32);
    return der;
}

// Chave privada crua (32) → EVP_PKEY via parser PKCS#8 DER.
EVP_PKEY* privateKeyFromRaw(const unsigned char* pkcs8Prefix, int prefixLen,
                            const QByteArray& raw32) {
    const QByteArray der = wrapRfc8410(pkcs8Prefix, prefixLen, raw32);
    const unsigned char* p = reinterpret_cast<const unsigned char*>(der.constData());
    return d2i_AutoPrivateKey(nullptr, &p, long(der.size()));
}

// Chave pública crua (32) → EVP_PKEY via parser SPKI DER.
EVP_PKEY* publicKeyFromRaw(const unsigned char* spkiPrefix, int prefixLen,
                           const QByteArray& raw32) {
    const QByteArray der = wrapRfc8410(spkiPrefix, prefixLen, raw32);
    const unsigned char* p = reinterpret_cast<const unsigned char*>(der.constData());
    return d2i_PUBKEY(nullptr, &p, long(der.size()));
}

// EVP_PKEY → pública crua (32): i2d_PUBKEY emite o SPKI RFC 8410 completo
// (exatamente 44 bytes para X25519/Ed25519 — os 32 finais são a chave).
QByteArray rawPublicFromKey(EVP_PKEY* key) {
    if (!key) return QByteArray();
    unsigned char* der = nullptr;
    const int derLen = i2d_PUBKEY(key, &der);
    QByteArray pub;
    if (der && derLen == 44)
        pub = QByteArray(reinterpret_cast<const char*>(der + 44 - 32), 32);
    OPENSSL_free(der);
    return pub;
}

} // namespace

// ------------------------------------------------------------------ X25519

bool generateDhKeyPair(DhKeyPair& out) {
    QByteArray priv(32, 0);
    if (RAND_bytes(reinterpret_cast<unsigned char*>(priv.data()), 32) != 1)
        return false;
    // pública = privada · G — derivada pelo próprio par de curvas, nunca
    // transmitida em claro na geração.
    const QByteArray pub = dhPublicFromPrivate(priv);
    if (pub.size() != 32) return false;
    out.priv = priv;
    out.pub = pub;
    return true;
}

QByteArray x25519SharedSecret(const QByteArray& myPriv, const QByteArray& theirPub) {
    if (myPriv.size() != 32 || theirPub.size() != 32) return QByteArray();
    EVP_PKEY* privKey = privateKeyFromRaw(kX25519Pkcs8Prefix,
                                           int(sizeof(kX25519Pkcs8Prefix)), myPriv);
    EVP_PKEY* peerKey = publicKeyFromRaw(kX25519SpkiPrefix,
                                         int(sizeof(kX25519SpkiPrefix)), theirPub);
    EVP_PKEY_CTX* ctx = privKey ? EVP_PKEY_CTX_new(privKey, nullptr) : nullptr;
    QByteArray secret;
    if (privKey && peerKey && ctx
            && EVP_PKEY_derive_init(ctx) == 1
            && EVP_PKEY_derive_set_peer(ctx, peerKey) == 1) {
        size_t len = 0;
        if (EVP_PKEY_derive(ctx, nullptr, &len) == 1 && len == 32) {
            secret.resize(32);
            if (EVP_PKEY_derive(ctx, reinterpret_cast<unsigned char*>(secret.data()), &len) != 1
                    || len != 32)
                secret.clear();
        }
    }
    EVP_PKEY_CTX_free(ctx);
    EVP_PKEY_free(peerKey);
    EVP_PKEY_free(privKey);
    return secret;
}

QByteArray dhPublicFromPrivate(const QByteArray& priv) {
    if (priv.size() != 32) return QByteArray();
    EVP_PKEY* key = privateKeyFromRaw(kX25519Pkcs8Prefix,
                                       int(sizeof(kX25519Pkcs8Prefix)), priv);
    const QByteArray pub = rawPublicFromKey(key);
    EVP_PKEY_free(key);
    return pub;
}

// ------------------------------------------------------------- HKDF-SHA256

QByteArray hkdfSha256(const QByteArray& ikm, const QByteArray& salt,
                      const QByteArray& info, int length) {
    if (length <= 0 || length > 255 * 32 || ikm.isEmpty()) return QByteArray();
    // Extract: PRK = HMAC(salt, IKM)
    const QByteArray prk = hmacSha256(salt.isEmpty() ? QByteArray(32, 0) : salt, ikm);
    if (prk.isEmpty()) return QByteArray();
    // Expand: T(1) = HMAC(PRK, info|0x01); T(n) = HMAC(PRK, T(n-1)|info|n)
    QByteArray out;
    out.reserve(length);
    QByteArray t;
    int remaining = length;
    for (unsigned char counter = 1; out.size() < length && counter != 0; ++counter) {
        QByteArray block = t;
        block.append(info);
        block.append(char(counter));
        t = hmacSha256(prk, block);
        if (t.isEmpty()) return QByteArray();
        out.append(t.left(qMin(remaining, t.size())));
        remaining = length - out.size();
    }
    out.resize(length);
    return out;
}

// ----------------------------------------------------------- AES-256-GCM

bool aeadSeal(const QByteArray& key, const QByteArray& nonce, const QByteArray& aad,
              const QByteArray& plain, QByteArray& out) {
    out.clear();
    if (key.size() != 32 || nonce.size() != 12) return false;
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return false;
    QByteArray ct(plain.size(), 0);
    QByteArray tag(16, 0);
    int outLen = 0;
    int total = 0;
    bool ok = EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1
        && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, 12, nullptr) == 1
        && EVP_EncryptInit_ex(ctx, nullptr, nullptr,
            reinterpret_cast<const unsigned char*>(key.constData()),
            reinterpret_cast<const unsigned char*>(nonce.constData())) == 1;
    if (ok && !aad.isEmpty()) {
        ok = EVP_EncryptUpdate(ctx, nullptr, &outLen,
            reinterpret_cast<const unsigned char*>(aad.constData()), int(aad.size())) == 1;
    }
    if (ok) {
        ok = plain.isEmpty() || EVP_EncryptUpdate(ctx,
            reinterpret_cast<unsigned char*>(ct.data()), &outLen,
            reinterpret_cast<const unsigned char*>(plain.constData()), int(plain.size())) == 1;
        total = qMax(0, outLen);
    }
    if (ok) ok = EVP_EncryptFinal_ex(ctx, reinterpret_cast<unsigned char*>(ct.data()) + total, &outLen) == 1;
    if (ok) ok = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_GET_TAG, 16, tag.data()) == 1;
    EVP_CIPHER_CTX_free(ctx);
    if (!ok) return false;
    out.reserve(ct.size() + tag.size());
    out.append(ct);
    out.append(tag);
    return true;
}

bool aeadOpen(const QByteArray& key, const QByteArray& nonce, const QByteArray& aad,
              const QByteArray& ctTag, QByteArray& out) {
    out.clear();
    if (key.size() != 32 || nonce.size() != 12 || ctTag.size() < 16) return false;
    const int ctLen = int(ctTag.size()) - 16;
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return false;
    QByteArray plain(ctLen, 0);
    int outLen = 0;
    int total = 0;
    bool ok = EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1
        && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, 12, nullptr) == 1
        && EVP_DecryptInit_ex(ctx, nullptr, nullptr,
            reinterpret_cast<const unsigned char*>(key.constData()),
            reinterpret_cast<const unsigned char*>(nonce.constData())) == 1;
    if (ok && !aad.isEmpty()) {
        ok = EVP_DecryptUpdate(ctx, nullptr, &outLen,
            reinterpret_cast<const unsigned char*>(aad.constData()), int(aad.size())) == 1;
    }
    if (ok) {
        ok = ctLen == 0 || EVP_DecryptUpdate(ctx,
            reinterpret_cast<unsigned char*>(plain.data()), &outLen,
            reinterpret_cast<const unsigned char*>(ctTag.constData()), ctLen) == 1;
        total = qMax(0, outLen);
    }
    if (ok) ok = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_TAG, 16,
        const_cast<char*>(ctTag.constData() + ctLen)) == 1;
    if (ok) ok = EVP_DecryptFinal_ex(ctx, reinterpret_cast<unsigned char*>(plain.data()) + total, &outLen) == 1;
    EVP_CIPHER_CTX_free(ctx);
    if (!ok) return false; // tag inválida ou ciphertext adulterado
    plain.resize(total);
    out = plain;
    return true;
}

// ------------------------------------------------------------- Envelope e2e_key

QByteArray envelopeWrap(const QByteArray& recipientDhPub, const QByteArray& aad,
                        const QByteArray& plain) {
    DhKeyPair eph;
    if (!generateDhKeyPair(eph)) return QByteArray();
    const QByteArray shared = x25519SharedSecret(eph.priv, recipientDhPub);
    if (shared.isEmpty()) return QByteArray();
    // O ECDH já vincula o envelope ao destinatário (só ele deriva o segredo);
    // o HKDF vincula ao domínio do protocolo (AAD).
    const QByteArray wrapKey = hkdfSha256(shared, QByteArray(aad), QByteArray(aad), 32);
    if (wrapKey.size() != 32) return QByteArray();
    const QByteArray nonce = randomBytes(12);
    QByteArray ct;
    if (!aeadSeal(wrapKey, nonce, aad, plain, ct)) return QByteArray();
    QByteArray out;
    out.reserve(32 + 12 + ct.size());
    out.append(eph.pub);
    out.append(nonce);
    out.append(ct);
    return out;
}

QByteArray envelopeUnwrap(const QByteArray& myDhPriv, const QByteArray& aad,
                          const QByteArray& envelope) {
    if (myDhPriv.size() != 32 || envelope.size() < 32 + 12 + 16) return QByteArray();
    const QByteArray ephPub = envelope.left(32);
    const QByteArray nonce = envelope.mid(32, 12);
    const QByteArray ctTag = envelope.mid(32 + 12);
    const QByteArray shared = x25519SharedSecret(myDhPriv, ephPub);
    if (shared.isEmpty()) return QByteArray();
    const QByteArray wrapKey = hkdfSha256(shared, QByteArray(aad), QByteArray(aad), 32);
    QByteArray plain;
    if (wrapKey.size() != 32 || !aeadOpen(wrapKey, nonce, aad, ctTag, plain))
        return QByteArray();
    return plain;
}

// ---------------------------------------------------------- Chave par-a-par

namespace {
QByteArray pairwiseKey(const QByteArray& myPriv, const QByteArray& theirPub,
                       const QByteArray& domain) {
    const QByteArray shared = x25519SharedSecret(myPriv, theirPub);
    if (shared.isEmpty()) return QByteArray();
    return hkdfSha256(shared, QByteArray(domain), QByteArray(domain), 32);
}
} // namespace

QByteArray pairwiseEncrypt(const QByteArray& myDhPriv, const QByteArray& theirDhPub,
                           const QByteArray& domain, const QByteArray& plain) {
    const QByteArray key = pairwiseKey(myDhPriv, theirDhPub, domain);
    if (key.size() != 32) return QByteArray();
    const QByteArray nonce = randomBytes(12);
    QByteArray ct;
    if (!aeadSeal(key, nonce, domain, plain, ct)) return QByteArray();
    QByteArray out;
    out.reserve(12 + ct.size());
    out.append(nonce);
    out.append(ct);
    return out;
}

QByteArray pairwiseDecrypt(const QByteArray& myDhPriv, const QByteArray& theirDhPub,
                           const QByteArray& domain, const QByteArray& blob) {
    if (blob.size() < 12 + 16) return QByteArray();
    const QByteArray key = pairwiseKey(myDhPriv, theirDhPub, domain);
    if (key.size() != 32) return QByteArray();
    const QByteArray nonce = blob.left(12);
    const QByteArray ctTag = blob.mid(12);
    QByteArray plain;
    if (!aeadOpen(key, nonce, domain, ctTag, plain)) return QByteArray();
    return plain;
}

// --------------------------------------------------------------- Ed25519

QByteArray ed25519Sign(const QByteArray& privMaterial, const QByteArray& msg) {
    if (privMaterial.isEmpty() || msg.isEmpty()) return QByteArray();
    EVP_PKEY* key = nullptr;
    if (privMaterial.size() == 32) {
        // seed crua → PKCS#8 mínimo (mesmo embrulho do IdentityDialog).
        key = privateKeyFromRaw(kEd25519Pkcs8Prefix,
                                int(sizeof(kEd25519Pkcs8Prefix)), privMaterial);
    } else {
        const unsigned char* p = reinterpret_cast<const unsigned char*>(privMaterial.constData());
        key = d2i_AutoPrivateKey(nullptr, &p, privMaterial.size());
    }
    if (!key) return QByteArray();
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    QByteArray sig(64, 0);
    size_t sigLen = sig.size();
    const bool ok = ctx
        && EVP_DigestSignInit(ctx, nullptr, nullptr, nullptr, key) == 1
        && EVP_DigestSign(ctx, reinterpret_cast<unsigned char*>(sig.data()), &sigLen,
                          reinterpret_cast<const unsigned char*>(msg.constData()),
                          size_t(msg.size())) == 1;
    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(key);
    if (!ok) return QByteArray();
    sig.resize(int(sigLen));
    return sig;
}

bool ed25519Verify(const QByteArray& pubSpkiDer, const QByteArray& msg, const QByteArray& sig) {
    if (pubSpkiDer.isEmpty() || msg.isEmpty() || sig.isEmpty()) return false;
    const unsigned char* p = reinterpret_cast<const unsigned char*>(pubSpkiDer.constData());
    EVP_PKEY* key = d2i_PUBKEY(nullptr, &p, pubSpkiDer.size());
    if (!key) return false;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    const bool ok = ctx
        && EVP_DigestVerifyInit(ctx, nullptr, nullptr, nullptr, key) == 1
        && EVP_DigestVerify(ctx,
                            reinterpret_cast<const unsigned char*>(sig.constData()),
                            size_t(sig.size()),
                            reinterpret_cast<const unsigned char*>(msg.constData()),
                            size_t(msg.size())) == 1;
    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(key);
    return ok;
}

QByteArray dhBindingMessage(const QByteArray& dhPub) {
    QByteArray m;
    m.reserve(int(sizeof(kDhBindingDomain)) + dhPub.size());
    m.append(kDhBindingDomain, int(sizeof(kDhBindingDomain)) - 1); // sem NUL
    m.append(dhPub);
    return m;
}

bool verifyDhBinding(const QByteArray& idPub, const QByteArray& dhPub, const QByteArray& dhSig) {
    return ed25519Verify(idPub, dhBindingMessage(dhPub), dhSig);
}

// ------------------------------------------------------------- Utilidades

QByteArray randomBytes(int n) {
    QByteArray out(n, 0);
    if (n <= 0 || RAND_bytes(reinterpret_cast<unsigned char*>(out.data()), n) != 1)
        return QByteArray();
    return out;
}

QByteArray sha256(const QByteArray& data) {
    return QCryptographicHash::hash(data, QCryptographicHash::Sha256);
}

QString uidForIdPub(const QByteArray& idPubSpkiDer) {
    return QString::fromLatin1(sha256(idPubSpkiDer).toBase64());
}

QString sasCode(const QByteArray& idPubA, const QByteArray& idPubB) {
    if (idPubA.isEmpty() || idPubB.isEmpty()) return QString();
    // Ordem canônica: ambas as pontas ordenam as MESMAS chaves da mesma
    // forma, sem saber quem é "eu" e quem é "o outro".
    const QByteArray lo = idPubA < idPubB ? idPubA : idPubB;
    const QByteArray hi = idPubA < idPubB ? idPubB : idPubA;
    QByteArray input;
    input.append("HALLA-SAS-V1");
    input.append(lo);
    input.append(hi);
    const QByteArray digest = sha256(input);
    // 30 bits → 9 dígitos: código curto, memorizável, colisão 1/10^9.
    quint32 v = 0;
    for (int i = 0; i < 4; ++i)
        v = (v << 8) | quint8(digest[digest.size() - 1 - i]);
    v %= 1'000'000'000u;
    const QString digits = QString::number(v).rightJustified(9, '0');
    return QStringLiteral("%1 %2 %3")
        .arg(digits.left(3), digits.mid(3, 3), digits.right(3));
}

#pragma GCC diagnostic pop

} // namespace E2ee

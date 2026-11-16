#include "E2eeCrypto.h"

#include <QCryptographicHash>
#include <QtGlobal>

#include <cstring>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/x509.h>

// Cross-library ABI safety notes:
// Windows WebRTC builds compile against OpenSSL 3 headers while linking against
// BoringSSL embedded in webrtc.lib:
//   1. Numeric NID constants (e.g., NID_X25519 / NID_ED25519) differ between
//      OpenSSL 3 and BoringSSL, so all keys are loaded via OID-tagged RFC 8410 DER.
//   2. OpenSSL's HMAC() / HMAC_Init_ex() take `int key_len`, whereas BoringSSL
//      declares `size_t key_len`. HMAC-SHA256 is therefore implemented directly
//      over EVP_DigestInit_ex / EVP_DigestUpdate / EVP_DigestFinal_ex, whose
//      signatures match byte-for-byte in both libraries.

namespace E2ee {

const char kDomainKeyWrap[]   = "HALLA-E2EKEY-V1";
const char kDomainChat[]      = "HALLA-CHAT-V1";
const char kDomainPoke[]      = "HALLA-POKE-V1";
const char kDomainOffline[]   = "HALLA-OFFLINE-V1";
const char kDhBindingDomain[] = "HALLA-DH-V1";

namespace {

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"

void cleanseBytes(QByteArray& buf) {
    if (!buf.isEmpty()) {
        OPENSSL_cleanse(buf.data(), size_t(buf.size()));
        buf.clear();
    }
}

// RFC 2104 HMAC-SHA256 built solely on ABI-identical EVP_Digest* primitives.
QByteArray hmacSha256(const QByteArray& key, const QByteArray& data) {
    unsigned char block[64];
    std::memset(block, 0, sizeof(block));

    if (key.size() > 64) {
        EVP_MD_CTX* kctx = EVP_MD_CTX_new();
        if (!kctx) return QByteArray();
        unsigned int kdLen = 0;
        const bool hashed = EVP_DigestInit_ex(kctx, EVP_sha256(), nullptr) == 1
            && EVP_DigestUpdate(kctx, key.constData(), size_t(key.size())) == 1
            && EVP_DigestFinal_ex(kctx, block, &kdLen) == 1;
        EVP_MD_CTX_free(kctx);
        if (!hashed) {
            OPENSSL_cleanse(block, sizeof(block));
            return QByteArray();
        }
    } else if (!key.isEmpty()) {
        std::memcpy(block, key.constData(), size_t(key.size()));
    }

    unsigned char ipad[64];
    unsigned char opad[64];
    for (int i = 0; i < 64; ++i) {
        ipad[i] = block[i] ^ 0x36;
        opad[i] = block[i] ^ 0x5c;
    }
    OPENSSL_cleanse(block, sizeof(block));

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) {
        OPENSSL_cleanse(ipad, sizeof(ipad));
        OPENSSL_cleanse(opad, sizeof(opad));
        return QByteArray();
    }

    unsigned char inner[32];
    unsigned int innerLen = 0;
    QByteArray out(32, 0);
    unsigned int outLen = 0;

    bool ok = EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) == 1
        && EVP_DigestUpdate(ctx, ipad, sizeof(ipad)) == 1
        && (data.isEmpty() || EVP_DigestUpdate(ctx, data.constData(), size_t(data.size())) == 1)
        && EVP_DigestFinal_ex(ctx, inner, &innerLen) == 1;
    if (ok) {
        ok = EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) == 1
            && EVP_DigestUpdate(ctx, opad, sizeof(opad)) == 1
            && EVP_DigestUpdate(ctx, inner, sizeof(inner)) == 1
            && EVP_DigestFinal_ex(ctx, reinterpret_cast<unsigned char*>(out.data()), &outLen) == 1;
    }

    EVP_MD_CTX_free(ctx);
    OPENSSL_cleanse(ipad, sizeof(ipad));
    OPENSSL_cleanse(opad, sizeof(opad));
    OPENSSL_cleanse(inner, sizeof(inner));

    if (!ok || outLen != 32) {
        cleanseBytes(out);
        return QByteArray();
    }
    return out;
}

// RFC 8410 minimal PKCS#8 and SubjectPublicKeyInfo prefixes for X25519 and Ed25519.
const unsigned char kX25519Pkcs8Prefix[] = {
    0x30, 0x2E, 0x02, 0x01, 0x00, 0x30, 0x05, 0x06, 0x03, 0x2B, 0x65, 0x6E,
    0x04, 0x22, 0x04, 0x20,
};
const unsigned char kEd25519Pkcs8Prefix[] = {
    0x30, 0x2E, 0x02, 0x01, 0x00, 0x30, 0x05, 0x06, 0x03, 0x2B, 0x65, 0x70,
    0x04, 0x22, 0x04, 0x20,
};
const unsigned char kX25519SpkiPrefix[] = {
    0x30, 0x2A, 0x30, 0x05, 0x06, 0x03, 0x2B, 0x65, 0x6E, 0x03, 0x21, 0x00,
};

QByteArray wrapRfc8410(const unsigned char* prefix, int prefixLen, const QByteArray& raw32) {
    QByteArray der(prefixLen + 32, 0);
    std::memcpy(der.data(), prefix, size_t(prefixLen));
    std::memcpy(der.data() + prefixLen, raw32.constData(), 32);
    return der;
}

EVP_PKEY* privateKeyFromRaw(const unsigned char* pkcs8Prefix, int prefixLen,
                            const QByteArray& raw32) {
    QByteArray der = wrapRfc8410(pkcs8Prefix, prefixLen, raw32);
    const unsigned char* p = reinterpret_cast<const unsigned char*>(der.constData());
    EVP_PKEY* key = d2i_AutoPrivateKey(nullptr, &p, long(der.size()));
    cleanseBytes(der);
    return key;
}

EVP_PKEY* publicKeyFromRaw(const unsigned char* spkiPrefix, int prefixLen,
                           const QByteArray& raw32) {
    const QByteArray der = wrapRfc8410(spkiPrefix, prefixLen, raw32);
    const unsigned char* p = reinterpret_cast<const unsigned char*>(der.constData());
    return d2i_PUBKEY(nullptr, &p, long(der.size()));
}

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

QByteArray pairwiseKey(const QByteArray& myPriv, const QByteArray& theirPub,
                       const QByteArray& domain) {
    QByteArray shared = x25519SharedSecret(myPriv, theirPub);
    if (shared.isEmpty()) return QByteArray();
    QByteArray derived = hkdfSha256(shared, QByteArray(domain), QByteArray(domain), 32);
    cleanseBytes(shared);
    return derived;
}

} // namespace

bool generateDhKeyPair(DhKeyPair& out) {
    QByteArray priv(32, 0);
    if (RAND_bytes(reinterpret_cast<unsigned char*>(priv.data()), 32) != 1)
        return false;
    const QByteArray pub = dhPublicFromPrivate(priv);
    if (pub.size() != 32) {
        cleanseBytes(priv);
        return false;
    }
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
                cleanseBytes(secret);
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

QByteArray hkdfSha256(const QByteArray& ikm, const QByteArray& salt,
                      const QByteArray& info, int length) {
    if (length <= 0 || length > 255 * 32 || ikm.isEmpty()) return QByteArray();
    QByteArray prk = hmacSha256(salt.isEmpty() ? QByteArray(32, 0) : salt, ikm);
    if (prk.isEmpty()) return QByteArray();

    QByteArray out;
    out.reserve(length);
    QByteArray t;
    int remaining = length;
    for (unsigned char counter = 1; out.size() < length && counter != 0; ++counter) {
        QByteArray block = t;
        block.append(info);
        block.append(char(counter));
        cleanseBytes(t);
        t = hmacSha256(prk, block);
        cleanseBytes(block);
        if (t.isEmpty()) {
            cleanseBytes(prk);
            cleanseBytes(out);
            return QByteArray();
        }
        out.append(t.left(qMin(remaining, t.size())));
        remaining = length - out.size();
    }
    cleanseBytes(t);
    cleanseBytes(prk);
    out.resize(length);
    return out;
}

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
    if (!ok) {
        cleanseBytes(plain);
        return false;
    }
    plain.resize(total);
    out = plain;
    return true;
}

QByteArray envelopeWrap(const QByteArray& recipientDhPub, const QByteArray& aad,
                        const QByteArray& plain) {
    DhKeyPair eph;
    if (!generateDhKeyPair(eph)) return QByteArray();
    QByteArray shared = x25519SharedSecret(eph.priv, recipientDhPub);
    cleanseBytes(eph.priv);
    if (shared.isEmpty()) return QByteArray();

    QByteArray wrapKey = hkdfSha256(shared, QByteArray(aad), QByteArray(aad), 32);
    cleanseBytes(shared);
    if (wrapKey.size() != 32) {
        cleanseBytes(wrapKey);
        return QByteArray();
    }
    const QByteArray nonce = randomBytes(12);
    QByteArray ct;
    const bool sealed = aeadSeal(wrapKey, nonce, aad, plain, ct);
    cleanseBytes(wrapKey);
    if (!sealed) return QByteArray();

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
    QByteArray shared = x25519SharedSecret(myDhPriv, ephPub);
    if (shared.isEmpty()) return QByteArray();

    QByteArray wrapKey = hkdfSha256(shared, QByteArray(aad), QByteArray(aad), 32);
    cleanseBytes(shared);
    QByteArray plain;
    const bool opened = (wrapKey.size() == 32) && aeadOpen(wrapKey, nonce, aad, ctTag, plain);
    cleanseBytes(wrapKey);
    return opened ? plain : QByteArray();
}

QByteArray pairwiseEncrypt(const QByteArray& myDhPriv, const QByteArray& theirDhPub,
                           const QByteArray& domain, const QByteArray& plain) {
    QByteArray key = pairwiseKey(myDhPriv, theirDhPub, domain);
    if (key.size() != 32) {
        cleanseBytes(key);
        return QByteArray();
    }
    const QByteArray nonce = randomBytes(12);
    QByteArray ct;
    const bool sealed = aeadSeal(key, nonce, domain, plain, ct);
    cleanseBytes(key);
    if (!sealed) return QByteArray();

    QByteArray out;
    out.reserve(12 + ct.size());
    out.append(nonce);
    out.append(ct);
    return out;
}

QByteArray pairwiseDecrypt(const QByteArray& myDhPriv, const QByteArray& theirDhPub,
                           const QByteArray& domain, const QByteArray& blob) {
    if (blob.size() < 12 + 16) return QByteArray();
    QByteArray key = pairwiseKey(myDhPriv, theirDhPub, domain);
    if (key.size() != 32) {
        cleanseBytes(key);
        return QByteArray();
    }
    const QByteArray nonce = blob.left(12);
    const QByteArray ctTag = blob.mid(12);
    QByteArray plain;
    const bool opened = aeadOpen(key, nonce, domain, ctTag, plain);
    cleanseBytes(key);
    return opened ? plain : QByteArray();
}

QByteArray ed25519Sign(const QByteArray& privMaterial, const QByteArray& msg) {
    if (privMaterial.isEmpty() || msg.isEmpty()) return QByteArray();
    EVP_PKEY* key = nullptr;
    if (privMaterial.size() == 32) {
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
    m.append(kDhBindingDomain, int(sizeof(kDhBindingDomain)) - 1);
    m.append(dhPub);
    return m;
}

bool verifyDhBinding(const QByteArray& idPub, const QByteArray& dhPub, const QByteArray& dhSig) {
    return ed25519Verify(idPub, dhBindingMessage(dhPub), dhSig);
}

QByteArray keyEnvelopeSignaturePayload(int toSessionId, const QByteArray& envelope) {
    QByteArray m;
    m.reserve(int(sizeof(kDomainKeyWrap)) + 4 + envelope.size());
    m.append(kDomainKeyWrap, int(sizeof(kDomainKeyWrap)) - 1);
    const quint32 beTo =
        ((quint32(toSessionId) & 0x000000FFu) << 24) |
        ((quint32(toSessionId) & 0x0000FF00u) << 8)  |
        ((quint32(toSessionId) & 0x00FF0000u) >> 8)  |
        ((quint32(toSessionId) & 0xFF000000u) >> 24);
    m.append(reinterpret_cast<const char*>(&beTo), 4);
    m.append(envelope);
    return m;
}

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
    const QByteArray lo = idPubA < idPubB ? idPubA : idPubB;
    const QByteArray hi = idPubA < idPubB ? idPubB : idPubA;
    QByteArray input;
    input.append("HALLA-SAS-V1");
    input.append(lo);
    input.append(hi);
    const QByteArray digest = sha256(input);
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

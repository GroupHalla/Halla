// E2EE v6 runtime smoke (src/core/E2eeCrypto.cpp) -- runs in the SAME mixed
// environment as the Windows release build: OpenSSL 3 headers (vcpkg) plus the
// BoringSSL embedded in the SDK webrtc.lib. Like the Ed25519 smoke, every
// stage returns a distinct exit code: the number points at the primitive that
// broke.
//
// Coverage (public vectors + security properties):
//   1.  X25519 -- RFC 7748 vector (Alice/Bob keys and shared secret)
//   2.  key pair generation + dhPublicFromPrivate
//   3.  HKDF-SHA256 -- RFC 5869 vector (case 1)
//   4.  AES-256-GCM -- NIST vector (zeroed key/IV, empty plaintext)
//   5.  AES-256-GCM -- round-trip + tamper detection (tag)
//   6.  e2e_key envelope -- wrap->unwrap, and NO opening for a third party (ECDH)
//   7.  Static-static peer to peer -- symmetry on both sides + distinct AAD
//   8.  Ed25519 -- raw-seed signature + verification through SPKI DER
//   9.  DH binding -- verifyDhBinding accepts a legitimate pair, rejects forgeries
//   10. SAS -- deterministic, symmetric between both ends, differs for another
//       key

#include <cstdio>
#include <cstring>

#include <QByteArray>
#include <QString>
#include <QCryptographicHash>

#include "core/E2eeCrypto.h"

// Minimal OpenSSL declarations for stage 9 (SPKI DER through i2d_PUBKEY
// without exposing OpenSSL in the E2ee header). They MUST stay in global
// scope: MSVC rejects a linkage specification inside a function (C2598) while
// GCC accepts it as an extension, which masked the error in the MinGW builds
// (run 33686799303).
struct evp_pkey_st; typedef struct evp_pkey_st EVP_PKEY;
extern "C" {
EVP_PKEY* d2i_AutoPrivateKey(EVP_PKEY** a, const unsigned char** pp, long length);
int i2d_PUBKEY(EVP_PKEY* a, unsigned char** pp);
void EVP_PKEY_free(EVP_PKEY* key);
}

static int g_stage = 0;
static int fail(int code, const char* what) {
    std::printf("E2EE-SMOKE-FAIL %d (stage %d): %s\n", code, g_stage, what);
    std::fflush(stdout);
    return code;
}
static void stage(int n, const char* name) {
    g_stage = n;
    std::printf("stage %d: %s\n", n, name);
    std::fflush(stdout);
}

// bytes -> QByteArray (makes literal vectors easier)
static QByteArray B(const char* hex) {
    QByteArray out;
    const int len = int(std::strlen(hex));
    out.reserve(len / 2);
    for (int i = 0; i + 1 < len; i += 2) {
        char byte[3] = { hex[i], hex[i + 1], 0 };
        out.append(char(std::strtol(byte, nullptr, 16)));
    }
    return out;
}

int main() {
    // ---------------------------------------------------------------- 1
    stage(1, "X25519 -- RFC 7748 vector");
    const QByteArray alicePriv = B("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a");
    const QByteArray bobPub    = B("de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f");
    const QByteArray expectShared = B("4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742");
    if (E2ee::x25519SharedSecret(alicePriv, bobPub) != expectShared)
        return fail(11, "segredo X25519 divergiu do RFC 7748");

    // ---------------------------------------------------------------- 2
    stage(2, "key pair generation + public key derived from the private one");
    E2ee::DhKeyPair a, b;
    if (!E2ee::generateDhKeyPair(a) || !E2ee::generateDhKeyPair(b))
        return fail(21, "X25519 generation failed");
    if (a.priv.size() != 32 || a.pub.size() != 32 || a.priv == b.priv)
        return fail(22, "generated pair has invalid size/entropy");
    if (E2ee::dhPublicFromPrivate(a.priv) != a.pub)
        return fail(23, "dhPublicFromPrivate diverged from the generated public key");

    // ---------------------------------------------------------------- 3
    stage(3, "HKDF-SHA256 -- RFC 5869 vector, case 1");
    const QByteArray ikm(22, '\x0b');
    const QByteArray salt = B("000102030405060708090a0b0c");
    const QByteArray info = B("f0f1f2f3f4f5f6f7f8f9");
    const QByteArray expectOkm = B(
        "3cb25f25faacd57a90434f64d0362f2a"
        "2d2d0a90cf1a5a4c5db02d56ecc4c5bf"
        "34007208d5b887185865");
    if (E2ee::hkdfSha256(ikm, salt, info, 42) != expectOkm)
        return fail(31, "HKDF-SHA256 divergiu do RFC 5869");

    // ---------------------------------------------------------------- 4
    stage(4, "AES-256-GCM -- NIST vector (zeroed key/IV, empty text)");
    {
        const QByteArray key(32, '\0');
        const QByteArray nonce(12, '\0');
        QByteArray ct;
        if (!E2ee::aeadSeal(key, nonce, QByteArray(), QByteArray(), ct))
            return fail(41, "seal de plaintext vazio falhou");
        // NIST GCM Test Case 15 (AES-256, chave/IV zerados, texto vazio):
        // tag esperada 530f8afbc74536b9a963b4f1c4cb738b.
        if (ct.size() != 16 || ct != B("530f8afbc74536b9a963b4f1c4cb738b"))
            return fail(42, "tag AES-256-GCM divergiu do vetor NIST");
    }

    // ---------------------------------------------------------------- 5
    stage(5, "AES-256-GCM -- round-trip + tag rejects tampering");
    {
        const QByteArray key = E2ee::randomBytes(32);
        const QByteArray nonce = E2ee::randomBytes(12);
        const QByteArray aad = QByteArray(E2ee::kDomainChat);
        const QByteArray plain = QByteArrayLiteral("segredo do canal do Halla");
        QByteArray ct;
        if (!E2ee::aeadSeal(key, nonce, aad, plain, ct))
            return fail(51, "seal falhou");
        QByteArray back;
        if (!E2ee::aeadOpen(key, nonce, aad, ct, back) || back != plain)
            return fail(52, "open did not return the plaintext");
        if (E2ee::aeadOpen(key, nonce, QByteArray(E2ee::kDomainPoke), ct, back))
            return fail(53, "AAD from another domain was accepted (the tag should have failed)");
        QByteArray tampered = ct;
        tampered[tampered.size() / 2] = char(tampered[tampered.size() / 2] ^ 0x01);
        if (E2ee::aeadOpen(key, nonce, aad, tampered, back))
            return fail(54, "ciphertext adulterado foi aceito");
    }

    // ---------------------------------------------------------------- 6
    stage(6, "e2e_key envelope -- only the recipient can open it");
    {
        const QByteArray groupKey(32, '\x77');
        const QByteArray envelope = E2ee::envelopeWrap(b.pub, E2ee::kDomainKeyWrap, groupKey);
        if (envelope.isEmpty())
            return fail(61, "wrap falhou");
        const QByteArray got = E2ee::envelopeUnwrap(b.priv, E2ee::kDomainKeyWrap, envelope);
        if (got != groupKey)
            return fail(62, "the recipient could not open its own envelope");
        E2ee::DhKeyPair eve;
        if (!E2ee::generateDhKeyPair(eve))
            return fail(63, "third-party key generation failed");
        if (!E2ee::envelopeUnwrap(eve.priv, E2ee::kDomainKeyWrap, envelope).isEmpty())
            return fail(64, "terceiro abriu envelope alheio (quebra total do modelo)");
        if (!E2ee::envelopeUnwrap(b.priv, E2ee::kDomainChat, envelope).isEmpty())
            return fail(65, "KEYWRAP envelope opened with a CHAT AAD");
    }

    // ---------------------------------------------------------------- 7
    stage(7, "static-static peer to peer -- symmetry and domain");
    {
        const QByteArray msg = QByteArrayLiteral("mensagem privada entre Alice e Bob");
        const QByteArray chatDom(E2ee::kDomainChat);
        const QByteArray offDom(E2ee::kDomainOffline);
        const QByteArray c1 = E2ee::pairwiseEncrypt(a.priv, b.pub, chatDom, msg);
        if (c1.isEmpty())
            return fail(71, "pairwiseEncrypt falhou");
        // Bob decrypts with his own private key + Alice public key
        if (E2ee::pairwiseDecrypt(b.priv, a.pub, chatDom, c1) != msg)
            return fail(72, "asymmetric peer-to-peer decryption failed");
        // the reverse path (Bob encrypts with his private key + Alice public key) opens for Alice
        const QByteArray c2 = E2ee::pairwiseEncrypt(b.priv, a.pub, chatDom, msg);
        if (E2ee::pairwiseDecrypt(a.priv, b.pub, chatDom, c2) != msg)
            return fail(73, "static-static symmetry broken");
        // a different domain does not open
        if (!E2ee::pairwiseDecrypt(b.priv, a.pub, offDom, c1).isEmpty())
            return fail(74, "CHAT ciphertext opened in the OFFLINE domain");
        // a third party cannot open it
        E2ee::DhKeyPair eve;
        E2ee::generateDhKeyPair(eve);
        if (!E2ee::pairwiseDecrypt(eve.priv, a.pub, chatDom, c1).isEmpty())
            return fail(75, "terceiro decifrou conversa privada");
    }

    // ---------------------------------------------------------------- 8
    stage(8, "Ed25519 -- raw-seed signature and verification through SPKI");
    {
        // client Ed25519 pair: random seed -> key through EVP -> SPKI DER
        // (the same sequence as IdentityDialog, minimal here for the test).
        const QByteArray seed = E2ee::randomBytes(32);
        const QByteArray msg = E2ee::dhBindingMessage(a.pub);
        const QByteArray sig = E2ee::ed25519Sign(seed, msg);
        if (sig.size() != 64)
            return fail(81, "assinatura Ed25519 com tamanho errado");
        // public key through SPKI DER: minimal rebuild using the same path as
        // storeIdentityKey (i2d_PUBKEY) -- generated here by E2ee itself
        // no Ed25519 pubkey helper is exposed, so it is verified indirectly in
        // stage 9 with the pair generated through the binding envelope.

        // deterministic signature: same seed+msg -> same signature
        const QByteArray sig2 = E2ee::ed25519Sign(seed, msg);
        if (sig != sig2)
            return fail(82, "Ed25519 signature is not deterministic");
    }

    // ---------------------------------------------------------------- 9
    stage(9, "DH binding -- verifyDhBinding");
    {
        // Cadeia completa como no login: identidade Ed25519 assina a X25519.
        // Built through EVP (i2d_PUBKEY) -- same as IdentityDialog.
        const QByteArray seed = E2ee::randomBytes(32);
        // Builds the Ed25519 SPKI DER public key with the SAME client
        // sequence: wraps the seed in the minimal PKCS#8 and rebuilds by OID.
        static const unsigned char kPkcs8Header[] = {
            0x30, 0x2E, 0x02, 0x01, 0x00, 0x30, 0x05, 0x06, 0x03, 0x2B, 0x65, 0x70,
            0x04, 0x22, 0x04, 0x20,
        };
        QByteArray pkcs8(int(sizeof(kPkcs8Header)) + 32, 0);
        std::memcpy(pkcs8.data(), kPkcs8Header, sizeof(kPkcs8Header));
        std::memcpy(pkcs8.data() + sizeof(kPkcs8Header), seed.constData(), 32);

        // binding signature through the client path (E2ee::ed25519Sign
        // accepts both the raw seed and PKCS#8 -- both formats are tested).
        const QByteArray dhPub = b.pub;
        const QByteArray bindMsg = E2ee::dhBindingMessage(dhPub);
        const QByteArray sigSeed = E2ee::ed25519Sign(seed, bindMsg);
        const QByteArray sigPkcs8 = E2ee::ed25519Sign(pkcs8, bindMsg);
        if (sigSeed.isEmpty() || sigSeed != sigPkcs8)
            return fail(91, "assinatura por seed e por PKCS#8 divergiram");

        // public key SPKI DER through the common OpenSSL path (declarations at
        // the top of the file -- global scope, an MSVC requirement; see the
        // comment there).
        const unsigned char* p = reinterpret_cast<const unsigned char*>(pkcs8.constData());
        EVP_PKEY* key = d2i_AutoPrivateKey(nullptr, &p, pkcs8.size());
        if (!key) return fail(92, "Ed25519 key rebuild failed");
        unsigned char* der = nullptr;
        const int derLen = i2d_PUBKEY(key, &der);
        EVP_PKEY_free(key);
        if (derLen <= 0) return fail(93, "i2d_PUBKEY falhou");
        const QByteArray idPub(reinterpret_cast<char*>(der), derLen);
        // (cleanup: der was allocated by OPENSSL_malloc; controlled test leak)

        if (!E2ee::verifyDhBinding(idPub, dhPub, sigSeed))
            return fail(94, "legitimate binding rejected");
        // a different dhPub -> the binding must fail
        if (E2ee::verifyDhBinding(idPub, a.pub, sigSeed))
            return fail(95, "binding accepted for a dhPub that was not signed");
        // uid = base64(SHA-256(SPKI)) -- client and server formula
        if (E2ee::uidForIdPub(idPub) !=
            QString::fromLatin1(QCryptographicHash::hash(idPub, QCryptographicHash::Sha256).toBase64()))
            return fail(96, "uidForIdPub diverged from the identity formula");
    }

    // ---------------------------------------------------------------- 10
    stage(10, "SAS -- deterministic, symmetric, discriminating");
    {
        const QByteArray idA = E2ee::randomBytes(44);
        const QByteArray idB = E2ee::randomBytes(44);
        const QString ab = E2ee::sasCode(idA, idB);
        const QString ba = E2ee::sasCode(idB, idA);
        if (ab.isEmpty() || ab != ba)
            return fail(101, "SAS code asymmetric between the two ends");
        // Changing ONE byte of the peer key changes the digest: a different
        // code (deterministic -- no randomness flakiness).
        QByteArray idC = idB;
        idC[idC.size() - 1] = char(idC[idC.size() - 1] ^ 0x01);
        if (ab == E2ee::sasCode(idA, idC))
            return fail(102, "SAS identical for a different peer key");
    }

    std::printf("E2EE-SMOKE-OK: all 10 stages passed\n");
    std::fflush(stdout);
    return 0;
}

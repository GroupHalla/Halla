// End-to-end encryption primitives for Halla protocol v6.
//
// All content encryption happens strictly on the client using:
//   X25519 (ECDH) + HKDF-SHA256 + AES-256-GCM + Ed25519 (identity/binding signatures).
//
// Backend ABI note:
// Windows WebRTC builds compile against OpenSSL 3 headers while linking against
// BoringSSL inside webrtc.lib. All asymmetric keys are constructed via NID-free
// RFC 8410 DER envelopes (d2i_AutoPrivateKey / d2i_PUBKEY / i2d_PUBKEY) and HMAC
// is implemented directly over EVP_Digest* so function signatures and constants
// match identically across OpenSSL and BoringSSL.
#pragma once

#include <QByteArray>
#include <QString>

namespace E2ee {

// Protocol-separated AAD domain tags.
extern const char kDomainKeyWrap[];
extern const char kDomainChat[];
extern const char kDomainPoke[];
extern const char kDomainOffline[];
extern const char kDhBindingDomain[];

struct DhKeyPair {
    QByteArray priv; // 32-byte raw X25519 private scalar
    QByteArray pub;  // 32-byte raw X25519 public point
};

bool generateDhKeyPair(DhKeyPair& out);
QByteArray x25519SharedSecret(const QByteArray& myPriv, const QByteArray& theirPub);
QByteArray dhPublicFromPrivate(const QByteArray& priv);

// RFC 5869 HKDF-SHA256 (extract + expand, length <= 255 * 32).
QByteArray hkdfSha256(const QByteArray& ikm, const QByteArray& salt,
                      const QByteArray& info, int length);

// AES-256-GCM authenticated encryption (12-byte nonce, 16-byte authentication tag).
bool aeadSeal(const QByteArray& key, const QByteArray& nonce, const QByteArray& aad,
              const QByteArray& plain, QByteArray& out);
bool aeadOpen(const QByteArray& key, const QByteArray& nonce, const QByteArray& aad,
              const QByteArray& ctTag, QByteArray& out);

// Ephemeral-static X25519 key-wrap envelope: ephPub(32) | nonce(12) | ciphertext | tag(16).
QByteArray envelopeWrap(const QByteArray& recipientDhPub, const QByteArray& aad,
                        const QByteArray& plain);
QByteArray envelopeUnwrap(const QByteArray& myDhPriv, const QByteArray& aad,
                          const QByteArray& envelope);

// Static-static X25519 pairwise authenticated encryption: nonce(12) | ciphertext | tag(16).
QByteArray pairwiseEncrypt(const QByteArray& myDhPriv, const QByteArray& theirDhPub,
                           const QByteArray& domain, const QByteArray& plain);
QByteArray pairwiseDecrypt(const QByteArray& myDhPriv, const QByteArray& theirDhPub,
                           const QByteArray& domain, const QByteArray& blob);

// Ed25519 signing (accepts 32-byte raw seed or PKCS#8 DER) and SPKI DER verification.
QByteArray ed25519Sign(const QByteArray& privMaterial, const QByteArray& msg);
bool ed25519Verify(const QByteArray& pubSpkiDer, const QByteArray& msg, const QByteArray& sig);

QByteArray dhBindingMessage(const QByteArray& dhPub);
bool verifyDhBinding(const QByteArray& idPub, const QByteArray& dhPub, const QByteArray& dhSig);

// Signed key-envelope transcript helper for authenticating e2e_key senders.
QByteArray keyEnvelopeSignaturePayload(int toSessionId, const QByteArray& envelope);

QByteArray randomBytes(int n);
QByteArray sha256(const QByteArray& data);
QString sasCode(const QByteArray& idPubA, const QByteArray& idPubB);
QString uidForIdPub(const QByteArray& idPubSpkiDer);

} // namespace E2ee

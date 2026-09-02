// E2eeCrypto — primitivas de criptografia ponta a ponta do Halla v6.
//
// Toda a criptografia de CONTEÚDO acontece aqui, no cliente. O servidor só
// enxerga bytes opacos: ele não gera, não distribui e não decifra chave
// alguma. As chaves de grupo nascem nos clientes (o "mestre" de cada
// componente — menor UID online) e viajam embrulhadas por X25519.
//
// Camadas:
//   X25519 (ECDH) + HKDF-SHA256 + AES-256-GCM + Ed25519 (assinatura/binding)
//
// Compatibilidade de backends: o código compila contra OpenSSL real (Linux,
// MinGW) e contra o BoringSSL embutido no SDK WebRTC (Windows) — ambos
// expõem EVP_PKEY_new_raw_private_key/EVP_PKEY_derive/HMAC/EVP_aes_256_gcm
// com a mesma semântica usada aqui.
//
// Invariantes de segurança:
//   * voz, tela, chat, sussurro, poke e offline NUNCA saem em claro;
//   * cada uso criptográfico tem um domínio AAD próprio (um ciphertext de
//     chat não serve como envelope de chave e vice-versa);
//   * envelopes de chave usam X25519 EFÊMERO (PFS por envelope) — mesmo a
//     revelação futura da chave estática do destinatário não abre envelopes
//     já entregues;
//   * chaves par-a-par (chat privado/poke/offline) são estático-estáticas —
//     os pares persistem nas identidades para que mensagens offline possam
//     ser decifradas independentemente de quem está online.
#pragma once

#include <QByteArray>
#include <QString>

namespace E2ee {

// Domínios AAD — distinguem cada protocolo que compartilha primitivas.
extern const char kDomainKeyWrap[];    // envelopes e2e_key (canal/sussurro)
extern const char kDomainChat[];       // chat (escopo servidor/canal)
extern const char kDomainPoke[];       // poke
extern const char kDomainOffline[];    // mensagem offline
extern const char kDhBindingDomain[];  // prefixo assinado no binding idPub→dhPub

// ------------------------------------------------------------------ X25519
struct DhKeyPair {
    QByteArray priv;   // 32 bytes crus
    QByteArray pub;    // 32 bytes crus
};

// Gera par X25519 a partir do CSPRNG da biblioteca cripto linkada.
bool generateDhKeyPair(DhKeyPair& out);

// Segredo compartilhado ECDH (32 bytes) a partir da minha privada e da
// pública do par. Devolve QByteArray vazio em caso de falha.
QByteArray x25519SharedSecret(const QByteArray& myPriv, const QByteArray& theirPub);

// Pública X25519 derivada da privada — valida pares armazenados.
QByteArray dhPublicFromPrivate(const QByteArray& priv);

// ------------------------------------------------------------- HKDF-SHA256
// HKDF (RFC 5869): extract + expand. `length` ≤ 255×32.
QByteArray hkdfSha256(const QByteArray& ikm, const QByteArray& salt,
                      const QByteArray& info, int length);

// ----------------------------------------------------------- AES-256-GCM
// nonce de 12 bytes, tag de 16. `out` recebe ciphertext||tag (e vazio em
// falha). `aad` autentica o contexto (domínio do protocolo).
bool aeadSeal(const QByteArray& key, const QByteArray& nonce, const QByteArray& aad,
              const QByteArray& plain, QByteArray& out);
bool aeadOpen(const QByteArray& key, const QByteArray& nonce, const QByteArray& aad,
              const QByteArray& ctTag, QByteArray& out);

// ------------------------------------------------------------ Envelope e2e_key
// Layout do envelope: ephPub(32) | nonce(12) | ct | tag(16).
// A chave de cifragem é HKDF(ECDH(efêmera, pública do destinatário)) — só o
// destinatário abre, e a efêmera morre após o wrap (PFS do envelope).
QByteArray envelopeWrap(const QByteArray& recipientDhPub, const QByteArray& aad,
                        const QByteArray& plain);
QByteArray envelopeUnwrap(const QByteArray& myDhPriv, const QByteArray& aad,
                          const QByteArray& envelope);

// ------------------------------------------------------- Chave par-a-par
// Derivação estático-estática: HKDF(ECDH(minhaPriv, públicaDoPar)), com o
// domínio no info — quem não possui o par não deriva a chave. O blob de
// saída é nonce(12)|ct|tag(16). Simétrico: pairwiseEncrypt(aPriv,bPub) é
// aberto por pairwiseDecrypt(bPriv,aPub) e vice-versa.
QByteArray pairwiseEncrypt(const QByteArray& myDhPriv, const QByteArray& theirDhPub,
                           const QByteArray& domain, const QByteArray& plain);
QByteArray pairwiseDecrypt(const QByteArray& myDhPriv, const QByteArray& theirDhPub,
                           const QByteArray& domain, const QByteArray& blob);

// --------------------------------------------------------------- Ed25519
// Assina `msg` com o material da identidade — aceita a seed crua de 32 bytes
// (formato atual do cofre) ou PKCS#8 legado. Devolve 64 bytes ou vazio.
QByteArray ed25519Sign(const QByteArray& privMaterial, const QByteArray& msg);

// Verifica `sig` contra a pública SPKI DER da identidade.
bool ed25519Verify(const QByteArray& pubSpkiDer, const QByteArray& msg, const QByteArray& sig);

// Mensagem do binding: "HALLA-DH-V1" || dhPub — liga a X25519 à Ed25519.
QByteArray dhBindingMessage(const QByteArray& dhPub);
bool verifyDhBinding(const QByteArray& idPub, const QByteArray& dhPub, const QByteArray& dhSig);

// ------------------------------------------------------------- Utilidades
QByteArray randomBytes(int n);
QByteArray sha256(const QByteArray& data);

// Código SAS de verificação de identidade: 9 dígitos derivados do par de
// chaves públicas Ed25519 (ordenado), comparáveis verbalmente entre as duas
// pontas. A ordem canônica garante que ambos os lados derivam o MESMO
// número sem negociação.
QString sasCode(const QByteArray& idPubA, const QByteArray& idPubB);

// UID esperado para uma idPub: base64(SHA-256(SPKI DER)) — igual ao cálculo
// do servidor e do cliente, usado na verificação local do diretório.
QString uidForIdPub(const QByteArray& idPubSpkiDer);

} // namespace E2ee

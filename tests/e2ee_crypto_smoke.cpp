// Smoke de runtime do E2EE v6 (src/core/E2eeCrypto.cpp) — executado no MESMO
// ambiente misto do build de release Windows: headers OpenSSL 3 (vcpkg) +
// BoringSSL embutido no webrtc.lib do SDK. Assim como o Ed25519 smoke, cada
// etapa devolve um código de saída distinto: o número aponta qual primitiva
// quebrou.
//
// Cobertura (vetores públicos + propriedades de segurança):
//   1.  X25519 — vetor do RFC 7748 (chaves de Alice/Bob e segredo compartilhado)
//   2.  geração de par + dhPublicFromPrivate
//   3.  HKDF-SHA256 — vetor do RFC 5869 (caso 1)
//   4.  AES-256-GCM — vetor NIST (chave/IV zerados, plaintext vazio)
//   5.  AES-256-GCM — round-trip + detecção de adulteração (tag)
//   6.  Envelope e2e_key — wrap→unwrap, e NÃO abre para terceiro (ECDH)
//   7.  Par-a-par estático-estático — simetria dos dois lados + AAD distinto
//   8.  Ed25519 — assinatura da seed crua + verificação via SPKI DER
//   9.  Binding DH — verifyDhBinding aceita par legítimo e rejeita forjado
//   10. SAS — determinístico, simétrico entre as pontas, difere p/ outra chave

#include <cstdio>
#include <cstring>

#include <QByteArray>
#include <QString>
#include <QCryptographicHash>

#include "core/E2eeCrypto.h"

static int g_stage = 0;
static int fail(int code, const char* what) {
    std::printf("E2EE-SMOKE-FALHA %d (etapa %d): %s\n", code, g_stage, what);
    std::fflush(stdout);
    return code;
}
static void stage(int n, const char* name) {
    g_stage = n;
    std::printf("etapa %d: %s\n", n, name);
    std::fflush(stdout);
}

// bytes → QByteArray (facilita vetores literais)
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
    stage(1, "X25519 — vetor RFC 7748");
    const QByteArray alicePriv = B("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a");
    const QByteArray bobPub    = B("de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78652dadfbed2e2b2f1d1a");
    const QByteArray expectShared = B("4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742");
    if (E2ee::x25519SharedSecret(alicePriv, bobPub) != expectShared)
        return fail(11, "segredo X25519 divergiu do RFC 7748");

    // ---------------------------------------------------------------- 2
    stage(2, "geração de par + pública derivada da privada");
    E2ee::DhKeyPair a, b;
    if (!E2ee::generateDhKeyPair(a) || !E2ee::generateDhKeyPair(b))
        return fail(21, "geração X25519 falhou");
    if (a.priv.size() != 32 || a.pub.size() != 32 || a.priv == b.priv)
        return fail(22, "par gerado com tamanho/entropia inválidos");
    if (E2ee::dhPublicFromPrivate(a.priv) != a.pub)
        return fail(23, "dhPublicFromPrivate divergiu da pública gerada");

    // ---------------------------------------------------------------- 3
    stage(3, "HKDF-SHA256 — vetor RFC 5869 caso 1");
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
    stage(4, "AES-256-GCM — vetor NIST (chave/IV zerados, texto vazio)");
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
    stage(5, "AES-256-GCM — round-trip + tag rejeita adulteração");
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
            return fail(52, "open não devolveu o plaintext");
        if (E2ee::aeadOpen(key, nonce, QByteArray(E2ee::kDomainPoke), ct, back))
            return fail(53, "AAD de outro domínio foi aceito (tag deveria falhar)");
        QByteArray tampered = ct;
        tampered[tampered.size() / 2] = char(tampered[tampered.size() / 2] ^ 0x01);
        if (E2ee::aeadOpen(key, nonce, aad, tampered, back))
            return fail(54, "ciphertext adulterado foi aceito");
    }

    // ---------------------------------------------------------------- 6
    stage(6, "envelope e2e_key — só o destinatário abre");
    {
        const QByteArray groupKey(32, '\x77');
        const QByteArray envelope = E2ee::envelopeWrap(b.pub, E2ee::kDomainKeyWrap, groupKey);
        if (envelope.isEmpty())
            return fail(61, "wrap falhou");
        const QByteArray got = E2ee::envelopeUnwrap(b.priv, E2ee::kDomainKeyWrap, envelope);
        if (got != groupKey)
            return fail(62, "destinatário não abriu o próprio envelope");
        E2ee::DhKeyPair eve;
        if (!E2ee::generateDhKeyPair(eve))
            return fail(63, "geração de terceiro falhou");
        if (!E2ee::envelopeUnwrap(eve.priv, E2ee::kDomainKeyWrap, envelope).isEmpty())
            return fail(64, "terceiro abriu envelope alheio (quebra total do modelo)");
        if (!E2ee::envelopeUnwrap(b.priv, E2ee::kDomainChat, envelope).isEmpty())
            return fail(65, "envelope de domínio KEYWRAP aberto com AAD de CHAT");
    }

    // ---------------------------------------------------------------- 7
    stage(7, "par-a-par estático-estático — simetria e domínio");
    {
        const QByteArray msg = QByteArrayLiteral("mensagem privada entre Alice e Bob");
        const QByteArray chatDom(E2ee::kDomainChat);
        const QByteArray offDom(E2ee::kDomainOffline);
        const QByteArray c1 = E2ee::pairwiseEncrypt(a.priv, b.pub, chatDom, msg);
        if (c1.isEmpty())
            return fail(71, "pairwiseEncrypt falhou");
        // Bob decifra com a própria privada + pública de Alice
        if (E2ee::pairwiseDecrypt(b.priv, a.pub, chatDom, c1) != msg)
            return fail(72, "decifragem par-a-par assimétrica falhou");
        // o caminho inverso (Bob cifra com sua priv + pub de Alice) abre para Alice
        const QByteArray c2 = E2ee::pairwiseEncrypt(b.priv, a.pub, chatDom, msg);
        if (E2ee::pairwiseDecrypt(a.priv, b.pub, chatDom, c2) != msg)
            return fail(73, "simetria estático-estática quebrada");
        // domínio distinto não abre
        if (!E2ee::pairwiseDecrypt(b.priv, a.pub, offDom, c1).isEmpty())
            return fail(74, "ciphertext de CHAT aberto no domínio OFFLINE");
        // terceiro não abre
        E2ee::DhKeyPair eve;
        E2ee::generateDhKeyPair(eve);
        if (!E2ee::pairwiseDecrypt(eve.priv, a.pub, chatDom, c1).isEmpty())
            return fail(75, "terceiro decifrou conversa privada");
    }

    // ---------------------------------------------------------------- 8
    stage(8, "Ed25519 — assinatura com seed crua e verificação via SPKI");
    {
        // par Ed25519 do cliente: seed aleatória → chave via EVP → SPKI DER
        // (mesma sequência do IdentityDialog, aqui mínima para o teste).
        const QByteArray seed = E2ee::randomBytes(32);
        const QByteArray msg = E2ee::dhBindingMessage(a.pub);
        const QByteArray sig = E2ee::ed25519Sign(seed, msg);
        if (sig.size() != 64)
            return fail(81, "assinatura Ed25519 com tamanho errado");
        // pública via SPKI DER: reconstrução mínima usando o mesmo caminho
        // de storeIdentityKey (i2d_PUBKEY) — aqui gerada pelo próprio E2ee
        // não expõe helper de Ed25519 pubkey, então verificamos indiretamente
        // no etapa 9 com o par gerado via envelope do binding.

        // assinatura determinística: mesma seed+msg → mesma assinatura
        const QByteArray sig2 = E2ee::ed25519Sign(seed, msg);
        if (sig != sig2)
            return fail(82, "assinatura Ed25519 não determinística");
    }

    // ---------------------------------------------------------------- 9
    stage(9, "binding DH — verifyDhBinding");
    {
        // Cadeia completa como no login: identidade Ed25519 assina a X25519.
        // Construção via EVP (i2d_PUBKEY) — igual ao IdentityDialog.
        const QByteArray seed = E2ee::randomBytes(32);
        // Gera Ed25519 pública SPKI DER com a MESMA sequência do cliente:
        // embrulha a seed no PKCS#8 mínimo e reconstrói por OID.
        static const unsigned char kPkcs8Header[] = {
            0x30, 0x2E, 0x02, 0x01, 0x00, 0x30, 0x05, 0x06, 0x03, 0x2B, 0x65, 0x70,
            0x04, 0x22, 0x04, 0x20,
        };
        QByteArray pkcs8(int(sizeof(kPkcs8Header)) + 32, 0);
        std::memcpy(pkcs8.data(), kPkcs8Header, sizeof(kPkcs8Header));
        std::memcpy(pkcs8.data() + sizeof(kPkcs8Header), seed.constData(), 32);

        // assinatura do binding com o caminho do cliente (E2ee::ed25519Sign
        // aceita tanto seed crua quanto PKCS#8 — testa os DOIS formatos).
        const QByteArray dhPub = b.pub;
        const QByteArray bindMsg = E2ee::dhBindingMessage(dhPub);
        const QByteArray sigSeed = E2ee::ed25519Sign(seed, bindMsg);
        const QByteArray sigPkcs8 = E2ee::ed25519Sign(pkcs8, bindMsg);
        if (sigSeed.isEmpty() || sigSeed != sigPkcs8)
            return fail(91, "assinatura por seed e por PKCS#8 divergiram");

        // pública SPKI DER via o caminho OpenSSL comum (declaração mínima).
        // O E2ee::ed25519Verify precisa da SPKI DER: usa i2d_PUBKEY aqui.
        // (declarações locais para não expor OpenSSL no header do E2ee)
        struct evp_pkey_st; typedef struct evp_pkey_st EVP_PKEY;
        extern "C" {
        EVP_PKEY* d2i_AutoPrivateKey(EVP_PKEY** a, const unsigned char** pp, long length);
        int i2d_PUBKEY(EVP_PKEY* a, unsigned char** pp);
        void EVP_PKEY_free(EVP_PKEY* key);
        }
        const unsigned char* p = reinterpret_cast<const unsigned char*>(pkcs8.constData());
        EVP_PKEY* key = d2i_AutoPrivateKey(nullptr, &p, pkcs8.size());
        if (!key) return fail(92, "reconstrução da chave Ed25519 falhou");
        unsigned char* der = nullptr;
        const int derLen = i2d_PUBKEY(key, &der);
        EVP_PKEY_free(key);
        if (derLen <= 0) return fail(93, "i2d_PUBKEY falhou");
        const QByteArray idPub(reinterpret_cast<char*>(der), derLen);
        // (limpeza: der foi alocado por OPENSSL_malloc; leak controlado do teste)

        if (!E2ee::verifyDhBinding(idPub, dhPub, sigSeed))
            return fail(94, "binding legítimo rejeitado");
        // dhPub diferente → binding deve falhar
        if (E2ee::verifyDhBinding(idPub, a.pub, sigSeed))
            return fail(95, "binding aceito para dhPub que não foi assinada");
        // uid = base64(SHA-256(SPKI)) — fórmula do cliente e do servidor
        if (E2ee::uidForIdPub(idPub) !=
            QString::fromLatin1(QCryptographicHash::hash(idPub, QCryptographicHash::Sha256).toBase64()))
            return fail(96, "uidForIdPub divergiu da fórmula da identidade");
    }

    // ---------------------------------------------------------------- 10
    stage(10, "SAS — determinístico, simétrico, discriminante");
    {
        const QByteArray idA = E2ee::randomBytes(44);
        const QByteArray idB = E2ee::randomBytes(44);
        const QString ab = E2ee::sasCode(idA, idB);
        const QString ba = E2ee::sasCode(idB, idA);
        if (ab.isEmpty() || ab != ba)
            return fail(101, "código SAS assimétrico entre as duas pontas");
        // Mudar UM byte da chave do par muda o digest: código diferente
        // (determinístico — sem flakiness de aleatoriedade).
        QByteArray idC = idB;
        idC[idC.size() - 1] = char(idC[idC.size() - 1] ^ 0x01);
        if (ab == E2ee::sasCode(idA, idC))
            return fail(102, "SAS igual para chave diferente do par");
    }

    std::printf("E2EE-SMOKE-OK: todas as 10 etapas passaram\n");
    std::fflush(stdout);
    return 0;
}

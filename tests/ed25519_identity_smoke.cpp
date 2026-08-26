// Smoke de runtime para a sequência EXATA de src/dialogs/IdentityDialog.cpp
// (geração de identidade Ed25519 + assinatura de desafio), executada contra
// o BoringSSL embutido no webrtc.lib do SDK — no MESMO ambiente misto do
// build de release Windows: headers do OpenSSL 3 (vcpkg) + implementação
// BoringSSL linkada estaticamente do webrtc.lib.
//
// O teste existe porque o CI de e2e (MinGW) usa OpenSSL de verdade e nunca
// exercitou o caminho BoringSSL: era assim que a geração de identidade
// quebrava em TODAS as máquinas do build WebRTC sem ninguém perceber.
//
// Cada etapa devolve um código de saída distinto: quando este teste falha,
// o número aponta exatamente qual chamada quebrou.

#include <cstdio>
#include <cstring>

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/x509.h>

static int fail(int code, const char* what) {
    const unsigned long e = ERR_get_error();
    char buf[256] = {0};
    if (e) ERR_error_string_n(e, buf, sizeof(buf));
    std::printf("SMOKE-FALHA %d: %s (openssl err: %s)\n", code, what, buf);
    std::fflush(stdout);
    return code;
}

// PKCS#8 DER de uma chave Ed25519 (openssl genpkey -algorithm ed25519) —
// formato que instalações antigas, construídas com OpenSSL de verdade,
// guardaram no cofre do sistema. signNonce() precisa continuar parseando.
static const unsigned char kLegacyPkcs8[] = {
    0x30, 0x2E, 0x02, 0x01, 0x00, 0x30, 0x05, 0x06, 0x03, 0x2B, 0x65, 0x70,
    0x04, 0x22, 0x04, 0x20, 0xBF, 0x91, 0x6E, 0x89, 0xDD, 0x7B, 0x0B, 0xB6,
    0xDE, 0xE1, 0x9B, 0x42, 0x6E, 0xB4, 0xD3, 0x45, 0x35, 0x31, 0x3E, 0xF7,
    0x84, 0x57, 0xFF, 0xD5, 0xB3, 0x57, 0x3F, 0x13, 0x5A, 0xD5, 0xED, 0x6A,
};

// 32 bytes que NÃO são ASN.1 válido (tag 0xAB + comprimento 171 > 30
// restantes): a seed crua gravada por storeIdentityKey() tem que ser
// rejeitada pelo parser PKCS#8 e cair no ramo EVP_PKEY_new_raw_private_key.
static const unsigned char kRawSeedNotAsn1[32] = {
    0xAB, 0xAB, 0xAB, 0xAB, 0xAB, 0xAB, 0xAB, 0xAB, 0xAB, 0xAB, 0xAB, 0xAB,
    0xAB, 0xAB, 0xAB, 0xAB, 0xAB, 0xAB, 0xAB, 0xAB, 0xAB, 0xAB, 0xAB, 0xAB,
    0xAB, 0xAB, 0xAB, 0xAB, 0xAB, 0xAB, 0xAB, 0xAB,
};

int main() {
    std::printf("ed25519_identity_smoke: inicio\n");

    // ---- [1] generateUniqueId(): keygen Ed25519 ----
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr);
    if (!ctx) return fail(1, "EVP_PKEY_CTX_new_id");
    if (EVP_PKEY_keygen_init(ctx) != 1) return fail(2, "EVP_PKEY_keygen_init");
    EVP_PKEY* key = nullptr;
    if (EVP_PKEY_keygen(ctx, &key) != 1) return fail(3, "EVP_PKEY_keygen");
    EVP_PKEY_CTX_free(ctx);
    std::printf("[1] keygen Ed25519 ok\n");

    // ---- [2] storeIdentityKey(): chave publica em SPKI DER ----
    const int pubLen = i2d_PUBKEY(key, nullptr);
    if (pubLen <= 0) return fail(4, "i2d_PUBKEY (previsao de tamanho)");
    unsigned char pub[128] = {0};
    unsigned char* p = pub;
    if (i2d_PUBKEY(key, &p) != pubLen) return fail(5, "i2d_PUBKEY");
    std::printf("[2] i2d_PUBKEY ok (%d bytes)\n", pubLen);

    // ---- [3] storeIdentityKey(): seed crua de 32 bytes ----
    unsigned char seed[64] = {0};
    size_t seedLen = sizeof(seed);
    if (EVP_PKEY_get_raw_private_key(key, seed, &seedLen) != 1)
        return fail(6, "EVP_PKEY_get_raw_private_key");
    if (seedLen != 32) return fail(7, "seed com tamanho inesperado");
    std::printf("[3] seed crua ok (%zu bytes)\n", seedLen);

    // ---- [4] diagnostico do bug historico (v1.1.0/v1.1.1) ----
    // i2d_PrivateKey com Ed25519: no BoringSSL do SDK devolve -1 e era a
    // causa raiz do ID unico vazio. Apenas informativo — o codigo atual
    // nao usa mais esta chamada.
    if (i2d_PrivateKey(key, nullptr) > 0)
        std::printf("[4] i2d_PrivateKey aceita Ed25519 (OpenSSL-like)\n");
    else
        std::printf("[4] i2d_PrivateKey rejeita Ed25519 (confirma a causa raiz antiga)\n");

    // ---- [5] signNonce(): reconstrucao da chave a partir da seed crua ----
    EVP_PKEY* rk = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, seed, 32);
    if (!rk) return fail(8, "EVP_PKEY_new_raw_private_key");
    std::printf("[5] chave reconstruida da seed ok\n");

    // ---- [6] signNonce(): assinatura do desafio de 32 bytes ----
    unsigned char nonce[32];
    for (int i = 0; i < 32; ++i) nonce[i] = static_cast<unsigned char>(i * 7 + 1);
    EVP_MD_CTX* sctx = EVP_MD_CTX_new();
    if (!sctx) return fail(9, "EVP_MD_CTX_new");
    unsigned char sig[128] = {0};
    size_t sigLen = sizeof(sig);
    if (EVP_DigestSignInit(sctx, nullptr, nullptr, nullptr, rk) != 1)
        return fail(10, "EVP_DigestSignInit");
    if (EVP_DigestSign(sctx, sig, &sigLen, nonce, sizeof(nonce)) != 1)
        return fail(11, "EVP_DigestSign");
    EVP_MD_CTX_free(sctx);
    if (sigLen != 64) return fail(12, "assinatura com tamanho != 64");
    std::printf("[6] assinatura do desafio ok (%zu bytes)\n", sigLen);

    // ---- [7] verificacao no estilo do servidor (SPKI DER + DigestVerify) ----
    const unsigned char* q = pub;
    EVP_PKEY* vk = d2i_PUBKEY(nullptr, &q, pubLen);
    if (!vk) return fail(13, "d2i_PUBKEY");
    EVP_MD_CTX* vctx = EVP_MD_CTX_new();
    if (!vctx) return fail(14, "EVP_MD_CTX_new (verificacao)");
    if (EVP_DigestVerifyInit(vctx, nullptr, nullptr, nullptr, vk) != 1)
        return fail(15, "EVP_DigestVerifyInit");
    if (EVP_DigestVerify(vctx, sig, sigLen, nonce, sizeof(nonce)) != 1)
        return fail(16, "EVP_DigestVerify");
    EVP_MD_CTX_free(vctx);
    EVP_PKEY_free(vk);
    std::printf("[7] verificacao estilo servidor ok\n");

    // ---- [8] migracao legada: PKCS#8 parseia via d2i_AutoPrivateKey ----
    const unsigned char* r = kLegacyPkcs8;
    EVP_PKEY* lk = d2i_AutoPrivateKey(nullptr, &r, static_cast<long>(sizeof(kLegacyPkcs8)));
    if (!lk) return fail(17, "d2i_AutoPrivateKey (PKCS#8 legado do cofre)");
    EVP_PKEY_free(lk);
    std::printf("[8] parse do PKCS#8 legado ok\n");

    // ---- [9] seed crua e rejeitada pelo parser ASN.1 (ramo size==32) ----
    const unsigned char* s = kRawSeedNotAsn1;
    EVP_PKEY* wrong = d2i_AutoPrivateKey(nullptr, &s, 32);
    if (wrong) {
        EVP_PKEY_free(wrong);
        return fail(18, "seed crua parseou como ASN.1 (inesperado)");
    }
    std::printf("[9] seed crua rejeitada pelo parser PKCS#8 (esperado)\n");

    EVP_PKEY_free(rk);
    EVP_PKEY_free(key);
    std::printf("ed25519_identity_smoke: TUDO OK\n");
    return 0;
}

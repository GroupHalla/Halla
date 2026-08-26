// Smoke de runtime para a sequência EXATA de src/dialogs/IdentityDialog.cpp
// (geração de identidade Ed25519 + assinatura de desafio), executada contra
// o BoringSSL embutido no webrtc.lib do SDK — no MESMO ambiente misto do
// build de release Windows: headers do OpenSSL 3 (vcpkg) + implementação
// BoringSSL linkada estaticamente do webrtc.lib.
//
// O teste existe porque o CI de e2e (MinGW) usa OpenSSL de verdade e nunca
// exercitou o caminho BoringSSL. Foi assim que o build WebRTC passou meses
// sem criar UMA única identidade: os headers do OpenSSL declaram
// NID_ED25519=1087, o BoringSSL linkado espera NID_ED25519=949, e
// EVP_PKEY_CTX_new_id(1087) devolvia UNSUPPORTED_ALGORITHM. Por isso a
// primeira coisa que este teste faz é RESOLVER o ID em runtime — igual ao
// ed25519Id() do IdentityDialog — e usá-lo em todas as chamadas.
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

// PKCS#8 DER de uma chave Ed25519 (openssl genpkey -algorithm ed25519) — o
// MESMO vetor de teste usado pelo ed25519Id() do IdentityDialog. O parse
// identifica o algoritmo pelo OID (1.3.101.112), idêntico em OpenSSL e
// BoringSSL; EVP_PKEY_id devolve o número que a implementação linkada usa.
static const unsigned char kProbePkcs8[] = {
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

static int ed25519Id() {
    static int cached = 0;
    if (cached) return cached;
    const unsigned char* p = kProbePkcs8;
    if (EVP_PKEY* probe = d2i_AutoPrivateKey(nullptr, &p, (long)sizeof(kProbePkcs8))) {
        const int id = EVP_PKEY_id(probe);
        EVP_PKEY_free(probe);
        if (id != 0) { cached = id; return cached; }
    }
    cached = EVP_PKEY_ED25519; // último recurso: valor do header
    return cached;
}

int main() {
    std::printf("ed25519_identity_smoke: inicio\n");

    // ---- [1] resolução do ID Ed25519 (ed25519Id) ----
    const int id = ed25519Id();
    std::printf("[1] NID Ed25519 — header: %d, implementacao linkada: %d%s\n",
                int(EVP_PKEY_ED25519), id,
                int(EVP_PKEY_ED25519) == id ? " (iguais)" : " (DIFERENTES — resolver em runtime e' obrigatorio)");
    if (id <= 0) return fail(1, "ed25519Id (resolucao de NID)");

    // ---- [2] generateUniqueId(): keygen Ed25519 com o ID resolvido ----
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(id, nullptr);
    if (!ctx) return fail(2, "EVP_PKEY_CTX_new_id(ed25519Id())");
    if (EVP_PKEY_keygen_init(ctx) != 1) return fail(3, "EVP_PKEY_keygen_init");
    EVP_PKEY* key = nullptr;
    if (EVP_PKEY_keygen(ctx, &key) != 1) return fail(4, "EVP_PKEY_keygen");
    EVP_PKEY_CTX_free(ctx);
    std::printf("[2] keygen Ed25519 ok\n");

    // ---- [3] storeIdentityKey(): chave publica em SPKI DER ----
    const int pubLen = i2d_PUBKEY(key, nullptr);
    if (pubLen <= 0) return fail(5, "i2d_PUBKEY (previsao de tamanho)");
    unsigned char pub[128] = {0};
    unsigned char* p = pub;
    if (i2d_PUBKEY(key, &p) != pubLen) return fail(6, "i2d_PUBKEY");
    std::printf("[3] i2d_PUBKEY ok (%d bytes)\n", pubLen);

    // ---- [4] storeIdentityKey(): seed crua de 32 bytes ----
    unsigned char seed[64] = {0};
    size_t seedLen = sizeof(seed);
    if (EVP_PKEY_get_raw_private_key(key, seed, &seedLen) != 1)
        return fail(7, "EVP_PKEY_get_raw_private_key");
    if (seedLen != 32) return fail(8, "seed com tamanho inesperado");
    std::printf("[4] seed crua ok (%zu bytes)\n", seedLen);

    // ---- [5] diagnostico do bug historico (v1.1.0 a v1.1.2) ----
    // i2d_PrivateKey com Ed25519: no BoringSSL do SDK devolve -1. E a
    // razao pela qual a chave privada nunca foi persistida como PKCS#8.
    if (i2d_PrivateKey(key, nullptr) > 0)
        std::printf("[5] i2d_PrivateKey aceita Ed25519 (OpenSSL-like)\n");
    else
        std::printf("[5] i2d_PrivateKey rejeita Ed25519 (confirma a causa raiz antiga)\n");

    // ---- [6] signNonce(): reconstrucao da chave a partir da seed crua ----
    EVP_PKEY* rk = EVP_PKEY_new_raw_private_key(id, nullptr, seed, 32);
    if (!rk) return fail(9, "EVP_PKEY_new_raw_private_key(ed25519Id())");
    std::printf("[6] chave reconstruida da seed ok\n");

    // ---- [7] signNonce(): assinatura do desafio de 32 bytes ----
    unsigned char nonce[32];
    for (int i = 0; i < 32; ++i) nonce[i] = static_cast<unsigned char>(i * 7 + 1);
    EVP_MD_CTX* sctx = EVP_MD_CTX_new();
    if (!sctx) return fail(10, "EVP_MD_CTX_new");
    unsigned char sig[128] = {0};
    size_t sigLen = sizeof(sig);
    if (EVP_DigestSignInit(sctx, nullptr, nullptr, nullptr, rk) != 1)
        return fail(11, "EVP_DigestSignInit");
    if (EVP_DigestSign(sctx, sig, &sigLen, nonce, sizeof(nonce)) != 1)
        return fail(12, "EVP_DigestSign");
    EVP_MD_CTX_free(sctx);
    if (sigLen != 64) return fail(13, "assinatura com tamanho != 64");
    std::printf("[7] assinatura do desafio ok (%zu bytes)\n", sigLen);

    // ---- [8] verificacao no estilo do servidor (SPKI DER + DigestVerify) ----
    const unsigned char* q = pub;
    EVP_PKEY* vk = d2i_PUBKEY(nullptr, &q, pubLen);
    if (!vk) return fail(14, "d2i_PUBKEY");
    EVP_MD_CTX* vctx = EVP_MD_CTX_new();
    if (!vctx) return fail(15, "EVP_MD_CTX_new (verificacao)");
    if (EVP_DigestVerifyInit(vctx, nullptr, nullptr, nullptr, vk) != 1)
        return fail(16, "EVP_DigestVerifyInit");
    if (EVP_DigestVerify(vctx, sig, sigLen, nonce, sizeof(nonce)) != 1)
        return fail(17, "EVP_DigestVerify");
    EVP_MD_CTX_free(vctx);
    EVP_PKEY_free(vk);
    std::printf("[8] verificacao estilo servidor ok\n");

    // ---- [9] migracao legada: PKCS#8 parseia via d2i_AutoPrivateKey ----
    const unsigned char* r = kProbePkcs8;
    EVP_PKEY* lk = d2i_AutoPrivateKey(nullptr, &r, static_cast<long>(sizeof(kProbePkcs8)));
    if (!lk) return fail(18, "d2i_AutoPrivateKey (PKCS#8 legado do cofre)");
    // e a chave legada ASSINA um desafio (caminho completo do cofre antigo)
    EVP_MD_CTX* lctx = EVP_MD_CTX_new();
    unsigned char lsig[128] = {0};
    size_t lsigLen = sizeof(lsig);
    if (!lctx) return fail(19, "EVP_MD_CTX_new (legado)");
    if (EVP_DigestSignInit(lctx, nullptr, nullptr, nullptr, lk) != 1)
        return fail(20, "EVP_DigestSignInit (legado)");
    if (EVP_DigestSign(lctx, lsig, &lsigLen, nonce, sizeof(nonce)) != 1)
        return fail(21, "EVP_DigestSign (legado)");
    EVP_MD_CTX_free(lctx);
    EVP_PKEY_free(lk);
    std::printf("[9] assinatura com chave PKCS#8 legada ok (%zu bytes)\n", lsigLen);

    // ---- [10] seed crua e rejeitada pelo parser ASN.1 (ramo size==32) ----
    const unsigned char* s = kRawSeedNotAsn1;
    EVP_PKEY* wrong = d2i_AutoPrivateKey(nullptr, &s, 32);
    if (wrong) {
        EVP_PKEY_free(wrong);
        return fail(22, "seed crua parseou como ASN.1 (inesperado)");
    }
    std::printf("[10] seed crua rejeitada pelo parser PKCS#8 (esperado)\n");

    EVP_PKEY_free(rk);
    EVP_PKEY_free(key);
    std::printf("ed25519_identity_smoke: TUDO OK\n");
    return 0;
}

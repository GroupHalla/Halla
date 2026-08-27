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
// EVP_PKEY_CTX_new_id(1087) devolvia UNSUPPORTED_ALGORITHM.
//
// Por isso a sequência testada aqui é 100% LIVRE DE NID — igual ao
// IdentityDialog: RAND_bytes para a seed, chave reconstruída pelo parser
// OID (d2i_AutoPrivateKey sobre um PKCS#8 mínimo), i2d_PUBKEY para a chave
// pública e EVP_DigestSign/EVP_DigestVerify para a assinatura.
//
// Cada etapa devolve um código de saída distinto: quando este teste falha,
// o número aponta exatamente qual chamada quebrou.

#include <cstdio>
#include <cstring>

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/x509.h>

static int fail(int code, const char* what) {
    const unsigned long e = ERR_get_error();
    char buf[256] = {0};
    if (e) ERR_error_string_n(e, buf, sizeof(buf));
    std::printf("SMOKE-FALHA %d: %s (openssl err: %s)\n", code, what, buf);
    std::fflush(stdout);
    return code;
}

// Cabeçalho PKCS#8 fixo (RFC 8410) — o MESMO do IdentityDialog.
static const unsigned char kPkcs8SeedHeader[] = {
    0x30, 0x2E, 0x02, 0x01, 0x00, 0x30, 0x05, 0x06, 0x03, 0x2B, 0x65, 0x70,
    0x04, 0x22, 0x04, 0x20,
};

// PKCS#8 DER completo de uma chave Ed25519 (openssl genpkey) — o formato
// que instalações antigas, construídas com OpenSSL de verdade, guardaram no
// cofre do sistema. signNonce() precisa continuar parseando.
static const unsigned char kLegacyPkcs8[] = {
    0x30, 0x2E, 0x02, 0x01, 0x00, 0x30, 0x05, 0x06, 0x03, 0x2B, 0x65, 0x70,
    0x04, 0x22, 0x04, 0x20, 0xBF, 0x91, 0x6E, 0x89, 0xDD, 0x7B, 0x0B, 0xB6,
    0xDE, 0xE1, 0x9B, 0x42, 0x6E, 0xB4, 0xD3, 0x45, 0x35, 0x31, 0x3E, 0xF7,
    0x84, 0x57, 0xFF, 0xD5, 0xB3, 0x57, 0x3F, 0x13, 0x5A, 0xD5, 0xED, 0x6A,
};

// 32 bytes que NÃO são ASN.1 válido (tag 0xAB + comprimento 171 > 30
// restantes): a seed crua gravada por storeIdentityKey() tem que ser
// rejeitada pelo parser PKCS#8 direto — por isso o embrulho no cabeçalho.
static const unsigned char kRawSeedNotAsn1[32] = {
    0xAB, 0xAB, 0xAB, 0xAB, 0xAB, 0xAB, 0xAB, 0xAB, 0xAB, 0xAB, 0xAB, 0xAB,
    0xAB, 0xAB, 0xAB, 0xAB, 0xAB, 0xAB, 0xAB, 0xAB, 0xAB, 0xAB, 0xAB, 0xAB,
    0xAB, 0xAB, 0xAB, 0xAB, 0xAB, 0xAB, 0xAB, 0xAB,
};

static EVP_PKEY* keyFromSeed(const unsigned char* seed) {
    unsigned char der[48];
    std::memcpy(der, kPkcs8SeedHeader, sizeof(kPkcs8SeedHeader));
    std::memcpy(der + sizeof(kPkcs8SeedHeader), seed, 32);
    const unsigned char* p = der;
    return d2i_AutoPrivateKey(nullptr, &p, sizeof(der));
}

int main() {
    std::printf("ed25519_identity_smoke: inicio (sequencia livre de NID)\n");

    // ---- [1] generateUniqueId(): seed do CSPRNG da biblioteca ----
    unsigned char seed[32];
    if (RAND_bytes(seed, sizeof(seed)) != 1) return fail(1, "RAND_bytes");
    std::printf("[1] RAND_bytes ok\n");

    // ---- [2] generateUniqueId(): chave via parser OID (keyFromSeed) ----
    EVP_PKEY* key = keyFromSeed(seed);
    if (!key) return fail(2, "d2i_AutoPrivateKey (PKCS#8 minimo + seed)");
    std::printf("[2] chave Ed25519 reconstruida por OID ok\n");

    // ---- [3] storeIdentityKey(): chave publica em SPKI DER ----
    const int pubLen = i2d_PUBKEY(key, nullptr);
    if (pubLen <= 0) return fail(3, "i2d_PUBKEY (previsao de tamanho)");
    unsigned char pub[128] = {0};
    unsigned char* p = pub;
    if (i2d_PUBKEY(key, &p) != pubLen) return fail(4, "i2d_PUBKEY");
    std::printf("[3] i2d_PUBKEY ok (%d bytes)\n", pubLen);

    // ---- [4] storeIdentityKey(): seed crua de 32 bytes + round-trip ----
    unsigned char seedBack[64] = {0};
    size_t seedLen = sizeof(seedBack);
    if (EVP_PKEY_get_raw_private_key(key, seedBack, &seedLen) != 1)
        return fail(5, "EVP_PKEY_get_raw_private_key");
    if (seedLen != 32) return fail(6, "seed com tamanho inesperado");
    if (std::memcmp(seed, seedBack, 32) != 0)
        return fail(7, "seed round-trip diferente da original");
    std::printf("[4] seed crua ok com round-trip identico (%zu bytes)\n", seedLen);

    // ---- [5] diagnostico do bug historico (v1.1.0 a v1.1.2) ----
    // i2d_PrivateKey com Ed25519: no BoringSSL do SDK devolve -1. E a
    // razao pela qual a chave privada nunca foi persistida como PKCS#8.
    if (i2d_PrivateKey(key, nullptr) > 0)
        std::printf("[5] i2d_PrivateKey aceita Ed25519 (OpenSSL-like)\n");
    else
        std::printf("[5] i2d_PrivateKey rejeita Ed25519 (confirma a causa raiz antiga)\n");

    // ---- [6] signNonce(): assinatura do desafio de 32 bytes ----
    unsigned char nonce[32];
    for (int i = 0; i < 32; ++i) nonce[i] = static_cast<unsigned char>(i * 7 + 1);
    EVP_MD_CTX* sctx = EVP_MD_CTX_new();
    if (!sctx) return fail(8, "EVP_MD_CTX_new");
    unsigned char sig[128] = {0};
    size_t sigLen = sizeof(sig);
    if (EVP_DigestSignInit(sctx, nullptr, nullptr, nullptr, key) != 1)
        return fail(9, "EVP_DigestSignInit");
    if (EVP_DigestSign(sctx, sig, &sigLen, nonce, sizeof(nonce)) != 1)
        return fail(10, "EVP_DigestSign");
    EVP_MD_CTX_free(sctx);
    if (sigLen != 64) return fail(11, "assinatura com tamanho != 64");
    std::printf("[6] assinatura do desafio ok (%zu bytes)\n", sigLen);

    // ---- [7] verificacao no estilo do servidor (SPKI DER + DigestVerify) ----
    const unsigned char* q = pub;
    EVP_PKEY* vk = d2i_PUBKEY(nullptr, &q, pubLen);
    if (!vk) return fail(12, "d2i_PUBKEY");
    EVP_MD_CTX* vctx = EVP_MD_CTX_new();
    if (!vctx) return fail(13, "EVP_MD_CTX_new (verificacao)");
    if (EVP_DigestVerifyInit(vctx, nullptr, nullptr, nullptr, vk) != 1)
        return fail(14, "EVP_DigestVerifyInit");
    if (EVP_DigestVerify(vctx, sig, sigLen, nonce, sizeof(nonce)) != 1)
        return fail(15, "EVP_DigestVerify");
    EVP_MD_CTX_free(vctx);
    EVP_PKEY_free(vk);
    std::printf("[7] verificacao estilo servidor ok\n");

    // ---- [8] migracao legada: PKCS#8 completo parseia e assina ----
    const unsigned char* r = kLegacyPkcs8;
    EVP_PKEY* lk = d2i_AutoPrivateKey(nullptr, &r, static_cast<long>(sizeof(kLegacyPkcs8)));
    if (!lk) return fail(16, "d2i_AutoPrivateKey (PKCS#8 legado do cofre)");
    EVP_MD_CTX* lctx = EVP_MD_CTX_new();
    unsigned char lsig[128] = {0};
    size_t lsigLen = sizeof(lsig);
    if (!lctx) return fail(17, "EVP_MD_CTX_new (legado)");
    if (EVP_DigestSignInit(lctx, nullptr, nullptr, nullptr, lk) != 1)
        return fail(18, "EVP_DigestSignInit (legado)");
    if (EVP_DigestSign(lctx, lsig, &lsigLen, nonce, sizeof(nonce)) != 1)
        return fail(19, "EVP_DigestSign (legado)");
    EVP_MD_CTX_free(lctx);
    EVP_PKEY_free(lk);
    std::printf("[8] assinatura com chave PKCS#8 legada ok (%zu bytes)\n", lsigLen);

    // ---- [9] seed crua e rejeitada pelo parser ASN.1 direto ----
    const unsigned char* s = kRawSeedNotAsn1;
    EVP_PKEY* wrong = d2i_AutoPrivateKey(nullptr, &s, 32);
    if (wrong) {
        EVP_PKEY_free(wrong);
        return fail(20, "seed crua parseou como ASN.1 (inesperado)");
    }
    std::printf("[9] seed crua rejeitada pelo parser PKCS#8 direto (esperado)\n");

    // ---- [10] backup portátil: PBKDF2-HMAC-SHA256 (mesma sequência do
    // exportIdentityBackupFile — formato "halla-identity-backup" v1, idêntico
    // ao Halla Mobile) ----
    unsigned char salt[16], iv[12];
    if (RAND_bytes(salt, sizeof(salt)) != 1) return fail(21, "RAND_bytes (salt)");
    if (RAND_bytes(iv, sizeof(iv)) != 1) return fail(22, "RAND_bytes (iv)");
    unsigned char backupKey[32];
    if (PKCS5_PBKDF2_HMAC("senha-do-backup-123", -1, salt, sizeof(salt), 310000,
                          EVP_sha256(), sizeof(backupKey), backupKey) != 1)
        return fail(23, "PKCS5_PBKDF2_HMAC");
    std::printf("[10] PBKDF2-HMAC-SHA256 (310000 iteracoes) ok\n");

    // ---- [11] backup portátil: AES-256-GCM com AAD + tag ----
    // O plaintext é o PKCS#8 mínimo (cabeçalho + seed) — exatamente o que o
    // export grava no arquivo (e o MESMO layout que Java/Android produz).
    unsigned char pkcs8[48];
    std::memcpy(pkcs8, kPkcs8SeedHeader, sizeof(kPkcs8SeedHeader));
    std::memcpy(pkcs8 + sizeof(kPkcs8SeedHeader), seedBack, 32);
    const char* aad = "halla-identity-backup|1|desktop|Ed25519|PublicKeyBase64==";
    EVP_CIPHER_CTX* ectx = EVP_CIPHER_CTX_new();
    if (!ectx) return fail(24, "EVP_CIPHER_CTX_new (encrypt)");
    unsigned char ct[64 + 16] = {0};
    int len = 0, total = 0;
    if (EVP_EncryptInit_ex(ectx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1)
        return fail(25, "EVP_EncryptInit_ex (GCM)");
    if (EVP_CIPHER_CTX_ctrl(ectx, EVP_CTRL_GCM_SET_IVLEN, 12, nullptr) != 1)
        return fail(26, "EVP_CTRL_GCM_SET_IVLEN");
    if (EVP_EncryptInit_ex(ectx, nullptr, nullptr, backupKey, iv) != 1)
        return fail(27, "EVP_EncryptInit_ex (chave/iv)");
    if (EVP_EncryptUpdate(ectx, nullptr, &len,
                          reinterpret_cast<const unsigned char*>(aad),
                          static_cast<int>(std::strlen(aad))) != 1)
        return fail(28, "EVP_EncryptUpdate (AAD)");
    if (EVP_EncryptUpdate(ectx, ct, &len, pkcs8, sizeof(pkcs8)) != 1)
        return fail(29, "EVP_EncryptUpdate (plaintext)");
    total = len;
    if (EVP_EncryptFinal_ex(ectx, ct + total, &len) != 1)
        return fail(30, "EVP_EncryptFinal_ex");
    total += len;
    if (EVP_CIPHER_CTX_ctrl(ectx, EVP_CTRL_GCM_GET_TAG, 16, ct + total) != 1)
        return fail(31, "EVP_CTRL_GCM_GET_TAG");
    total += 16;
    EVP_CIPHER_CTX_free(ectx);
    if (total != 48 + 16) return fail(32, "cifrado com tamanho inesperado");
    std::printf("[11] AES-256-GCM cifrada com AAD e tag ok (%d bytes)\n", total);

    // ---- [12] backup portátil: decifra e confere o plaintext ----
    EVP_CIPHER_CTX* dctx = EVP_CIPHER_CTX_new();
    if (!dctx) return fail(33, "EVP_CIPHER_CTX_new (decrypt)");
    unsigned char pt[64] = {0};
    unsigned char tag[16];
    std::memcpy(tag, ct + 48, sizeof(tag));
    len = 0;
    if (EVP_DecryptInit_ex(dctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1)
        return fail(34, "EVP_DecryptInit_ex (GCM)");
    if (EVP_CIPHER_CTX_ctrl(dctx, EVP_CTRL_GCM_SET_IVLEN, 12, nullptr) != 1)
        return fail(35, "EVP_CTRL_GCM_SET_IVLEN (decrypt)");
    if (EVP_DecryptInit_ex(dctx, nullptr, nullptr, backupKey, iv) != 1)
        return fail(36, "EVP_DecryptInit_ex (chave/iv)");
    if (EVP_DecryptUpdate(dctx, nullptr, &len,
                          reinterpret_cast<const unsigned char*>(aad),
                          static_cast<int>(std::strlen(aad))) != 1)
        return fail(37, "EVP_DecryptUpdate (AAD decrypt)");
    if (EVP_DecryptUpdate(dctx, pt, &len, ct, 48) != 1)
        return fail(38, "EVP_DecryptUpdate (cifrado)");
    if (EVP_CIPHER_CTX_ctrl(dctx, EVP_CTRL_GCM_SET_TAG, sizeof(tag), tag) != 1)
        return fail(39, "EVP_CTRL_GCM_SET_TAG");
    if (EVP_DecryptFinal_ex(dctx, pt + len, &len) != 1)
        return fail(40, "EVP_DecryptFinal_ex (tag ou senha errada)");
    EVP_CIPHER_CTX_free(dctx);
    if (std::memcmp(pt, pkcs8, sizeof(pkcs8)) != 0)
        return fail(41, "plaintext decifrado difere do original");
    std::printf("[12] decifragem GCM confere com o PKCS#8 original\n");

    // ---- [13] AAD adulterada: a decifragem TEM que falhar ----
    EVP_CIPHER_CTX* wctx = EVP_CIPHER_CTX_new();
    if (!wctx) return fail(42, "EVP_CIPHER_CTX_new (AAD errada)");
    const char* badAad = "halla-identity-backup|1|desktop|Ed25519|OutraChave==";
    len = 0;
    bool aadRejected = EVP_DecryptInit_ex(wctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1
        && EVP_CIPHER_CTX_ctrl(wctx, EVP_CTRL_GCM_SET_IVLEN, 12, nullptr) == 1
        && EVP_DecryptInit_ex(wctx, nullptr, nullptr, backupKey, iv) == 1
        && EVP_DecryptUpdate(wctx, nullptr, &len,
                             reinterpret_cast<const unsigned char*>(badAad),
                             static_cast<int>(std::strlen(badAad))) == 1
        && EVP_DecryptUpdate(wctx, pt, &len, ct, 48) == 1
        && EVP_CIPHER_CTX_ctrl(wctx, EVP_CTRL_GCM_SET_TAG, sizeof(tag), tag) == 1
        && EVP_DecryptFinal_ex(wctx, pt + len, &len) == 1;
    EVP_CIPHER_CTX_free(wctx);
    if (aadRejected) return fail(43, "AAD adulterada foi aceita (inesperado)");
    std::printf("[13] AAD adulterada rejeitada pela tag GCM (esperado)\n");

    EVP_PKEY_free(key);
    std::printf("ed25519_identity_smoke: TUDO OK\n");
    return 0;
}

// Runtime smoke for the EXACT sequence of src/dialogs/IdentityDialog.cpp
// (Ed25519 identity generation + challenge signing), executed against the
// BoringSSL embedded in the SDK webrtc.lib -- in the SAME mixed environment as
// the Windows release build: OpenSSL 3 headers (vcpkg) plus the BoringSSL
// implementation linked statically from webrtc.lib.
//
// The test exists because the MinGW e2e CI uses real OpenSSL and never
// exercised the BoringSSL path. That is how the WebRTC build spent months
// without creating a SINGLE identity: the OpenSSL headers declare
// NID_ED25519=1087, the linked BoringSSL expects NID_ED25519=949, and
// EVP_PKEY_CTX_new_id(1087) returned UNSUPPORTED_ALGORITHM.
//
// That is why the sequence tested here is 100% NID-FREE -- same as
// IdentityDialog: RAND_bytes for the seed, the key rebuilt by the OID parser
// (d2i_AutoPrivateKey over a minimal PKCS#8), i2d_PUBKEY for the public key
// and EVP_DigestSign/EVP_DigestVerify for the signature.
//
// Every stage returns a distinct exit code: when this test fails, the number
// points exactly at the call that broke.
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

// Fixed PKCS#8 header (RFC 8410) -- the SAME one IdentityDialog uses.
static const unsigned char kPkcs8SeedHeader[] = {
    0x30, 0x2E, 0x02, 0x01, 0x00, 0x30, 0x05, 0x06, 0x03, 0x2B, 0x65, 0x70,
    0x04, 0x22, 0x04, 0x20,
};

// Complete PKCS#8 DER of an Ed25519 key (openssl genpkey) -- the format that
// older installations, built against real OpenSSL, stored in the system vault.
// signNonce() must keep parsing it.
static const unsigned char kLegacyPkcs8[] = {
    0x30, 0x2E, 0x02, 0x01, 0x00, 0x30, 0x05, 0x06, 0x03, 0x2B, 0x65, 0x70,
    0x04, 0x22, 0x04, 0x20, 0xBF, 0x91, 0x6E, 0x89, 0xDD, 0x7B, 0x0B, 0xB6,
    0xDE, 0xE1, 0x9B, 0x42, 0x6E, 0xB4, 0xD3, 0x45, 0x35, 0x31, 0x3E, 0xF7,
    0x84, 0x57, 0xFF, 0xD5, 0xB3, 0x57, 0x3F, 0x13, 0x5A, 0xD5, 0xED, 0x6A,
};

// 32 bytes that are NOT valid ASN.1 (tag 0xAB plus length 171 > the 30
// remaining): the raw seed written by storeIdentityKey() must be rejected by
// the direct PKCS#8 parser -- hence the header wrap.
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

// ---------------------------------------------------------------------------
// Hand-written PBKDF2-HMAC-SHA256 -- EXACT mirror of IdentityDialog.cpp.
//
// Do NOT use PKCS5_PBKDF2_HMAC here or in the app: the OpenSSL 3 headers
// declare (int passlen, int saltlen, int iter, int keylen) while the BoringSSL
// in webrtc.lib implements (size_t password_len, size_t salt_len,
// uint32_t iterations, size_t key_len). The Windows build compiles against the
// OpenSSL headers and LINKS BoringSSL -- passlen = -1 (the OpenSSL "strlen"
// convention) became 4294967295 on the BoringSSL size_t and the process died
// with an ACCESS VIOLATION without printing a single line (run 33026636018;
// pwsh swallowed the cl stderr, diagnosed in run 33028370189).
// Below, only primitives with an identical ABI in both libraries
// (EVP_MD_CTX_new/free, EVP_DigestInit_ex, EVP_DigestUpdate size_t,
// EVP_DigestFinal_ex, EVP_sha256) -- and the result is checked against public
// RFC 8018 vectors.
// ---------------------------------------------------------------------------
static bool smokeHmacSha256(const unsigned char* key, int keyLen,
                            const unsigned char* msg1, int msg1Len,
                            const unsigned char* msg2, int msg2Len,
                            unsigned char out[32]) {
    unsigned char block[64];
    std::memset(block, 0, sizeof(block));
    if (keyLen > 64) {
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
    return ok;
}

// PBKDF2 (RFC 8018), a single 32-byte block -- same as IdentityDialog.
static bool smokePbkdf2(const unsigned char* pass, int passLen,
                        const unsigned char* salt, int saltLen,
                        int iterations, unsigned char out[32]) {
    const unsigned char beOne[4] = {0, 0, 0, 1};
    unsigned char u[32];
    if (!smokeHmacSha256(pass, passLen, salt, saltLen, beOne, sizeof(beOne), u))
        return false;
    unsigned char t[32];
    std::memcpy(t, u, sizeof(t));
    for (int i = 1; i < iterations; ++i) {
        if (!smokeHmacSha256(pass, passLen, u, sizeof(u), nullptr, 0, u))
            return false;
        for (int b = 0; b < 32; ++b) t[b] ^= u[b];
    }
    std::memcpy(out, t, 32);
    return true;
}

static bool smokeHexEq(const unsigned char* got, const char* expectedHex) {
    for (int i = 0; i < 32; ++i) {
        int hi = 0, lo = 0;
        if (std::sscanf(expectedHex + 2 * i, "%1x%1x", &hi, &lo) != 2) return false;
        if (got[i] != (hi * 16 + lo)) return false;
    }
    return true;
}

// Public PBKDF2-HMAC-SHA256 vectors (RFC 7914 section 11 / standard suites).
static const char kPbkdf2Vec1[] =
    "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b"; // c=1
static const char kPbkdf2Vec2[] =
    "ae4d0c95af6b46d32d0adff928f06dd02a303f8ef3c251dfd6e2d85a95474c43"; // c=2
static const char kPbkdf2Vec3[] =
    "c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a"; // c=4096

int main() {
    std::printf("ed25519_identity_smoke: start (NID-free sequence)\n");

    // ---- [1] generateUniqueId(): seed do CSPRNG da biblioteca ----
    unsigned char seed[32];
    if (RAND_bytes(seed, sizeof(seed)) != 1) return fail(1, "RAND_bytes");
    std::printf("[1] RAND_bytes ok\n");

    // ---- [2] generateUniqueId(): chave via parser OID (keyFromSeed) ----
    EVP_PKEY* key = keyFromSeed(seed);
    if (!key) return fail(2, "d2i_AutoPrivateKey (PKCS#8 minimo + seed)");
    std::printf("[2] Ed25519 key rebuilt by OID ok\n");

    // ---- [3] storeIdentityKey(): chave publica em SPKI DER ----
    const int pubLen = i2d_PUBKEY(key, nullptr);
    if (pubLen <= 0) return fail(3, "i2d_PUBKEY (size prediction)");
    unsigned char pub[128] = {0};
    unsigned char* p = pub;
    if (i2d_PUBKEY(key, &p) != pubLen) return fail(4, "i2d_PUBKEY");
    std::printf("[3] i2d_PUBKEY ok (%d bytes)\n", pubLen);

    // ---- [4] storeIdentityKey(): seed crua de 32 bytes + round-trip ----
    unsigned char seedBack[64] = {0};
    size_t seedLen = sizeof(seedBack);
    if (EVP_PKEY_get_raw_private_key(key, seedBack, &seedLen) != 1)
        return fail(5, "EVP_PKEY_get_raw_private_key");
    if (seedLen != 32) return fail(6, "unexpected seed size");
    if (std::memcmp(seed, seedBack, 32) != 0)
        return fail(7, "seed round-trip differs from the original");
    std::printf("[4] raw seed ok with identical round-trip (%zu bytes)\n", seedLen);

    // ---- [5] diagnosis of the historical bug (v1.1.0 to v1.1.2) ----
    // i2d_PrivateKey with Ed25519: the SDK BoringSSL returns -1. That is the
    // reason the private key was never persisted as PKCS#8.
    if (i2d_PrivateKey(key, nullptr) > 0)
        std::printf("[5] i2d_PrivateKey accepts Ed25519 (OpenSSL-like)\n");
    else
        std::printf("[5] i2d_PrivateKey rejects Ed25519 (confirms the old root cause)\n");

    // ---- [6] signNonce(): signature of the 32-byte challenge ----
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
    if (sigLen != 64) return fail(11, "signature size != 64");
    std::printf("[6] challenge signature ok (%zu bytes)\n", sigLen);

    // ---- [7] server-style verification (SPKI DER + DigestVerify) ----
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
    std::printf("[7] server-style verification ok\n");

    // ---- [8] migracao legada: PKCS#8 completo parseia e assina ----
    const unsigned char* r = kLegacyPkcs8;
    EVP_PKEY* lk = d2i_AutoPrivateKey(nullptr, &r, static_cast<long>(sizeof(kLegacyPkcs8)));
    if (!lk) return fail(16, "d2i_AutoPrivateKey (legacy vault PKCS#8)");
    EVP_MD_CTX* lctx = EVP_MD_CTX_new();
    unsigned char lsig[128] = {0};
    size_t lsigLen = sizeof(lsig);
    if (!lctx) return fail(17, "EVP_MD_CTX_new (legado)");
    if (EVP_DigestSignInit(lctx, nullptr, nullptr, nullptr, lk) != 1)
        return fail(18, "EVP_DigestSignInit (legacy key)");
    if (EVP_DigestSign(lctx, lsig, &lsigLen, nonce, sizeof(nonce)) != 1)
        return fail(19, "EVP_DigestSign (legacy key)");
    EVP_MD_CTX_free(lctx);
    EVP_PKEY_free(lk);
    std::printf("[8] signature with the legacy PKCS#8 key ok (%zu bytes)\n", lsigLen);

    // ---- [9] seed crua e rejeitada pelo parser ASN.1 direto ----
    const unsigned char* s = kRawSeedNotAsn1;
    EVP_PKEY* wrong = d2i_AutoPrivateKey(nullptr, &s, 32);
    if (wrong) {
        EVP_PKEY_free(wrong);
        return fail(20, "seed crua parseou como ASN.1 (inesperado)");
    }
    std::printf("[9] seed crua rejeitada pelo parser PKCS#8 direto (esperado)\n");

    // ---- [10] portable backup: PBKDF2-HMAC-SHA256 (same sequence as
    // exportIdentityBackupFile -- "halla-identity-backup" v1 format, identical
    // to Halla Mobile) ----
    unsigned char salt[16], iv[12];
    if (RAND_bytes(salt, sizeof(salt)) != 1) return fail(21, "RAND_bytes (salt)");
    if (RAND_bytes(iv, sizeof(iv)) != 1) return fail(22, "RAND_bytes (iv)");
    unsigned char backupKey[32];

    // Known vectors BEFORE the real use: they prove the hand-written PBKDF2 is
    // byte for byte the same as Java PBKDF2WithHmacSHA256 (Halla Mobile).
    unsigned char vec[32];
    if (!smokePbkdf2(reinterpret_cast<const unsigned char*>("password"), 8,
                     reinterpret_cast<const unsigned char*>("salt"), 4, 1, vec)
            || !smokeHexEq(vec, kPbkdf2Vec1))
        return fail(23, "PBKDF2 RFC vector c=1");
    if (!smokePbkdf2(reinterpret_cast<const unsigned char*>("password"), 8,
                     reinterpret_cast<const unsigned char*>("salt"), 4, 2, vec)
            || !smokeHexEq(vec, kPbkdf2Vec2))
        return fail(24, "PBKDF2 RFC vector c=2");
    if (!smokePbkdf2(reinterpret_cast<const unsigned char*>("password"), 8,
                     reinterpret_cast<const unsigned char*>("salt"), 4, 4096, vec)
            || !smokeHexEq(vec, kPbkdf2Vec3))
        return fail(25, "PBKDF2 RFC vector c=4096");
    std::printf("[10] hand-written PBKDF2-HMAC-SHA256 ok (3 RFC vectors checked)\n");

    // The real export sequence: password with an EXPLICIT length (never -1),
    // random salt, 310000 iterations.
    if (!smokePbkdf2(reinterpret_cast<const unsigned char*>("senha-do-backup-123"), 19,
                     salt, sizeof(salt), 310000, backupKey))
        return fail(26, "PBKDF2 (310000 iterations)");
    std::printf("[10b] PBKDF2 310000 iterations ok\n");

    // ---- [11] portable backup: AES-256-GCM with AAD + tag ----
    // The plaintext is the minimal PKCS#8 (header + seed) -- exactly what the
    // export writes to the file (and the SAME layout Java/Android produces).
    unsigned char pkcs8[48];
    std::memcpy(pkcs8, kPkcs8SeedHeader, sizeof(kPkcs8SeedHeader));
    std::memcpy(pkcs8 + sizeof(kPkcs8SeedHeader), seedBack, 32);
    const char* aad = "halla-identity-backup|1|desktop|Ed25519|PublicKeyBase64==";
    EVP_CIPHER_CTX* ectx = EVP_CIPHER_CTX_new();
    if (!ectx) return fail(27, "EVP_CIPHER_CTX_new (encrypt)");
    unsigned char ct[64 + 16] = {0};
    int len = 0, total = 0;
    if (EVP_EncryptInit_ex(ectx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1)
        return fail(28, "EVP_EncryptInit_ex (GCM)");
    if (EVP_CIPHER_CTX_ctrl(ectx, EVP_CTRL_GCM_SET_IVLEN, 12, nullptr) != 1)
        return fail(29, "EVP_CTRL_GCM_SET_IVLEN");
    if (EVP_EncryptInit_ex(ectx, nullptr, nullptr, backupKey, iv) != 1)
        return fail(30, "EVP_EncryptInit_ex (key/iv)");
    if (EVP_EncryptUpdate(ectx, nullptr, &len,
                          reinterpret_cast<const unsigned char*>(aad),
                          static_cast<int>(std::strlen(aad))) != 1)
        return fail(31, "EVP_EncryptUpdate (AAD)");
    if (EVP_EncryptUpdate(ectx, ct, &len, pkcs8, sizeof(pkcs8)) != 1)
        return fail(32, "EVP_EncryptUpdate (plaintext)");
    total = len;
    if (EVP_EncryptFinal_ex(ectx, ct + total, &len) != 1)
        return fail(33, "EVP_EncryptFinal_ex");
    total += len;
    if (EVP_CIPHER_CTX_ctrl(ectx, EVP_CTRL_GCM_GET_TAG, 16, ct + total) != 1)
        return fail(34, "EVP_CTRL_GCM_GET_TAG");
    total += 16;
    EVP_CIPHER_CTX_free(ectx);
    if (total != 48 + 16) return fail(35, "unexpected ciphertext size");
    std::printf("[11] AES-256-GCM with AAD and tag ok (%d bytes)\n", total);

    // ---- [12] portable backup: decrypt and check the plaintext ----
    EVP_CIPHER_CTX* dctx = EVP_CIPHER_CTX_new();
    if (!dctx) return fail(36, "EVP_CIPHER_CTX_new (decrypt)");
    unsigned char pt[64] = {0};
    unsigned char tag[16];
    std::memcpy(tag, ct + 48, sizeof(tag));
    len = 0;
    if (EVP_DecryptInit_ex(dctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1)
        return fail(37, "EVP_DecryptInit_ex (GCM)");
    if (EVP_CIPHER_CTX_ctrl(dctx, EVP_CTRL_GCM_SET_IVLEN, 12, nullptr) != 1)
        return fail(38, "EVP_CTRL_GCM_SET_IVLEN (decrypt)");
    if (EVP_DecryptInit_ex(dctx, nullptr, nullptr, backupKey, iv) != 1)
        return fail(39, "EVP_DecryptInit_ex (key/iv)");
    if (EVP_DecryptUpdate(dctx, nullptr, &len,
                          reinterpret_cast<const unsigned char*>(aad),
                          static_cast<int>(std::strlen(aad))) != 1)
        return fail(40, "EVP_DecryptUpdate (AAD decrypt)");
    if (EVP_DecryptUpdate(dctx, pt, &len, ct, 48) != 1)
        return fail(41, "EVP_DecryptUpdate (cifrado)");
    if (EVP_CIPHER_CTX_ctrl(dctx, EVP_CTRL_GCM_SET_TAG, sizeof(tag), tag) != 1)
        return fail(42, "EVP_CTRL_GCM_SET_TAG");
    if (EVP_DecryptFinal_ex(dctx, pt + len, &len) != 1)
        return fail(43, "EVP_DecryptFinal_ex (tag ou senha errada)");
    EVP_CIPHER_CTX_free(dctx);
    if (std::memcmp(pt, pkcs8, sizeof(pkcs8)) != 0)
        return fail(44, "decrypted plaintext differs from the original");
    std::printf("[12] GCM decryption matches the original PKCS#8\n");

    // ---- [13] AAD adulterada: a decifragem TEM que falhar ----
    EVP_CIPHER_CTX* wctx = EVP_CIPHER_CTX_new();
    if (!wctx) return fail(45, "EVP_CIPHER_CTX_new (AAD errada)");
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
    if (aadRejected) return fail(46, "AAD adulterada foi aceita (inesperado)");
    std::printf("[13] AAD adulterada rejeitada pela tag GCM (esperado)\n");

    EVP_PKEY_free(key);
    std::printf("ed25519_identity_smoke: TUDO OK\n");
    return 0;
}

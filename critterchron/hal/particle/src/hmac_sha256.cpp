#include "hmac_sha256.h"
#include <string.h>

void hmac_sha256_init(HMAC_SHA256_CTX *ctx, const uint8_t *key, size_t key_len) {
    uint8_t k_ipad[64];
    uint8_t key_hashed[32];
    const uint8_t *key_used = key;
    size_t key_used_len = key_len;

    if (key_len > 64) {
        SHA256_CTX tmp;
        sha256_init(&tmp);
        sha256_update(&tmp, key, key_len);
        sha256_final(&tmp, key_hashed);
        key_used = key_hashed;
        key_used_len = 32;
    }

    memset(k_ipad,      0, sizeof(k_ipad));
    memset(ctx->k_opad, 0, sizeof(ctx->k_opad));
    memcpy(k_ipad,      key_used, key_used_len);
    memcpy(ctx->k_opad, key_used, key_used_len);

    for (int i = 0; i < 64; i++) {
        k_ipad[i]      ^= 0x36;
        ctx->k_opad[i] ^= 0x5c;
    }

    sha256_init(&ctx->inner);
    sha256_update(&ctx->inner, k_ipad, 64);
}

void hmac_sha256_update(HMAC_SHA256_CTX *ctx, const uint8_t *data, size_t len) {
    sha256_update(&ctx->inner, data, len);
}

void hmac_sha256_final(HMAC_SHA256_CTX *ctx, uint8_t mac[32]) {
    sha256_final(&ctx->inner, mac);

    SHA256_CTX outer;
    sha256_init(&outer);
    sha256_update(&outer, ctx->k_opad, 64);
    sha256_update(&outer, mac, 32);
    sha256_final(&outer, mac);
}

void hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *data, size_t data_len, uint8_t *mac) {
    HMAC_SHA256_CTX ctx;
    hmac_sha256_init  (&ctx, key, key_len);
    hmac_sha256_update(&ctx, data, data_len);
    hmac_sha256_final (&ctx, mac);
}

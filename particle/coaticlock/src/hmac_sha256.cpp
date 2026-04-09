#include "hmac_sha256.h"
#include <string.h>

void hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *data, size_t data_len, uint8_t *mac) {
    SHA256_CTX context;
    uint8_t k_ipad[64];
    uint8_t k_opad[64];
    uint8_t key_hashed[32];
    const uint8_t *key_used = key;
    size_t key_used_len = key_len;

    if (key_len > 64) {
        sha256_init(&context);
        sha256_update(&context, key, key_len);
        sha256_final(&context, key_hashed);
        key_used = key_hashed;
        key_used_len = 32;
    }

    memset(k_ipad, 0, sizeof(k_ipad));
    memset(k_opad, 0, sizeof(k_opad));
    memcpy(k_ipad, key_used, key_used_len);
    memcpy(k_opad, key_used, key_used_len);

    for (int i = 0; i < 64; i++) {
        k_ipad[i] ^= 0x36;
        k_opad[i] ^= 0x5c;
    }

    sha256_init(&context);
    sha256_update(&context, k_ipad, 64);
    sha256_update(&context, data, data_len);
    sha256_final(&context, mac);

    sha256_init(&context);
    sha256_update(&context, k_opad, 64);
    sha256_update(&context, mac, 32);
    sha256_final(&context, mac);
}

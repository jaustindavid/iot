#ifndef HMAC_SHA256_H
#define HMAC_SHA256_H

#include "sha256.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    SHA256_CTX inner;
    uint8_t    k_opad[64];
} HMAC_SHA256_CTX;

// Streaming API — lets callers hash data as it arrives (e.g. a TCP body
// read in chunks) without buffering the whole payload.
void hmac_sha256_init  (HMAC_SHA256_CTX *ctx, const uint8_t *key, size_t key_len);
void hmac_sha256_update(HMAC_SHA256_CTX *ctx, const uint8_t *data, size_t len);
void hmac_sha256_final (HMAC_SHA256_CTX *ctx, uint8_t mac[32]);

// One-shot convenience wrapper; implemented in terms of the streaming API.
//
// Renamed vs the Particle version because ESP-IDF's wpa_supplicant
// bundles its own `hmac_sha256` free symbol (crypto_mbedtls.c) and the
// ESP32 toolchain force-links libwpa_supplicant.a — `multiple definition
// of hmac_sha256` otherwise. The streaming variants (init/update/final)
// don't collide because mbedtls uses different names for those, so they
// ride through the header unchanged.
void hmac_sha256_oneshot(const uint8_t *key, size_t key_len, const uint8_t *data, size_t data_len, uint8_t *mac);

#ifdef __cplusplus
}
#endif

#endif

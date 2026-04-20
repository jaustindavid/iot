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
void hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *data, size_t data_len, uint8_t *mac);

#ifdef __cplusplus
}
#endif

#endif

#ifndef HMAC_SHA256_H
#define HMAC_SHA256_H

#include "sha256.h"

#ifdef __cplusplus
extern "C" {
#endif

void hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *data, size_t data_len, uint8_t *mac);

#ifdef __cplusplus
}
#endif

#endif

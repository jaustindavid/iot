#pragma once
// Native Stra2us client using POSIX BSD sockets — no esp_http_client dependency.
// BSD sockets are always available in ESP-IDF; no cmake component tweaks needed.
//
// Signing scheme matches upstream IoTClient:
//   HMAC-SHA256 over: uri + body_bytes + timestamp_string
//   Headers: X-Client-ID, X-Timestamp, X-Signature

#include <cstdio>
#include <cstring>
#include <ctime>
#include <cerrno>
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "mbedtls/md.h"
#include "esp_log.h"

static const char* STRA2US_TAG = "stra2us";

static void _s2_hex_to_bytes(const char* hex, uint8_t* out, size_t n) {
    for (size_t i = 0; i < n; i++) {
        char o[3] = {hex[i*2], hex[i*2+1], '\0'};
        out[i] = (uint8_t)strtol(o, nullptr, 16);
    }
}

static void _s2_sign(const char* secret_hex,
                      const char* uri,
                      const char* body, size_t body_len,
                      uint32_t ts,
                      char* out_hex /* 65 bytes */) {
    uint8_t secret[32];
    _s2_hex_to_bytes(secret_hex, secret, 32);

    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    mbedtls_md_setup(&ctx, info, 1);
    mbedtls_md_hmac_starts(&ctx, secret, 32);
    mbedtls_md_hmac_update(&ctx, (const uint8_t*)uri, strlen(uri));
    if (body && body_len > 0)
        mbedtls_md_hmac_update(&ctx, (const uint8_t*)body, body_len);
    char ts_str[16];
    snprintf(ts_str, sizeof(ts_str), "%lu", (unsigned long)ts);
    mbedtls_md_hmac_update(&ctx, (const uint8_t*)ts_str, strlen(ts_str));
    uint8_t result[32];
    mbedtls_md_hmac_finish(&ctx, result);
    mbedtls_md_free(&ctx);
    for (int i = 0; i < 32; i++) snprintf(&out_hex[i*2], 3, "%02x", result[i]);
    out_hex[64] = '\0';
}

// Publish a plain-text string to a Stra2us queue topic (FR-1: text/plain).
// Returns HTTP status code, or -1 on connection failure.
static int stra2us_publish(const char* host, int port,
                            const char* client_id, const char* secret_hex,
                            const char* topic,     const char* message) {
    char uri[64];
    snprintf(uri, sizeof(uri), "/q/%s", topic);

    uint32_t ts       = (uint32_t)::time(nullptr);
    size_t   body_len = strlen(message);

    char sig[65];
    _s2_sign(secret_hex, uri, message, body_len, ts, sig);

    // Resolve host via lwip DNS
    struct addrinfo hints{};
    addrinfo* res = nullptr;
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", port);

    if (lwip_getaddrinfo(host, port_str, &hints, &res) != 0 || !res) {
        ESP_LOGE(STRA2US_TAG, "DNS failed for %s", host);
        return -1;
    }

    // Use lwip_ prefixed calls to avoid collision with esphome::socket namespace
    int fd = lwip_socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) { lwip_freeaddrinfo(res); return -1; }

    struct timeval tv = {5, 0};
    lwip_setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    lwip_setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (lwip_connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
        ESP_LOGE(STRA2US_TAG, "Connect failed to %s:%d (err %d)", host, port, errno);
        lwip_freeaddrinfo(res);
        lwip_close(fd);
        return -1;
    }
    lwip_freeaddrinfo(res);

    // Build HTTP/1.1 request
    char req[768];
    int req_len = snprintf(req, sizeof(req),
        "POST %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: %zu\r\n"
        "X-Client-ID: %s\r\n"
        "X-Timestamp: %lu\r\n"
        "X-Signature: %s\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%s",
        uri, host, port, body_len,
        client_id, (unsigned long)ts, sig,
        message);

    lwip_send(fd, req, req_len, 0);

    // Read just the status line
    char resp[64] = {};
    lwip_recv(fd, resp, sizeof(resp) - 1, 0);
    lwip_close(fd);

    // Parse "HTTP/1.1 NNN ..."
    int status = -1;
    const char* sp = strchr(resp, ' ');
    if (sp) status = atoi(sp + 1);

    ESP_LOGI(STRA2US_TAG, "POST /q/%s -> %d", topic, status);
    return status;
}

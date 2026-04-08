#pragma once
// Native Stra2us client using POSIX BSD sockets — no esp_http_client dependency.
//
// Connection model: persistent keep-alive socket.
//   The first call in a cycle connects; subsequent calls in the same cycle
//   reuse the open socket (server keepalive timeout >> time between calls in
//   one lambda execution).  Between cycles (~60 s) the server will have closed
//   its end; the next call detects the dead socket on write, reconnects, and
//   retries the request once automatically.
//
// Signing scheme: HMAC-SHA256 over (uri + body_bytes + timestamp_string)
// Headers: X-Client-ID, X-Timestamp, X-Signature

#include <cstdio>
#include <cstring>
#include <ctime>
#include <cerrno>
#include <fcntl.h>
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "mbedtls/md.h"
#include "esp_log.h"

static const char* STRA2US_TAG = "stra2us";

// Persistent socket — shared across all calls within a single ESPHome cycle.
// Reset to -1 whenever the connection is lost; re-established on next call.
static int s_stra2us_fd = -1;


// ============================================================
// Internal helpers
// ============================================================

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

// Close the persistent socket and reset state.
static void _s2_close() {
    if (s_stra2us_fd >= 0) {
        lwip_close(s_stra2us_fd);
        s_stra2us_fd = -1;
        ESP_LOGI(STRA2US_TAG, "socket closed");
    }
}

// Return the open fd, connecting first if needed.  -1 on failure.
static int _s2_ensure_connected(const char* host, int port) {
    if (s_stra2us_fd >= 0) return s_stra2us_fd;

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

    int fd = lwip_socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) { lwip_freeaddrinfo(res); return -1; }

    // 5 s recv/send timeout for data phase
    struct timeval tv = {5, 0};
    lwip_setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    lwip_setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    // Non-blocking connect with a hard 4 s select() timeout.
    // lwip_connect() ignores SO_SNDTIMEO; without select() the call blocks
    // until the TCP stack gives up (~75 s), which kills the ESPHome WDT.
    int flags = lwip_fcntl(fd, F_GETFL, 0);
    lwip_fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    int cr = lwip_connect(fd, res->ai_addr, res->ai_addrlen);
    if (cr != 0 && errno != EINPROGRESS) {
        ESP_LOGE(STRA2US_TAG, "connect failed %s:%d err=%d", host, port, errno);
        lwip_freeaddrinfo(res); lwip_close(fd); return -1;
    }

    fd_set wfds; FD_ZERO(&wfds); FD_SET(fd, &wfds);
    struct timeval conn_tv = {4, 0};
    if (lwip_select(fd + 1, nullptr, &wfds, nullptr, &conn_tv) <= 0) {
        ESP_LOGE(STRA2US_TAG, "connect timeout %s:%d", host, port);
        lwip_freeaddrinfo(res); lwip_close(fd); return -1;
    }
    int so_err = 0; socklen_t so_len = sizeof(so_err);
    lwip_getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_err, &so_len);
    if (so_err != 0) {
        ESP_LOGE(STRA2US_TAG, "connect refused %s:%d err=%d", host, port, so_err);
        lwip_freeaddrinfo(res); lwip_close(fd); return -1;
    }

    lwip_fcntl(fd, F_SETFL, flags);  // restore blocking
    lwip_freeaddrinfo(res);
    s_stra2us_fd = fd;
    ESP_LOGI(STRA2US_TAG, "connected to %s:%d fd=%d", host, port, fd);
    return fd;
}

// Send all bytes, retrying on partial writes.  Returns false (and closes
// socket) on error.
static bool _s2_send_all(const char* data, int len) {
    int sent = 0;
    while (sent < len) {
        int n = lwip_send(s_stra2us_fd, data + sent, len - sent, 0);
        if (n <= 0) { _s2_close(); return false; }
        sent += n;
    }
    return true;
}

// Read the full HTTP response from the persistent socket.
//   - Parses the status code from the status line.
//   - Parses Content-Length to consume exactly the right body bytes, leaving
//     the socket positioned at the start of the next response.
//   - Copies up to (body_out_len - 1) body bytes into body_out if non-null.
//   - Returns HTTP status code, or -1 on socket error (socket is closed).
static int _s2_read_response(char* body_out, size_t body_out_len) {
    int fd = s_stra2us_fd;
    char buf[512];
    int total = 0;

    // Read until we have the complete header block (\r\n\r\n).
    while (total < (int)sizeof(buf) - 1) {
        int n = lwip_recv(fd, buf + total, sizeof(buf) - 1 - total, 0);
        if (n <= 0) { _s2_close(); return -1; }
        total += n;
        buf[total] = '\0';
        if (strstr(buf, "\r\n\r\n")) break;
    }

    // Parse status code from "HTTP/1.1 NNN ..."
    int status = -1;
    const char* sp = strchr(buf, ' ');
    if (sp) status = atoi(sp + 1);

    // Parse Content-Length.  FastAPI/uvicorn sends lowercase headers.
    int content_length = 0;
    const char* cl = strstr(buf, "content-length:");
    if (!cl) cl = strstr(buf, "Content-Length:");
    if (cl) {
        cl += 15;  // both spellings are 15 chars
        while (*cl == ' ') cl++;
        content_length = atoi(cl);
    }

    // Locate body start
    const char* hdr_end = strstr(buf, "\r\n\r\n");
    if (!hdr_end) { _s2_close(); return -1; }
    int hdr_len   = (int)(hdr_end - buf) + 4;
    int body_have = total - hdr_len;
    const char* body_start = buf + hdr_len;

    // Hand body to caller — first from bytes already in buf alongside headers.
    int body_filled = 0;
    if (body_out && body_out_len > 0 && body_have > 0) {
        int copy = (body_have < (int)body_out_len - 1)
                   ? body_have : (int)body_out_len - 1;
        memcpy(body_out, body_start, copy);
        body_filled = copy;
    }

    // Read any remaining body bytes still in flight.
    // Fill body_out first (handles the case where headers and body arrived in
    // separate TCP packets — body_have==0 but content_length>0), then drain
    // any overflow to trash so the socket is clean for the next request.
    int remaining = content_length - body_have;
    while (remaining > 0) {
        if (body_out && body_out_len > 0 && body_filled < (int)body_out_len - 1) {
            int space   = (int)body_out_len - 1 - body_filled;
            int to_read = remaining < space ? remaining : space;
            int n = lwip_recv(fd, body_out + body_filled, to_read, 0);
            if (n <= 0) { _s2_close(); return -1; }
            body_filled += n;
            remaining   -= n;
        } else {
            char trash[64];
            int to_read = remaining < (int)sizeof(trash) ? remaining : (int)sizeof(trash);
            int n = lwip_recv(fd, trash, to_read, 0);
            if (n <= 0) { _s2_close(); return -1; }
            remaining -= n;
        }
    }
    if (body_out && body_out_len > 0)
        body_out[body_filled] = '\0';


    return status;
}

// ============================================================
// Public API
// ============================================================

// Publish a plain-text string to a Stra2us queue topic.
// Returns the HTTP status code, or -1 on connection failure.
//
// Uses (and keeps open) the shared persistent socket.  If the socket has
// been closed by the server since the last call, it reconnects and retries
// the request exactly once.
static int stra2us_publish(const char* host, int port,
                            const char* client_id, const char* secret_hex,
                            const char* topic,     const char* message) {
    char uri[64];
    snprintf(uri, sizeof(uri), "/q/%s", topic);

    uint32_t ts       = (uint32_t)::time(nullptr);
    size_t   body_len = strlen(message);
    char sig[65];
    _s2_sign(secret_hex, uri, message, body_len, ts, sig);

    char req[768];
    int req_len = snprintf(req, sizeof(req),
        "POST %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: %zu\r\n"
        "X-Client-ID: %s\r\n"
        "X-Timestamp: %lu\r\n"
        "X-Signature: %s\r\n"
        "Connection: keep-alive\r\n"
        "\r\n"
        "%s",
        uri, host, port, body_len,
        client_id, (unsigned long)ts, sig,
        message);

    // Try once; on dead-socket error reconnect and retry once.
    for (int attempt = 0; attempt < 2; attempt++) {
        if (_s2_ensure_connected(host, port) < 0) return -1;
        if (_s2_send_all(req, req_len)) break;
        if (attempt == 1) return -1;  // retry failed
        // s_stra2us_fd is now -1 (closed by _s2_send_all); loop reconnects
    }

    int status = _s2_read_response(nullptr, 0);
    if (status == -1) {
        // The server closed the keepalive connection between our write and
        // read (common after the 60 s idle gap between heartbeats). The send
        // likely succeeded, but we can't confirm — resend on a fresh socket.
        // Duplicate heartbeats are harmless for telemetry.
        ESP_LOGW(STRA2US_TAG, "POST %s read failed, retrying on new socket", uri);
        if (_s2_ensure_connected(host, port) >= 0 && _s2_send_all(req, req_len))
            status = _s2_read_response(nullptr, 0);
    }
    ESP_LOGI(STRA2US_TAG, "POST %s -> %d", uri, status);
    return status;
}

// Read a KV value from Stra2us.
//
// Checks the key in the order provided; the first hit wins (cascade pattern):
//   stra2us_kv_get(..., "coaticlock/bb32/wobble_max_minutes", val, sizeof(val))
//   stra2us_kv_get(..., "coaticlock/wobble_max_minutes",      val, sizeof(val))
//
// Returns true and fills val_out (null-terminated string) when the key exists
// and holds a msgpack-encoded plain-text value (as written by POST /kv with
// Content-Type: text/plain).
// Returns false when the key is absent, or on any network/parse error.
//
// Uses the same shared persistent socket as stra2us_publish().
static bool stra2us_kv_get(const char* host, int port,
                            const char* client_id, const char* secret_hex,
                            const char* key,
                            char* val_out, size_t val_out_len) {
    char uri[128];
    snprintf(uri, sizeof(uri), "/kv/%s", key);

    uint32_t ts = (uint32_t)::time(nullptr);
    char sig[65];
    _s2_sign(secret_hex, uri, nullptr, 0, ts, sig);

    char req[512];
    int req_len = snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "X-Client-ID: %s\r\n"
        "X-Timestamp: %lu\r\n"
        "X-Signature: %s\r\n"
        "Connection: keep-alive\r\n"
        "\r\n",
        uri, host, port,
        client_id, (unsigned long)ts, sig);

    for (int attempt = 0; attempt < 2; attempt++) {
        if (_s2_ensure_connected(host, port) < 0) return false;
        if (_s2_send_all(req, req_len)) break;
        if (attempt == 1) return false;
    }

    // Body is small: either msgpack-encoded string (hit) or
    // msgpack-encoded {"status":"not_found"} map (miss).
    char body[64] = {};
    int status = _s2_read_response(body, sizeof(body));
    if (status == -1) {
        // Read failed after send — keepalive connection closed by server.
        // Retry GET on a fresh socket; GETs are naturally idempotent.
        ESP_LOGW(STRA2US_TAG, "GET %s read failed, retrying on new socket", uri);
        if (_s2_ensure_connected(host, port) >= 0 && _s2_send_all(req, req_len))
            status = _s2_read_response(body, sizeof(body));
    }
    if (status != 200) {
        ESP_LOGW(STRA2US_TAG, "GET %s -> %d", uri, status);
        return false;
    }

    // Decode the msgpack response.
    // Strings (text/plain-encoded values):
    //   fixstr  0xa0..0xbf : 1-byte header, len in low 5 bits
    //   str8    0xd9       : 2-byte header
    //   str16   0xda       : 3-byte header
    // Integers (values stored as numeric types by some clients):
    //   positive fixint 0x00..0x7f : value is the byte
    //   uint8   0xcc, uint16 0xcd, uint32 0xce
    //   int8    0xd0, int16  0xd1, int32  0xd2
    //   negative fixint 0xe0..0xff
    // Anything else (e.g. fixmap 0x81 = {"status":"not_found"}) is a miss.
    uint8_t* b = (uint8_t*)body;
    const char* str_data = nullptr;
    int str_len = 0;
    long long ival = 0;
    bool is_int = false;

    if ((b[0] & 0xe0) == 0xa0) {        // fixstr
        str_len  = b[0] & 0x1f;
        str_data = body + 1;
    } else if (b[0] == 0xd9) {          // str8
        str_len  = b[1];
        str_data = body + 2;
    } else if (b[0] == 0xda) {          // str16
        str_len  = ((uint16_t)b[1] << 8) | b[2];
        str_data = body + 3;
    } else if (b[0] <= 0x7f) {          // positive fixint
        ival = b[0]; is_int = true;
    } else if (b[0] == 0xcc) {          // uint8
        ival = b[1]; is_int = true;
    } else if (b[0] == 0xcd) {          // uint16
        ival = ((uint16_t)b[1] << 8) | b[2]; is_int = true;
    } else if (b[0] == 0xce) {          // uint32
        ival = ((uint32_t)b[1] << 24) | ((uint32_t)b[2] << 16)
             | ((uint32_t)b[3] << 8)  |  b[4]; is_int = true;
    } else if ((b[0] & 0xe0) == 0xe0) { // negative fixint
        ival = (int8_t)b[0]; is_int = true;
    } else if (b[0] == 0xd0) {          // int8
        ival = (int8_t)b[1]; is_int = true;
    } else if (b[0] == 0xd1) {          // int16
        ival = (int16_t)(((uint16_t)b[1] << 8) | b[2]); is_int = true;
    } else if (b[0] == 0xd2) {          // int32
        ival = (int32_t)(((uint32_t)b[1] << 24) | ((uint32_t)b[2] << 16)
                       | ((uint32_t)b[3] << 8)  |  b[4]); is_int = true;
    } else {
        // Not a string or integer — genuine not_found or unknown type.
        ESP_LOGI(STRA2US_TAG, "GET %s -> not_found", uri);
        return false;
    }

    if (is_int) {
        snprintf(val_out, val_out_len, "%lld", ival);
        ESP_LOGI(STRA2US_TAG, "GET %s -> \"%s\"", uri, val_out);
        return true;
    }

    int copy = (str_len < (int)val_out_len - 1) ? str_len : (int)val_out_len - 1;
    memcpy(val_out, str_data, copy);
    val_out[copy] = '\0';
    ESP_LOGI(STRA2US_TAG, "GET %s -> \"%s\"", uri, val_out);
    return true;
}


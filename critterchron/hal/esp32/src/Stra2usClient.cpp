#if defined(ARDUINO_ARCH_ESP32)

// Ported from hal/particle/src/Stra2usClient.cpp. Mechanical swaps only:
//   * PLATFORM_ID gate  →  ARDUINO_ARCH_ESP32
//   * Time.now()        →  (uint32_t)::time(nullptr)   (libc wall clock)
//   * Particle.process() →  yield()                   (FreeRTOS preempts, but
//                                                      yield still lets the
//                                                      tel task give up the
//                                                      rest of its slice on
//                                                      a cooperative stall)
//   * Log.info/warn/error →  LOG_INFO/LOG_WARN/LOG_ERR macros
//                            (Serial.printf with an "[s2s] " tag so the
//                            serial stream is still grep-friendly)
//   * TCPClient         →  WiFiClient                 (header-level swap
//                                                      only; API surface
//                                                      used here —
//                                                      connected/connect/
//                                                      stop/read/write/
//                                                      available — is
//                                                      identical)
// Everything else — HMAC stream-verify, msgpack parse, content_sha
// normalization — is portable C++ and copies verbatim. Keeping this
// file line-diff-able with the Particle version is worth more than any
// cosmetic reshuffle; resist the urge.
#include "Stra2usClient.h"
#include "ErrLog.h"
#include "ir/IrRuntime.h"
#include "sha256.h"
#include <Arduino.h>
#include <Update.h>     // arduino-esp32 Update class for OTA partition writes
#include <string.h>
#include <strings.h>   // strcasestr (newlib GNU extension; available on
                       // Arduino-ESP32 under the default -D_GNU_SOURCE)
#include <time.h>

// Error-channel shorthand. Mirror of hal/particle/src/Stra2usClient.cpp;
// LOG_WARN/LOG_ERR macros remain in place but the OTA failure paths now
// fan out to g_errlog.record() so the heartbeat carries the message
// remotely. See hal/ErrLog.h for the contract.
using critterchron::g_errlog;
using critterchron::ErrCat;

// strcasestr is a GNU extension; Arduino-ESP32's newlib exposes it, but
// guard anyway with a tiny fallback in case a future core version drops it.
#if !defined(strcasestr)
static const char* s2s_strcasestr_(const char* h, const char* n) {
    if (!*n) return h;
    for (; *h; ++h) {
        const char* a = h; const char* b = n;
        while (*a && *b && (tolower((unsigned char)*a) == tolower((unsigned char)*b))) { ++a; ++b; }
        if (!*b) return h;
    }
    return nullptr;
}
#define strcasestr s2s_strcasestr_
#endif

// Serial-backed log shim. Particle's LOG_INFO("fmt", ...) was a sugar over
// Serial.printf with a priority level and a newline. We don't have the
// priority gate on ESP32 yet (no SerialLogHandler equivalent) so every call
// goes to the console. Newline appended to match the Particle log format
// so scripts watching for line breaks don't have to know which platform
// emitted the line.
#define LOG_INFO(...) do { Serial.printf("[s2s] "  __VA_ARGS__); Serial.print('\n'); } while (0)
#define LOG_WARN(...) do { Serial.printf("[s2s!] " __VA_ARGS__); Serial.print('\n'); } while (0)
#define LOG_ERR(...)  do { Serial.printf("[s2sX] " __VA_ARGS__); Serial.print('\n'); } while (0)

Stra2usClient::Stra2usClient(const char* host, int port,
                             const char* client_id, const char* secret_hex,
                             const char* app, const char* device)
    : host_(host), port_(port),
      client_id_(client_id), secret_hex_(secret_hex),
      app_(app), device_(device) {}

// -------- Config surface (hot path, RAM only) --------

size_t Stra2usClient::register_key_(const char* key, bool is_float,
                                    int def_i, float def_f) const {
    for (size_t i = 0; i < cache_count_; ++i) {
        if (strncmp(cache_[i].key, key, KEY_MAX) == 0) return i;
    }
    if (cache_count_ >= CACHE_CAP) return (size_t)-1;

    Entry& e = cache_[cache_count_];
    strncpy(e.key, key, KEY_MAX - 1);
    e.key[KEY_MAX - 1] = '\0';
    e.is_float = is_float;
    e.has_live = false;
    e.def_i = def_i;
    e.def_f = def_f;
    e.live_i = def_i;
    e.live_f = def_f;

    size_t idx = cache_count_;
    cache_count_ = idx + 1;   // bump last so pollers never see a partial entry
    return idx;
}

bool Stra2usClient::has(const char* key) const {
    for (size_t i = 0; i < cache_count_; ++i) {
        if (strncmp(cache_[i].key, key, KEY_MAX) == 0)
            return cache_[i].has_live;
    }
    return false;
}

int Stra2usClient::get_int(const char* key, int def) const {
    size_t i = register_key_(key, false, def, 0.0f);
    if (i == (size_t)-1) return def;
    return cache_[i].has_live ? cache_[i].live_i : def;
}

float Stra2usClient::get_float(const char* key, float def) const {
    size_t i = register_key_(key, true, 0, def);
    if (i == (size_t)-1) return def;
    return cache_[i].has_live ? cache_[i].live_f : def;
}

// -------- Telemetry side --------

void Stra2usClient::record_latency_(uint32_t ms) {
    if (ms < latency_min_ms_) latency_min_ms_ = ms;
    if (ms > latency_max_ms_) latency_max_ms_ = ms;
    // Don't let the sum/count grow unboundedly if a consumer never runs.
    // 1<<30 samples worth of headroom is far more than a sane heartbeat
    // cadence will ever produce, but the cap prevents overflow if a
    // consumer is wired wrong and never drains.
    if (latency_count_ < (1u << 30)) {
        latency_sum_ms_ += ms;
        latency_count_  += 1;
    }
}

bool Stra2usClient::consume_latency_stats(uint32_t* out_min_ms,
                                          uint32_t* out_mean_ms,
                                          uint32_t* out_max_ms) {
    if (latency_count_ == 0) return false;
    uint32_t mean = latency_sum_ms_ / latency_count_;
    if (out_min_ms)  *out_min_ms  = latency_min_ms_;
    if (out_mean_ms) *out_mean_ms = mean;
    if (out_max_ms)  *out_max_ms  = latency_max_ms_;
    latency_min_ms_ = UINT32_MAX;
    latency_max_ms_ = 0;
    latency_sum_ms_ = 0;
    latency_count_  = 0;
    return true;
}

bool Stra2usClient::connect() {
    if (tcp_.connected()) tcp_.stop();
    return tcp_.connect(host_, port_);
}

void Stra2usClient::close() {
    if (tcp_.connected()) tcp_.stop();
}

bool Stra2usClient::ensure_connected_() {
    if (tcp_.connected()) return true;

    // Reset per-request diag so a stale value from a previous failure
    // doesn't muddle the next failure's log line.
    diag_resolved_ip_[0]  = '\0';
    diag_local_ip_   [0]  = '\0';
    diag_local_port_      = 0;
    diag_dns_ms_          = 0;
    diag_connect_ms_      = 0;
    diag_sent_bytes_      = 0;
    diag_resp_head_len_   = 0;

    // Resolve hostname explicitly before connect so DNS time and the
    // resolved IP are captured separately. Without this, DNS time gets
    // folded into tcp_.connect() and we can't tell "DNS misdirected us
    // to a wrong host" apart from "right host but server is silent."
    IPAddress addr;
    uint32_t t0 = millis();
    bool dns_ok = WiFi.hostByName(host_, addr);
    diag_dns_ms_ = millis() - t0;
    if (dns_ok) {
        snprintf(diag_resolved_ip_, sizeof(diag_resolved_ip_),
                 "%u.%u.%u.%u", addr[0], addr[1], addr[2], addr[3]);
    } else {
        strncpy(diag_resolved_ip_, "DNS_FAIL", sizeof(diag_resolved_ip_));
        diag_resolved_ip_[sizeof(diag_resolved_ip_) - 1] = '\0';
    }

    t0 = millis();
    bool ok = tcp_.connect(host_, port_);
    diag_connect_ms_ = millis() - t0;
    if (ok) {
        IPAddress lip = tcp_.localIP();
        snprintf(diag_local_ip_, sizeof(diag_local_ip_),
                 "%u.%u.%u.%u", lip[0], lip[1], lip[2], lip[3]);
        diag_local_port_ = tcp_.localPort();
    }
    return ok;
}

void Stra2usClient::hex_to_bytes_(const char* hex, uint8_t* out, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        char o[3] = {hex[i*2], hex[i*2+1], '\0'};
        out[i] = (uint8_t)strtol(o, nullptr, 16);
    }
}

void Stra2usClient::kvenc_xor_(uint32_t nonce, uint8_t* data, size_t len) const {
    // Must match server-side `kvenc_xor` in
    // stra2us/backend/src/core/security.py byte-for-byte; cross-impl
    // agreement is unit-tested on the server side. Don't change the
    // label string, the nonce-encoding (uint32 BE), or the counter
    // width (1 byte) without coordinating with the server.
    static constexpr char     LABEL[]   = "stra2us-kvenc-v1";  // 16 bytes, no NUL
    static constexpr size_t   LABEL_LEN = 16;
    uint8_t secret[32];
    hex_to_bytes_(secret_hex_, secret, 32);

    // HMAC input: LABEL(16) + NONCE_BE(4) + COUNTER(1) = 21 bytes
    uint8_t input[LABEL_LEN + 4 + 1];
    memcpy(input, LABEL, LABEL_LEN);
    input[LABEL_LEN + 0] = (uint8_t)((nonce >> 24) & 0xff);
    input[LABEL_LEN + 1] = (uint8_t)((nonce >> 16) & 0xff);
    input[LABEL_LEN + 2] = (uint8_t)((nonce >>  8) & 0xff);
    input[LABEL_LEN + 3] = (uint8_t)( nonce        & 0xff);

    uint8_t block[32];
    size_t  produced = 0;
    uint8_t counter  = 0;
    while (produced < len) {
        input[LABEL_LEN + 4] = counter;
        // ESP32 uses `hmac_sha256_oneshot` (renamed to avoid clashing
        // with arduino-esp32's mbedtls-bundled `hmac_sha256` symbol —
        // see hmac_sha256.h note). Particle has the same shape under
        // the `hmac_sha256` name.
        hmac_sha256_oneshot(secret, 32, input, sizeof(input), block);
        size_t take = (len - produced) < 32 ? (len - produced) : 32;
        for (size_t i = 0; i < take; ++i) {
            data[produced + i] ^= block[i];
        }
        produced += take;
        if (counter == 255) break;  // safety: 256*32 = 8KiB ceiling
        counter++;
    }
}

void Stra2usClient::sign_(const char* uri, const char* body, size_t body_len,
                          uint32_t ts, char* out_hex) {
    uint8_t secret[32];
    hex_to_bytes_(secret_hex_, secret, 32);

    char ts_str[16];
    snprintf(ts_str, sizeof(ts_str), "%lu", (unsigned long)ts);

    // Streaming HMAC over URI || body || ts_str — the previous oneshot
    // path concatenated everything into a 512-byte stack buffer and
    // silently bailed (out_hex empty → server 401 "Invalid Signature")
    // when the total exceeded that. Heartbeat bodies (~600 B) fit;
    // FAILURE_TRIAGE.md §1 snapshot dumps (~3 KB) blew past it. The
    // server-side response-verify path is already streaming — bring
    // the request-side up to the same standard.
    HMAC_SHA256_CTX ctx;
    hmac_sha256_init(&ctx, secret, 32);
    hmac_sha256_update(&ctx, (const uint8_t*)uri, strlen(uri));
    if (body && body_len > 0) {
        hmac_sha256_update(&ctx, (const uint8_t*)body, body_len);
    }
    hmac_sha256_update(&ctx, (const uint8_t*)ts_str, strlen(ts_str));
    uint8_t result[32];
    hmac_sha256_final(&ctx, result);
    for (int i = 0; i < 32; ++i)
        snprintf(&out_hex[i*2], 3, "%02x", result[i]);
    out_hex[64] = '\0';
}

bool Stra2usClient::send_all_(const char* data, int len) {
    int sent = 0;
    while (sent < len) {
        int n = tcp_.write((const uint8_t*)(data + sent), len - sent);
        if (n <= 0) {
            diag_sent_bytes_ = (uint32_t)sent;  // partial-send count for diag
            close();
            return false;
        }
        sent += n;
    }
    diag_sent_bytes_ = (uint32_t)sent;
    return true;
}

bool Stra2usClient::hex_equal_(const char* a, const char* b) {
    // Constant-time 64-char hex compare. Returns false on any length mismatch
    // or differing byte. Loops over a fixed 64 to avoid early-exit timing.
    unsigned char diff = 0;
    for (int i = 0; i < 64; ++i) {
        unsigned char ac = (unsigned char)a[i];
        unsigned char bc = (unsigned char)b[i];
        diff |= ac ^ bc;
        if (ac == 0 || bc == 0) return false;
    }
    return diff == 0;
}

int Stra2usClient::read_response_(const char* uri,
                                  char* body_out, size_t body_out_len,
                                  uint32_t* out_ts) {
    unsigned long start = millis();
    char buf[1024];
    int total = 0;

    while (millis() - start < 5000) {
        if (tcp_.available()) {
            int n = tcp_.read((uint8_t*)(buf + total),
                              sizeof(buf) - 1 - total);
            if (n <= 0) break;
            total += n;
            buf[total] = '\0';
            if (strstr(buf, "\r\n\r\n")) break;
        } else {
            // Particle.process() kept DeviceOS's cooperative event pump
            // alive during a blocking TCP read. On FreeRTOS the kernel
            // preempts on tick, so the equivalent isn't strictly needed,
            // but yield() lets us drop the rest of our slice immediately
            // on a stall — smoother behavior for the rest of the system
            // than a ~10ms busy-check delay alone.
            yield();
        }
        delay(10);
    }
    if (!strstr(buf, "\r\n\r\n")) {
        // Capture first 16 bytes of whatever we DID receive — most embedded
        // HTTP libs throw away "garbage before headers" silently, so if the
        // server sent something but it didn't look like HTTP, this lets the
        // operator see what it was.
        diag_resp_head_len_ = total < (int)sizeof(diag_resp_head_)
                            ? (size_t)total
                            : sizeof(diag_resp_head_);
        if (diag_resp_head_len_ > 0) {
            memcpy(diag_resp_head_, buf, diag_resp_head_len_);
        }

        // Hex-encode the response head for the log line. 16 bytes → 32 hex
        // chars + NUL. If we got nothing (total=0), head_hex stays empty.
        char head_hex[2 * sizeof(diag_resp_head_) + 1] = {0};
        for (size_t i = 0; i < diag_resp_head_len_; ++i) {
            snprintf(head_hex + i * 2, 3, "%02x", diag_resp_head_[i]);
        }
        uint32_t wait_ms = (uint32_t)(millis() - start);

        LOG_WARN("read_response_: header timeout host=%s:%d ip=%s "
                 "local=%s:%u uri=%s sent=%lu recv=%d head=%s "
                 "phases=dns=%lums,conn=%lums,wait=%lums",
                 host_, port_, diag_resolved_ip_,
                 diag_local_ip_, (unsigned)diag_local_port_, uri,
                 (unsigned long)diag_sent_bytes_, total, head_hex,
                 (unsigned long)diag_dns_ms_,
                 (unsigned long)diag_connect_ms_,
                 (unsigned long)wait_ms);
        g_errlog.record(ErrCat::OtaFetch,
                        "rr hdr timeout %s ip=%s sent=%lu recv=%d head=%s "
                        "dns=%lums conn=%lums wait=%lums",
                        host_, diag_resolved_ip_,
                        (unsigned long)diag_sent_bytes_, total, head_hex,
                        (unsigned long)diag_dns_ms_,
                        (unsigned long)diag_connect_ms_,
                        (unsigned long)wait_ms);
        close(); return -1;
    }

    int status = -1;
    const char* sp = strchr(buf, ' ');
    if (!sp) {
        LOG_WARN("read_response_: malformed status line");
        g_errlog.record(ErrCat::OtaFetch, "rr bad status line");
        close(); return -1;
    }
    status = atoi(sp + 1);

    int content_length = 0;
    const char* cl = strcasestr(buf, "content-length:");
    if (cl) {
        cl += 15;
        while (*cl == ' ') cl++;
        content_length = atoi(cl);
    }

    // Pull the two signing headers. Cap lengths: timestamp is a uint32 (max
    // 10 digits), signature is fixed 64 hex chars.
    char resp_ts [16] = {0};
    char resp_sig[72] = {0};
    auto copy_header = [&](const char* name, char* dst, size_t dst_cap) {
        const char* h = strcasestr(buf, name);
        if (!h) return;
        h += strlen(name);
        while (*h == ' ') h++;
        size_t i = 0;
        while (*h && *h != '\r' && *h != '\n' && i + 1 < dst_cap) {
            dst[i++] = *h++;
        }
        dst[i] = '\0';
    };
    copy_header("x-response-timestamp:", resp_ts,  sizeof(resp_ts));
    copy_header("x-response-signature:", resp_sig, sizeof(resp_sig));

    // Detect server-side close. Stra2us (uvicorn / ASGI) responds with
    // `Connection: close` after every request — at that point the server
    // FINs the socket and our cached connection is dead-on-next-write.
    // arduino-esp32's WiFiClient::connected() doesn't notice until the FIN
    // has fully propagated locally, so the next ensure_connected_() will
    // happily reuse a half-closed socket: tcp_.write() succeeds into the
    // local buffer, the server never sees the request, and read_response_
    // times out with total=0 (observed on timmy 2026-04-28). Honor the
    // header by closing our end after we've finished reading the body —
    // ensure_connected_() then opens a fresh socket on the next call.
    char resp_conn[16] = {0};
    copy_header("connection:", resp_conn, sizeof(resp_conn));
    bool server_will_close = (strcasecmp(resp_conn, "close") == 0);

    const char* hdr_end = strstr(buf, "\r\n\r\n");
    int hdr_len    = (int)(hdr_end - buf) + 4;
    int body_have  = total - hdr_len;
    const char* body_start = buf + hdr_len;

    // Start the HMAC context now — we'll feed body bytes as they arrive so
    // multi-KB responses (future OTA IR blobs) don't need a buffer big enough
    // to hold the whole body. Only initialize if the server actually supplied
    // the signing headers AND this is a 2xx response (error bodies are
    // unsigned by design).
    const bool verify = (status >= 200 && status < 300) &&
                        resp_ts[0] != '\0' && resp_sig[0] != '\0';
    HMAC_SHA256_CTX hctx;
    if (verify) {
        uint8_t secret[32];
        hex_to_bytes_(secret_hex_, secret, 32);
        hmac_sha256_init(&hctx, secret, 32);
        hmac_sha256_update(&hctx, (const uint8_t*)uri, strlen(uri));
    }

    int body_filled = 0;
    if (body_out && body_out_len > 0 && body_have > 0) {
        int copy = (body_have < (int)body_out_len - 1) ? body_have : (int)body_out_len - 1;
        memcpy(body_out, body_start, copy);
        body_filled = copy;
    }
    if (verify && body_have > 0) {
        hmac_sha256_update(&hctx, (const uint8_t*)body_start, body_have);
    }

    int remaining = content_length - body_have;
    while (remaining > 0 && (millis() - start < 5000)) {
        if (tcp_.available()) {
            if (body_out && body_out_len > 0 && body_filled < (int)body_out_len - 1) {
                int space = (int)body_out_len - 1 - body_filled;
                int to_read = remaining < space ? remaining : space;
                int n = tcp_.read((uint8_t*)(body_out + body_filled), to_read);
                if (n <= 0) {
                    LOG_WARN("read_response_: body read EOF status=%d cl=%d filled=%d rem=%d",
                             status, content_length, body_filled, remaining);
                    g_errlog.record(ErrCat::OtaFetch,
                                    "rr body EOF cl=%d filled=%d rem=%d",
                                    content_length, body_filled, remaining);
                    close(); return -1;
                }
                if (verify) hmac_sha256_update(&hctx,
                                               (const uint8_t*)(body_out + body_filled),
                                               (size_t)n);
                body_filled += n;
                remaining   -= n;
            } else {
                char trash[64];
                int to_read = remaining < (int)sizeof(trash) ? remaining : (int)sizeof(trash);
                int n = tcp_.read((uint8_t*)trash, to_read);
                if (n <= 0) { close(); return -1; }
                if (verify) hmac_sha256_update(&hctx, (const uint8_t*)trash, (size_t)n);
                remaining -= n;
            }
        } else {
            yield();  // see note above — Particle.process() equivalent.
        }
        delay(1);
    }

    // Drain residual chunked-encoding fragments so keep-alive stays sane.
    int empty_loops = 0;
    while (empty_loops < 5) {
        if (tcp_.available()) { tcp_.read(); empty_loops = 0; }
        else { delay(10); empty_loops++; }
    }

    if (body_out && body_out_len > 0) body_out[body_filled] = '\0';

    // Verification pass. For 2xx responses the server MUST have signed — if
    // the headers were absent we fail closed. For non-2xx we return the
    // status as-is so callers can see 4xx/5xx normally.
    if (status >= 200 && status < 300) {
        if (!verify) {
            LOG_WARN("read_response_: 2xx but unsigned ts=%d sig=%d",
                     (int)(resp_ts[0] != '\0'), (int)(resp_sig[0] != '\0'));
            g_errlog.record(ErrCat::OtaFetch, "rr 2xx unsigned");
            close(); return -1;
        }

        // Drift window mirrors the server's request check (±300s).
        long ts_val = atol(resp_ts);
        long now    = (long)::time(nullptr);
        if (now > 0 && (now - ts_val > 300 || ts_val - now > 300)) {
            LOG_WARN("read_response_: ts drift now=%ld resp=%ld", now, ts_val);
            g_errlog.record(ErrCat::OtaFetch,
                            "rr ts drift now=%ld resp=%ld", now, ts_val);
            close(); return -1;
        }

        hmac_sha256_update(&hctx, (const uint8_t*)resp_ts, strlen(resp_ts));
        uint8_t mac[32];
        hmac_sha256_final(&hctx, mac);

        char mac_hex[65];
        for (int i = 0; i < 32; ++i)
            snprintf(&mac_hex[i*2], 3, "%02x", mac[i]);
        mac_hex[64] = '\0';

        if (!hex_equal_(mac_hex, resp_sig)) {
            LOG_WARN("read_response_: HMAC mismatch cl=%d filled=%d ours=%.8s theirs=%.8s",
                     content_length, body_filled, mac_hex, resp_sig);
            g_errlog.record(ErrCat::OtaFetch,
                            "rr HMAC fail cl=%d filled=%d",
                            content_length, body_filled);
            close(); return -1;
        }

        // Surface the verified timestamp for callers that need it as a
        // nonce — currently the encrypted-value decrypt path in
        // `kv_fetch_str_` (see `fr_encrypted_values.md`). Only set on
        // verified 2xx responses so a caller using this can rely on
        // having a trustworthy nonce.
        if (out_ts) *out_ts = (uint32_t)ts_val;
    }

    // Server told us it's closing — drop our end too so the next request
    // doesn't try to reuse a half-closed socket. See Connection-close note
    // above the copy_header() call.
    if (server_will_close) close();

    return status;
}

int Stra2usClient::publish(const char* topic, const char* message) {
    LatencyScope _t(*this);
    char uri[64];
    snprintf(uri, sizeof(uri), "/q/%s", topic);

    uint32_t ts = (uint32_t)::time(nullptr);
    size_t body_len = strlen(message);
    char sig[65];
    sign_(uri, message, body_len, ts, sig);

    // Static, not stack: publish() is serial on the tel task, so static
    // is safe and avoids tel-thread stack pressure (cf.
    // debug_ota_hardfault_stack.md). Size depends on whether snapshots
    // are enabled — heartbeats are ~700 B, snapshot dumps
    // (FAILURE_TRIAGE.md §1) are ~3 KB. ESP32 has plenty of RAM but
    // mirror the Particle conditional anyway for code symmetry.
#ifdef NO_SNAPSHOT_BUFFER
    static char req[1024];
#else
    static char req[5120];
#endif
    int req_len = snprintf(req, sizeof(req),
        "POST %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: %u\r\n"
        "X-Client-ID: %s\r\n"
        "X-Timestamp: %lu\r\n"
        "X-Signature: %s\r\n"
        "Connection: keep-alive\r\n"
        "\r\n"
        "%s",
        uri, host_, port_, (unsigned int)body_len,
        client_id_, (unsigned long)ts, sig, message);
    if (req_len >= (int)sizeof(req)) return -1;

    for (int attempt = 0; attempt < 2; ++attempt) {
        if (!ensure_connected_()) { delay(100); continue; }
        if (send_all_(req, req_len)) break;
        if (attempt == 1) return -1;
    }
    return read_response_(uri, nullptr, 0);
}

bool Stra2usClient::kv_fetch_(const char* full_key,
                              bool& is_float, int& out_i, float& out_f) {
    LatencyScope _t(*this);
    char uri[160];
    snprintf(uri, sizeof(uri), "/kv/%s", full_key);

    uint32_t ts = (uint32_t)::time(nullptr);
    char sig[65];
    sign_(uri, nullptr, 0, ts, sig);

    char req[512];
    int req_len = snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "X-Client-ID: %s\r\n"
        "X-Timestamp: %lu\r\n"
        "X-Signature: %s\r\n"
        "Connection: keep-alive\r\n"
        "\r\n",
        uri, host_, port_, client_id_, (unsigned long)ts, sig);
    if (req_len >= (int)sizeof(req)) return false;

    for (int attempt = 0; attempt < 2; ++attempt) {
        if (!ensure_connected_()) { delay(100); continue; }
        if (send_all_(req, req_len)) break;
        if (attempt == 1) return false;
    }

    char body[128] = {};
    int status = read_response_(uri, body, sizeof(body));
    if (status != 200) return false;

    // msgpack: we only care about int/float shapes. Strings aren't used for
    // config knobs. Mirrors the coaticlock decoder but skips the string tail.
    uint8_t* b = (uint8_t*)body;
    if (b[0] <= 0x7f)              { out_i = b[0];                                    is_float = false; return true; }
    if ((b[0] & 0xe0) == 0xe0)     { out_i = (int8_t)b[0];                            is_float = false; return true; }
    if (b[0] == 0xcc)              { out_i = b[1];                                    is_float = false; return true; }
    if (b[0] == 0xcd)              { out_i = ((uint16_t)b[1] << 8) | b[2];            is_float = false; return true; }
    if (b[0] == 0xce) {
        out_i = (int)(((uint32_t)b[1] << 24) | ((uint32_t)b[2] << 16) |
                      ((uint32_t)b[3] << 8)  |  b[4]);
        is_float = false; return true;
    }
    if (b[0] == 0xd0)              { out_i = (int8_t)b[1];                            is_float = false; return true; }
    if (b[0] == 0xd1)              { out_i = (int16_t)(((uint16_t)b[1] << 8) | b[2]); is_float = false; return true; }
    if (b[0] == 0xd2) {
        out_i = (int32_t)(((uint32_t)b[1] << 24) | ((uint32_t)b[2] << 16) |
                          ((uint32_t)b[3] << 8)  |  b[4]);
        is_float = false; return true;
    }
    if (b[0] == 0xca) {
        uint32_t v = ((uint32_t)b[1] << 24) | ((uint32_t)b[2] << 16) |
                     ((uint32_t)b[3] << 8)  |  b[4];
        float f; memcpy(&f, &v, 4);
        out_f = f; is_float = true; return true;
    }
    if (b[0] == 0xcb) {
        uint64_t v = ((uint64_t)b[1] << 56) | ((uint64_t)b[2] << 48) |
                     ((uint64_t)b[3] << 40) | ((uint64_t)b[4] << 32) |
                     ((uint64_t)b[5] << 24) | ((uint64_t)b[6] << 16) |
                     ((uint64_t)b[7] << 8)  |  b[8];
        double d; memcpy(&d, &v, 8);
        out_f = (float)d; is_float = true; return true;
    }
    return false;
}

void Stra2usClient::poll_key(size_t idx) {
    if (idx >= cache_count_) return;
    Entry& e = cache_[idx];

    // Try <app>/<device>/<key>, then fall back to <app>/<key>.
    char full[KEY_MAX + 64];
    bool fetched = false;
    bool got_float = false;
    int   got_i = 0;
    float got_f = 0.0f;

    snprintf(full, sizeof(full), "%s/%s/%s", app_, device_, e.key);
    if (kv_fetch_(full, got_float, got_i, got_f)) {
        fetched = true;
    } else {
        snprintf(full, sizeof(full), "%s/public/%s", app_, e.key);
        if (kv_fetch_(full, got_float, got_i, got_f)) fetched = true;
    }
    if (!fetched) return;

    // Coerce to the registered type. Float key with int payload = promote;
    // int key with float payload = truncate.
    if (e.is_float) {
        e.live_f = got_float ? got_f : (float)got_i;
    } else {
        e.live_i = got_float ? (int)got_f : got_i;
    }
    e.has_live = true;   // flip last — readers see a committed value
}

void Stra2usClient::poll_all() {
    size_t n = cache_count_;
    for (size_t i = 0; i < n; ++i) poll_key(i);

    // Brightness schedule (string-valued KV). Mirror of the Particle path —
    // see hal/particle/src/Stra2usClient.cpp::poll_all for the full
    // rationale. Try device-scope first, app-scope fallback; only
    // overwrite the cached buffer on success so a transient miss doesn't
    // drop a known-good schedule.
    char full[KEY_MAX + 64];
    char buf[sizeof(brightness_schedule_)];
    size_t out_len = 0;

    snprintf(full, sizeof(full), "%s/%s/brightness_schedule", app_, device_);
    bool fetched = kv_fetch_str_(full, buf, sizeof(buf), out_len);
    if (!fetched) {
        snprintf(full, sizeof(full), "%s/public/brightness_schedule", app_);
        fetched = kv_fetch_str_(full, buf, sizeof(buf), out_len);
    }
    if (fetched) {
        size_t copy_len = out_len < sizeof(brightness_schedule_) - 1
                        ? out_len
                        : sizeof(brightness_schedule_) - 1;
        memcpy(brightness_schedule_, buf, copy_len);
        brightness_schedule_[copy_len] = '\0';
    }

    // Procyon-rescue WiFi credentials (two string-valued KVs). Mirror
    // of hal/particle/src/Stra2usClient.cpp. Same device-then-app
    // fallback shape; only-overwrite-on-success so a transient KV miss
    // doesn't drop a known-good value (the wifi-cred apply path on
    // the .ino reads these and the hash dedup would treat a temporary
    // empty as "operator unset the value" — bad).
    {
        char ssid_buf[sizeof(wifi_ssid_)];
        size_t ssid_len = 0;
        snprintf(full, sizeof(full), "%s/%s/wifi_ssid", app_, device_);
        bool ssid_ok = kv_fetch_str_(full, ssid_buf, sizeof(ssid_buf), ssid_len);
        if (!ssid_ok) {
            snprintf(full, sizeof(full), "%s/public/wifi_ssid", app_);
            ssid_ok = kv_fetch_str_(full, ssid_buf, sizeof(ssid_buf), ssid_len);
        }
        if (ssid_ok) {
            size_t copy_len = ssid_len < sizeof(wifi_ssid_) - 1
                            ? ssid_len
                            : sizeof(wifi_ssid_) - 1;
            memcpy(wifi_ssid_, ssid_buf, copy_len);
            wifi_ssid_[copy_len] = '\0';
        }

        char pw_buf[sizeof(wifi_password_)];
        size_t pw_len = 0;
        snprintf(full, sizeof(full), "%s/%s/wifi_password", app_, device_);
        bool pw_ok = kv_fetch_str_(full, pw_buf, sizeof(pw_buf), pw_len);
        if (!pw_ok) {
            snprintf(full, sizeof(full), "%s/public/wifi_password", app_);
            pw_ok = kv_fetch_str_(full, pw_buf, sizeof(pw_buf), pw_len);
        }
        if (pw_ok) {
            size_t copy_len = pw_len < sizeof(wifi_password_) - 1
                            ? pw_len
                            : sizeof(wifi_password_) - 1;
            memcpy(wifi_password_, pw_buf, copy_len);
            wifi_password_[copy_len] = '\0';
        }
    }
}

// ---------- OTA IR ----------

bool Stra2usClient::kv_fetch_str_(const char* full_key,
                                  char* buf, size_t buf_cap, size_t& out_len) {
    LatencyScope _t(*this);
    out_len = 0;
    if (!buf || buf_cap < 2) return false;

    char uri[160];
    snprintf(uri, sizeof(uri), "/kv/%s", full_key);

    uint32_t ts = (uint32_t)::time(nullptr);
    char sig[65];
    sign_(uri, nullptr, 0, ts, sig);

    char req[512];
    int req_len = snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "X-Client-ID: %s\r\n"
        "X-Timestamp: %lu\r\n"
        "X-Signature: %s\r\n"
        "Connection: keep-alive\r\n"
        "\r\n",
        uri, host_, port_, client_id_, (unsigned long)ts, sig);
    if (req_len >= (int)sizeof(req)) {
        g_errlog.record(ErrCat::OtaFetch, "kvs req too big: %d", req_len);
        return false;
    }

    bool sent = false;
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (!ensure_connected_()) { delay(100); continue; }
        if (send_all_(req, req_len)) { sent = true; break; }
        if (attempt == 1) {
            g_errlog.record(ErrCat::OtaFetch, "kvs send_all failed (2x)");
            return false;
        }
    }
    if (!sent) {
        g_errlog.record(ErrCat::OtaFetch, "kvs ensure_connected failed (2x)");
        return false;
    }

    // read_response_ fills buf with raw body bytes (and null-terminates at
    // the byte after the last filled byte). For msgpack str we then strip
    // the header in place.
    uint32_t resp_ts = 0;     // populated by read_response_ on 2xx; used
                              // as keystream nonce for ext-0x21 values.
    int status = read_response_(uri, buf, buf_cap, &resp_ts);
    if (status != 200) {
        g_errlog.record(ErrCat::OtaFetch, "kvs status=%d", status);
        return false;
    }

    uint8_t* b = (uint8_t*)buf;
    size_t hdr_len = 0;
    size_t payload_len = 0;
    bool   is_encrypted = false;
    uint8_t ext_type = 0;

    // Accept both str and bin msgpack types — wire layout is identical
    // (length prefix + raw bytes), only the type byte differs. Stra2us
    // serializes large blob values as bin16 (0xc5) on the current server
    // build (observed 2026-04-28 / timmy/fraggle); smaller values come
    // back as str. The IR blob is ASCII text either way, so accepting
    // both keeps the device tolerant of either serializer choice.
    //
    // Ext family (0xd4-0xd8 fixext, 0xc7-0xc9 ext8/16/32) is reserved
    // for encrypted values per `stra2us/docs/fr_encrypted_values.md`.
    // Type byte 0x21 is the cipher marker; any other ext type is a
    // protocol error.
    if ((b[0] & 0xe0) == 0xa0) {                 // fixstr
        payload_len = b[0] & 0x1f;
        hdr_len = 1;
    } else if (b[0] == 0xd9 || b[0] == 0xc4) {   // str8 / bin8
        payload_len = b[1];
        hdr_len = 2;
    } else if (b[0] == 0xda || b[0] == 0xc5) {   // str16 / bin16
        payload_len = ((size_t)b[1] << 8) | b[2];
        hdr_len = 3;
    } else if (b[0] == 0xdb || b[0] == 0xc6) {   // str32 / bin32
        payload_len = ((size_t)b[1] << 24) | ((size_t)b[2] << 16) |
                      ((size_t)b[3] << 8)  |  b[4];
        hdr_len = 5;
    } else if (b[0] >= 0xd4 && b[0] <= 0xd8) {
        // fixext1/2/4/8/16: marker, type, then exactly 2^(n-0xd4)
        // bytes of payload. Layout = `<marker><type><data:fixed_len>`.
        static const size_t fixext_lens[] = {1, 2, 4, 8, 16};
        payload_len = fixext_lens[b[0] - 0xd4];
        ext_type    = b[1];
        hdr_len     = 2;
        is_encrypted = true;
    } else if (b[0] == 0xc7) {                   // ext8
        payload_len = b[1];
        ext_type    = b[2];
        hdr_len     = 3;
        is_encrypted = true;
    } else if (b[0] == 0xc8) {                   // ext16
        payload_len = ((size_t)b[1] << 8) | b[2];
        ext_type    = b[3];
        hdr_len     = 4;
        is_encrypted = true;
    } else if (b[0] == 0xc9) {                   // ext32 (oversize for KV)
        payload_len = ((size_t)b[1] << 24) | ((size_t)b[2] << 16) |
                      ((size_t)b[3] << 8)  |  b[4];
        ext_type    = b[5];
        hdr_len     = 6;
        is_encrypted = true;
    } else if (b[0] == 0xc0 || (b[0] & 0xf0) == 0x80) {
        // nil (0xc0) or fixmap error envelope (0x8X) = key not found.
        // Silent miss — caller decides whether absence is an error
        // (fw_target/brightness_schedule both treat unset as normal).
        return false;
    } else {
        g_errlog.record(ErrCat::OtaFetch, "kvs msgpack hdr=0x%02x", b[0]);
        return false;
    }

    if (is_encrypted && ext_type != 0x21) {
        // Unknown ext type — not the kvenc marker we know about. Fail
        // closed; future ext types added by Stra2us will need explicit
        // client support.
        g_errlog.record(ErrCat::OtaFetch, "kvs ext type=0x%02x", ext_type);
        return false;
    }

    if (hdr_len + payload_len > buf_cap - 1) {
        g_errlog.record(ErrCat::OtaFetch, "kvs payload=%u>cap=%u",
                        (unsigned)(hdr_len + payload_len), (unsigned)(buf_cap - 1));
        return false;
    }

    // Shift payload to the start of the buffer, null-terminate.
    if (hdr_len > 0) memmove(buf, buf + hdr_len, payload_len);
    if (is_encrypted) {
        // Decrypt in-place. resp_ts populated by read_response_ above.
        // Symmetric XOR with HMAC-keystream — see kvenc_xor_ for the
        // cipher details and the server-side reference impl.
        kvenc_xor_(resp_ts, (uint8_t*)buf, payload_len);
    }
    buf[payload_len] = '\0';
    out_len = payload_len;
    return true;
}

// Streaming KV fetch for large blobs (firmware OTA, ~1MB). Mirror of
// kv_fetch_str_'s request side, but the body is forwarded chunk-by-chunk
// to a caller-supplied callback rather than buffered in RAM. The msgpack
// bin header (1-5 bytes depending on length) is consumed internally and
// the callback only sees raw payload bytes — same semantic as
// kv_fetch_str_'s in-place header strip, just streamed.
//
// Body HMAC verification stays in this layer: every response byte still
// flows through hmac_sha256_update, and verification fires after the
// last byte is consumed. So even though the callback can't validate
// individual chunks, the *whole* fetch is HMAC-verified before this
// function returns true. A trustworthy callback (one that actually
// commits the bytes — e.g., Update.write) should still do its own
// integrity check (e.g., compare a streaming SHA256 against the
// sidecar's claimed sha) as belt-and-suspenders, since the HMAC
// verification only catches in-flight tampering, not whatever happens
// after the byte leaves this function.
//
// Timeout is 30s (vs 5s for kv_fetch_str_): 1MB at a slow WiFi line
// rate (~50KB/s on a marginal AP) can take 20s to stream; 30s gives
// margin without papering over genuine wedges.
bool Stra2usClient::kv_fetch_stream_(const char* full_key,
                                     ChunkCallback cb, void* userdata,
                                     size_t* out_payload_size) {
    if (out_payload_size) *out_payload_size = 0;
    if (!cb) return false;

    char uri[160];
    snprintf(uri, sizeof(uri), "/kv/%s", full_key);

    uint32_t ts = (uint32_t)::time(nullptr);
    char sig[65];
    sign_(uri, nullptr, 0, ts, sig);

    char req[512];
    int req_len = snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "X-Client-ID: %s\r\n"
        "X-Timestamp: %lu\r\n"
        "X-Signature: %s\r\n"
        "Connection: keep-alive\r\n"
        "\r\n",
        uri, host_, port_, client_id_, (unsigned long)ts, sig);
    if (req_len >= (int)sizeof(req)) {
        g_errlog.record(ErrCat::OtaFetch, "kvstream req too big: %d", req_len);
        return false;
    }

    bool sent = false;
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (!ensure_connected_()) { delay(100); continue; }
        if (send_all_(req, req_len)) { sent = true; break; }
        if (attempt == 1) {
            g_errlog.record(ErrCat::OtaFetch, "kvstream send_all failed (2x)");
            return false;
        }
    }
    if (!sent) {
        g_errlog.record(ErrCat::OtaFetch, "kvstream ensure_connected failed (2x)");
        return false;
    }

    // ---------- read response headers (same shape as read_response_) ----------
    unsigned long start = millis();
    char hdr_buf[1024];
    int total = 0;
    while (millis() - start < 30000) {
        if (tcp_.available()) {
            int n = tcp_.read((uint8_t*)(hdr_buf + total),
                              sizeof(hdr_buf) - 1 - total);
            if (n <= 0) break;
            total += n;
            hdr_buf[total] = '\0';
            if (strstr(hdr_buf, "\r\n\r\n")) break;
        } else {
            yield();
        }
        delay(10);
    }
    if (!strstr(hdr_buf, "\r\n\r\n")) {
        g_errlog.record(ErrCat::OtaFetch, "kvstream hdr timeout total=%d", total);
        close(); return false;
    }

    int status = -1;
    const char* sp = strchr(hdr_buf, ' ');
    if (!sp) {
        g_errlog.record(ErrCat::OtaFetch, "kvstream bad status line");
        close(); return false;
    }
    status = atoi(sp + 1);
    if (status != 200) {
        g_errlog.record(ErrCat::OtaFetch, "kvstream status=%d", status);
        close(); return false;
    }

    int content_length = 0;
    const char* cl = strcasestr(hdr_buf, "content-length:");
    if (cl) {
        cl += 15;
        while (*cl == ' ') cl++;
        content_length = atoi(cl);
    }
    if (content_length <= 0) {
        g_errlog.record(ErrCat::OtaFetch, "kvstream missing content-length");
        close(); return false;
    }

    char resp_ts [16] = {0};
    char resp_sig[72] = {0};
    char resp_conn[16] = {0};
    auto copy_header = [&](const char* name, char* dst, size_t dst_cap) {
        const char* h = strcasestr(hdr_buf, name);
        if (!h) return;
        h += strlen(name);
        while (*h == ' ') h++;
        size_t i = 0;
        while (*h && *h != '\r' && *h != '\n' && i + 1 < dst_cap) {
            dst[i++] = *h++;
        }
        dst[i] = '\0';
    };
    copy_header("x-response-timestamp:", resp_ts,  sizeof(resp_ts));
    copy_header("x-response-signature:", resp_sig, sizeof(resp_sig));
    copy_header("connection:",           resp_conn, sizeof(resp_conn));
    bool server_will_close = (strcasecmp(resp_conn, "close") == 0);

    const bool verify = resp_ts[0] != '\0' && resp_sig[0] != '\0';
    if (!verify) {
        g_errlog.record(ErrCat::OtaFetch, "kvstream 2xx unsigned");
        close(); return false;
    }

    HMAC_SHA256_CTX hctx;
    {
        uint8_t secret[32];
        hex_to_bytes_(secret_hex_, secret, 32);
        hmac_sha256_init(&hctx, secret, 32);
        hmac_sha256_update(&hctx, (const uint8_t*)uri, strlen(uri));
    }

    const char* hdr_end = strstr(hdr_buf, "\r\n\r\n");
    int hdr_size      = (int)(hdr_end - hdr_buf) + 4;
    int body_have_buf = total - hdr_size;
    const uint8_t* body_start = (const uint8_t*)hdr_buf + hdr_size;

    // ---------- msgpack bin header state machine ----------
    // First byte is the type marker; subsequent 1, 2, or 4 bytes are
    // length prefix. We accept bin8/bin16/bin32 for parity with
    // kv_fetch_str_'s widening (str variants too — same wire layout,
    // different type byte). For 1MB firmware, expected type is bin32
    // (0xc6) since payload > 64KB.
    uint8_t  mp_hdr[5] = {0};
    int      mp_hdr_filled = 0;
    int      mp_hdr_total  = 0;     // 0 = type byte not yet seen; 2/3/5 once known
    size_t   payload_size = 0;
    bool     payload_known = false;
    size_t   payload_consumed = 0;
    bool     callback_aborted = false;

    auto consume = [&](const uint8_t* data, size_t n) -> bool {
        // Phase 1: feed bytes into the msgpack header until we know the size.
        while (!payload_known && n > 0) {
            if (mp_hdr_filled == 0) {
                mp_hdr[0] = data[0];
                uint8_t b0 = mp_hdr[0];
                if (b0 == 0xc4 || b0 == 0xd9)        mp_hdr_total = 2;
                else if (b0 == 0xc5 || b0 == 0xda)   mp_hdr_total = 3;
                else if (b0 == 0xc6 || b0 == 0xdb)   mp_hdr_total = 5;
                else {
                    g_errlog.record(ErrCat::OtaFetch,
                                    "kvstream msgpack hdr=0x%02x", b0);
                    return false;
                }
                mp_hdr_filled = 1;
                data++; n--;
                continue;
            }
            int want = mp_hdr_total - mp_hdr_filled;
            int take = (int)n < want ? (int)n : want;
            memcpy(mp_hdr + mp_hdr_filled, data, (size_t)take);
            mp_hdr_filled += take;
            data += take;
            n    -= (size_t)take;
            if (mp_hdr_filled == mp_hdr_total) {
                if (mp_hdr_total == 2) {
                    payload_size = mp_hdr[1];
                } else if (mp_hdr_total == 3) {
                    payload_size = ((size_t)mp_hdr[1] << 8) | mp_hdr[2];
                } else {  // 5
                    payload_size = ((size_t)mp_hdr[1] << 24)
                                 | ((size_t)mp_hdr[2] << 16)
                                 | ((size_t)mp_hdr[3] <<  8)
                                 |  mp_hdr[4];
                }
                payload_known = true;
                if (out_payload_size) *out_payload_size = payload_size;
            }
        }
        // Phase 2: forward payload bytes to the callback.
        if (payload_known && n > 0) {
            size_t remaining_payload = (payload_consumed < payload_size)
                                     ? (payload_size - payload_consumed) : 0;
            size_t forward = n < remaining_payload ? n : remaining_payload;
            if (forward > 0) {
                if (!cb(userdata, data, forward)) {
                    callback_aborted = true;
                    return false;
                }
                payload_consumed += forward;
            }
        }
        return true;
    };

    // Fold the header-buffer's body tail into HMAC + consume pipeline.
    if (body_have_buf > 0) {
        hmac_sha256_update(&hctx, body_start, (size_t)body_have_buf);
        if (!consume(body_start, (size_t)body_have_buf)) {
            close();
            if (callback_aborted) {
                g_errlog.record(ErrCat::OtaFetch, "kvstream cb aborted early");
            }
            return false;
        }
    }

    // ---------- stream remaining body ----------
    int remaining = content_length - body_have_buf;
    while (remaining > 0 && (millis() - start < 30000)) {
        if (tcp_.available()) {
            uint8_t chunk[1024];
            int to_read = remaining < (int)sizeof(chunk) ? remaining : (int)sizeof(chunk);
            int n = tcp_.read(chunk, to_read);
            if (n <= 0) {
                g_errlog.record(ErrCat::OtaFetch,
                                "kvstream body EOF cl=%d consumed=%u rem=%d",
                                content_length, (unsigned)payload_consumed, remaining);
                close(); return false;
            }
            hmac_sha256_update(&hctx, chunk, (size_t)n);
            if (!consume(chunk, (size_t)n)) {
                close();
                if (callback_aborted) {
                    g_errlog.record(ErrCat::OtaFetch,
                                    "kvstream cb aborted at consumed=%u",
                                    (unsigned)payload_consumed);
                }
                return false;
            }
            remaining -= n;
        } else {
            yield();
        }
        delay(1);
    }
    if (remaining > 0) {
        g_errlog.record(ErrCat::OtaFetch,
                        "kvstream timeout cl=%d rem=%d consumed=%u",
                        content_length, remaining, (unsigned)payload_consumed);
        close(); return false;
    }
    if (!payload_known || payload_consumed != payload_size) {
        g_errlog.record(ErrCat::OtaFetch,
                        "kvstream short payload size=%u consumed=%u",
                        (unsigned)payload_size, (unsigned)payload_consumed);
        close(); return false;
    }

    // Drain residual chunked-encoding fragments (mirror read_response_).
    int empty_loops = 0;
    while (empty_loops < 5) {
        if (tcp_.available()) { tcp_.read(); empty_loops = 0; }
        else { delay(10); empty_loops++; }
    }

    // ---------- HMAC verify ----------
    long ts_val = atol(resp_ts);
    long now    = (long)::time(nullptr);
    if (now > 0 && (now - ts_val > 300 || ts_val - now > 300)) {
        g_errlog.record(ErrCat::OtaFetch,
                        "kvstream ts drift now=%ld resp=%ld", now, ts_val);
        close(); return false;
    }
    hmac_sha256_update(&hctx, (const uint8_t*)resp_ts, strlen(resp_ts));
    uint8_t mac[32];
    hmac_sha256_final(&hctx, mac);
    char mac_hex[65];
    for (int i = 0; i < 32; ++i)
        snprintf(&mac_hex[i*2], 3, "%02x", mac[i]);
    mac_hex[64] = '\0';
    if (!hex_equal_(mac_hex, resp_sig)) {
        g_errlog.record(ErrCat::OtaFetch,
                        "kvstream HMAC fail consumed=%u",
                        (unsigned)payload_consumed);
        close(); return false;
    }

    if (server_will_close) close();
    return true;
}

static inline bool is_hex_(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

// Compute SHA256 over the blob with wall-clock drift normalized out:
//   * the `encoded_at <ts>` line value is replaced with `encoded_at <stripped>`
//   * the trailing `END <fletcher>` line is replaced with `END <stripped>`
//     (and trailing whitespace consumed, matching Python's `\s*$` greedy).
//
// This MUST mirror _normalize_for_hash + _content_sha in tools/publish_ir.py
// — the device recomputes this on the fetched blob and compares against the
// sidecar. If the two sides normalize differently, the device either reloads
// every poll or skips a real update.
//
// Writes 64 lowercase hex chars + '\0' to out_hex. Returns false if the blob
// is malformed (missing encoded_at or END line).
static bool compute_content_sha_(const char* buf, size_t len, char* out_hex) {
    const char* bend = buf + len;

    // Find the encoded_at line. Anchor on "\nencoded_at " (always preceded
    // by "CRIT <ver>\n" so never at position 0).
    const char* ea_start = nullptr;
    {
        const char* needle = "\nencoded_at ";
        const size_t nlen = 12;
        size_t scan = len < 1024 ? len : 1024;
        for (size_t i = 0; i + nlen <= scan; ++i) {
            if (memcmp(buf + i, needle, nlen) == 0) { ea_start = buf + i + 1; break; }
        }
    }
    if (!ea_start) return false;
    const char* ea_nl = ea_start;
    while (ea_nl < bend && *ea_nl != '\n') ++ea_nl;  // exclusive: points at '\n'

    // Find the END line. Scan from buf start to match Python's first-match
    // semantics; in practice END is the final line so it's at the tail.
    const char* end_start = nullptr;
    {
        const char* needle = "\nEND ";
        const size_t nlen = 5;
        for (size_t i = 0; i + nlen <= len; ++i) {
            if (memcmp(buf + i, needle, nlen) == 0) { end_start = buf + i + 1; break; }
        }
    }
    if (!end_start) return false;

    // Consume "END " + hex + trailing whitespace, matching Python's
    // `^END [0-9a-fA-F]+\s*$` under MULTILINE + count=1. Subtle: `$` in
    // MULTILINE matches *before* a `\n` or at EOS, so greedy `\s*` has to
    // leave `$` a landing spot. For blobs that end at END (body-only), all
    // trailing whitespace gets consumed up to EOS. For blobs followed by a
    // SOURCE trailer, the engine backtracks to keep the `\n` between END
    // and SOURCE — `\s*` matches zero chars and `$` matches before that
    // newline. We emulate the backtrack: greedy-advance, then if we landed
    // on non-whitespace (a SOURCE trailer, not EOS), rewind one byte iff
    // the byte behind us is `\n`. Without this step the device's hash
    // diverges from publish_ir.py on every with-source blob.
    const char* p = end_start + 4;
    while (p < bend && is_hex_(*p)) ++p;
    while (p < bend && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p;
    if (p < bend && p > end_start && *(p - 1) == '\n') --p;
    const char* end_stop = p;

    SHA256_CTX ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, (const uint8_t*)buf, (size_t)(ea_start - buf));
    static const char LIT_EA[]  = "encoded_at <stripped>";
    sha256_update(&ctx, (const uint8_t*)LIT_EA, sizeof(LIT_EA) - 1);
    sha256_update(&ctx, (const uint8_t*)ea_nl, (size_t)(end_start - ea_nl));
    static const char LIT_END[] = "END <stripped>";
    sha256_update(&ctx, (const uint8_t*)LIT_END, sizeof(LIT_END) - 1);
    sha256_update(&ctx, (const uint8_t*)end_stop, (size_t)(bend - end_stop));

    uint8_t digest[32];
    sha256_final(&ctx, digest);
    // Renamed from HEX[] to HEX_DIGITS[] — Arduino.h's Print.h defines
    // `HEX` as the numeric literal 16 for print(n, HEX), and that macro
    // replacement fires on any identifier literally named HEX, including
    // this local array. Same collision will bite any identifier named
    // DEC/OCT/BIN too — rename rather than #undef so a future reader
    // grepping Print.h still sees the macro where they expect it.
    static const char HEX_DIGITS[] = "0123456789abcdef";
    for (int i = 0; i < 32; ++i) {
        out_hex[2*i]   = HEX_DIGITS[(digest[i] >> 4) & 0xf];
        out_hex[2*i+1] = HEX_DIGITS[digest[i] & 0xf];
    }
    out_hex[64] = '\0';
    return true;
}

void Stra2usClient::ir_poll() {
    // Skip if the main thread hasn't consumed the previous fetch yet. Next
    // poll cycle will retry.
    if (ir_pending_len_ != 0) return;

    // Pointer key is device-specific with no app-level fallback: every
    // device should explicitly declare which script it runs. If the key is
    // unset or empty, we keep whatever's currently loaded.
    char ptr_key[96];
    snprintf(ptr_key, sizeof(ptr_key), "%s/%s/ir", app_, device_);
    char new_ptr[IR_SCRIPT_NAME_MAX];
    size_t n = 0;
    if (!kv_fetch_str_(ptr_key, new_ptr, sizeof(new_ptr), n)) return;
    if (n == 0) return;

    // Two-step fetch. The sidecar `<app>/scripts/<name>/sha` holds the
    // 64-char hex *content_sha* of the blob (SHA256 over the blob with
    // encoded_at + trailing END lines normalized out — see
    // compute_content_sha_ above and _content_sha in publish_ir.py).
    // Fetching it is ~100 bytes instead of multi-KB. If the sidecar's sha
    // matches what we've got loaded, we're done for this cycle and skip
    // the big blob fetch entirely.
    //
    // content_sha shifts on source edits AND on encoder behavior changes,
    // but NOT on pure republishes of the same source with the same encoder
    // (those only perturb encoded_at + Fletcher). So devices reload iff
    // there's real content to pull — no missed updates, no needless reloads.
    //
    // publish_ir.py writes blob-then-sidecar, so a torn upload leaves the
    // sidecar pointing at the *old* sha; devices see no change and wait
    // for the next publish. The verify step below catches the reversed
    // case (sidecar says new, blob is still old) by recomputing content_sha
    // on the blob we fetched and checking it against the sidecar.
    char sha_key[96 + IR_SCRIPT_NAME_MAX + 8];
    snprintf(sha_key, sizeof(sha_key), "%s/public/scripts/%s/sha", app_, new_ptr);
    // Sidecar format:
    //   <64-char hex content_sha>                         (legacy, pre-2026-04-22)
    //   <64-char hex content_sha>:<decimal size_bytes>    (current)
    // The size suffix lets us skip the blob fetch entirely when the blob
    // would overrun ir_ota_buf_ — closing the oversize-crash path (see
    // TODO.md "OTA crash — oversize blob kills the device hard"). 64 hex
    // + ':' + 10-digit size leaves ample headroom in a 96-byte scratch.
    char   sidecar_raw[96] = {0};
    size_t raw_len = 0;
    bool   have_sidecar = kv_fetch_str_(sha_key, sidecar_raw, sizeof(sidecar_raw), raw_len);

    char   sidecar_sha[65] = {0};
    size_t sidecar_size    = 0;
    bool   have_size       = false;
    if (have_sidecar) {
        if (raw_len == 64) {
            // Legacy format — sha only, size unknown.
            memcpy(sidecar_sha, sidecar_raw, 64);
        } else if (raw_len > 65 && sidecar_raw[64] == ':') {
            // Extended format — parse decimal size after the colon.
            memcpy(sidecar_sha, sidecar_raw, 64);
            size_t n = 0;
            bool   ok = true;
            for (size_t i = 65; i < raw_len; ++i) {
                char c = sidecar_raw[i];
                if (c < '0' || c > '9') { ok = false; break; }
                n = n * 10 + (size_t)(c - '0');
            }
            if (ok) {
                sidecar_size = n;
                have_size    = true;
            }
            // Size parse failure: still have a valid sha, just no size.
            // Proceed as if legacy — we'll learn the size the hard way
            // (fetch succeeds or buffer rejects).
        } else {
            // Malformed sidecar. Treat as missing.
            have_sidecar = false;
        }
    }

    // Fast path: sidecar sha matches what we have loaded — no blob fetch.
    // Compare against the accessor rather than ir_loaded_sha_ directly so
    // a freshly-flashed device whose compiled-in SCRIPT_SHA equals the
    // sidecar's sha also short-circuits; otherwise every cold boot would
    // re-fetch the blob we already have burned into flash. The accessor
    // returns SCRIPT_SHA when ir_loaded_sha_ is empty (pre-first-OTA).
    if (have_sidecar && strcmp(sidecar_sha, ir_loaded_sha()) == 0) {
        return;
    }

    // OTA candidate: sidecar differs from loaded (or is missing). Announce
    // the intent on serial so a reader can see we got this far. On a fresh
    // boot pre-first-OTA the private ir_loaded_* buffers are empty; fall
    // back to the compiled-in blob's identity (via the accessor) so the
    // log shows e.g. `loaded=fraggle@d0920aba` instead of `(none)@00000000`.
    LOG_INFO("ir_poll: OTA candidate %s sidecar=%.8s%s (loaded=%s@%.8s)",
             new_ptr,
             have_sidecar ? sidecar_sha : "none",
             have_size    ? " (sized)" : "",
             ir_loaded_script(),
             ir_loaded_sha()[0] ? ir_loaded_sha() : "00000000");

    // Pre-fetch size gate. If the sidecar tells us the blob won't fit,
    // don't attempt the fetch — it would either silently fail at the
    // msgpack unwrap (`hdr_len + payload_len > buf_cap - 1`) or, worse,
    // hit the oversize-crash path. -1 leaves room for the NUL the
    // msgpack unwrap appends after the payload.
    if (have_size && sidecar_size > sizeof(ir_ota_buf_) - 1) {
        g_errlog.record(ErrCat::OtaFetch,
                 "%s size=%u>buf=%u",
                 new_ptr,
                 (unsigned)sidecar_size,
                 (unsigned)(sizeof(ir_ota_buf_) - 1));
        return;
    }

    // Lifecycle publish #1: `ota_detected`. Snapshot identity + flip the
    // flag here; the tel worker main loop publishes on its *next* iteration,
    // outside this ir_poll frame. An earlier version published inline right
    // here and hard-froze the device — a POST wedged between the sidecar
    // GET and the blob GET on the same keep-alive socket wedges the tel
    // thread (see TODO.md completed entry 2026-04-22). The flag-and-snapshot
    // shape matches `ota_matrix` / `ota_loaded` so the three events reach
    // the stream the same way.
    //
    // Gated on have_sidecar: a missing sidecar means we don't know the
    // target sha yet, and firing ota_detected with a partial identity is
    // worse than not firing. Legacy no-sidecar path still fetches + applies
    // — it just skips the pre-announcement, as before.
    if (have_sidecar) {
        // Same fallback as the log line above: before the first successful
        // OTA apply the device is running the compiled-in blob, so report
        // its real identity as the from= side of ota_detected rather than
        // a placeholder "default".
        const char* from_name = ir_loaded_script();
        const char* from_sha  = ir_loaded_sha()[0] ? ir_loaded_sha() : "00000000";
        strncpy(ir_detected_from_name_, from_name, sizeof(ir_detected_from_name_) - 1);
        ir_detected_from_name_[sizeof(ir_detected_from_name_) - 1] = '\0';
        strncpy(ir_detected_from_sha_,  from_sha,  sizeof(ir_detected_from_sha_)  - 1);
        ir_detected_from_sha_[sizeof(ir_detected_from_sha_)   - 1] = '\0';
        strncpy(ir_detected_to_name_,   new_ptr,   sizeof(ir_detected_to_name_)   - 1);
        ir_detected_to_name_[sizeof(ir_detected_to_name_)     - 1] = '\0';
        strncpy(ir_detected_to_sha_,    sidecar_sha, sizeof(ir_detected_to_sha_)  - 1);
        ir_detected_to_sha_[sizeof(ir_detected_to_sha_)       - 1] = '\0';
        ir_detected_size_ = sidecar_size;
        // Flag flipped last so a concurrent reader that sees true is
        // guaranteed to see the snapshot already committed. Same
        // length-last / flag-last ordering used elsewhere in this file.
        ir_detected_flag_ = true;
    }

    char script_key[96 + IR_SCRIPT_NAME_MAX];
    snprintf(script_key, sizeof(script_key), "%s/public/scripts/%s", app_, new_ptr);
    LOG_INFO("ir_poll: fetching blob %s", script_key);
    size_t blob_len = 0;
    if (!kv_fetch_str_(script_key, ir_ota_buf_, sizeof(ir_ota_buf_), blob_len)) {
        g_errlog.record(ErrCat::OtaFetch, "fetch failed: %s", script_key);
        return;
    }
    // Blob is in ir_ota_buf_ and accounted for. Log length here so a
    // crash in the subsequent content_sha compute or strcmp path leaves
    // proof the fetch itself succeeded (server-side success logs only
    // tell us bytes *left* the server, not that the device finished
    // reading them).
    LOG_INFO("ir_poll: blob in (%u bytes); computing content_sha", (unsigned)blob_len);

    char new_sha[65];
    if (!compute_content_sha_(ir_ota_buf_, blob_len, new_sha)) {
        g_errlog.record(ErrCat::OtaFetch,
                 "malformed blob: %s (no encoded_at/END)", new_ptr);
        return;
    }
    // ASCII "..." not UTF-8 "…" — the serial monitor doesn't decode UTF-8
    // and prints three replacement glyphs (`���`) in place of the ellipsis.
    LOG_INFO("ir_poll: content_sha=%.8s...; cross-checking", new_sha);

    // Torn-upload guard: if we got a sidecar, content_sha(blob) must match
    // it. Reject the mismatch — a later publish will resolve it. Without a
    // sidecar (older publisher or transient fetch error), we trust the
    // blob's own content_sha as the identity.
    if (have_sidecar && strcmp(new_sha, sidecar_sha) != 0) {
        g_errlog.record(ErrCat::OtaFetch,
                 "sha mismatch %s sc=%.8s blob=%.8s",
                  new_ptr, sidecar_sha, new_sha);
        return;
    }

    if (strcmp(new_sha, ir_loaded_sha()) == 0) {
        // Content-identical to what's already loaded. This path only runs
        // when the sidecar was missing (otherwise the fast path above
        // already short-circuited). Accessor-based comparison (vs the raw
        // ir_loaded_sha_ field) also covers the pre-first-OTA case where
        // the fetched blob is what we already have compiled in.
        return;
    }
    LOG_INFO("ir_poll: staged %s (%u bytes, sha=%.8s...)",
             new_ptr, (unsigned)blob_len, new_sha);

    // Commit: name + sha first (readers of ir_pending_ptr_/sha only trust
    // them when len>0), then length last.
    strncpy(ir_pending_ptr_, new_ptr, sizeof(ir_pending_ptr_) - 1);
    ir_pending_ptr_[sizeof(ir_pending_ptr_) - 1] = '\0';
    memcpy(ir_pending_sha_, new_sha, 65);
    ir_pending_len_ = blob_len;
}

bool Stra2usClient::ir_apply_if_ready() {
    size_t len = ir_pending_len_;
    if (len == 0) return false;

    // critter_ir::load mutates the buffer in place (flips whitespace to NUL)
    // — the buffer is ours and we're about to zero the pending slot anyway,
    // so that's fine.
    // Preview the first line (CRIT N) and the byte count before parse —
    // catches truncation / msgpack-unwrap bugs that would otherwise look like
    // a parser problem.
    {
        char preview[24] = {0};
        size_t pv = len < sizeof(preview) - 1 ? len : sizeof(preview) - 1;
        for (size_t i = 0; i < pv; ++i) {
            char c = ir_ota_buf_[i];
            preview[i] = (c == '\n') ? '|' : (c >= 32 && c < 127 ? c : '?');
        }
        LOG_INFO("ir_apply: bytes=%u preview=\"%s\"", (unsigned)len, preview);
    }
    bool ok = critter_ir::load(ir_ota_buf_, len);
    if (ok) {
        strncpy(ir_loaded_ptr_, ir_pending_ptr_, sizeof(ir_loaded_ptr_) - 1);
        ir_loaded_ptr_[sizeof(ir_loaded_ptr_) - 1] = '\0';
        memcpy(ir_loaded_sha_, ir_pending_sha_, 65);
        LOG_INFO("ir_apply: loaded %s (sha=%.8s...) "
                 "colors=%u landmarks=%u agents=%u spawns=%u behaviors=%u insns=%u tick=%lums",
                 ir_loaded_ptr_, ir_loaded_sha_,
                 (unsigned)critter_ir::COLOR_COUNT,
                 (unsigned)critter_ir::LANDMARK_COUNT,
                 (unsigned)critter_ir::AGENT_TYPE_COUNT,
                 (unsigned)critter_ir::SPAWN_RULE_COUNT,
                 (unsigned)critter_ir::BEHAVIOR_COUNT,
                 (unsigned)(critter_ir::BEHAVIOR_COUNT > 0
                            ? critter_ir::BEHAVIORS[0].insn_count : 0),
                 (unsigned long)critter_ir::RUNTIME_TICK_MS);
    } else {
        g_errlog.record(ErrCat::OtaApply, "parse failed %s: %s",
                  ir_pending_ptr_, critter_ir::lastLoadError());
    }
    // Free the slot regardless: a bad blob shouldn't keep us retrying the
    // same parse on every tick. The server needs to push a new blob (or the
    // pointer has to change) before we'll try again.
    ir_pending_len_ = 0;
    return ok;
}

// ---------- OTA Firmware (Phase 2 of pull-OTA) ----------

void Stra2usClient::set_running_fw_sha(const char* sha_hex_64) {
    if (!sha_hex_64) { fw_running_sha_[0] = '\0'; return; }
    size_t i = 0;
    while (sha_hex_64[i] && i + 1 < sizeof(fw_running_sha_)) {
        fw_running_sha_[i] = sha_hex_64[i];
        ++i;
    }
    fw_running_sha_[i] = '\0';
}

// Streaming-fetch callback context for fw_poll. Carries:
//   - the SHA256 ctx that hashes the payload as it arrives (compared
//     against the sidecar's claimed sha after the stream completes)
//   - bytes-consumed counter (sanity check vs. sidecar's claimed size)
namespace {
struct FwApplyCtx {
    SHA256_CTX sha_ctx;
    size_t     consumed = 0;
};

bool fw_chunk_cb_(void* userdata, const uint8_t* chunk, size_t len) {
    auto* ctx = (FwApplyCtx*)userdata;
    sha256_update(&ctx->sha_ctx, chunk, len);
    // Update.write returns bytes-actually-written. Anything other than
    // `len` means flash error (out of partition space, hardware fault,
    // wrong U_FLASH command). Abort the stream by returning false; the
    // outer fw_poll will see this, call Update.abort(), and record an
    // ErrLog entry.
    size_t wrote = Update.write((uint8_t*)chunk, len);
    if (wrote != len) return false;
    ctx->consumed += len;
    return true;
}
}  // anonymous namespace

void Stra2usClient::fw_poll() {
    // 1) Read target pointer with the standard device-then-app fallback.
    char ptr_buf[48];
    size_t ptr_len = 0;
    char ptr_key[96];
    snprintf(ptr_key, sizeof(ptr_key), "%s/%s/fw_target", app_, device_);
    bool got_target = kv_fetch_str_(ptr_key, ptr_buf, sizeof(ptr_buf), ptr_len);
    if (!got_target || ptr_len == 0) {
        snprintf(ptr_key, sizeof(ptr_key), "%s/public/fw_target", app_);
        got_target = kv_fetch_str_(ptr_key, ptr_buf, sizeof(ptr_buf), ptr_len);
    }
    if (!got_target || ptr_len == 0) {
        // No target set at either scope. Not an error — just nothing to do.
        return;
    }
    // ptr_buf is now the target name (e.g. "esp32c3").

    // 2) Fetch the sidecar.
    char sha_key[96 + sizeof(ptr_buf) + 16];
    snprintf(sha_key, sizeof(sha_key), "%s/public/fw/%s/sha", app_, ptr_buf);
    char sidecar[96] = {0};
    size_t sidecar_len = 0;
    if (!kv_fetch_str_(sha_key, sidecar, sizeof(sidecar), sidecar_len)) {
        g_errlog.record(ErrCat::OtaFetch, "fw sidecar fetch failed");
        return;
    }
    // Format: "<64 hex>:<decimal size>"
    if (sidecar_len < 66 || sidecar[64] != ':') {
        g_errlog.record(ErrCat::OtaFetch, "fw sidecar malformed len=%u",
                        (unsigned)sidecar_len);
        return;
    }
    char expected_sha[65];
    memcpy(expected_sha, sidecar, 64);
    expected_sha[64] = '\0';
    size_t expected_size = 0;
    for (size_t i = 65; i < sidecar_len; ++i) {
        char c = sidecar[i];
        if (c < '0' || c > '9') {
            g_errlog.record(ErrCat::OtaFetch, "fw sidecar bad size");
            return;
        }
        expected_size = expected_size * 10 + (size_t)(c - '0');
    }
    if (expected_size == 0) {
        g_errlog.record(ErrCat::OtaFetch, "fw sidecar size=0");
        return;
    }

    // 3) Already running this firmware? Skip.
    if (fw_running_sha_[0] && strcmp(expected_sha, fw_running_sha_) == 0) {
        return;
    }

    LOG_INFO("fw_poll: candidate %s sidecar=%.8s size=%u (running=%.8s)",
             ptr_buf, expected_sha, (unsigned)expected_size,
             fw_running_sha_[0] ? fw_running_sha_ : "00000000");

    // 4) Reserve OTA partition + start streaming write.
    if (!Update.begin(expected_size, U_FLASH)) {
        g_errlog.record(ErrCat::OtaFetch, "fw Update.begin failed: %s",
                        Update.errorString());
        return;
    }

    char blob_key[96 + sizeof(ptr_buf)];
    snprintf(blob_key, sizeof(blob_key), "%s/public/fw/%s", app_, ptr_buf);
    LOG_INFO("fw_poll: fetching blob %s (%u bytes)",
             blob_key, (unsigned)expected_size);

    FwApplyCtx ctx;
    sha256_init(&ctx.sha_ctx);

    size_t streamed_payload_size = 0;
    if (!kv_fetch_stream_(blob_key, fw_chunk_cb_, &ctx, &streamed_payload_size)) {
        Update.abort();
        // kv_fetch_stream_ recorded the specific failure; the abort here
        // is just to release the OTA partition reservation. Note: if the
        // failure was inside fw_chunk_cb_ (Update.write returned short),
        // kv_fetch_stream_'s "kvstream cb aborted" record names that —
        // distinct from network-side failures.
        return;
    }

    // 5) Verify size matches sidecar's claim.
    if (streamed_payload_size != expected_size) {
        g_errlog.record(ErrCat::OtaFetch, "fw size mismatch sc=%u got=%u",
                        (unsigned)expected_size, (unsigned)streamed_payload_size);
        Update.abort();
        return;
    }
    if (ctx.consumed != expected_size) {
        // Should be impossible if streamed_payload_size matches and the
        // callback wrote every byte, but belt-and-suspenders.
        g_errlog.record(ErrCat::OtaFetch, "fw consumed mismatch sc=%u wrote=%u",
                        (unsigned)expected_size, (unsigned)ctx.consumed);
        Update.abort();
        return;
    }

    // 6) Verify SHA matches sidecar's claim. The sidecar itself was
    // HMAC-verified at fetch time (kv_fetch_str_'s response signing
    // path), so a matching computed-vs-sidecar sha proves the firmware
    // body was not tampered with in transit. The kv_fetch_stream_ call
    // also did response-body HMAC verification, so this is
    // belt-and-suspenders against weird MITM scenarios that affect
    // body bytes after our HMAC verification (unlikely, but cheap).
    uint8_t digest[32];
    sha256_final(&ctx.sha_ctx, digest);
    char computed_sha[65];
    static const char HEX_DIGITS[] = "0123456789abcdef";
    for (int i = 0; i < 32; ++i) {
        computed_sha[2*i]   = HEX_DIGITS[(digest[i] >> 4) & 0xf];
        computed_sha[2*i+1] = HEX_DIGITS[digest[i] & 0xf];
    }
    computed_sha[64] = '\0';
    if (strcmp(computed_sha, expected_sha) != 0) {
        g_errlog.record(ErrCat::OtaFetch,
                        "fw sha mismatch ours=%.8s expected=%.8s",
                        computed_sha, expected_sha);
        Update.abort();
        return;
    }

    // 7) Finalize the partition and reboot. After Update.end(true),
    // the new partition is marked for next-boot. ESP.restart() picks
    // it up; if anything in the new image fails before mark-valid
    // (Phase 2 step 5), the bootloader rolls back.
    if (!Update.end(true)) {
        g_errlog.record(ErrCat::OtaFetch, "fw Update.end failed: %s",
                        Update.errorString());
        return;
    }

    LOG_INFO("fw_poll: applied %s sha=%.8s, rebooting", ptr_buf, computed_sha);
    delay(500);  // let serial drain
    ESP.restart();
    // unreachable
}

#endif  // ARDUINO_ARCH_ESP32

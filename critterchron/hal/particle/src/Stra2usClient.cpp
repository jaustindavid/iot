#if defined(PLATFORM_ID)

#include "Stra2usClient.h"
#include <string.h>

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

bool Stra2usClient::connect() {
    if (tcp_.connected()) tcp_.stop();
    return tcp_.connect(host_, port_);
}

void Stra2usClient::close() {
    if (tcp_.connected()) tcp_.stop();
}

bool Stra2usClient::ensure_connected_() {
    if (tcp_.connected()) return true;
    return tcp_.connect(host_, port_);
}

void Stra2usClient::hex_to_bytes_(const char* hex, uint8_t* out, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        char o[3] = {hex[i*2], hex[i*2+1], '\0'};
        out[i] = (uint8_t)strtol(o, nullptr, 16);
    }
}

void Stra2usClient::sign_(const char* uri, const char* body, size_t body_len,
                          uint32_t ts, char* out_hex) {
    uint8_t secret[32];
    hex_to_bytes_(secret_hex_, secret, 32);

    char ts_str[16];
    snprintf(ts_str, sizeof(ts_str), "%lu", (unsigned long)ts);

    uint8_t payload[512];
    size_t uri_len = strlen(uri);
    size_t ts_len  = strlen(ts_str);
    size_t total   = uri_len + body_len + ts_len;
    if (total >= sizeof(payload)) { out_hex[0] = '\0'; return; }

    memcpy(payload, uri, uri_len);
    if (body && body_len > 0) memcpy(payload + uri_len, body, body_len);
    memcpy(payload + uri_len + body_len, ts_str, ts_len);

    uint8_t result[32];
    hmac_sha256(secret, 32, payload, total, result);
    for (int i = 0; i < 32; ++i)
        snprintf(&out_hex[i*2], 3, "%02x", result[i]);
    out_hex[64] = '\0';
}

bool Stra2usClient::send_all_(const char* data, int len) {
    int sent = 0;
    while (sent < len) {
        int n = tcp_.write((const uint8_t*)(data + sent), len - sent);
        if (n <= 0) { close(); return false; }
        sent += n;
    }
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
                                  char* body_out, size_t body_out_len) {
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
            Particle.process();
        }
        delay(10);
    }
    if (!strstr(buf, "\r\n\r\n")) { close(); return -1; }

    int status = -1;
    const char* sp = strchr(buf, ' ');
    if (!sp) { close(); return -1; }
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
                if (n <= 0) { close(); return -1; }
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
            Particle.process();
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
        if (!verify) { close(); return -1; }

        // Drift window mirrors the server's request check (±300s).
        long ts_val = atol(resp_ts);
        long now    = (long)Time.now();
        if (now > 0 && (now - ts_val > 300 || ts_val - now > 300)) {
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
            close(); return -1;
        }
    }

    return status;
}

int Stra2usClient::publish(const char* topic, const char* message) {
    char uri[64];
    snprintf(uri, sizeof(uri), "/q/%s", topic);

    uint32_t ts = (uint32_t)Time.now();
    size_t body_len = strlen(message);
    char sig[65];
    sign_(uri, message, body_len, ts, sig);

    char req[1024];
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
    char uri[160];
    snprintf(uri, sizeof(uri), "/kv/%s", full_key);

    uint32_t ts = (uint32_t)Time.now();
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
        snprintf(full, sizeof(full), "%s/%s", app_, e.key);
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
}

#endif  // PLATFORM_ID

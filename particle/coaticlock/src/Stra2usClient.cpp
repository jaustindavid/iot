#include "Stra2usClient.h"

Stra2usClient::Stra2usClient(const char* host, int port, const char* client_id, const char* secret_hex)
    : _host(host), _port(port), _client_id(client_id), _secret_hex(secret_hex) {}

void Stra2usClient::hex_to_bytes(const char* hex, uint8_t* out, size_t n) {
    for (size_t i = 0; i < n; i++) {
        char o[3] = {hex[i*2], hex[i*2+1], '\0'};
        out[i] = (uint8_t)strtol(o, nullptr, 16);
    }
}

void Stra2usClient::sign(const char* uri, const char* body, size_t body_len, uint32_t ts, char* out_hex) {
    uint8_t secret[32];
    hex_to_bytes(_secret_hex, secret, 32);

    char ts_str[16];
    snprintf(ts_str, sizeof(ts_str), "%lu", (unsigned long)ts);

    // Concat data to sign: uri + body + ts
    // Use a stack buffer to avoid heap fragmentation during periodic sync
    uint8_t payload[512];
    size_t uri_len = strlen(uri);
    size_t ts_len = strlen(ts_str);
    size_t total_len = uri_len + body_len + ts_len;
    
    if (total_len >= sizeof(payload)) {
        Log.error("sign: Payload too large (%zu bytes)", total_len);
        return;
    }

    memcpy(payload, uri, uri_len);
    if (body && body_len > 0) memcpy(payload + uri_len, body, body_len);
    memcpy(payload + uri_len + body_len, ts_str, ts_len);

    if (total_len < sizeof(payload)) payload[total_len] = '\0';
    Log.info("sign: payload='%s' len=%u", (const char*)payload, (unsigned int)total_len);

    uint8_t result[32];
    hmac_sha256(secret, 32, payload, total_len, result);
    
    for (int i = 0; i < 32; i++) snprintf(&out_hex[i*2], 3, "%02x", result[i]);
    out_hex[64] = '\0';
}

void Stra2usClient::close() {
    if (_client.connected()) {
        _client.stop();
    }
}

bool Stra2usClient::ensure_connected() {
    if (_client.connected()) return true;
    
    if (_client.connect(_host, _port)) {
        return true;
    }
    return false;
}

bool Stra2usClient::send_all(const char* data, int len) {
    int sent = 0;
    while (sent < len) {
        int n = _client.write((const uint8_t*)(data + sent), len - sent);
        if (n <= 0) { close(); return false; }
        sent += n;
    }
    return true;
}

int Stra2usClient::read_response(char* body_out, size_t body_out_len) {
    unsigned long start = millis();
    char buf[1024];
    int total = 0;

    // Read headers
    while (millis() - start < 5000) {
        if (_client.available()) {
            int n = _client.read((uint8_t*)(buf + total), sizeof(buf) - 1 - total);
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
    if (sp) {
        status = atoi(sp + 1);
    } else {
        close();
        return -1;
    }

    int content_length = 0;
    const char* cl = strcasestr(buf, "content-length:");
    if (cl) {
        cl += 15;
        while (*cl == ' ') cl++;
        content_length = atoi(cl);
    }

    const char* hdr_end = strstr(buf, "\r\n\r\n");
    int hdr_len = (int)(hdr_end - buf) + 4;
    int body_have = total - hdr_len;
    const char* body_start = buf + hdr_len;

    int body_filled = 0;
    if (body_out && body_out_len > 0 && body_have > 0) {
        int copy = (body_have < (int)body_out_len - 1) ? body_have : (int)body_out_len - 1;
        memcpy(body_out, body_start, copy);
        body_filled = copy;
    }

    int remaining = content_length - body_have;
    while (remaining > 0 && (millis() - start < 5000)) {
        if (_client.available()) {
            if (body_out && body_out_len > 0 && body_filled < (int)body_out_len - 1) {
                int space = (int)body_out_len - 1 - body_filled;
                int to_read = remaining < space ? remaining : space;
                int n = _client.read((uint8_t*)(body_out + body_filled), to_read);
                if (n <= 0) { close(); return -1; }
                body_filled += n;
                remaining -= n;
            } else {
                char trash[64];
                int to_read = remaining < (int)sizeof(trash) ? remaining : (int)sizeof(trash);
                int n = _client.read((uint8_t*)trash, to_read);
                if (n <= 0) { close(); return -1; }
                remaining -= n;
            }
        } else {
            Particle.process();
        }
        delay(1);
    }
    
    // Flush any leftover unread chunked-encoding fragments to prevent socket poisoning on keep-alive
    int empty_loops = 0;
    while (empty_loops < 5) {
        if (_client.available()) { 
            _client.read(); 
            empty_loops = 0; 
        } else { 
            delay(10); 
            empty_loops++; 
        }
    }

    if (body_out && body_out_len > 0) body_out[body_filled] = '\0';
    return status;
}

int Stra2usClient::publish(const char* topic, const char* message) {
    char uri[64];
    snprintf(uri, sizeof(uri), "/q/%s", topic);
    Log.info("publish: uri=%s", uri);

    uint32_t ts = (uint32_t)Time.now();
    size_t body_len = strlen(message);
    char sig[65];
    Log.info("publish: signing...");
    sign(uri, message, body_len, ts, sig);

    char req[1024];
    Log.info("publish: formatting request for host %s...", _host);
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
        uri, _host, _port, (unsigned int)body_len,
        _client_id, (unsigned long)ts, sig,
        message);

    if (req_len >= (int)sizeof(req)) {
        Log.error("publish: Request too large (%d bytes)", req_len);
        return -1;
    }

    Log.info("publish: connecting to %s:%d...", _host, _port);
    for (int attempt = 0; attempt < 2; attempt++) {
        if (!ensure_connected()) {
            delay(100);
            continue;
        }
        Log.info("publish: sending...");
        if (send_all(req, req_len)) break;
        if (attempt == 1) return -1;
    }

    Log.info("publish: reading response...");
    int status = read_response(nullptr, 0);
    return status;
}

bool Stra2usClient::kv_get(const char* key, char* val_out, size_t val_out_len) {
    char uri[128];
    snprintf(uri, sizeof(uri), "/kv/%s", key);

    uint32_t ts = (uint32_t)Time.now();
    char sig[65];
    sign(uri, nullptr, 0, ts, sig);

    char req[512];
    int req_len = snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "X-Client-ID: %s\r\n"
        "X-Timestamp: %lu\r\n"
        "X-Signature: %s\r\n"
        "Connection: keep-alive\r\n"
        "\r\n",
        uri, _host, _port,
        _client_id, (unsigned long)ts, sig);

    if (req_len >= (int)sizeof(req)) return false;

    for (int attempt = 0; attempt < 2; attempt++) {
        if (!ensure_connected()) {
            delay(100);
            continue;
        }
        if (send_all(req, req_len)) break;
        if (attempt == 1) return false;
    }

    char body[128] = {};
    int status = read_response(body, sizeof(body));
    if (status != 200) return false;

    // Msgpack decoding logic (mirrors IoTClientIDF.h)
    uint8_t* b = (uint8_t*)body;
    const char* str_data = nullptr;
    int str_len = 0;
    long long ival = 0;
    double fval = 0.0;
    bool is_int = false;
    bool is_float = false;

    if ((b[0] & 0xe0) == 0xa0) { str_len = b[0] & 0x1f; str_data = body + 1; }
    else if (b[0] == 0xd9) { str_len = b[1]; str_data = body + 2; }
    else if (b[0] == 0xda) { str_len = ((uint16_t)b[1] << 8) | b[2]; str_data = body + 3; }
    else if (b[0] <= 0x7f) { ival = b[0]; is_int = true; }
    else if (b[0] == 0xcc) { ival = b[1]; is_int = true; }
    else if (b[0] == 0xcd) { ival = ((uint16_t)b[1] << 8) | b[2]; is_int = true; }
    else if (b[0] == 0xce) { ival = ((uint32_t)b[1] << 24) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 8) | b[4]; is_int = true; }
    else if ((b[0] & 0xe0) == 0xe0) { ival = (int8_t)b[0]; is_int = true; }
    else if (b[0] == 0xd0) { ival = (int8_t)b[1]; is_int = true; }
    else if (b[0] == 0xd1) { ival = (int16_t)(((uint16_t)b[1] << 8) | b[2]); is_int = true; }
    else if (b[0] == 0xd2) { ival = (int32_t)(((uint32_t)b[1] << 24) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 8) | b[4]); is_int = true; }
    else if (b[0] == 0xca) { uint32_t v = ((uint32_t)b[1] << 24) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 8) | b[4]; float f; memcpy(&f, &v, 4); fval = f; is_float = true; }
    else if (b[0] == 0xcb) { uint64_t v = ((uint64_t)b[1] << 56) | ((uint64_t)b[2] << 48) | ((uint64_t)b[3] << 40) | ((uint64_t)b[4] << 32) | ((uint64_t)b[5] << 24) | ((uint64_t)b[6] << 16) | ((uint64_t)b[7] << 8) | b[8]; double d; memcpy(&d, &v, 8); fval = d; is_float = true; }
    else return false;

    if (is_int) {
        snprintf(val_out, val_out_len, "%lld", ival);
        return true;
    }
    if (is_float) {
        snprintf(val_out, val_out_len, "%.2f", fval);
        return true;
    }

    int copy = (str_len < (int)val_out_len - 1) ? str_len : (int)val_out_len - 1;
    memcpy(val_out, str_data, copy);
    val_out[copy] = '\0';
    return true;
}

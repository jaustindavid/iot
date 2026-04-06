#include "IoTClient.h"
#include <string.h>

// On ESP32, mbedtls is built-in.
#if defined(ESP32) || defined(PARTICLE)
#include "mbedtls/md.h"
#else
// A fallback would be needed for regular Arduino Uno, 
// e.g. using Arduino Cryptography Library:
// #include <Crypto.h>
// #include <SHA256.h>
#endif

// CMP Buffer Callbacks
static bool mem_buf_reader(cmp_ctx_t *ctx, void *data, size_t limit) {
    return false; // Not implemented for simple memory buffer here
}
static bool mem_buf_skipper(cmp_ctx_t *ctx, size_t count) { return false; }

static size_t mem_buf_writer(cmp_ctx_t *ctx, const void *data, size_t count) {
    struct memory_buffer *mb = (struct memory_buffer *)ctx->buf;
    if (mb->size + count > mb->capacity) {
        return 0; // Overflow
    }
    memcpy(mb->data + mb->size, data, count);
    mb->size += count;
    return count;
}

void init_memory_buffer(struct memory_buffer* mb, uint8_t* mem, size_t capacity) {
    mb->data = mem;
    mb->size = 0;
    mb->capacity = capacity;
}

IoTClient::IoTClient(Client& client, const char* host, uint16_t port, const char* clientId, const char* secretHex)
    : _client(client), _host(host), _port(port), _clientId(clientId), _secretHex(secretHex), _timeFunc(nullptr) {
}

void IoTClient::setTimeFunction(uint32_t (*timeFunc)()) {
    _timeFunc = timeFunc;
}

void IoTClient::calculateSignature(const char* uri, const uint8_t* payload, size_t payloadLen, uint32_t timestamp, char* outHex) {
#if defined(ESP32) || defined(PARTICLE)
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);

    const mbedtls_md_info_t* md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!md_info) return;

    mbedtls_md_setup(&ctx, md_info, 1);

    // Convert hex secret to bytes
    uint8_t secretBytes[32];
    for (int i = 0; i < 32; i++) {
        char octet[3] = {_secretHex[i * 2], _secretHex[i * 2 + 1], '\0'};
        secretBytes[i] = (uint8_t)strtol(octet, NULL, 16);
    }

    mbedtls_md_hmac_starts(&ctx, secretBytes, 32);

    // HMAC over: URI + Body + Timestamp
    mbedtls_md_hmac_update(&ctx, (const unsigned char*)uri, strlen(uri));
    if (payloadLen > 0 && payload != nullptr) {
        mbedtls_md_hmac_update(&ctx, payload, payloadLen);
    }
    
    char tsStr[16];
    snprintf(tsStr, sizeof(tsStr), "%lu", (unsigned long)timestamp);
    mbedtls_md_hmac_update(&ctx, (const unsigned char*)tsStr, strlen(tsStr));

    uint8_t hmacResult[32];
    mbedtls_md_hmac_finish(&ctx, hmacResult);
    mbedtls_md_free(&ctx);

    // Convert to hex
    for (int i = 0; i < 32; i++) {
        sprintf(&outHex[i * 2], "%02x", hmacResult[i]);
    }
    outHex[64] = '\0';
#else
    // Dummy placeholder for non-ESP32
    strcpy(outHex, "mock_signature_for_testing");
#endif
}

int IoTClient::sendSignedRequest(const char* method, const char* uri, const uint8_t* payload, size_t payloadLen, uint8_t* responseBuffer, size_t maxLen, size_t* outLen, const char* contentType) {
    if (!_client.connected()) {
        if (!_client.connect(_host, _port)) {
            return -1;
        }
    }

    uint32_t ts = _timeFunc ? _timeFunc() : 0;
    char sigHex[65];
    calculateSignature(uri, payload, payloadLen, ts, sigHex);

    // Write HTTP Request with zero-malloc technique
    _client.print(method);
    _client.print(" ");
    _client.print(uri);
    _client.print(" HTTP/1.1\r\n");

    _client.print("Host: ");
    _client.print(_host);
    _client.print("\r\n");
    
    _client.print("Connection: keep-alive\r\n");

    _client.print("X-Client-ID: ");
    _client.print(_clientId);
    _client.print("\r\n");

    _client.print("X-Timestamp: ");
    _client.print(ts);
    _client.print("\r\n");

    _client.print("X-Signature: ");
    _client.print(sigHex);
    _client.print("\r\n");

    if (payloadLen > 0) {
        _client.print("Content-Type: ");
        _client.print(contentType);
        _client.print("\r\n");
        _client.print("Content-Length: ");
        _client.print(payloadLen);
        _client.print("\r\n\r\n");
        _client.write(payload, payloadLen);
    } else {
        _client.print("Content-Length: 0\r\n\r\n");
    }

    // Await Response briefly
    unsigned long start = millis();
    while(!_client.available() && millis() - start < 5000) {
        delay(10);
    }

    // HTTP/1.1 Keep-Alive response processing
    bool isBody = false;
    String currentLine = "";
    int readHead = 0;
    int contentLength = -1;
    int statusCode = -1;
    bool isFirstLine = true;
    
    while (_client.connected() || _client.available()) {
        if (_client.available()) {
            char c = _client.read();
            if (!isBody) {
                currentLine += c;
                if (currentLine.endsWith("\n")) {
                    if (isFirstLine) {
                        // Parse "HTTP/1.1 200 OK"
                        int firstSpace = currentLine.indexOf(' ');
                        if (firstSpace != -1) {
                            String codePart = currentLine.substring(firstSpace + 1);
                            int secondSpace = codePart.indexOf(' ');
                            if (secondSpace != -1) {
                                statusCode = codePart.substring(0, secondSpace).toInt();
                            } else {
                                statusCode = codePart.toInt();
                            }
                        }
                        isFirstLine = false;
                    }

                    String lowerLine = currentLine;
                    lowerLine.toLowerCase();
                    if (lowerLine.startsWith("content-length:")) {
                        contentLength = lowerLine.substring(15).toInt();
                    }
                    if (currentLine == "\r\n" || currentLine == "\n") {
                        isBody = true;
                        if (contentLength == 0) {
                            break; // 204 No Content or Empty
                        }
                    }
                    currentLine = "";
                }
            } else {
                if (responseBuffer != nullptr && outLen != nullptr && readHead < maxLen) {
                    responseBuffer[readHead] = c;
                }
                readHead++;
                
                if (contentLength >= 0 && readHead >= contentLength) {
                    break; // Finished reading HTTP body
                }
            }
        } else {
            if (!_client.connected()) break;
            delay(1);
        }
    }
    
    if (outLen != nullptr) {
        *outLen = readHead;
    }

    // We purposely do NOT call _client.stop() to preserve the TCP socket
    return statusCode;
}

int IoTClient::writeKV(const char* key, const uint8_t* payload, size_t payloadLen) {
    char uri[64];
    snprintf(uri, sizeof(uri), "/kv/%s", key);
    return sendSignedRequest("POST", uri, payload, payloadLen, nullptr, 0, nullptr);
}

int IoTClient::readKV(const char* key, uint8_t* responseBuffer, size_t maxLen, size_t* outLen) {
    char uri[64];
    snprintf(uri, sizeof(uri), "/kv/%s", key);
    return sendSignedRequest("GET", uri, nullptr, 0, responseBuffer, maxLen, outLen);
}

int IoTClient::publishQueue(const char* topic, const uint8_t* payload, size_t payloadLen) {
    char uri[64];
    snprintf(uri, sizeof(uri), "/q/%s", topic);
    return sendSignedRequest("POST", uri, payload, payloadLen, nullptr, 0, nullptr);
}

int IoTClient::publishQueue(const char* topic, const uint8_t* payload, size_t payloadLen, uint32_t ttl) {
    char uri[128];
    snprintf(uri, sizeof(uri), "/q/%s?ttl=%lu", topic, (unsigned long)ttl);
    return sendSignedRequest("POST", uri, payload, payloadLen, nullptr, 0, nullptr);
}

int IoTClient::publishQueue(const char* topic, const char* message) {
    char uri[64];
    snprintf(uri, sizeof(uri), "/q/%s", topic);
    return sendSignedRequest("POST", uri, (const uint8_t*)message, strlen(message), nullptr, 0, nullptr, "text/plain");
}

int IoTClient::consumeQueue(const char* topic, uint8_t* responseBuffer, size_t maxLen, size_t* outLen, bool envelope) {
    char uri[96];
    if (envelope) {
        snprintf(uri, sizeof(uri), "/q/%s?envelope=true", topic);
    } else {
        snprintf(uri, sizeof(uri), "/q/%s", topic);
    }

    size_t localOutLen = 0;
    int status = sendSignedRequest("GET", uri, nullptr, 0, responseBuffer, maxLen, &localOutLen);

    if (outLen != nullptr) {
        *outLen = localOutLen;
    }

    return status;
}

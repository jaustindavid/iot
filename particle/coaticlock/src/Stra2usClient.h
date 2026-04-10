#pragma once

#include "Particle.h"
#include "hmac_sha256.h"

/**
 * Stra2usClient
 * 
 * Native Stra2us client for Particle using TCPClient and mbedtls for signing.
 * Maintains a persistent socket for efficiency across multiple calls in a cycle.
 */
class Stra2usClient {
public:
    Stra2usClient(const char* host, int port, const char* client_id, const char* secret_hex);
    
    // topic: queue name (e.g. "coaticlock")
    // message: text payload
    int publish(const char* topic, const char* message);
    
    // key: KV key (e.g. "coaticlock/wobble_max_seconds")
    // val_out: buffer for the returned value string
    // returns true if key was found
    bool kv_get(const char* key, char* val_out, size_t val_out_len);

    bool connect();
    void close();

private:
    const char* _host;
    int _port;
    const char* _client_id;
    const char* _secret_hex;
    TCPClient _client;

    bool ensure_connected();
    bool send_all(const char* data, int len);
    int read_response(char* body_out, size_t body_out_len);
    void sign(const char* uri, const char* body, size_t body_len, uint32_t ts, char* out_hex);
    void hex_to_bytes(const char* hex, uint8_t* out, size_t n);
};

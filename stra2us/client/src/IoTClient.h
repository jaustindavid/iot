#ifndef IOT_CLIENT_H
#define IOT_CLIENT_H

#include <Arduino.h>
#include <Client.h>
#include "cmp.h"

class IoTClient {
public:
    IoTClient(Client& client, const char* host, uint16_t port, const char* clientId, const char* secretHex);

    // Write persistent configuration data to the KV store
    int writeKV(const char* key, const uint8_t* payload, size_t payloadLen);

    // Read persistent configuration data from the KV store
    int readKV(const char* key, uint8_t* responseBuffer, size_t maxLen, size_t* outLen);

    // Publish ephemeral data to a queue (uses backend default TTL of 1 hour)
    int publishQueue(const char* topic, const uint8_t* payload, size_t payloadLen);

    // Publish ephemeral data to a queue with a custom TTL (in seconds)
    int publishQueue(const char* topic, const uint8_t* payload, size_t payloadLen, uint32_t ttl);

    // Consume messages from a queue using the pulling cursor.
    // Returns HTTP status code (200 = msg, 204 = empty, etc.) or -1 on error.
    int consumeQueue(const char* topic, uint8_t* responseBuffer, size_t maxLen, size_t* outLen);

    // Set a function to get unix time (required for timestamp signing)
    void setTimeFunction(uint32_t (*timeFunc)());

    // Utility: calculate HMAC-SHA256 (exposed for advanced use or internal)
    void calculateSignature(const char* uri, const uint8_t* payload, size_t payloadLen, uint32_t timestamp, char* outHex);

private:
    Client& _client;
    const char* _host;
    uint16_t _port;
    const char* _clientId;
    const char* _secretHex;

    uint32_t (*_timeFunc)();

    int sendSignedRequest(const char* method, const char* uri, const uint8_t* payload, size_t payloadLen, uint8_t* responseBuffer, size_t maxLen, size_t* outLen);
    void readResponse(uint8_t* responseBuffer, size_t maxLen, size_t* outLen);
};

// Custom minimal buffer writer for cmp
struct memory_buffer {
    uint8_t* data;
    size_t size;
    size_t capacity;
};

// Helper initialization
void init_memory_buffer(struct memory_buffer* buf, uint8_t* mem, size_t capacity);

#endif // IOT_CLIENT_H

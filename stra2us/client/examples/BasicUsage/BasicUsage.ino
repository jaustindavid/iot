#include <WiFi.h>  // or Particle.h
#include "IoTClient.h"
#include "cmp.h"

const char* ssid = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PWD";

const char* SERVER_HOST = "192.168.1.100"; // Replace with actual backend IP
const uint16_t SERVER_PORT = 8000;
const char* CLIENT_ID = "Device-A-123";
const char* CLIENT_SECRET = "REPLACE_WITH_HEX_SECRET_FROM_DASHBOARD";

WiFiClient wifiClient;
IoTClient iotClient(wifiClient, SERVER_HOST, SERVER_PORT, CLIENT_ID, CLIENT_SECRET);

// Simulated RTC time function
uint32_t get_unix_time() {
    // In real app, sync with NTP at boot and return actual Unix Epoch Time.
    // E.g., return Time.now(); (on Particle) or similar.
    return 1711985000; 
}

void setup() {
    Serial.begin(115200);
    delay(10);

    Serial.println("Connecting to WiFi...");
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\nWiFi connected.");

    iotClient.setTimeFunction(get_unix_time);

    // --- Example 1: Publish to Queue (Zero Malloc) ---
    uint8_t payload_buf[64];
    struct memory_buffer mb;
    init_memory_buffer(&mb, payload_buf, sizeof(payload_buf));

    cmp_ctx_t ctx;
    cmp_init(&ctx, &mb, mem_buf_reader, mem_buf_skipper, mem_buf_writer);

    // Pack a simple map: {"temp": 24.5, "status": "ok"}
    cmp_write_map(&ctx, 2);
    cmp_write_str(&ctx, "temp", 4);
    cmp_write_float(&ctx, 24.5f);
    cmp_write_str(&ctx, "status", 6);
    cmp_write_str(&ctx, "ok", 2);

    Serial.println("Publishing telemetry data...");
    bool pubSuccess = iotClient.publishQueue("sensors/temp", mb.data, mb.size);
    if (pubSuccess) Serial.println("Publish SUCCESS!");
    else Serial.println("Publish FAILED!");

    // --- Example 2: Read from KV ---
    Serial.println("Reading configuration key...");
    uint8_t rx_buf[128];
    size_t rx_len = 0;
    bool rxSuccess = iotClient.readKV("device_config", rx_buf, sizeof(rx_buf), &rx_len);
    
    if (rxSuccess && rx_len > 0) {
        Serial.printf("KV Read SUCCESS (Length: %d)\n", rx_len);
        
        // We could decode MessagePack here using cmp again.
        struct memory_buffer rx_mb;
        init_memory_buffer(&rx_mb, rx_buf, rx_len);
        rx_mb.size = rx_len; // Set the amount of data we received

        cmp_ctx_t rx_ctx;
        cmp_init(&rx_ctx, &rx_mb, mem_buf_reader, mem_buf_skipper, mem_buf_writer);
        // ... perform read logic
    } else {
        Serial.println("KV Read FAILED or empty.");
    }
}

void loop() {
    // Publish telemetry every 60 seconds
    delay(60000);
}

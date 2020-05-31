#ifndef ESP32_WIFI_H
#define ESP32_WIFI_H

#include <WiFi.h>
#include "wifisecrets.h"

class WiFiManager {
    public:
        void setup();
        void connect();
        void disconnect();
};


void WiFiManager::setup() {
    Serial.println("setup");
}

void WiFiManager::connect() {
    uint32_t start = millis();
    Serial.println("Connect!");
    WiFi.begin(ssid, password);
    Serial.print("Establishing connection to WiFi");
 
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }

    Serial.printf("\nMAC: %s, IP: ", WiFi.macAddress().c_str());
    Serial.println(WiFi.localIP());
    Serial.printf("Elapsed time: %lu ms\n", millis() - start);
}

void WiFiManager::disconnect() {
    Serial.println("Disconnect!");
    WiFi.disconnect(true);
}


#endif
#ifndef ESP32_WIFI_H
#define ESP32_WIFI_H

#include <esp_wifi.h>
#include <WiFi.h>
#include <WiFiClient.h>
// #include "wifisecrets.h"

#include <ESP_WiFiManager.h>              //https://github.com/khoih-prog/ESP_WiFiManager


#define ESP_getChipId()   ((uint32_t)ESP.getEfuseMac())
// SSID and PW for Config Portal
String ssid = "dumpsterfire_" + String(ESP_getChipId(), HEX);
const char* password = "";

// SSID and PW for your Router
// note that these will be discovered and stored;
// defaults are neither required nor encouraged
String Router_SSID;
String Router_Pass;

// Use false if you don't like to display Available Pages in Information Page of Config Portal
// Comment out or use true to display Available Pages in Information Page of Config Portal
// Must be placed before #include <ESP_WiFiManager.h>
#define USE_AVAILABLE_PAGES     false

#define PIN_D0            0         // Pin D0 mapped to pin GPIO0/BOOT/ADC11/TOUCH1 of ESP32
#define PIN_LED           2         // Pin D2 mapped to pin GPIO2/ADC12 of ESP32, control on-board LED
#define PIN_D25           25        // Pin D25 mapped to pin GPIO25/ADC18/DAC1 of ESP32
#define LED_ON      HIGH
#define LED_OFF     LOW

/* Trigger for inititating config mode is Pin D3 and also flash button on NodeMCU
   Flash button is convenient to use but if it is pressed it will stuff up the serial port device driver
   until the computer is rebooted on windows machines.
*/
const int TRIGGER_PIN = PIN_D0;   // Pin D0 mapped to pin GPIO0/BOOT/ADC11/TOUCH1 of ESP32
/*
   Alternative trigger pin. Needs to be connected to a button to use this pin. It must be a momentary connection
   not connected permanently to ground. Either trigger pin will work.
*/
const int TRIGGER_PIN2 = PIN_D25; // Pin D25 mapped to pin GPIO25/ADC18/DAC1 of ESP32


// Indicates whether ESP has WiFi credentials saved from previous session
bool initialConfig = false;

void heartBeatPrint(void)
{
  static int num = 1;

  if (WiFi.status() == WL_CONNECTED)
    Serial.print("H");        // H means connected to WiFi
  else
    Serial.print("F");        // F means not connected to WiFi

  if (num == 80)
  {
    Serial.println();
    num = 1;
  }
  else if (num++ % 10 == 0)
  {
    Serial.print(" ");
  }
}

void check_status()
{
  static ulong checkstatus_timeout = 0;

#define HEARTBEAT_INTERVAL    10000L
  // Print hearbeat every HEARTBEAT_INTERVAL (10) seconds.
  if ((millis() > checkstatus_timeout) || (checkstatus_timeout == 0))
  {
    heartBeatPrint();
    checkstatus_timeout = millis() + HEARTBEAT_INTERVAL;
  }
}

void softAP_setup() {
  // put your setup code here, to run once:
  // initialize the LED digital pin as an output.
  pinMode(PIN_LED, OUTPUT);
  pinMode(TRIGGER_PIN, INPUT_PULLUP);

  unsigned long startedAt = millis();

  //Local intialization. Once its business is done, there is no need to keep it around

  ESP_WiFiManager ESP_wifiManager(ssid.c_str()); // ("ConfigOnSwitch");

  ESP_wifiManager.setMinimumSignalQuality(-1);
  // Set static IP, Gateway, Subnetmask, DNS1 and DNS2. New in v1.0.5
  ESP_wifiManager.setSTAStaticIPConfig(IPAddress(192, 168, 2, 114), IPAddress(192, 168, 2, 1), IPAddress(255, 255, 255, 0),
                                       IPAddress(192, 168, 2, 1), IPAddress(8, 8, 8, 8));

  // We can't use WiFi.SSID() in ESP32 as it's only valid after connected.
  // SSID and Password stored in ESP32 wifi_ap_record_t and wifi_config_t are also cleared in reboot
  // Have to create a new function to store in EEPROM/SPIFFS for this purpose
  Router_SSID = ESP_wifiManager.WiFi_SSID();
  Router_Pass = ESP_wifiManager.WiFi_Pass();

  //Remove this line if you do not want to see WiFi password printed
  Serial.println("Stored: SSID = '" + Router_SSID + "', Pass = '" + Router_Pass + "'");

  // SSID to uppercase
  ssid.toUpperCase();

  if (Router_SSID == "")
  {
    Serial.println("We haven't got any access point credentials, so get them now");

    digitalWrite(PIN_LED, LED_ON); // Turn led on as we are in configuration mode.
    
    //it starts an access point
    //and goes into a blocking loop awaiting configuration
    if (!ESP_wifiManager.startConfigPortal((const char *) ssid.c_str(), password))
      Serial.println("Not connected to WiFi but continuing anyway.");
    else
      Serial.println("WiFi connected...yeey :)");
  }

  digitalWrite(PIN_LED, LED_OFF); // Turn led off as we are not in configuration mode.

#define WIFI_CONNECT_TIMEOUT        30000L
#define WHILE_LOOP_DELAY            1000L
#define WHILE_LOOP_STEPS            (WIFI_CONNECT_TIMEOUT / ( 3 * WHILE_LOOP_DELAY ))

  startedAt = millis();

  while ( (WiFi.status() != WL_CONNECTED) && (millis() - startedAt < WIFI_CONNECT_TIMEOUT ) )
  {
    WiFi.mode(WIFI_STA);
    WiFi.persistent (true);
    // We start by connecting to a WiFi network

    Serial.print("Connecting to ");
    Serial.println(Router_SSID);

    WiFi.begin(Router_SSID.c_str(), Router_Pass.c_str());

    int i = 0;
    while ((!WiFi.status() || WiFi.status() >= WL_DISCONNECTED) && i++ < WHILE_LOOP_STEPS)
    {
      delay(WHILE_LOOP_DELAY);
    }
  }

  Serial.print("After waiting ");
  Serial.print((millis() - startedAt) / 1000);
  Serial.print(" secs more in setup(), connection result is ");

  if (WiFi.status() == WL_CONNECTED)
  {
    Serial.print("connected. Local IP: ");
    Serial.println(WiFi.localIP());
  }
  else
    Serial.println(ESP_wifiManager.getStatus(WiFi.status()));
} // softAP_setup()


void softAP_loop(bool forceAP = false) {
  // is configuration portal requested?
  if (forceAP or digitalRead(TRIGGER_PIN) == LOW) {
    Serial.println("\nConfiguration portal requested.");
    digitalWrite(PIN_LED, LED_ON); // turn the LED on by making the voltage LOW to tell us we are in configuration mode.

    //Local intialization. Once its business is done, there is no need to keep it around
    ESP_WiFiManager ESP_wifiManager;

    //Check if there is stored WiFi router/password credentials.
    //If not found, device will remain in configuration mode until switched off via webserver.
    Serial.print("Opening configuration portal. ");
    Router_SSID = ESP_wifiManager.WiFi_SSID();
    if (Router_SSID != "") {
      ESP_wifiManager.setConfigPortalTimeout(300); //If no access point name has been previously entered disable timeout.
      Serial.println("Got stored Credentials. Timeout 300s");
    } else
      Serial.println("No stored Credentials. No timeout");

    //it starts an access point
    //and goes into a blocking loop awaiting configuration
    if (!ESP_wifiManager.startConfigPortal((const char *) ssid.c_str(), password)) {
      Serial.println("Not connected to WiFi but continuing anyway.");
    } else {
      //if you get here you have connected to the WiFi
      Serial.println("connected...yeey :)");
      Router_SSID = ESP_wifiManager.WiFi_SSID();
      Router_Pass = ESP_wifiManager.WiFi_Pass();

      //Remove this line if you do not want to see WiFi password printed
      Serial.println("Stored: SSID = '" + Router_SSID + "', Pass = '" + Router_Pass + "'");
    }

    digitalWrite(PIN_LED, LED_OFF); // Turn led off as we are not in configuration mode.
  }

  // put your main code here, to run repeatedly
  check_status();

} // softAP_loop()


class WiFiManager {
    public:
        void setup();
        bool connect();
        void disconnect();
};


void WiFiManager::setup() {
    Serial.println("setup");
    softAP_setup();
    if (WiFi.status() != WL_CONNECTED)
        softAP_loop(true);
}

bool WiFiManager::connect() {
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("Already connected!");
      return true;
    }
    uint32_t start = millis();
    Serial.println("Connect!");
    WiFi.begin(Router_SSID.c_str(), Router_Pass.c_str());
    Serial.print("Establishing connection to WiFi");
 
    while (WiFi.status() != WL_CONNECTED and (millis() - start < 30 * 1000)) {
        delay(500);
        Serial.print(".");
    }

    Serial.printf("\nMAC: %s, IP: ", WiFi.macAddress().c_str());
    Serial.println(WiFi.localIP());
    Serial.printf("Elapsed time: %lu ms\n", millis() - start);

    return WiFi.status() == WL_CONNECTED;
}

void WiFiManager::disconnect() {
    Serial.println("Disconnect!");
    WiFi.disconnect(true);
}


#endif
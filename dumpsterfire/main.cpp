#include <Arduino.h>
#include "esp32_wifi.h"
#include "time.h"

#define DEBUGGING

#ifdef ESP32
  #define LED_DATA  14
  #define LED_CLOCK 12
  #define LED_POWER 27
#else
  // confirmed
  #define LED_DATA  PIN_D7
  #define LED_CLOCK PIN_D8
#endif

#define TIME_NOW (millis()/1000) // time.now()
#define COLOR_ORDER BGR

#include "SimpleTimer.h"

// #define STRATUS_DEBUGGING
#include "stratus.h"
Stratus stratus("http://stratus-iot.s3.amazonaws.com/dumpsterfire.txt", "Dumpst3rF1r3");

// managed globals
int16_t REFRESH_INTERVAL = 300;

#include "leds.h"
#include "tweeter.h"

WiFiManager wfm;

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 5000);
  Serial.println("\nStarting");
  delay(5000);
  // softAP_setup();
  wfm.setup();
  setupLEDs();
  setupTweeter();
} //setup()


void loop() {
  static SimpleTimer loopTimer(50);
  static SimpleTimer apTimer(30000, true);
  static SimpleTimer updateTimer(REFRESH_INTERVAL * 1000, true);

  if (updateTimer.isExpired()) {
    wfm.connect();
    stratus.update();
    updateTimer.setInterval(stratus.getInt("refresh interval", REFRESH_INTERVAL) * 1000);
    wfm.disconnect();
  }

  uint8_t recency = checkRecency();
  uint8_t level = checkIntensity();
  burn(recency, level);

  loopTimer.wait();
} // loop()

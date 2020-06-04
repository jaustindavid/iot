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

#define STATUS_OK 0
#define STATUS_OFFLINE 1
uint32_t heartbeep = 0;
byte status = STATUS_OK;

#include "FastLED.h"
FASTLED_USING_NAMESPACE
#include "tweeter.h"
#include "leds.h"

WiFiManager wfm;

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 5000);
  Serial.println("\nStarting");
  delay(5000);
  // softAP_setup();
  wfm.setup();
  setupTweeter();
  setupLEDs();
  // REFRESH_INTERVAL = stratus.getInt("refresh interval", REFRESH_INTERVAL);
  // Serial.printf("Refreshing every %d seconds\n", REFRESH_INTERVAL);
} //setup()


void loop() {
  static SimpleTimer loopTimer(100);
  static SimpleTimer updateTimer(REFRESH_INTERVAL * 1000, true);

  if (millis() < 900 * 1000) { // first 900s / 15m after startup
    softAP_loop();
  }

  if (updateTimer.isExpired()) {
    if (wfm.connect()) {
      Serial.println("doin an update");
      stratus.update();
      updateTimer.setInterval(stratus.getInt("refresh interval", REFRESH_INTERVAL) * 1000);
      wfm.disconnect();
      status = STATUS_OK;
    } else {
      // if I can't connect, randomly stoke the fire a little
      if (rand() > 0.9) {
        callback("fuel", "100");
      }
      status = STATUS_OFFLINE;
    }
  }

/*
  // now runs async in a task from setup_leds()
  uint8_t recency = checkRecency();
  uint8_t level = checkIntensity();
  burn(recency, level);
*/

  // Serial.printf("%d: looped; %d remaining\n", millis(), loopTimer.remaining());
  loopTimer.wait(false);
} // loop()



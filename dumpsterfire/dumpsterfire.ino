
#include "Particle.h"

/*
 * Hardware:
 *   1. A dumpster (duh).  140% or about 3.5" long
 *   2. Dense APA102 strip.  WS2811 would work with very minor code change
 *        144 LED/M I think?
 *        Cut 10 LEDs (~ 2.5").  More or Fewer SHOULD work but not tested
 *   3. austindavid.com/phobrd
 *   4. and a photon
 *   5? Optional: 3306 OLED, I2C.  Use a 4-pin female header on the underside
 *
 *   Wiring: 
 *     LED 5V  -> phobrd pin 1 (5v); 
 *     LED GND -> phobrd pin 2 (GND);
 *     LED D   -> phobrd A4 
 *     LED C   -> phobrd A5
 * 
 *     Button 1 (L) -> phobrd D2 + GND
 *     Button 2 (R) -> phobrd D3 + GND
 */
 
#define LBUTTON_PIN D2
#define RBUTTON_PIN D3
#define BUTTON_GND  D4

// #define DEBUGGING
// #define DEBUGGING_STRATUS
// #define STRATUS_DEBUGGING


// This #include statement was automatically added by the Particle IDE.
#include "http.h"

#include <md5.h>
#include <clickButton.h>

#include <FastLED.h>
FASTLED_USING_NAMESPACE

#define PHOTON
#include "stratus.h"
Stratus stratus("http://austindavid.com/stratus/dumpsterfire.txt", "Dumpst3rF1r3");
// managed globals
int16_t REFRESH_INTERVAL = 300;


#include "softap.h"
#include "timezone.h"

// #define DISPLAY_ENABLED
#ifdef DISPLAY_ENABLED
    #include "display.h"
#endif

// #include "tweeter.h"
#include "leds.h"

#include "buddy.h"
Buddy buddy;


void setup() {
    Serial.begin(115200);
    Serial.println("Starting");
    pinMode(D7, OUTPUT);
    // pinMode(D2, INPUT_PULLUP);
    pinMode(D2, OUTPUT);
    digitalWrite(D2, HIGH);
    
    setupSoftAP();
    setupLEDs();
    
    #ifdef DISPLAY_ENABLED
        setupDisplay();
        banner(2, "dumpster\n   fire!\n\n @realDJT");
        delay(500); 
    #endif
    
    Serial.println("updating stratus");
    stratus.update();
    Serial.println("My GUID: ");
    Serial.println(stratus.getGUID());
    
    setupTZOffset();
    // setupTweeter();
    buddy.setup();
} // setup()


void loop() {
    // Serial.printf("L button: %d\n", digitalRead(D2));
    static uint32_t nextHB = 0;
    if (millis() > nextHB) {
        Serial.println("beep");
        digitalWrite(D7, !digitalRead(D7));
        nextHB = millis() + 1000;
    }
    
    buddy.update();
    
    EVERY_N_SECONDS (stratus.getInt("refresh interval", REFRESH_INTERVAL)) {
        Particle.syncTime();
        stratus.update();
    }
    /*
        int intensity = checkIntensity();
        int recency = checkRecency();
        burn(recency, intensity);
    */
} // loop()
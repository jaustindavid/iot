#ifndef LEDS_H
#define LEDS_H

#define DEBUGGING
// #undef DEBUGGING


#include "FastLED.h"
FASTLED_USING_NAMESPACE

#define FRAMES_PER_SECOND 60
#define NUM_LEDS 10
CRGB leds[NUM_LEDS];

// https://github.com/FastLED/FastLED/wiki/Gradient-color-palettes
DEFINE_GRADIENT_PALETTE( heatmap_gp ) {
  0,    0,  0,  0,   //black
 50, 255,   0,  0,   //red
200, 255, 255,  0,   //bright yellow
255, 255, 255, 255 }; //full white

CRGBPalette16 palette = heatmap_gp; // HeatColors_p;

class Ember {
    private:
        uint8_t _position;    // my place in the strip
        uint8_t _threshold;   // I will only illuminate above this
        uint8_t _colorIndex; 
    
        void printHeader() {
            Serial.printf("Ember[%d](%d)", _position, _threshold);
        } // print()
        
    
    public:
        Ember() {} 
        
        void init(const uint8_t position, const uint8_t threshold) {
            _position = position;
            _threshold = threshold;
            _colorIndex = threshold;
            #ifdef DEBUGGING
                Serial.print("Creating ");
                printHeader();
                Serial.println();
            #endif
        } // init(position, threshold)
        
         
        // algo:
        //   recency dictates whether this LED is lit at all
        //   level [0..100] dictates a range of possible colors from the pallet
        //      as well as the delta between cycles
        void burn(const uint8_t recency, const uint8_t level, bool printing = false) {
            if (printing) {
                printHeader();
            }
            if (recency >= _threshold) {
                #define MINCOLOR 10
                uint8_t maxColor = map(min(level, (uint8_t)100), 0, 100, MINCOLOR, 255);
                maxColor = constrain(maxColor, MINCOLOR, 255);
                int8_t range = constrain(level, 5, 50);
                if (printing)
                    Serial.printf(" [max=%d; range=%d;", maxColor, range);
                int8_t delta = range/2 - random8(range);
                if (printing)
                    Serial.printf("; delta=%d; index=%d]", delta, _colorIndex);
                _colorIndex = constrain(_colorIndex + delta, MINCOLOR, maxColor);
                // uint8_t colorIndex = random8(MINCOLOR, maxColor);
                if (printing) 
                    Serial.printf(": intensity=%d, index=%d", level, _colorIndex);
                leds[_position] = ColorFromPalette(palette, _colorIndex);
                leds[_position].subtractFromRGB(random8(50));
            } else {
                leds[_position] = CRGB::Black;
                if (printing) Serial.println(" NOP");
            }
            if (printing) Serial.println();
        } // burn(level)
}; // class Ember


Ember embers[NUM_LEDS];

#undef CENTERED

void createEmbers() {
    #ifdef CENTERED
    uint8_t oddPosition = 4;
    uint8_t evenPosition = 5;
    for (uint8_t i = 0; i < NUM_LEDS; i++) {
        #ifdef DEBUGGING
        Serial.print("i:");
        Serial.print(i);
        #endif
        if (i % 2 == 0) { // even
            #ifdef DEBUGGING
                Serial.print("; even: ");
                Serial.println(evenPosition);
            #endif
            embers[i].init(evenPosition, i*NUM_LEDS);
            evenPosition += 1;
        } else {
            #ifdef DEBUGGING
                Serial.print("; odd: ");
                Serial.println(oddPosition);
            #endif
            embers[i].init(oddPosition, i*NUM_LEDS);
            oddPosition -= 1;
        }
    }
    #else
    for (uint8_t i = 0; i < NUM_LEDS; i++) {
        #ifdef DEBUGGING
            Serial.print("i:");
            Serial.println(i);
        #endif
        embers[i].init(i, (NUM_LEDS-i-1)*NUM_LEDS);
    }
   #endif
} // createEmbers()


void setupLEDs() {
  #ifdef ESP32
    pinMode(LED_POWER, OUTPUT);
    digitalWrite(LED_POWER, HIGH);
  #endif
    FastLED.addLeds<APA102, LED_DATA, LED_CLOCK, COLOR_ORDER>(leds, NUM_LEDS);
    FastLED.setBrightness(32);
    FastLED.clear();
    FastLED.show();
    
    // palette = CRGBPalette16(CRGB::Black, CRGB::Red, CRGB::Orange, CRGB::Yellow);

    createEmbers();
} // setupLEDs()


void barGraph(const int level) {
    for (int i = 0; i < NUM_LEDS; i++) {
        if (level/10 > i) {
            leds[i] = CRGB::Yellow;
            fadeToBlackBy(leds, NUM_LEDS, 1000/NUM_LEDS);
        }
    }
} // barGraph(level)


#define MIN_BRIGHTNESS 16
#define MAX_BRIGHTNESS 96

// recency, level will vary 0..100
void burn(const uint8_t recency, const uint8_t level) {
    bool printing = false;
    EVERY_N_SECONDS(5) {
        Serial.printf("%lu: burning: recency=%d, level=%d\n", 
                        TIME_NOW, recency, level);
        printing = true;
    }
  
    for (uint8_t i = 0; i < NUM_LEDS; i++) {
        embers[i].burn(recency, level, printing);
    }
    int brightness = map(level, 0, 100, MIN_BRIGHTNESS, MAX_BRIGHTNESS);
    FastLED.setBrightness(brightness);
    
    FastLED.show();
    FastLED.delay(1000/FRAMES_PER_SECOND); 
}

#endif

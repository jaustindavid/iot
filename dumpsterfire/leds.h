#ifndef LEDS_H
#define LEDS_H

/*
 * TOO:
 *   Ember[i]:
 *     burn with an intensity
 *     glow with a color + intesity
 *     blink with a color
 *            
 *   driver:
 *     #s 9-5 always burn based on recency
 *     if no glowing / blinking, also #s 0-4
 *     if blinkers, 0-4 will blink with each color
 *     if glowers:
 *        breath(color)
 *        next color;
 *       
 */

// #define DEBUGGING
#undef DEBUGGING

#define LED_DATA   A4
#define LED_CLOCK  A5


#define FRAMES_PER_SECOND 60
#define NUM_LEDS 10
CRGB leds[NUM_LEDS];
CRGBPalette16 palette = HeatColors_p;


String CRGBstring(const CRGB color) {
    return String::format("0x%02x%02x%02x", color.raw[0], color.raw[1], color.raw[2]);
} // String CRGBstring(CRGB color)


#define BUFSIZE 5
template <class T> 
class CircularBuffer {
    private:
        time_t _expiry;
        time_t _expirations[BUFSIZE];
        T _data[BUFSIZE];
        uint8_t _index;

    
        // returns an index, or -1
        int8_t _search(const T datum) {
            for (uint8_t i = 0; i < BUFSIZE; i++) {
                if (_data[i] == datum) {
                    return i;
                } 
            }
            return -1;
        } // int contains(datum)


    public:
        CircularBuffer() {
            _index = 0;
            _expiry = 10; // seconds
            memset((void *)_expirations, 0, sizeof(_expirations));
        }
        
        void setExpiry(time_t expiry) {
            _expiry = expiry;
        } // constructor


        // add a thing to the queue; if it exists, just update the expiry
        bool enqueue(const T datum) {
            Serial.printf("CB enqueueing %s for %d seconds\n", CRGBstring((CRGB)datum).c_str(), _expiry);
            int index = -1;
            if ((index = _search(datum)) != -1) {
                _expirations[index] = Time.now() + _expiry;
            } else {
                for (uint8_t i = 0; i < BUFSIZE; i++) {
                    if (_expirations[i] < Time.now()) {
                        _data[i] = datum;
                        _expirations[i] = Time.now() + _expiry;
                        return true;
                    }
                }
            }
            return false;
        } // enqueue(data);


        bool next(T &data) {
            for (uint8_t i = 0; i < BUFSIZE; i++) {
                _index = (_index + 1) % BUFSIZE;
                if (_expirations[_index] >= Time.now()) {
                    data = _data[_index];
                    Serial.printf("returning %d: %s\n", _index, CRGBstring(data).c_str());
                    return true;
                }
            }
            return false;
        }
        
        
        void print() {
            Serial.printf("CircularBuffer:\n");
            for (uint8_t i = 0; i < BUFSIZE; i++) {
                Serial.printf("\tdata[%d]: %s, %ds\n", i, CRGBstring(_data[i]).c_str(), 
                                _expirations[i] ? _expirations[i] - Time.now() : 0);
            }
        }
}; // CircularBuffer


/*
 * Breather:
 *   isActive() if colors are still avialble to be breathed
 *   color() returns a weighted color value
 *   will cycle through available colors (stored in a CircularBuffer)
 *   will always start low and breath all the way in, out (brightest, dimmest) 
 *      before cycling or expiring
 *
 * Usage:
 *   start(color):
 *     enqueues a color, implicitly adding to the rotation.
 *   color(): returns a weighted color value
 *   isActive(): true if there are any colors in the queue
 */
#define BREATH_BPM 12 
class Breather {
    private:
        CircularBuffer<CRGB> _colors;// = CircularBuffer<CRGB>(3600);
        CRGB _currentColor;
        uint32_t _timebase;
        
    
    public:
        Breather() {
            _colors.setExpiry(stratus.getInt("glow duration", 3600));
        }
        
        
        void start(const CRGB color) {
            Serial.printf("Breather starting with %s\n", CRGBstring(color).c_str());
            _colors.enqueue(color);
            Serial.printf("I %s active\n", isActive() ? "am" : "am not");
        } // start(color)
        
        
        // TODO: use something other than isActive()
        // if not Active:
        //   pick a new color
        // if I have a color
        //   return currentColor scaled 
        CRGB color() {
            bool printing = false;
            EVERY_N_MILLIS(500) {
                printing = true;
                _colors.print();
            }
            if (printing) Serial.printf("Breather.color(%s):", CRGBstring(_currentColor).c_str());
            if (isActive()) {
                /*
                Serial.print(" inactive;");
                if (_colors.next(_currentColor)) {
                    if (printing) Serial.printf("got color %s", CRGBstring(_currentColor).c_str());
                    _timebase = Time.now();
                } else {
                    if (printing) Serial.print("no colors\n");
                    return CRGB::Black;
                }
            } else {
                if (printing) Serial.printf(" active, returning %s\n", CRGBstring(_currentColor).c_str());
            }
            */
                uint8_t beats = beatsin8(12, _timebase); // 0, 256, _timebase);
                if (printing) Serial.printf(" beats=%d;", beats);
                CRGB ret = _currentColor;
                ret.fadeToBlackBy(beats);
                if (printing) Serial.printf(" returning %s\n", CRGBstring(ret).c_str());
                return ret;
            } else {
                if (printing) Serial.println(" inactive; returning black");
                return CRGB::Black;
            }
        } // CRGB color()
        
        
        bool isActive() {
            if (Time.now() < _timebase + (60/BREATH_BPM)) {
                // true if we are currently breathing
                return true;
            } else if (_colors.next(_currentColor)) { 
                // true if we can retrieve a new color
                _timebase = Time.now();
                return true;
            } else {
                // else false
                return false;
            }
        } // bool isActive()
}; // class Breather



class Ember {
    private:
        uint8_t _position;    // my place in the strip
        uint8_t _colorIndex; 
        
    
        void printHeader() {
            Serial.printf("Ember[%d] ", _position);
        } // print()

        
    public:
        Ember() {} 
        
        void init(const uint8_t position) {
            _position = position;
            _colorIndex = 0;
        } // init(position, threshold)

         
        // algo:
        //   intensity dictates a range of possible colors from the pallet
        //      as well as the delta between cycles
        void burn(const uint8_t intensity, bool printing=false) {
                #define MINCOLOR 100
                uint8_t maxColor = map(min(intensity,100), 0, 100, MINCOLOR, 255);
                maxColor = constrain(maxColor, MINCOLOR, 255);
                int8_t range = constrain(intensity, 5, 50);
                int8_t delta = range/2 - random8(range);
                if (printing) {
                    Serial.printf("ember[%d]:[max=%d; range=%d; delta=%d; index=%d]", 
                                    _position, maxColor, range, delta, _colorIndex);
                }
                _colorIndex = constrain(_colorIndex + delta, MINCOLOR, maxColor);
                uint8_t colorIndex = random8(MINCOLOR, maxColor);
                if (printing) {
                    Serial.printf(": intensity=%d, index=%d\n", intensity, _colorIndex);
                }
                leds[_position] = ColorFromPalette(palette, _colorIndex);
                leds[_position].subtractFromRGB(random8(50));
        } // burn(recency, intensity)


        // simple on/off for 0.5s each
        void blink(const CRGB color, bool printing=false) {
            if (printing)
                Serial.printf("ember[%d](blink): %s\n", _position, CRGBstring(color).c_str());
            if (millis()/500 % 2) {
                leds[_position] = color;
            } else {
                leds[_position] = CRGB::Black;
            }
        } // blink(color)
        
        
        // just do what they tell me
        void breathe(const CRGB color, bool printing=false) {
            if (printing) {
                Serial.printf("ember[%d](breathe):  %s\n", _position, CRGBstring(color).c_str());
            }
            leds[_position] = color;
            // leds[_position] = CRGB::Green;
        } // breathe(color)
        
        
        void off(bool printing=false) {
            if (printing) 
                Serial.printf("ember[%d]: off\n", _position);
            leds[_position] = CRGB::Black;
        } // off()
        
}; // class Ember


Ember embers[NUM_LEDS];
Breather breather;
time_t blinkTimer;
CRGB blinkColor;


CRGB colorize(String colorString) {
    Serial.printf("colorize(%s)\n", colorString.c_str());
    if (colorString.equals("blue")) {
        return CRGB::Blue;
    } else if (colorString.equals("green")) {
        Serial.printf("it's green; returning %#06x\n", CRGB::Green);
        return 0x00ff00;
        return CRGB::Green;
    } else if (colorString.equals("red")) {
        return CRGB::Red;
    } else {
        Serial.println("nfi");
        return CRGB::White;
    }
} // colorize(colorString)


void createEmbers() {
    for (uint8_t i = 0; i < NUM_LEDS; i++) {
        #ifdef DEBUGGING
            Serial.print("i:");
            Serial.println(i);
        #endif
        embers[i].init(i);
    }
} // createEmbers()





#define MIN_BRIGHTNESS 16
#define MAX_BRIGHTNESS 96


// recency, intensity will vary 0..100
void burn(const uint8_t recency, const uint8_t intensity) {
    bool printing = false;
    #ifdef DEBUGGING
        EVERY_N_SECONDS(1) {
            Serial.printf("burn(%d, %d)\n", recency, intensity);
            printing = true;
        }
    #endif
    for (uint8_t i = 0; i < NUM_LEDS; i++) {
        uint8_t threshold = 90-(NUM_LEDS)*i;
        if (i < 4 && blinkTimer > Time.now()) {
            if (false && printing)
                Serial.printf("Ember %d: blinking\n", i);
            embers[i].blink(blinkColor, printing);
        } else if (i < 4 && breather.isActive()) {
            if (false && printing)
                Serial.printf("Ember %d: breathing\n", i);
            embers[i].breathe(breather.color(), printing);
            // embers[i].breathe(CRGB::Blue, printing);
        } else if (recency >= threshold and random8(100) < 85) {
            if (false && printing)
                Serial.printf("Ember %d: burning\n", i);
            embers[i].burn(intensity, printing);
        } else {
            if (false && printing)
                Serial.printf("Ember %d: off\n", i);
            embers[i].off(printing);
        }
    }
    // int brightness = map(intensity, 0, 100, MIN_BRIGHTNESS, MAX_BRIGHTNESS);
    // FastLED.setBrightness(brightness);
    if (printing) {
        Serial.printf("leds[0] = %s\n", CRGBstring(leds[0]).c_str());
    }
    FastLED.show();
    FastLED.delay(1000/FRAMES_PER_SECOND); 
} // burn(recency, intensity)


void glow(uint32_t color) {
    breather.start(color);
} // glow(colorString)


int glowHandler(String data) {
    glow(colorize(data));
    return 1;
} // int glowHandler(data)


void blink(uint32_t color) {
    blinkColor = color;
    blinkTimer = Time.now() + 5; 
} // blink(colorString)


int blinkHandler(String data) {
    blink(colorize(data));
    return 1;
}


void setupLEDs() {
    FastLED.addLeds<APA102, LED_DATA, LED_CLOCK>(leds, NUM_LEDS);
    FastLED.setBrightness(32);
    FastLED.clear();
    FastLED.show();
    
    palette = CRGBPalette16(CRGB::Black, CRGB::Red, CRGB::Orange, CRGB::Yellow);

    createEmbers();
    Particle.function("glow", glowHandler);
    Particle.function("blink", blinkHandler);
} // setupLEDs()

#endif
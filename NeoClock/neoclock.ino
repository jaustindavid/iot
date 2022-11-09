#include <particle-dst.h>
DST dst;

dst_limit_t beginning;
dst_limit_t end;

#include <SimpleTimer.h>
#include <neopixel.h>

#include "WobblyTime.h"

// IMPORTANT: Set pixel COUNT, PIN and TYPE
#define PIXEL_PIN D0
#define PIXEL_COUNT 144
#define PIXEL_TYPE WS2812B

// #define DEBUG_LED

Adafruit_NeoPixel ring(PIXEL_COUNT, PIXEL_PIN, PIXEL_TYPE);

typedef uint32_t color;
#define RED         (ring.Color(255, 0, 0))
#define LIGHTRED    (ring.Color(128, 0, 0))
#define GREEN       (ring.Color(0, 255, 0))
#define LIGHTGREEN  (ring.Color(0, 128, 0))
#define BLUE        (ring.Color(0, 0, 255))
#define BLACK       (ring.Color(0, 0, 0))
#define DARKGREY    (ring.Color(8, 8, 8))
#define LIGHTGREY   (ring.Color(32, 32, 32))
#define DARKWHITE   (ring.Color(64, 64, 64))
#define WHITE       (ring.Color(255, 255, 255))

#define HOUR_COLOR   DARKWHITE
#define MINUTE_COLOR BLUE

#define ROTATION 72 // the ring is "rotated" this many pixels CW
#define TXLATE(M) ((M+ROTATION) % PIXEL_COUNT)

WobblyTime wTime;


void setup() {
    Serial.begin(115200);
    Particle.syncTime();
    Time.zone(-5);

    beginning.hour = 2;
    //beginning.day = 3; // tuesday
    beginning.day = DST::days::tue;
    //beginning.month = 2; // february
    beginning.month = DST::months::feb;
    beginning.occurrence = 2;
        
    end.hour = 3;
    //end.day = 4; // wednesday
    end.day = DST::days::sun;
    //end.month = 10; // october
    end.month = DST::months::nov;
    end.occurrence = 1;
        
    dst.begin(beginning, end, 1);
    dst.automatic(true);
    dst.check();

    ring.begin();
    ring.setBrightness(64);
    for (int i = 0; i < ring.numPixels(); i++) {
        ring.setPixelColor(i, BLACK);
    }
    ring.show(); // Initialize all pixels to 'off'
} // setup()


SimpleTimer every50(50);
SimpleTimer second(1000);

time_t next_timecheck = 24*60*60;
void loop() {
    if (next_timecheck < millis()) {
        next_timecheck = millis() + 24*60*60;
        Particle.syncTime();
        dst.check();
    }
    // testDigits();

    fadeAll(1);
    // for (int i = 0; i < ring.numPixels(); i++) {
    //    ring.setPixelColor(i, BLACK);
    // }
  
    showHour();
    showQuarters();
    showMinute();
    
    ring.show();
    
    every50.wait();    
} // loop()



byte fade(byte c) {
    return constrain(c - 8, 0, 255);
} // byte fade(byte)


void fadeAll(int f) {
    for (int j = 0; j < f; j++) {
        for (int i = 0; i < ring.numPixels(); i++) {
            long c = ring.getPixelColor(i);
            byte r = fade(c >> 16 & 255);
            byte g = fade(c >> 8 & 255);
            byte b = fade(c & 255);
            ring.setPixelColor(i, r, g, b);
        }
    }
} // fadeAll(int)


void showQuarters() {
    int i;
    
    for (i = 1; i < 12; i++) {
        if (i%3 == 0) {
            ring.setPixelColor(TXLATE(i*ring.numPixels()/12 - 1), LIGHTGREY);
            ring.setPixelColor(TXLATE(i*ring.numPixels()/12), LIGHTGREY);            
        } else {
            // ring.setPixelColor(TXLATE(i*ring.numPixels()/12), LIGHTGREY);
        }
    }
    
    // 12
    ring.setPixelColor(TXLATE(ring.numPixels()-2), LIGHTGREY);
    ring.setPixelColor(TXLATE(ring.numPixels()-1), LIGHTGREY);
    ring.setPixelColor(TXLATE(0), LIGHTGREY);
    ring.setPixelColor(TXLATE(1), LIGHTGREY);
    
} // showQuarters()


// returns the first pixel (not TXLATE'd) for the current hour
int hourStart() {
    int h = wTime.hour() % 12;
    int start = ring.numPixels()/12 * h - ring.numPixels()/24;
    return start;    
} // int hourStart()


// illuminate the entire "hour" of pixels
void showHour() {
    int start = hourStart();
    int end = start + ring.numPixels()/12;
    
    for (int i = start + 1; i < end - 1; i++) {
        ring.setPixelColor(TXLATE(i), HOUR_COLOR);
    }
} // showHour()


// make a specified pixel "breathe"
// odd seconds inhales, even exhales
void breathe(int pixel, uint32_t c) {
    int t = millis() % 1000;
    if (millis() / 1000 % 2 == 0) {
        t = 1000 - t;  
    }
    ring.setPixelColor(pixel, c*t/1000);
} // breathe(int, uint32_t)


int getMinute() {
    int h = wTime.hour() % 12;
    int m = wTime.minute();
    int s = wTime.second();
    
    // absolute minute
    float target = ring.numPixels() * (m * 60 + s)/ 3600;
    
    // relative minute
    // int target = hourStart() + (m * ring.numPixels()/12 / 60);
    return int(target);    
} // int getMinute()


void showMinute() {
    static int last_pixel = 0;
    int pixel = getMinute();
    
    if (pixel != last_pixel && Time.second() == 0) {
        animateMinute();
        last_pixel = pixel;
    }
    
    // breathe(TXLATE(pixel-1), MINUTE_COLOR);
    breathe(TXLATE(pixel), MINUTE_COLOR);
} // showMinute()


// do a full loop from now-1 
// spend ~ 1s doing it
void animateMinute() {
    int animationDelay = 1000 / ring.numPixels();
    int pixel = getMinute() - 1;
    int i;
    
    for (i = 0; i < ring.numPixels()+1; i++) {
        fadeAll(1);
        showHour();
        showQuarters();
        ring.setPixelColor(TXLATE(pixel + i), MINUTE_COLOR);
        ring.show();
        delay(animationDelay);
    }
} // animateMinute()



#define BOUNCE 12 // number of pixels around which the time will bounce
/*
void showTime() {
    static int direction = 1;
    static int posn = -BOUNCE/2;
    int h = Time.hour() % 12;
    int m = Time.minute();
    int s = Time.second();
    
    float tod = (h*60+m)*60 + s;
    float tod_pct = tod/(12*60*60);
    int target = ring.numPixels() * tod_pct;
    
    if (false) {
        int pixel = target+posn;
        ring.setPixelColor(TXLATE(pixel), WHITE);
        if (direction > 0) {
            posn++;
            if (posn >= BOUNCE/2) {
                posn = BOUNCE/2-1;
                direction = -1;
            }
        } else {
            --posn;
            if (posn < -BOUNCE/2) {
                posn = -BOUNCE/2;
                direction = 1;
            }
        }
    }
    
    if (millis() % 1000 < 100) {
        ring.setPixelColor(TXLATE(target), GREEN);
    }
} // showTime()


void maybeShowTime() {
    int h = Time.hour() % 12;
    int hp = (map(h, 0, 11, 0, 23) + 12) % 24;
    int m = Time.minute();
    if (m < 30) { hp--; }
    int mp = (map(m, 0, 59, 0, 23) + 12) % 24;
    int s = Time.second();
    int sp = (map(s, 0, 59, 0, 23) + 12) % 24;    
    Serial.printf("%02d:%02d:%02d -> %d %d %d\n", h, m, s, hp, mp, sp);
    
    ring.setPixelColor(sp, GREEN);
    showQuarters();
    ring.setPixelColor(mp, LIGHTRED);
    ring.setPixelColor(hp, BLUE);
    if (millis() % 1000 > 500) {
        ring.setPixelColor(sp, LIGHTGREEN);
    }
    ring.show();
}
*/
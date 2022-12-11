// This #include statement was automatically added by the Particle IDE.
#include <elapsedMillis.h>

#include <neopixel.h>
#include <SimpleTimer.h>


// IMPORTANT: Set pixel COUNT, PIN and TYPE
#define PIXEL_PIN D0
#define PIXEL_COUNT 8*32
#define PIXEL_TYPE WS2812B

// #define DEBUG_LED

Adafruit_NeoPixel display(PIXEL_COUNT, PIXEL_PIN, PIXEL_TYPE);

#include "matrix.h"
#include "TinyQueue.h"
#include "turtle.h"
#include "WobblyTime.h"
#include "digits.h"

#include <particle-dst.h>
DST dst;

dst_limit_t beginning;
dst_limit_t end;

color bg[MATRIX_X][MATRIX_Y];

SimpleTimer mspf(200);    // ms per frame
SimpleTimer second(1000);


byte lastMinute = 0; 
byte lastHour = 0;
byte digits[4];
int forcedH = -1;
int forcedM = -1;


Matrix matrix(&display);
Turtle turtle(&matrix, 25);
// Turtle secondHand(&matrix);
WobblyTime wTime(30, 180);
Digits digs(&turtle);


// void catchup();
void recolor(byte position, color c);
void markDirty(byte h, byte m);
bool maybeUpdateTime(byte h, byte m);

int platency = 100;
int ploss = 0;
// void ping();


// sets a time hh:mm; for debugging,
// the time will get displayed then immediately corrected
int setTime(String hhmm) {
    if (hhmm.length() == 5) {
        String hh = hhmm.substring(0, 2);
        forcedH = hh.toInt();
        String mm = hhmm.substring(3, 5);
        forcedM = mm.toInt();
        return forcedH * 100 + forcedM;
    } else {
        return -1;
    }
} // int setTime(hhmm)


// returns tenths-of-hours running
int uptime(String j) {
    return millis() / (360*1000);
} // int uptime(junque)


void setup() {
    Serial.begin(115200);
    display.begin();
    display.setBrightness(64); // redundant
    setupPtrans();
    setBrightness("");
    Particle.function("brightness", setBrightness);
    for (int i = 0; i < display.numPixels(); i++) {
        display.setPixelColor(i, BLACK);
    }
    display.show(); // Initialize all pixels to 'off'

    Particle.syncTime();
    waitUntil(Particle.syncTimeDone);

    Time.zone(-5);

    beginning.hour = 2;
    beginning.day = DST::days::sun;
    beginning.month = DST::months::feb;
    beginning.occurrence = 2;
        
    end.hour = 2;
    end.day = DST::days::sun;
    end.month = DST::months::nov;
    end.occurrence = 1;
        
    dst.begin(beginning, end, 1);
    dst.automatic(true);
    dst.check();
    
    ping();

    wTime.setup();

    lastMinute = wTime.minute(); 
    lastHour = wTime.hour();
    digits[0] = lastHour/10;
    digits[1] = lastHour%10;
    digs.draw(0, digits[0]);
    digs.draw(1, digits[1]);
    digs.drawColon();
    digits[2] = lastMinute/10;
    digits[3] = lastMinute%10;
    digs.draw(2, digits[2]);
    digs.draw(3, digits[3]);
    Particle.function("setTime", setTime);
    Particle.function("uptime", uptime);
    Particle.variable("packet latency", platency);
    Particle.variable("packet loss", ploss);
} // setup()


SimpleTimer day(24*60*60*1000);
SimpleTimer tenSec(5*1000);
elapsedMillis pingTimer = 0;
int nextPing = 5000;

void loop() {
    if (day.isExpired()) {
        if (Particle.connected()) {
            Particle.syncTime();
            dst.check();
        }
    }
    
    byte h = wTime.hour();
    byte m = wTime.minute();
    byte s = wTime.second();
    
    /*
    if (digs.count() > 1) {
        catchup();
    } else 
    */
    
    if (digs.isBusy()) {
        digs.go();
    } else {
        turtle.walkTo(MATRIX_X-1, s/MATRIX_Y, TRANSPARENT, Turtle::fast);
        if (forcedH != -1) {
            h = forcedH;
            m = forcedM;
            forcedH = forcedM = -1;
        }

        maybeUpdateTime(h, m);
    }

    matrix.fadeSome(0, 0, 1, MATRIX_Y-1, 2);

    if (pingTimer > nextPing) {
        int duration = ping();
        if (duration < 100) {
            nextPing = 5000;
            if (! Particle.connected()) { Particle.connect(); }
        } else if (duration < 500) {
            nextPing = 10000;
            if (! Particle.connected()) { Particle.connect(); }
        } else {
            nextPing = 15000;
            if (Particle.connected()) { Particle.disconnect(); }
        }
        pingTimer = 0;
    }
    setBrightness("");
    matrix.show();
} // loop()


int luna = 0;
void setupPtrans() {
    #define PTRANS_5V    A0
    #define PTRANS_SENSE A1
    #define PTRANS_GND   A2
    pinMode(PTRANS_5V, OUTPUT);
    pinMode(PTRANS_SENSE, INPUT);
    pinMode(PTRANS_GND, OUTPUT);
    
    digitalWrite(PTRANS_GND, LOW);
    digitalWrite(PTRANS_5V, HIGH);
    Particle.variable("Luna", luna);
} // setupPtrans();


float ma(int n, float avg, float value) {
    return (avg * (n-1)/n) + value/n;
} // float ma(n, avg, value)


// callable function; if input is an int, it will update the table
// note that the table is overwritten on restart.
// "luna" is a raw value from a phototransistor; the highest value greater
// than luna corresponds to the actual brightness used.
// 0 is a stop value, luna will always be greater than this.
int setBrightness(String input) {
    static int lastBright = 0;
    static SimpleTimer hysteresis(15*1000); // 15-second hysteresis
    
    int tune = input.toInt();
    static byte brights[6][2] = {{100, 64}, {75, 48}, {30, 32}, {13, 16}, {8, 8}, {0, 4}};
    luna = analogRead(A1);
    Serial.printf("Raw luna: %d\n", luna);
    byte i = 0;
    while (luna < brights[i][0]) {
        i++;
    }
    if (tune > 0 && tune < 255) {
        brights[i][1] = tune;
        Serial.printf("Updating threshold @ %d -> %d", brights[i][0], tune);
        display.setBrightness(brights[i][1]);
    } else if (lastBright != i && hysteresis.isExpired()) {
        Serial.printf("i=%d; luna >= %d, brightness->%d\n", i, brights[i][0], brights[i][1]);
        display.setBrightness(brights[i][1]);
        lastBright = i;
        hysteresis.reset();
    }
    return brights[i][1];
} // int setBrightness(input)


// the X position of digits, and the colon
byte positions[5] = { 3, 9, 18, 24, 15 };
void recolor(byte position, color c) {
    for (byte x = positions[position]; x <= positions[position] + 5; x++) {
        for (byte y = 0; y < MATRIX_Y; y++) {
            if (matrix.getPixel(x, y) == BLUE) {
                matrix.setPixel(x, y, c);
            }
        }
    }
} // recolor(position, color)


/*
// marks some digits "dirty" (paints them DARKWHITE instantly)
void mark_Dirty(byte h, byte m) {
    md2(h,m);
    return;
    Serial.printf("marking dirty; %02d:%02d vs. digits=[%d %d : %d %d]\n", h, m, digits[0], digits[1], digits[2], digits[3]);
    if (h/10 != digits[0]) {
        recolor(0, DARKWHITE);
    }
    
    if (h%10 != digits[1]) {
        recolor(1, DARKWHITE);
    }

    if (m/10 != digits[2]) {
        recolor(2, DARKWHITE);
    }
        
    if (m%10 != digits[3]) {
        recolor(3, DARKWHITE);
    }
} // markDirty(h, m)


void md2(byte h, byte m) {
     if (m%10 != digits[3]) {
        recolor(3, DARKWHITE);
        digs.erase(3);
    }
    
    if (m/10 != digits[2]) {
        recolor(2, DARKWHITE);
        digs.erase(2);
    }
     
    if (h%10 != digits[1]) {
        recolor(1, DARKWHITE);
        digs.erase(1);
    }

    if (h/10 != digits[0]) {
        recolor(0, DARKWHITE);
        digs.erase(0);
    }
}
*/


// true if h:m is MORE than 1 minute delayed
// assumes h:m came from wTime, and wTime always increases
bool bigTimeDifference(byte h, byte m) {
    // 9:32 <> 9:30; 9::59 <> 9::58; 9:01 <> 9:00
    if (wTime.hour() == h) {
        return (wTime.minute() - m) > 1;
    } 
    
    // 9:00 <> 8:59 -> 60 <> 59; 9:01 <> 8:59 -> 61 <> 59
    byte wTm = wTime.minute() + 60;
    return wTm > (m + 1);
} // bool bigTimeDifference(h,m)


/*
void catchup() {
    SimpleTimer halfS(500);
    while (digs.isBusy()) {
        digs.go();
        matrix.show();
        halfS.wait();
    }
}
*/

// given an hour and minute, possibly change what's on the clock
bool maybeUpdateTime(byte h, byte m) {
    if (m != lastMinute) {
        if (m%10 != digits[3]) {
            digs.erase(3);
        }
        if (m/10 != digits[2]) {
            digs.erase(2);
        }
        if (h%10 != digits[1]) {
            digs.erase(1);
        }
        if (h/10 != digits[0]) {
            digs.erase(0);
            digits[0] = h/10;
            digs.draw(0, digits[0]);
        }
        if (h%10 != digits[1]) {
            digits[1] = h%10;
            digs.draw(1, digits[1]);
        }
        if (m/10 != digits[2]) {
            digits[2] = m/10;
            digs.draw(2, digits[2]);
        }
        if (m%10 != digits[3]) {
            digits[3] = m%10;
            digs.draw(3, digits[3]);
        }

        lastMinute = m;
        return true;
    }
    return false;
} // bool maybeUpdateTime(h, m)

/*
// given an hour and minute, possibly change what's on the clock
bool maybentUpdateTime(byte h, byte m) {
    return maybeUpdateTime(h,m);

    if (m != lastMinute) {
        markDirty(h, m);
        if (h/10 != digits[0]) {
            digs.erase(0);
            digits[0] = h/10;
            digs.draw(0, digits[0]);
        }
        
        if (h%10 != digits[1]) {
            digs.erase(1);
            digits[1] = h%10;
            digs.draw(1, digits[1]);
        }

        // drawColon();
        // catchup();
        
        if (m/10 != digits[2]) {
            digs.erase(2);
            digits[2] = m/10;
            digs.draw(2, digits[2]);
        }
        
        if (m%10 != digits[3]) {
            digs.erase(3);
            digits[3] = m%10;
            digs.draw(3, digits[3]);
        }
        
        lastMinute = m;
        return true;
    }
    return false;
} // bool maybeUpdateTime(h, m)
*/

#define N 30
/*
#define UGLY 100
// returns the rolling avg of the last N things
// if the new value is UGLY it's over-weighted by (N/3)x
// this means high values will take a long time to "drain" out
int rollingAvg(int value) {
    static int sum = UGLY*N;     // preload
    // Serial.printf("rollingAvg: sum: %d value: %d; ", sum, value);
//     if (value >= UGLY) {
//        sum += 10*value;
//    } else {
        sum += value;
//    }
    int avg = sum / N;
    sum -= avg;
    // Serial.printf("Sum: %d, avg: %d\n", sum, avg);
    return avg;
} // int rollingAvg(value)
*/

// each ping() call does one timed ping, and updates counters for ploss and platency
// last duration is returned
int ping() {
    // static byte strength = 0;
    static IPAddress innernet(8,8,8,8);
    color c = display.getPixelColor(7);
    display.setPixelColor(7, WHITE);
    display.show();
    unsigned long start = millis();
    byte n = WiFi.ping(innernet, 1);
    int duration = (millis() - start);
    display.setPixelColor(7, c);
    display.show();
    
    // platency = rollingAvg(min(duration, 500));
    platency = ma(N, platency, min(duration,500));
    // String s = String::format("%d replies, now %d ms, avg %d ms", n, duration, platency);
    // Serial.println(s);
    if (n) {
        // no less than 0
        ploss = max(0, ploss -1);
    } else {
        // no more than 5
        ploss = min(5, ploss + 1);
    }
    
    color status;
    if (n == 0 || ploss > 3) {
        // this ping is a missed packet, or > 3 recently lost
        status = RED;
    } else if (ploss == 0) {
        // no loss
        status = GREEN;
    } else {
        // previous loss (<= 3) but not this one
        status = YELLOW;
    }

    for (byte y = 0; y < MATRIX_Y - 1; y++) {
        matrix.setPixel(0, y, BLACK);
        matrix.setPixel(1, y, BLACK);
    }
    int strength = constrain(map(platency, 30, 150, 0, MATRIX_Y-1), 0, MATRIX_Y-1);
    int currStrength = constrain(map(duration, 30, 150, 0, MATRIX_Y-1), 0, MATRIX_Y-1);
    Serial.printf("platency %d -> %d; ", platency, strength);
    Serial.printf("duration %d -> %d\n", duration, currStrength);
    
    for (byte y = currStrength; y < MATRIX_Y; y++) {
        matrix.setPixel(0, y, status);
    }
    for (byte y = strength; y < MATRIX_Y; y++) {
        matrix.setPixel(1, y, status);
    }
    
    return duration;
} // ping()

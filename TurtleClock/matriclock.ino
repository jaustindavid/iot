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
WobblyTime wTime;
Digits digs(&turtle);


void catchup();
void recolor(byte position, color c);
void markDirty(byte h, byte m);
bool maybeUpdateTime(byte h, byte m);

int platency = 100;
int ploss = 0;
// void ping();


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
}

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
SimpleTimer oneSec(1000);
SimpleTimer tenSec(10*1000);

void loop() {
    if (day.isExpired()) {
        Particle.syncTime();
        dst.check();
    }
    
    byte h = wTime.hour();
    byte m = wTime.minute();
    byte s = wTime.second();
    
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
    if (tenSec.isExpired()) {
        setBrightness("");
        ping();
    }
    matrix.show();
    // mspf.wait();    // NOP; fps is actually dictated by the matrix, with xfader
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


int setBrightness(String input) {
    int tune = input.toInt();
    static byte brights[6][2] = {{100, 64}, {75, 48}, {30, 32}, {13, 16}, {10, 8}, {0, 4}};
    luna = analogRead(A1);
    Serial.printf("Raw luna: %d\n", luna);
    byte i = 0;
    while (luna < brights[i][0]) {
        i++;
    }
    if (tune > 0 && tune < 255) {
        brights[i][1] = tune;
        Serial.printf("Updating threshold @ %d -> %d", brights[i][0], tune);
    }
    Serial.printf("i=%d; luna >= %d, brightness->%d\n", i, brights[i][0], brights[i][1]);
    display.setBrightness(brights[i][1]);
    return brights[i][1];
} // int setBrightness(input)


/*
void catchup() {
    while (turtle.isBusy()) { 
        turtle.go(); 
        // secondHand.go();
        matrix.show(); 
        mspf.wait(); 
    }
}
*/

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


void markDirty(byte h, byte m) {
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


bool maybeUpdateTime(byte h, byte m) {
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


#define N 30
#define UGLY 100
// returns the rolling avg of the last N things
// if the new value is UGLY it's over-weighted by (N/3)x
// this means high values will take a long time to "drain" out
int rollingAvg(int value) {
    static int sum = UGLY*N;     // preload
    // Serial.printf("rollingAvg: sum: %d value: %d; ", sum, value);
    if (value >= UGLY) {
        sum += 10*value;
    } else {
        sum += value;
    }
    int avg = sum / N;
    sum -= avg;
    // Serial.printf("Sum: %d, avg: %d\n", sum, avg);
    return avg;
} // int rollingAvg(value)


// TODO: multiping
// each ping() call does one timed ping, and updates counters for ploss and platency
void ping() {
    // static byte strength = 0;
    IPAddress innernet(8,8,8,8);
    color c = display.getPixelColor(7);
    display.setPixelColor(7, WHITE);
    display.show();
    unsigned long start = millis();
    byte n = WiFi.ping(innernet, 1);
    int duration = (millis() - start);
    display.setPixelColor(7, c);
    display.show();
    
    platency = rollingAvg(min(duration, 500));
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
    int strength = map(platency, 30, 150, 0, 6);
    int currStrength = map(duration, 30, 150, 0, 6);
    Serial.printf("platency %d -> %d; ", platency, strength);
    Serial.printf("duration %d -> %d\n", duration, currStrength);
    
    for (byte y = currStrength; y < MATRIX_Y; y++) {
        matrix.setPixel(0, y, status);
    }
    for (byte y = strength; y < MATRIX_Y; y++) {
        matrix.setPixel(1, y, status);
    }
} // ping()

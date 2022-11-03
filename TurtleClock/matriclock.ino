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

SimpleTimer mspf(100);    // ms per frame
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
//void drawDigit(byte digit, byte position, color c, byte speed);
void drawColon(byte speed);
/*
void animate0(Turtle *turtle, byte startX, color c, byte speed);
void animate1(Turtle *turtle, byte startX, color c, byte speed);
void animate2(Turtle *turtle, byte startX, color c, byte speed);
void animate3(Turtle *turtle, byte startX, color c, byte speed);
void animate4(Turtle *turtle, byte startX, color c, byte speed);
void animate5(Turtle *turtle, byte startX, color c, byte speed);
void animate6(Turtle *turtle, byte startX, color c, byte speed);
void animate7(Turtle *turtle, byte startX, color c, byte speed);
void animate8(Turtle *turtle, byte startX, color c, byte speed);
void animate9(Turtle *turtle, byte startX, color c, byte speed);
*/
void ping();

int brightCallback(String parameter) {
    int b = parameter.toInt();
    if (b) {
        display.setBrightness(b);
        return 0;
    }
    return -1;
} // int brightness(parameter)


int setTime(String hhmm) {
    String hh = hhmm.substring(0, 2);
    forcedH = hh.toInt();
    String mm = hhmm.substring(3, 5);
    forcedM = mm.toInt();
    return 1;
} // int setTime(hhmm)


// returns tenths-of-hours running
int uptime(String j) {
    return millis() / (360*1000);
}

void setup() {
    Serial.begin(115200);
    display.begin();
    display.setBrightness(64);
    setupPtrans();
    setBrightness();
    Particle.function("brightness", brightCallback);
    for (int i = 0; i < display.numPixels(); i++) {
        display.setPixelColor(i, BLACK);
    }
    display.show(); // Initialize all pixels to 'off'

    Time.zone(-5);

    beginning.hour = 2;
    //beginning.day = 3; // tuesday
    beginning.day = DST::days::tue;
    //beginning.month = 2; // february
    beginning.month = DST::months::feb;
    beginning.occurrence = 2;
        
    end.hour = 2;
    //end.day = 4; // wednesday
    end.day = DST::days::sun;
    //end.month = 10; // october
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
    // drawDigit(digits[2], 2, BLUE, FAST);
    digs.draw(2, digits[2]);
    // drawDigit(digits[3], 3, BLUE, FAST);
    digs.draw(3, digits[3]);
    Particle.function("setTime", setTime);
    Particle.function("uptime", uptime);
} // setup()


SimpleTimer day(24*60*60*1000);
SimpleTimer fiver(5*1000);
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
        turtle.walkTo(MATRIX_X-1, s/MATRIX_Y, TRANSPARENT, FAST);
        if (forcedH != -1) {
            h = forcedH;
            m = forcedM;
            forcedH = forcedM = -1;
        }

        maybeUpdateTime(h, m);
    }
    matrix.show();
    mspf.wait();
    if (tenSec.isExpired()) {
        setBrightness();
        ping();
    }
} // loop()


void setupPtrans() {
#define PTRANS_5V    A0
#define PTRANS_SENSE A1
#define PTRANS_GND   A2
    pinMode(PTRANS_5V, OUTPUT);
    pinMode(PTRANS_SENSE, INPUT);
    pinMode(PTRANS_GND, OUTPUT);
    
    digitalWrite(PTRANS_GND, LOW);
    digitalWrite(PTRANS_5V, HIGH);
} // setupPtrans();


void setBrightness() {
    byte brights[6][2] = {{100, 64}, {75, 48}, {30, 32}, {15, 16}, {10, 8}, {0, 4}};
    int luna = analogRead(A1);
    Serial.printf("Raw luna: %d\n", luna);
    byte i = 0;
    while (luna < brights[i][0]) {
        i++;
    }
    Serial.printf("i=%d; luna >= %d, brightness->%d\n", i, brights[i][0], brights[i][1]);
    display.setBrightness(brights[i][1]);
} // setBrightness()


void catchup() {
    while (turtle.isBusy()) { 
        turtle.go(); 
        // secondHand.go();
        matrix.show(); 
        mspf.wait(); 
    }
}


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
        recolor(0, LIGHTGREY);
    }
    
    if (h%10 != digits[1]) {
        recolor(1, LIGHTGREY);
    }

    if (m/10 != digits[2]) {
        recolor(2, LIGHTGREY);
    }
        
    if (m%10 != digits[3]) {
        recolor(3, LIGHTGREY);
    }
} // markDirty(h, m)


bool maybeUpdateTime(byte h, byte m) {
    if (m != lastMinute) {
        markDirty(h, m);
        if (h/10 != digits[0]) {
            // Serial.printf("Hour: %d|x", h/10);
            // drawDigit(digits[0], 0, BLACK, FAST);
            digs.erase(0);
            digits[0] = h/10;
            // drawDigit(digits[0], 0, BLUE, SLOW);
            digs.draw(0, digits[0]);
        }
        
        if (h%10 != digits[1]) {
            // Serial.printf("Hour: x|%d", h%10);
            // drawDigit(digits[1], 1, BLACK, FAST);
            digs.erase(1);
            digits[1] = h%10;
            // drawDigit(digits[1], 1, BLUE, SLOW);
            digs.draw(1, digits[1]);
        }

        // drawColon();
        catchup();
        
        if (m/10 != digits[2]) {
            // Serial.printf("Minute: %d|x", m/10);
            // drawDigit(digits[2], 2, BLACK, FAST);
            digs.erase(2);
            digits[2] = m/10;
            // drawDigit(digits[2], 2, BLUE, SLOW);
            digs.draw(2, digits[2]);
        }
        
        if (m%10 != digits[3]) {
            // Serial.printf("Minute: x|%d", m%10);
            // drawDigit(digits[3], 3, BLACK, FAST);
            digs.erase(3);
            digits[3] = m%10;
            // drawDigit(digits[3], 3, BLUE, SLOW);
            digs.draw(3, digits[3]);
        }
        
        lastMinute = m;
        return true;
    }
    return false;
} // bool maybeUpdateTime(h, m)


void drawColon(byte speed) {
    turtle.walkTo(15, 1, TRANSPARENT, FAST);
    turtle.walk(1, 0, BLUE, speed);
    turtle.walk(0, 1, BLUE, speed);
    turtle.walk(-1, 0, BLUE, speed);
    
    turtle.walk(0, 2, TRANSPARENT, FAST);
    turtle.walk(1, 0, BLUE, speed);
    turtle.walk(0, 1, BLUE, speed);
    turtle.walk(-1, 0, BLUE, speed);
}


void ping() {
    static byte strength = 0;
    IPAddress innernet(8,8,8,8);
    unsigned long start = millis();
    byte n = WiFi.ping(innernet, 3);
    float duration = 1.0*(millis() - start)/3;
    String s = String::format("%d replies, avg %5.2f ms", n, duration);
    //Particle.publish("pung", s);
    Serial.println(s);
    color status;
    switch (n) {
        case 3: 
            status = GREEN;
            break;
        case 2:
            status = YELLOW;
            break;
        default:
            status = RED;
    }
    if (duration < 50) {
        strength = min(strength + 1, 8);
    } else if (duration < 100) {
        strength = max(strength - 1, 6);
    } else if (duration < 150) {
        strength = max(strength - 1, 3);
    } else {
        strength = 2;
    }
    for (byte y = 0; y < MATRIX_Y - strength; y++) {
        matrix.setPixel(0, y, BLACK);
        matrix.setPixel(1, y, BLACK);            
    }
    for (byte y = MATRIX_Y - strength; y < MATRIX_Y; y++) {
        matrix.setPixel(0, y, status);
        matrix.setPixel(1, y, status);    
    }
} // ping()

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
Turtle turtle(&matrix, 100);
// Turtle secondHand(&matrix);
WobblyTime wTime;


void catchup();
void recolor(byte position, color c);
void markDirty(byte h, byte m);
bool maybeUpdateTime(byte h, byte m);
void drawDigit(byte digit, byte position, color c, byte speed);
void drawColon(byte speed);
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
    display.setBrightness(32);
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

    wTime.setup();

    lastMinute = wTime.minute(); 
    lastHour = wTime.hour();
    digits[0] = lastHour/10;
    digits[1] = lastHour%10;
    drawDigit(digits[0], 0, BLUE, FAST);
    drawDigit(digits[1], 1, BLUE, FAST);
    drawColon(FAST);
    // catchup();
    digits[2] = lastMinute/10;
    digits[3] = lastMinute%10;
    drawDigit(digits[2], 2, BLUE, FAST);
    drawDigit(digits[3], 3, BLUE, FAST);
    Particle.function("setTime", setTime);
    Particle.function("uptime", uptime);
} // setup()


/* DEAD
void wander() {
    while (true) {
        if (random(100) < 25) {
            turtle.turn(random(100) > 50 ? -90 : 90);
        } else {
            turtle.step();
        }
        matrix.show();
        mspf.wait();
    }
}
*/


SimpleTimer day(24*60*60*1000);
void loop() {
    if (day.isExpired()) {
        Particle.syncTime();
        dst.check();
    }
    
    byte h = wTime.hour();
    byte m = wTime.minute();
    byte s = wTime.second();
    
    if (turtle.isBusy()) {
        turtle.go();
    } else {
        if (forcedH != -1) {
            h = forcedH;
            m = forcedM;
            forcedH = forcedM = -1;
        }

        maybeUpdateTime(h, m);

        if (!turtle.isBusy()) {
            turtle.walkTo(MATRIX_X-1, s/MATRIX_Y, TRANSPARENT, FAST);
        }
    }
    matrix.show();
    mspf.wait();
} // loop()


/* DEAD
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
            drawDigit(digits[0], 0, BLACK, FAST);
            digits[0] = h/10;
            drawDigit(digits[0], 0, BLUE, SLOW);
        }
        
        if (h%10 != digits[1]) {
            // Serial.printf("Hour: x|%d", h%10);
            drawDigit(digits[1], 1, BLACK, FAST);
            digits[1] = h%10;
            drawDigit(digits[1], 1, BLUE, SLOW);
        }

        // drawColon();
        // catchup();
        
        if (m/10 != digits[2]) {
            // Serial.printf("Minute: %d|x", m/10);
            drawDigit(digits[2], 2, BLACK, FAST);
            digits[2] = m/10;
            drawDigit(digits[2], 2, BLUE, SLOW);
        }
        
        if (m%10 != digits[3]) {
            // Serial.printf("Minute: x|%d", m%10);
            drawDigit(digits[3], 3, BLACK, FAST);
            digits[3] = m%10;
            drawDigit(digits[3], 3, BLUE, SLOW);
        }
        
        lastMinute = m;
        return true;
    }
    return false;
} // bool maybeUpdateTime(h, m)


void drawDigit(byte digit, byte position, color c, byte speed) {
    Serial.printf("drawing %d at %d\n", digit, position);
    byte start = positions[position];
    // Serial.printf("drawing %d @ %d\n", digit, position);
    switch (digit) {
        case 1:
            animate1(&turtle, start, c, speed);
            break;
        case 2:
            animate2(&turtle, start, c, speed);
            break;
        case 3:
            animate3(&turtle, start, c, speed);
            break;
        case 4:
            animate4(&turtle, start, c, speed);
            break;
        case 5:
            animate5(&turtle, start, c, speed);
            break;
        case 6:
            animate6(&turtle, start, c, speed);
            break;
        case 7:
            animate7(&turtle, start, c, speed);
            break;
        case 8:
            animate8(&turtle, start, c, speed);
            break;
        case 9:
            animate9(&turtle, start, c, speed);
            break;
        case 0:
        default:
            animate0(&turtle, start, c, speed);
    }
//    digits[position] = digit;
}


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


void animate0(Turtle *turtle, byte startX, color c, byte speed) {
    turtle->walkTo(startX + 1, 0, TRANSPARENT, FAST);
    turtle->walk(2, 0, c, speed);
    turtle->walk(1, 1, c, speed);
    turtle->walk(0, 5, c, speed);
    turtle->walk(-1, 1, c, speed);
    turtle->walk(-2, 0, c, speed);
    turtle->walk(-1, -1, c, speed);
    turtle->walk(0, -5, c, speed);
    turtle->walk(1, -1, c, speed);
    // while (turtle->isBusy()) { turtle->go(); matrix.show(); }
    // walk(2, 0, c);
}


void animate1(Turtle *turtle, byte startX, color c, byte speed) {
    turtle->walkTo(startX, 3, TRANSPARENT, FAST);
    
    turtle->walk(3, -3, c, speed);
    turtle->walk(0, 7, c, speed);
}


void animate2(Turtle *turtle, byte startX, color c, byte speed) {
    turtle->walkTo(startX, 1, TRANSPARENT, FAST);
    
    turtle->walk(1, -1, c, speed);
    turtle->walk(2, 0, c, speed);
    turtle->walk(1, 1, c, speed);
    turtle->walk(0, 1, c, speed);
    turtle->walk(-4, 4, c, speed);
    turtle->walk(0, 1, c, speed);
    turtle->walk(4, 0, c, speed);
    turtle->walk(0, -1, c, speed);
}


void animate3(Turtle *turtle, byte startX, color c, byte speed) {
    turtle->walkTo(startX, 1, TRANSPARENT, FAST);
    
    turtle->walk(0, -1, c, speed);
    turtle->walk(4, 0, c, speed);
    turtle->walk(0, 1, c, speed);
    turtle->walk(-3, 2, c, speed);
    turtle->walk(2, 0, c, speed);
    turtle->walk(1,1, c, speed);
    turtle->walk(0, 2, c, speed);
    turtle->walk(-1, 1, c, speed);
    turtle->walk(-2, 0, c, speed);
    turtle->walk(-1, -1, c, speed);
}


void animate4(Turtle *turtle, byte startX, color c, byte speed) {
    turtle->walkTo(startX+1, 0, TRANSPARENT, FAST);
    
    turtle->walk(-1, 3, c, speed);
    turtle->walk(4, 0, c, speed);
    
    turtle->walk(-1, -2, TRANSPARENT, FAST);
    turtle->walk(0,6, c, speed);
}


void animate5(Turtle *turtle, byte startX, color c, byte speed) {
    turtle->walkTo(startX + 4, 1, TRANSPARENT, FAST);
    
    turtle->walk(0, -1, c, speed);
    turtle->walk(-4, 0, c, speed);
    turtle->walk(0, 3, c, speed);
    turtle->walk(3, 0, c, speed);
    turtle->walk(1,1, c, speed);
    turtle->walk(0,2, c, speed);
    turtle->walk(-1, 1, c, speed);
    turtle->walk(-2, 0, c, speed);
    turtle->walk(-1, -1, c, speed);
}


void animate6(Turtle *turtle, byte startX, color c, byte speed) {
    turtle->walkTo(startX + 4, 1, TRANSPARENT, FAST);
    
    turtle->walk(-1, -1, c, speed);
    turtle->walk(-2, 0, c, speed);
    turtle->walk(-1, 1, c, speed);
    turtle->walk(0, 5, c, speed);
    turtle->walk(1, 1, c, speed);
    turtle->walk(2, 0, c, speed);
    turtle->walk(1, -1, c, speed);
    turtle->walk(0, -2, c, speed);
    turtle->walk(-1, -1, c, speed);
    turtle->walk(-1, 0, c, speed);
}


void animate7(Turtle *turtle, byte startX, color c, byte speed) {
    turtle->walkTo(startX, 1, TRANSPARENT, FAST);
    
    turtle->walk(0, -1, c, speed);
    turtle->walk(4, 0, c, speed);
    turtle->walk(0, 2, c, speed);
    turtle->walk(-2, 2, c, speed);
    turtle->walk(0, 3, c, speed);
}


void animate8(Turtle *turtle, byte startX, color c, byte speed) {
    turtle->walkTo(startX + 1, 0, TRANSPARENT, FAST);
    
    turtle->walk(2, 0, c, speed);
    turtle->walk(1, 1, c, speed);
    turtle->walk(0, 1, c, speed);
    turtle->walk(-1, 1, c, speed);
    turtle->walk(-2, 0, c, speed);
    turtle->walk(-1, 1, c, speed);
    turtle->walk(0, 2, c, speed);
    turtle->walk(1, 1, c, speed);
    turtle->walk(2, 0, c, speed);
    turtle->walk(1, -1, c, speed);
    turtle->walk(0, -2, c, speed);
    turtle->walk(-1, -1, c, speed);
    turtle->walk(-2, 0, c, speed);
    turtle->walk(-1, -1, c, speed);
    turtle->walk(0, -1, c, speed);
    turtle->walk(1, -1, c, speed);
    // walk(2, 0, c);
}


void animate9(Turtle *turtle, byte startX, color c, byte speed) {
    turtle->walkTo(startX + 4, 2, TRANSPARENT, FAST);
    
    turtle->walk(-1, 1, c, speed);
    turtle->walk(-2, 0, c, speed);
    turtle->walk(-1, -1, c, speed);
    turtle->walk(0, -1, c, speed);
    turtle->walk(1, -1, c, speed);
    turtle->walk(2, 0, c, speed);
    turtle->walk(1, 1, c, speed);
    turtle->walk(0, 5, c, speed);
    turtle->walk(-1, 1, c, speed);
    turtle->walk(-2, 0, c, speed);
    turtle->walk(-1, -1, c, speed);
}

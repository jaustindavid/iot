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

color bg[MATRIX_X][MATRIX_Y];
color feelie(float temperature);
color lookie(byte condition);

// SimpleTimer mspf(200);    // ms per frame
// SimpleTimer second(1000);

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


bool maybeUpdateTime(byte h, byte m);

int platency = 100;
int ploss = 0;


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

    setup_dst();
    ping();
    setup_forecast();

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


void setup_dst() {
    dst_limit_t beginning;
    dst_limit_t end;
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
} // setup_dst()


float forecast_temp[8];
byte forecast_cond[8];
time_t forecast_freshness = 0;
void setup_forecast() {
    for (int i = 0; i < 8; i++) {
        forecast_temp[i] = -99.0;
        forecast_cond[i] = 0;
    }
    
    Particle.subscribe("weather", weatherHandler);
    Particle.function("weather_freshness", freshmaker);
} // setup_forecast()


color feelie(float temperature) {
    if (temperature >= 35) {
        return COLOR_HOT;
    } else if (temperature >= 30) {
        return COLOR_WARM;
    } else if (temperature >= 25) {
        return COLOR_COOL;
    } else if (temperature >= 15) {
        return COLOR_CHILLY;
    } else if (temperature >= 10) {
        return COLOR_COLD;
    } else if (temperature >= 5) {
        return COLOR_FREEZING;
    } else {
        return COLOR_FROZE;
    }
    return BLUE;
}


color lookie(byte condition) {
    switch (condition) {
        case 1:
            return COLOR_SUNNY;
        case 2:
            return COLOR_CLOUDY;
        case 3:
            return COLOR_RAINY;
        default:
            return RED;
    }
}


void update_forecast() {
    if (Time.now() - forecast_freshness < 3*3600) {
        // flash
        for (int i = 0; i < 8; i++) {
            matrix.setPixel(MATRIX_X-2, i, WHITE);
            matrix.setPixel(MATRIX_X-1, i, WHITE);
        }        
        matrix.show();
        for (int i = 0; i < 8; i++) {
            matrix.setPixel(MATRIX_X-2, i, feelie(forecast_temp[i]));
            matrix.setPixel(MATRIX_X-1, i, lookie(forecast_cond[i]));
        }
        matrix.show();
    } else {
        // flash
        for (int i = 0; i < 8; i++) {
            matrix.setPixel(MATRIX_X-2, i, RED);
            matrix.setPixel(MATRIX_X-1, i, RED);
        }        
        matrix.show();
        // PAINT IT BLACK
        for (int i = 0; i < 8; i++) {
            matrix.setPixel(MATRIX_X-2, i, BLACK);
            matrix.setPixel(MATRIX_X-1, i, BLACK);
        }
        matrix.show();
    }
} // update_forecast()


// returns minutes since last weather
int freshmaker(String j) {
    return (Time.now() - forecast_freshness) / 60;
} // int uptime(junque)


void weatherHandler(const char *eventName, const char *data) {
    Serial.printf("eventName: %s; data: %s\n", eventName, data);
    bool valid = false;
    float temps[8];
    byte conds[8];
    for (int i = 0; i < 8; i++) {
        temps[i] = -99;
        conds[i] = 0;
    }

    JSONValue outerObj = JSONValue::parseCopy(data);
    JSONObjectIterator iter(outerObj);
    while (iter.next()) {
        // Serial.print(String(iter.name()));
        // Serial.print(": ");
        // Serial.println(String(iter.value().toString()));
        if (iter.name() == "freshness") {
            time_t stamp = iter.value().toInt();
            int age = Time.now() - stamp;
            Serial.printf("data is %d secs old\n", age);
            if (age > -30 && age < 3*3600) {
                Serial.println("Age is valid");
                valid = true;
                for (int i = 0; i < 8; i++) {
                    if (temps[i] == -99 || conds[i] == 0) {
                        valid = false;
                    }
                }
                if (valid) {
                    Serial.println("All values have been set.  Copying...");
                    for (int i = 0; i < 8; i++) {
                        forecast_temp[i] = temps[i];
                        forecast_cond[i] = conds[i];
                    }
                    forecast_freshness = stamp;
                    update_forecast();
                }
            }
        } else {
            int eighth = String(iter.name()).toInt();
            // Serial.printf("eighth: %d\n", eighth);
            JSONObjectIterator inner(iter.value());
            while (inner.next()) {
                if (inner.name() == "temp") {
                    temps[eighth] = inner.value().toDouble();
                } else if (inner.name() == "cond") {
                    String cond = String(inner.value().toString());
                    if (cond.equals("SUNNY")) {
                        conds[eighth] = 1;
                    } else if (cond.equals("CLOUDY")) {
                        conds[eighth] = 2;
                    } else {
                        conds[eighth] = 3;
                    }
                } else {
                    Serial.print("unknown name:");
                    Serial.println(String(inner.name()));
                }
            }
        }
    }
}


// return the (updated) moving average over N
// avg == old average, "value" = new
// usage: average = ma(30, average, value)
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


#define N 30

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

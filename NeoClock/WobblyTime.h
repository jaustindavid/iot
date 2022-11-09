#ifndef WOBBLYTIME
#define WOBBLYTIME

#include <SimpleTimer.h>

/* 
 * full disclosure: I read once about a clock which displayed imprecise time.
 * I am modeling the bahvior, but I have not attempted to go back and find 
 * the author or make attribution.
 *
 * concept: display imprecise time, but always a lil fast
 *    hour() >= Time.hour()
 *    minute() >= Time.minute()
 *    second() >= Time.second()
 *
 * values will be strictly increasing; subsequent calls to second() will
 * never go backward (except for rollover obviously).  But it might appear to
 * stall, depending on settings.
 *
 * TODO: add a few particle functions for console interaction
 * TODO: speed up, rather than lurching forward
 *
 * update: pace is a "percent of realtime" measure; 
 *   > 100 to accelerate,
 *   < 100 to slow down
 *
 */


int rando(int rmin, int rmax) {
    float r = 0.000001 * random(0, 1000000);
    return rmin + (int)(r*r*r*(rmax - rmin));
}


class WobblyTime {
    private:
        byte h, m, s;
        int MAX_ADVANCE, MIN_ADVANCE, pace, dT;
        time_t fakeTime;
        SimpleTimer *tickTimer;
        void update();
        int setAdvance(String);
        void updateAdvance();
        void tick();

    public:
        WobblyTime();
        void setup();
        byte hour();
        byte minute();
        byte second();
};


WobblyTime::WobblyTime() {
    MAX_ADVANCE = 300;         // at most, this fast
    MIN_ADVANCE = 30;           // at least this fast
    dT = rando(MIN_ADVANCE, MAX_ADVANCE);
    fakeTime = Time.now() + rando(MIN_ADVANCE, MAX_ADVANCE);
    tickTimer = new SimpleTimer(1000);
} // WobblyTime()


int WobblyTime::setAdvance(String s) {
    long t = s.toInt();
    if (t) {
        // fakeTime = max(Time.now() + t, fakeTime);
        dT = constrain(t, MIN_ADVANCE, MAX_ADVANCE);
    }
    return fakeTime - Time.now();
}


void WobblyTime::setup() {
    Particle.function("advance", &WobblyTime::setAdvance, this);
} // setup()


/*
// update the amount of "advance" in the clock
void WobblyTime::updateAdvance() {
    if (Time.now() + MIN_ADVANCE >= fakeTime) {
        fakeTime = rando(MIN_ADVANCE, MAX_ADVANCE) + Time.now();
        Serial.print("New advance: ");
        Serial.println(fakeTime - Time.now());
    }
//    return;
    if (Time.now() + dT == fakeTime) {
        dT = rando(MIN_ADVANCE, MAX_ADVANCE);
        int pacer = rando(100, 500);
        int delta = fakeTime - Time.now();
        Serial.printf("delta %d, dt %d, pacer %d; ", delta, dT, pacer);
        if (dT > fakeTime - Time.now()) {
            pace = map(pacer, 100, 500, 90, 30);   // slow down, 30-90% real time
        } else {
            pace = map(pacer, 100, 500, 110, 300); // speed up, 110-300% real time
        }
        Serial.printf(" new pace: %d\n", pace);
    }
} // updateAdvance()
*/


// advance fakeTime (about) half as fast as real time
// we.g 50% chance of ticking not more than 1/sec
/*
 * if dT
 */
void WobblyTime::tick() {
    /*
    static time_t lastTick = Time.now();
    // Serial.printf("Tick?  (%ds since last)\n", Time.now() - lastTick);
    if (Time.now() > lastTick) {
    */
    if (tickTimer->isExpired()) {
        /*
        if (random(0, 100) < 50) {
            Serial.printf("Tick: delta = %ds\n", fakeTime - Time.now());
            fakeTime += 1;
        }
        int tock = 0;
        if (pace > 100) {
            tock = random(pace) / 100;
        } else if (random(0, 100) < pace) {
            tock = 1; 
        }
        Serial.printf("pace %d -> tock: %d\n", pace, tock);
        // lastTick = Time.now();
        */
        int offset = dT - (fakeTime - Time.now());
        Serial.printf("dT: %d; ", dT);
        Serial.printf("fT: %d; ", fakeTime - Time.now());
        Serial.printf("offset: %d; ", offset);
        int tock = 0;
        if (offset > 0) {
            tock = 1 + min(offset, random(3));
        } else if (offset < 0) {
            if (random(100) < 50) {
                tock = 1;
            }
        } else {
            dT = rando(MIN_ADVANCE, MAX_ADVANCE);
        }
        Serial.printf("tock: %d\n", tock);
        fakeTime += tock;
    }
} // tick()


void WobblyTime::update() {
    tick();
    // updateAdvance();
    h = Time.hour(fakeTime);
    m = Time.minute(fakeTime);
    s = Time.second(fakeTime);
} // update()


byte WobblyTime::hour() {
    update();
    return h;
} // byte hour()


byte WobblyTime::minute() {
    update();
    return m;
} // byte minute()


byte WobblyTime::second() {
    update();
    return s;
} // byte second()

#endif
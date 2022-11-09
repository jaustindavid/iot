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


// advance fakeTime (about) half as fast as real time
// e.g. 50% chance of ticking not more than 1/sec
void WobblyTime::tick() {

    if (tickTimer->isExpired()) {
        int offset = dT - (fakeTime - Time.now());
        // Serial.printf("dT: %d; ", dT);
        // Serial.printf("fT: %d; ", fakeTime - Time.now());
        // Serial.printf("offset: %d; ", offset);
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
        // Serial.printf("tock: %d\n", tock);
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
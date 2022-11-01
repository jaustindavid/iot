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
 */


int rando(int rmin, int rmax) {
    float r = 0.000001 * random(0, 1000000);
    return rmin + (int)(r*r*r*(rmax - rmin));
}


class WobblyTime {
    private:
        byte h, m, s;
        int MAX_ADVANCE, MIN_ADVANCE;
        time_t fakeTime, initial, target;
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
    target = MIN_ADVANCE + Time.now();
    initial = Time.now();
    fakeTime = MIN_ADVANCE * 1.5 + Time.now();
    tickTimer = new SimpleTimer(1000);
} // WobblyTime()


int WobblyTime::setAdvance(String s) {
    long t = s.toInt();
    if (t) {
        fakeTime = max(Time.now() + t, fakeTime);
    }
    return fakeTime - Time.now();
}


void WobblyTime::setup() {
    Particle.function("advance", &WobblyTime::setAdvance, this);
    return;
    for (int i = 0; i < 25; i++) {
        Serial.printf("%d: %d\n", i, rando(MIN_ADVANCE, MAX_ADVANCE));
    }
} // setup()


// update the amount of "advance" in the clock
void WobblyTime::updateAdvance() {
    if (Time.now() + MIN_ADVANCE >= fakeTime) {
        fakeTime = rando(MIN_ADVANCE, MAX_ADVANCE) + Time.now();
        Serial.print("New advance: ");
        Serial.println(fakeTime - Time.now());
    }
} // updateAdvance()


// advance fakeTime (about) half as fast as real time
// we.g 50% chance of ticking not more than 1/sec
void WobblyTime::tick() {
    /*
    static time_t lastTick = Time.now();
    // Serial.printf("Tick?  (%ds since last)\n", Time.now() - lastTick);
    if (Time.now() > lastTick) {
    */
    if (tickTimer->isExpired()) {
        if (random(0, 100) < 50) {
            Serial.printf("Tick: delta = %ds\n", fakeTime - Time.now());
            fakeTime += 1;
        }
        // lastTick = Time.now();
    }
} // tick()


void WobblyTime::update() {
    tick();
    updateAdvance();
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
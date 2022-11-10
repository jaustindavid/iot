#ifndef WOBBLYTIME
#define WOBBLYTIME

#include <SimpleTimer.h>

/* 
 * full disclosure: I read once about a clock which displayed imprecise time.
 * I am modeling the behavior, but I have not attempted to go back and find 
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
 * internal Theory of Operation:
 *   "target_offset" is a *goal* 
 *   actual offset = fakeTime - Time.now()
 *
 * if actual offset < target offset, 
 *   speed up time: tick faster (1, 2, or 3s ticks per s)
 * else if actual offset > target offset,
 *   slow down time: tick slower (33% chance tick per s)
 * else recalculate a target offset
 * 
 * TODO: add a few particle functions for console interaction
 */

#undef DEBUG_WT

int rando(int rmin, int rmax) {
    float r = 0.000001 * random(0, 1000000);
    return rmin + (int)(r*r*r*(rmax - rmin));
}


class WobblyTime {
    private:
        byte h, m, s;
        int MAX_ADVANCE, MIN_ADVANCE, dT, target_offset;
        time_t fakeTime, lastTick;
        SimpleTimer *tickTimer;
        void init(int, int);
        void update();
        int setAdvance(String);
        void updateAdvance();
        void tick();
        void t1ck();
        void printStatus();

    public:
        WobblyTime();
        WobblyTime(int, int);
        void setup();
        time_t now();
        byte hour();
        byte minute();
        byte second();
};


WobblyTime::WobblyTime() {
    init(30, 300);
} // WobblyTime()


WobblyTime::WobblyTime(int min, int max) {
    init(min, max);
} // WobblyTime(min, max)


void WobblyTime::init(int min, int max) {
    MIN_ADVANCE = min;      // at least this fast
    MAX_ADVANCE = max;      // at most, this fast
    dT = rando(MIN_ADVANCE, MAX_ADVANCE);
    target_offset = rando(MIN_ADVANCE, MAX_ADVANCE);
    fakeTime = Time.now() + rando(MIN_ADVANCE, MAX_ADVANCE);
    tickTimer = new SimpleTimer(1000);
    lastTick = Time.now();
    printStatus();
} // init(min, max)


int WobblyTime::setAdvance(String s) {
    long t = s.toInt();
    if (t) {
        // fakeTime = max(Time.now() + t, fakeTime);
        dT = constrain(t, MIN_ADVANCE, MAX_ADVANCE);
        target_offset = constrain(t, MIN_ADVANCE, MAX_ADVANCE);
    }
    return fakeTime - Time.now();
}


void WobblyTime::setup() {
    Particle.function("advance", &WobblyTime::setAdvance, this);
} // setup()


// advance up to 3x real time
void WobblyTime::tick() {
    if (tickTimer->isExpired()) {
        int lapsed = Time.now() - lastTick;
        int actual_offset = fakeTime - Time.now();
        #ifdef DEBUG_WT
            Serial.printf("actual offset: %d <> target offset %d; ", actual_offset, target_offset);
        #endif
        int tick;
        // shortcut: if the difference is less than the time lapsed, recompute
        if (abs(target_offset - actual_offset) <= lapsed) {
            #ifdef DEBUG_WT
                Serial.print("!! ");
            #endif
            tick = abs(target_offset - actual_offset);
            target_offset = rando(MIN_ADVANCE, MAX_ADVANCE);
        } else if (actual_offset < target_offset) {
            #ifdef DEBUG_WT
                Serial.print("^ ");
            #endif
            // tick 300% of realtime, but not more than the diffference; plus the ~second gap
            tick = 1 + min(target_offset - actual_offset, round(0.01*rando(lapsed*133, lapsed*300)));
        } else { //  if (actual_offset > target_offset) {
            #ifdef DEBUG_WT
                Serial.print("v ");
            #endif
            // tick 33% of realtime; ignore that 1s
            tick = round(0.01*rando(lapsed*33, lapsed*90));
        } // else {
        // }
        #ifdef DEBUG_WT
            Serial.printf("%d lapsed; ticking %d\n", lapsed, tick);
        #endif
        fakeTime += tick;
        lastTick = Time.now();
    }
} // tick()


void WobblyTime::update() {
    tick();
    // updateAdvance();
    h = Time.hour(fakeTime);
    m = Time.minute(fakeTime);
    s = Time.second(fakeTime);
} // update()


time_t WobblyTime::now() {
    update();
    return fakeTime;
} // time_t now()


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


void WobblyTime::printStatus() {
    Serial.printf("Wobbly Time: %02d:%02d:%02d, delta %ds; actual %02d:%02d:%02d\n", 
                    hour(), minute(), second(),
                    fakeTime - Time.now(), 
                    Time.hour(), Time.minute(), Time.second());
}

#endif
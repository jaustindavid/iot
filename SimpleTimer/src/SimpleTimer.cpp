#include "Particle.h"
#include "simpletimer.h"

SimpleTimer::SimpleTimer() {
    _interval = 0;
    _milestone = 0;
}


SimpleTimer::SimpleTimer(time_t interval) {
    setInterval(interval);
    reset();
}


void SimpleTimer::setInterval(time_t interval) {
    _interval = interval;
}


void SimpleTimer::wait() {
    if (! isExpired()) {
        delay(constrain(_milestone + _interval - Time.now(), 0, _interval));
        _softReset(false);
    }
}


void SimpleTimer::reset() {
    _milestone = millis();
}


bool SimpleTimer::isExpired() {
    return isExpired(false);
} 


bool SimpleTimer::isExpired(bool printing = false) {
    if (printing) 
        Serial.printf("isExpired: %dms > %d + %d (%d)? ", millis(), _milestone, _interval, _milestone + _interval);
    if (millis() > _milestone + _interval) {
        if (printing)
            Serial.println("yep");
        _softReset(printing);
        return true;
    }
    if (printing)
        Serial.println("nope");
    return false;
}


void SimpleTimer::_softReset(bool printing = false) {
    uint32_t millisTarget = millis();
    if (printing) 
        Serial.printf("SoftReset: milestone=%d, target >= %d ", _milestone, millisTarget);
    while ((_milestone + _interval) < millisTarget) {
        _milestone += _interval;
        if (printing) 
            Serial.printf(" += %d (%d)", _interval, _milestone);
    }
}
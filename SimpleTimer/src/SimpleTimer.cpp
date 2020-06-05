#include "Particle.h"
#include "SimpleTimer.h"

SimpleTimer::SimpleTimer() {
    _interval = 0;
    _milestone = 0;
}


SimpleTimer::SimpleTimer(uint32_t interval, bool startExpired = false) {
    setInterval(interval);
    _startExpired = startExpired;
    reset();
}


void SimpleTimer::setInterval(uint32_t interval) {
    _interval = interval;
}


uint32_t SimpleTimer::remaining() {
    return (_milestone + _interval) - millis();
}


void SimpleTimer::wait(bool printing = false) {
    if (! isExpired(printing)) {
        uint32_t delayms = constrain(_milestone + _interval - millis(), 0, _interval);
        if (printing) Serial.printf("waiting for %d ms\n", delayms);
        delay(delayms);
        _softReset(false); // not a bug; isExpired == true -> softReset internally
    } else {
        if (printing) Serial.println("must have been expired");
    }
}


void SimpleTimer::reset() {
    _milestone = millis();
}


bool SimpleTimer::isExpired(bool printing = false) {
    if (printing) 
        Serial.printf("isExpired: %lums > %du + %du (%du)? ", millis(), _milestone, _interval, _milestone + _interval);
    if ((millis() >= _milestone + _interval) or _startExpired) {
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
    while ((_milestone + _interval) <= millisTarget) {
        _milestone += _interval;
        if (printing) 
            Serial.printf(" += %d (%d)", _interval, _milestone);
    }
    if (printing) Serial.println();
    _startExpired = false;
}
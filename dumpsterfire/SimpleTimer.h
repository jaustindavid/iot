#ifndef SIMPLETIMER_H
#define SIMPLETIMER_H

/*
 * A very simple ... timer.  millis() accurate.  
 * Very long delays (wrapping uint32_t ms) may
 * be weird.  This will "maintain rhythm": 
 *   a 100ms timer started at time=0 will fire
 *     at t=100, t=200, t=300
 *   if delayed (isExpired called at t=102)
 *     the next fire/wait() will still happen t=200
 *
 * Usage:
 *   SimpleTimer every50(50);
 *   SimpleTimer hourly(3600);
 *   void loop() {
 *      // do stuff 
 *      if (hourly.isExpired()) {
 *         // this will happen precisely once per hour
 *      }
 *      every50.wait(); // will delay (0..50)ms 
 *   }
 *
 *   ...
 */
class SimpleTimer {
    private:
        uint32_t _interval;
        uint32_t _milestone;
        bool _startExpired;
        
        // reset the timer while maintaining rhythm
        void _softReset(bool);

    public:
        // default constructor.  Will require setInterval()
        SimpleTimer();
        
        // false (default): will immediately start timing.  Use reset() if needed.
        // true: will immediately start *expired*
        SimpleTimer(uint32_t, bool);
        
        // sets a new interval.  Does not implicity reset() the timer.
        void setInterval(uint32_t); 
        
        // waits for a timer to expire.  Resets the timer.
        void wait();
        
        // reset the timer
        void reset();
        
        // returns the time (ms) remaining
        uint32_t remaining();

        // true if the timer has expired; resets the timer if needed,
        // maintaining rhythm.  
        // note that a timer will expire exactly once per cycle, and 
        // cycles will maintain a very strict cadence 
        // unless externally reset()
        bool isExpired(bool);
}; 


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


void SimpleTimer::wait() {
    if (! isExpired(false)) {
        delay(constrain(_milestone + _interval - millis(), 0, _interval));
        _softReset(false); // not a bug; isExpired == true -> softReset internally
    }
}


void SimpleTimer::reset() {
    _milestone = millis();
}


bool SimpleTimer::isExpired(bool printing = false) {
    if (printing) 
        Serial.printf("isExpired: %lums > %du + %du (%du)? ", millis(), _milestone, _interval, _milestone + _interval);
    if ((millis() > _milestone + _interval) or _startExpired) {
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
    _startExpired = false;
}

#endif

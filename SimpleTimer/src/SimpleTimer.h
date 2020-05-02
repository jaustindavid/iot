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
        
        // reset the timer while maintaining rhythm
        void _softReset(bool);

    public:
        // default constructor.  Will require setInterval()
        SimpleTimer();
        
        // will immediately start timing.  Use reset() if needed.
        SimpleTimer(time_t);
        
        // sets a new interval.  Does not implicity reset() the timer.
        void setInterval(time_t); 
        
        // waits for a timer to expire.  Resets the timer.
        void wait();
        
        // reset the timer()
        void reset();
        
        // true if the timer has expired; resets the timer if needed,
        // maintaining rhythm.  
        // note that a timer will expire exactly once per cycle, and 
        // cycles will maintain a very strict cadence 
        // unless externally reset()
        bool isExpired(bool);
        bool isExpired();
}; 
#endif
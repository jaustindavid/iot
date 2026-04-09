#pragma once

#include "Particle.h"
#include <cmath>
#include <cstdlib>

/**
 * WobblyTime
 * 
 * Tracks a virtual "display" time that stays ahead of real time by a random
 * amount in [min, max] seconds. It ticks faster when behind its target offset,
 * slower when ahead.
 */
class WobblyTime {
public:
    int wobble_min_seconds;
    int wobble_max_seconds;
    float fast_rate;
    float slow_rate;

    double target_offset_secs; // Current target advance in seconds
    double virtual_epoch_secs; // Virtual display time as unix epoch
    unsigned long last_wall_ms;
    bool initialized;

    WobblyTime(int mn, int mx, float fast, float slow)
        : wobble_min_seconds(mn), wobble_max_seconds(mx),
          fast_rate(fast), slow_rate(slow),
          target_offset_secs(0), virtual_epoch_secs(0),
          last_wall_ms(0), initialized(false) {}

    void seed(time_t real_now) {
        int range_secs = wobble_max_seconds - wobble_min_seconds;
        target_offset_secs = wobble_min_seconds + (rand() % (range_secs + 1));
        virtual_epoch_secs = (double)real_now + target_offset_secs;
        last_wall_ms = millis();
        initialized = true;
    }

    // Call periodically. Returns updated virtual epoch.
    time_t advance(time_t real_now) {
        // Only initialize if we have a valid actual time (after Jan 1 2024)
        if (!initialized) {
            if (real_now < 1704067200) return 0;
            seed(real_now);
            return (time_t)virtual_epoch_secs;
        }

        // If real time has jumped (e.g. cloud sync just happened), re-seed if drift is too large
        double current_offset = virtual_epoch_secs - (double)real_now;
        if (std::abs(current_offset) > (double)wobble_max_seconds * 2.0) {
            Log.info("WobblyTime: Large drift detected (%.1fs), snapping...", current_offset);
            seed(real_now);
            return (time_t)virtual_epoch_secs;
        }

        unsigned long now_ms = millis();
        double wall_elapsed = (now_ms - last_wall_ms) / 1000.0;
        last_wall_ms = now_ms;

        // Pick tick rate based on whether we're ahead or behind target
        float rate = (current_offset < target_offset_secs) ? fast_rate : slow_rate;
        virtual_epoch_secs += wall_elapsed * rate;

        // If we've reached the target offset, pick a new target
        if (std::abs(current_offset - target_offset_secs) < 5.0) {
            int range_secs = wobble_max_seconds - wobble_min_seconds;
            target_offset_secs = wobble_min_seconds + (rand() % (range_secs + 1));
        }

        return (time_t)virtual_epoch_secs;
    }
};

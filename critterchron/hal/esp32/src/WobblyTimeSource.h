#pragma once

// WobblyTimeSource — CritTimeSource decorator that drifts a virtual wall clock
// around the real one by a randomized offset in [min_seconds, max_seconds].
// Ticks faster than real time when behind its current target offset, slower
// when ahead, then picks a new target when it gets there. Creates the
// intentional imprecision that makes the critter clock feel alive — the
// display isn't trying to be NTP.
//
// Port note: this is a near-verbatim copy of hal/particle/src/WobblyTimeSource.h
// — the body uses only portable APIs (`millis()`, `std::abs`, `rand()`,
// `time_t`) so the only changes are the platform gate and the Arduino.h
// include. Keep it diff-able with the Particle version.
//
// Decorator pattern: valid() and zone_offset_hours() pass through to the
// inner source; wall_now() is the wobbled value. The engine is unaware.
//
// wall_now() is const but advances mutable state on each call — the elapsed
// wall time since the previous call drives the virtual advance, so callers
// don't need to remember to invoke a separate "tick" method. A re-sync of
// the underlying clock (SNTP) that produces a huge jump re-seeds rather
// than chasing the drift.
//
// Tuning knobs are pulled from the Config surface on every read (hot-path
// safe per Config.h contract). First call auto-registers the key + default.
// Defaults: drift 2..5 minutes ahead of real time, ±30% tick-rate swings.

#if defined(ARDUINO_ARCH_ESP32)

#include <Arduino.h>
#include "interface/Config.h"
#include "interface/CritTimeSource.h"
#include <cmath>
#include <cstdlib>

class WobblyTimeSource : public CritTimeSource {
public:
    WobblyTimeSource(CritTimeSource& inner, const Config& cfg)
        : inner_(inner), cfg_(cfg) {}

    bool   valid()             const override { return inner_.valid(); }
    float  zone_offset_hours() const override { return inner_.zone_offset_hours(); }

    time_t wall_now() const override {
        if (!inner_.valid()) return 0;
        time_t real_now = inner_.wall_now();

        if (!init_) {
            seed_(real_now);
            return (time_t)virt_epoch_s_;
        }

        int max_s = cfg_.get_int("wobble_max_seconds", 300);

        // If the real clock jumped (SNTP resync after a long offline spell),
        // chasing the drift would take minutes. Snap to a fresh target.
        double offset = virt_epoch_s_ - (double)real_now;
        if (std::abs(offset) > (double)max_s * 2.0) {
            seed_(real_now);
            return (time_t)virt_epoch_s_;
        }

        unsigned long now_ms = millis();
        double elapsed = (double)(now_ms - last_wall_ms_) / 1000.0;
        last_wall_ms_ = now_ms;

        float fast = cfg_.get_float("wobble_fast_rate", 1.3f);
        float slow = cfg_.get_float("wobble_slow_rate", 0.7f);
        float rate = (offset < target_offset_s_) ? fast : slow;
        virt_epoch_s_ += elapsed * rate;

        if (std::abs(offset - target_offset_s_) < 5.0) pick_target_();
        return (time_t)virt_epoch_s_;
    }

private:
    void seed_(time_t real_now) const {
        pick_target_();
        virt_epoch_s_ = (double)real_now + target_offset_s_;
        last_wall_ms_ = millis();
        init_ = true;
    }

    void pick_target_() const {
        int min_s = cfg_.get_int("wobble_min_seconds", 120);
        int max_s = cfg_.get_int("wobble_max_seconds", 300);
        int span = max_s - min_s;
        target_offset_s_ = (double)min_s + (span > 0 ? (rand() % (span + 1)) : 0);
    }

    CritTimeSource&   inner_;
    const Config& cfg_;

    mutable double        virt_epoch_s_    = 0.0;
    mutable double        target_offset_s_ = 0.0;
    mutable unsigned long last_wall_ms_    = 0;
    mutable bool          init_            = false;
};

#endif  // ARDUINO_ARCH_ESP32

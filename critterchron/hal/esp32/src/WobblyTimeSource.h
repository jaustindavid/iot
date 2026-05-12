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
// `elapsed` is derived from the inner (RTC) clock, NOT millis(). On the
// OG Photon, millis() / System.uptime() runs at ~74% of real time
// (debug_photon_gen2_systick_drift) — using millis-elapsed there made
// virt grow at 0.74 × rate, defeating catchup so wobble drifted below
// min_s and stayed there. The RTC is honest on every platform we ship.
// Resolution loss (1s vs 1ms) is harmless: wall_now() returns time_t
// (whole seconds anyway) so callers see no behavioral change.
//
// Tuning knobs are pulled from the Config surface on every read (hot-path
// safe per Config.h contract). First call auto-registers the key + default.
// Defaults: drift 2..5 minutes ahead of real time, ±30% tick-rate swings.

#if defined(ARDUINO_ARCH_ESP32)

#include <Arduino.h>
#include "interface/Config.h"
#include "interface/CritTimeSource.h"
#include "Dst.h"
#include <cmath>
#include <cstdlib>

class WobblyTimeSource : public CritTimeSource {
public:
    WobblyTimeSource(CritTimeSource& inner, const Config& cfg)
        : inner_(inner), cfg_(cfg) {}

    bool   valid()             const override { return inner_.valid(); }

    // Runtime-tunable zone offset + optional DST adjustment. Falls back
    // to the inner source's compile-time offset if `timezone_offset_hours`
    // KV is unset, so a fresh-boot device with no live KV reads yet
    // still gets a sensible offset from creds.h.
    //
    // `dst_enabled` is an enum (see Dst.h): 0=disabled, 1=US. Unknown
    // values fall through to "disabled" via Dst.cpp's switch default.
    //
    // Called from CritterEngine::syncTime() once per virtual minute by
    // the tel loop — hot-path safe (Config getters never block, Dst
    // math is pure µs-scale arithmetic).
    float  zone_offset_hours() const override {
        float base = cfg_.get_float("timezone_offset_hours",
                                    inner_.zone_offset_hours());
        int rule_i = cfg_.get_int("dst_enabled", 0);
        auto rule = static_cast<critterchron::dst::Rule>(rule_i);
        if (!inner_.valid()) return base;
        float adj = critterchron::dst::dst_adjustment_hours(
            rule, inner_.wall_now(), base);
        return base + adj;
    }

    // Heartbeat diagnostic — emit `wobble=(min<cur<max)s` per the
    // existing `(a<b<c)` triple convention. Caller is expected to
    // print these in literal min<cur<max position WITHOUT sorting:
    // when the algorithm is healthy the `<` ordering holds; if the
    // current offset somehow falls outside [min,max] the symbols
    // visually break (e.g. `(120<-30<300)`), making the bug obvious
    // at a glance. Same idea as `bri=(min<cur<max)`.
    //
    // Reads `virt_epoch_s_` un-locked. Engine thread updates it on
    // every wall_now() call, so this can return a value off by one
    // tick of drift; for a heartbeat snprintf that's noise. No double
    // alignment guarantees on 32-bit platforms either, but a torn
    // read produces a plausible-looking offset, not a crash.
    int wobble_min_s() const { return cfg_.get_int("wobble_min_seconds", 120); }
    int wobble_max_s() const { return cfg_.get_int("wobble_max_seconds", 300); }
    int wobble_offset_s() const {
        if (!init_ || !inner_.valid()) return 0;
        return (int)(virt_epoch_s_ - (double)inner_.wall_now());
    }

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

        // Elapsed since last call, RTC-based. See header comment re:
        // millis() drift on OG Photon — same shim runs there, same fix.
        // Negative elapsed (RTC stepped backward, e.g. minor SNTP
        // adjustment) is clamped to 0; large jumps are caught by the
        // |offset| > max_s*2 reseed gate above.
        time_t real_s = real_now;
        double elapsed = (double)(real_s - last_real_s_);
        if (elapsed < 0) elapsed = 0;
        last_real_s_ = real_s;

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
        last_real_s_ = real_now;
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
    mutable time_t        last_real_s_     = 0;
    mutable bool          init_            = false;
};

#endif  // ARDUINO_ARCH_ESP32

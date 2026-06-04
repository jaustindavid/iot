#pragma once
#include <ctime>
#include "interface/CritTimeSource.h"

// Fixed-time CritTimeSource for parity runs. `valid()` is always true so the
// engine's sync-time path runs from tick 0.
//
// advance_ms() lets the soak harness drive virtual wall-clock time forward
// independently of real time — the key to fast-forward simulation. The
// parity path (frozen time) just never calls it, preserving old behavior.
// Sub-second resolution is accumulated in `accum_ms_` and folded into whole
// seconds, so a 400ms tick rate advances `now_` by one second every ~2-3
// ticks the same way real wall time would.

class FakeTimeSource : public CritTimeSource {
public:
    FakeTimeSource(time_t utc_now, float zone_offset_hours)
        : now_(utc_now), zone_(zone_offset_hours) {}

    void set_now(time_t utc_now) { now_ = utc_now; accum_ms_ = 0; }

    // Advance virtual wall time by `ms` milliseconds. Accumulates sub-second
    // remainder so repeated sub-second advances roll whole seconds correctly.
    void advance_ms(uint32_t ms) {
        accum_ms_ += ms;
        now_      += (time_t)(accum_ms_ / 1000u);
        accum_ms_ %= 1000u;
    }

    bool   valid() const override             { return true; }
    time_t wall_now() const override          { return now_; }
    float  zone_offset_hours() const override { return zone_; }

private:
    time_t   now_;
    float    zone_;
    uint32_t accum_ms_ = 0;
};

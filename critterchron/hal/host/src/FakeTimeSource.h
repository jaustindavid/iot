#pragma once
#include <ctime>
#include "interface/CritTimeSource.h"

// Fixed-time CritTimeSource for parity runs. `valid()` is always true so the
// engine's sync-time path runs from tick 0.

class FakeTimeSource : public CritTimeSource {
public:
    FakeTimeSource(time_t utc_now, float zone_offset_hours)
        : now_(utc_now), zone_(zone_offset_hours) {}

    void set_now(time_t utc_now) { now_ = utc_now; }

    bool   valid() const override             { return true; }
    time_t wall_now() const override          { return now_; }
    float  zone_offset_hours() const override { return zone_; }

private:
    time_t now_;
    float  zone_;
};

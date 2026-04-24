#pragma once

// MockTimeSource — TimeSource that reports valid()==true with a fixed
// epoch, so the engine ticks and behaviors run on a device that has no
// wall clock (no WiFi, no SNTP, no RTC). Advances with millis() so the
// virtual time moves forward even though it starts at an arbitrary
// anchor.
//
// Purpose: milestone-1 bring-up of the ESP32 port, where the shim
// doesn't have WiFi or Stra2us yet but we still want to see agents
// moving on the panel. Once EspTimeSource (SNTP-backed) lands, swap
// this out for WobblyTimeSource(EspTimeSource(...)) — no engine changes
// needed, the TimeSource interface is the seam.
//
// Anchor epoch: Sat 2026-04-23 16:00:00 UTC — picked arbitrarily in the
// current project week so `time`-based behaviors (if any) don't land
// on a nonsense date. wall_now() adds millis()/1000 to that, so at the
// one-minute syncTime cadence the engine sees a normally-advancing
// clock.

#include <Arduino.h>
#include "interface/TimeSource.h"

class MockTimeSource : public TimeSource {
public:
    // Default anchor = 2026-04-23T16:00:00Z (Unix 1776844800).
    explicit MockTimeSource(time_t anchor_epoch = 1776844800,
                            float zone_offset_hours = 0.0f)
        : anchor_(anchor_epoch), zone_off_(zone_offset_hours) {}

    bool   valid()             const override { return true; }
    time_t wall_now()          const override {
        return anchor_ + (time_t)(millis() / 1000UL);
    }
    float  zone_offset_hours() const override { return zone_off_; }

private:
    time_t anchor_;
    float  zone_off_;
};

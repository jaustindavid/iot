#pragma once
#include <ctime>

// Abstract wall-clock source. Phase-1 impl wraps Particle `Time.*`;
// phase 2 wraps it in WobblyTime. The engine calls only through this
// interface for `sync_time()`. See HAL_SPEC §4.2.

struct TimeSource {
    virtual bool   valid() const = 0;          // cloud sync state
    virtual time_t wall_now() const = 0;       // UTC seconds
    virtual float  zone_offset_hours() const = 0;
    virtual ~TimeSource() = default;
};

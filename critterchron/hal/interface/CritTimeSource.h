#pragma once
#include <ctime>

// Abstract wall-clock source. Phase-1 impl wraps Particle `Time.*`;
// phase 2 wraps it in WobblyTime. The engine calls only through this
// interface for `sync_time()`. See HAL_SPEC §4.2.
//
// Named `CritTimeSource` (not just `TimeSource`) to avoid colliding with
// Particle DeviceOS 5+'s `TimeSource` symbol in `spark_wiring_time.h`
// (the enum feeding `Time.setSource(TimeSource::...)`). That collision
// broke photon2 cloud compiles in April 2026 — Gen 2 (Photon/P1) runs
// an older LTS DeviceOS and was unaffected, which made it look like a
// regression until the source file was inspected. Don't rename back.

struct CritTimeSource {
    virtual bool   valid() const = 0;          // cloud sync state
    virtual time_t wall_now() const = 0;       // UTC seconds
    virtual float  zone_offset_hours() const = 0;
    virtual ~CritTimeSource() = default;
};

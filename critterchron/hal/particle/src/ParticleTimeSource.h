#pragma once

// ParticleTimeSource — CritTimeSource backed by Particle's cloud-synced RTC.

#if defined(PLATFORM_ID)

#include "Particle.h"
#include "interface/CritTimeSource.h"

class ParticleTimeSource : public CritTimeSource {
public:
    explicit ParticleTimeSource(float zone_offset_hours = 0.0f)
        : zone_off_(zone_offset_hours) {}

    void set_zone(float hours) { zone_off_ = hours; }

    bool valid() const override { return Time.isValid(); }
    time_t wall_now() const override { return Time.now(); }
    float zone_offset_hours() const override { return zone_off_; }

private:
    float zone_off_;
};

#endif  // PLATFORM_ID

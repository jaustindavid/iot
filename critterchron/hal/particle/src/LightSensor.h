#pragma once

// Ambient light sensor driver — maps a raw ADC reading to a target brightness
// in the range [min_bri, max_bri], both expressed as uint8_t 0..255.
//
// Auto-calibrates: cal_dark and cal_bright widen immediately as brighter/
// darker raw readings come in, then slowly pull back in toward the observed
// working range. A transient anomaly (flashlight at night, finger over the
// sensor) still registers for motion but doesn't permanently pin the
// calibration — it decays out over a few hours. A floor on (cal_dark -
// cal_bright) prevents full collapse when ambient is stable. Cold boot uses
// conservative defaults; re-learns within a handful of minutes. A power-curve
// exponent shapes the response:
//
//   exponent < 1 -> eager to brighten; bright room pops to full quickly
//   exponent = 1 -> linear
//   exponent > 1 -> stays dim in low light, only brightens in a well-lit room
//   exponent ~ 2.5 -> "bedroom" curve (coaticlock default)
//
// The exponent is a tunable knob pulled from Config on every update — hot-
// path safe per Config.h contract. cal_dark / cal_bright are runtime-learned
// state, not config.
//
// Pure math — the shim owns hardware config (pin modes, power, sample rate)
// and any smoothing / EMA applied to the output.

#include <cstdint>
#include <cmath>
#include "interface/Config.h"

class LightSensor {
public:
    int cal_dark   = 3500;   // raw value that maps to "fully dark"
    int cal_bright = 2000;   // raw value that maps to "fully bright"
    // Most recent raw ADC read, captured at the top of update(). Exposed
    // publicly so the telemetry path (which doesn't own the sample loop)
    // can report it in the heartbeat without duplicating the read —
    // observability hook for diagnosing calibration poisoning (TODO.md
    // "Light-sensor calibration poisons itself"). Zero until update()
    // has been called at least once.
    int last_raw  = 0;

    explicit LightSensor(const Config& cfg) : cfg_(cfg) {}

    uint8_t update(int raw, uint8_t min_bri, uint8_t max_bri) {
        last_raw = raw;
        // Slow pull-in: every DECAY_EVERY samples (~173s at 5Hz), nudge each
        // cal bound one unit toward raw. A 1000-unit excursion un-learns over
        // ~48 hours — slow enough that a normal day's range stays learned,
        // fast enough that a one-off flashlight hit ages out over a weekend.
        // The MIN_CAL_RANGE floor stops the two from collapsing in a perfectly
        // still environment — below that the sensor would over-interpret tiny
        // variations as full range. Pull-in is much slower than the brightness
        // EMA (α≈1/50) so the two don't fight.
        constexpr int DECAY_EVERY   = 864;
        constexpr int MIN_CAL_RANGE = 200;
        if (++decay_counter_ >= DECAY_EVERY) {
            decay_counter_ = 0;
            if (raw < cal_dark   && (cal_dark - cal_bright) > MIN_CAL_RANGE) cal_dark--;
            if (raw > cal_bright && (cal_dark - cal_bright) > MIN_CAL_RANGE) cal_bright++;
        }
        if (raw > cal_dark)   cal_dark   = raw;
        if (raw < cal_bright) cal_bright = raw;

        float range = (float)(cal_dark - cal_bright);
        if (range < 1.0f)
            return (uint8_t)(((int)min_bri + (int)max_bri) / 2);

        float n = (float)(cal_dark - raw) / range;  // 0=dark, 1=bright
        if (n < 0.0f) n = 0.0f; else if (n > 1.0f) n = 1.0f;

        float exponent = cfg_.get_float("light_exponent", 2.5f);
        if (exponent < 0.1f) exponent = 0.1f;
        if (exponent > 10.0f) exponent = 10.0f;
        float curved = powf(n, exponent);

        int span = (int)max_bri - (int)min_bri;
        int scaled = (int)min_bri + (int)(curved * span + 0.5f);
        if (scaled < min_bri) scaled = min_bri;
        if (scaled > max_bri) scaled = max_bri;
        return (uint8_t)scaled;
    }

private:
    const Config& cfg_;
    uint32_t      decay_counter_ = 0;
};

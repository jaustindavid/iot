#pragma once

// BH1750 ambient light sensor driver — I2C lux → uint8_t brightness.
//
// Contract mirrors LightSensor.h in spirit (update(min,max) → uint8_t
// target) but the math is meaningfully different: BH1750 reports calibrated
// lux, so there's no auto-calibration machinery (cal_dark / cal_bright /
// decay / MIN_CAL_RANGE). Compare to the CDS version which learns the raw
// ADC range from whatever the photocell + divider happens to produce.
//
// Mapping:
//   target = min_bri + clamp((lux / lux_full)^exponent, 0, 1) * span
//
//   light_lux_full (Config, default 375)
//     Lux at which we want full output. 375 ≈ "bright room" per the
//     ESPHome BH1750 recipe this port is modeled on; smaller → reaches
//     full bright sooner, larger → needs more light. Tunable live.
//
//   light_exponent (Config, default 0.5)
//     Curve shape. 0.5 (sqrt) is the ESPHome default — eager to brighten
//     in low-mid lux, approximates the eye's log response. 1.0 = linear;
//     >1.0 stays dim longer (the CDS path defaults to 2.5 for a "bedroom"
//     curve). Same Config key as LightSensor with an INVERTED default:
//     both drivers share the key so a cross-device tune reaches both,
//     but the semantically-useful value is different because the CDS
//     math normalizes raw→n *inverted* (lower raw = more light), while
//     BH1750 is direct (more lux = more light). See LightSensor.h for
//     the CDS convention.
//
// Smoothing (EMA) is applied by the shim at the sample site, not here —
// mirrors the CDS path at critterchron_particle.cpp:785. One-read-in,
// one-target-out keeps the class pure and the smoothing behavior
// identical across platforms.
//
// Failure mode on dropped I2C reads: return the previously-reported
// brightness (or max_bri on first-call failure). Better than flashing
// the panel to black on a single dropped read.
//
// last_raw_lux is exposed for the heartbeat payload (`lux=X.X` —
// diagnostic for "is the room bright enough to saturate the curve, or
// is lux_full set too high"). 0.0 until the first successful read.

#if defined(ARDUINO_ARCH_ESP32)

#include <Arduino.h>
#include <Wire.h>
#include <cstdint>
#include <cmath>
#include "interface/Config.h"

class LightSensorBH1750 {
public:
    // Addresses: 0x23 (ADDR pin LOW / floating, factory default), 0x5C
    // (ADDR pin HIGH). Most breakouts default to 0x23.
    explicit LightSensorBH1750(const Config& cfg, uint8_t addr = 0x23)
        : cfg_(cfg), addr_(addr) {}

    // Initialize Wire on the given pins and put the sensor into
    // continuous H-resolution mode (1 lux resolution, ~120ms per cycle).
    // Returns true if both POWER_ON and CONT_HRES commands ACK'd.
    // Wire.begin is idempotent — safe to re-call.
    bool begin(int sda_pin, int scl_pin) {
        Wire.begin(sda_pin, scl_pin);
        Wire.setClock(100000);  // 100kHz; BH1750 max is 400kHz, 100 is safe
        Wire.beginTransmission(addr_);
        Wire.write(0x01);       // POWER_ON
        if (Wire.endTransmission() != 0) return false;
        Wire.beginTransmission(addr_);
        Wire.write(0x10);       // CONT_HIGH_RES_MODE (1 lux, 120ms cycle)
        return Wire.endTransmission() == 0;
    }

    // Read one measurement, return a target brightness in [min_bri, max_bri].
    uint8_t update(uint8_t min_bri, uint8_t max_bri) {
        uint16_t raw;
        if (!read_raw_(raw)) {
            return last_output_valid_ ? last_output_ : max_bri;
        }
        // BH1750 datasheet: lux = raw / 1.2 in H-res mode.
        float lux = (float)raw / 1.2f;
        last_raw_lux = lux;

        float lux_full = cfg_.get_float("light_lux_full", 375.0f);
        if (lux_full < 1.0f) lux_full = 1.0f;

        float n = lux / lux_full;
        if (n < 0.0f) n = 0.0f;
        if (n > 1.0f) n = 1.0f;

        float exponent = cfg_.get_float("light_exponent", 0.5f);
        if (exponent < 0.1f) exponent = 0.1f;
        if (exponent > 10.0f) exponent = 10.0f;
        float curved = powf(n, exponent);

        int span = (int)max_bri - (int)min_bri;
        int scaled = (int)min_bri + (int)(curved * span + 0.5f);
        if (scaled < min_bri) scaled = min_bri;
        if (scaled > max_bri) scaled = max_bri;
        last_output_ = (uint8_t)scaled;
        last_output_valid_ = true;
        return last_output_;
    }

    // Latest lux reading, exposed for heartbeat. 0.0 until first read.
    float last_raw_lux = 0.0f;

private:
    bool read_raw_(uint16_t& out) {
        // In continuous mode the sensor maintains a rolling result; a 2-
        // byte read returns the latest measurement, no register address.
        uint8_t got = Wire.requestFrom((int)addr_, 2);
        if (got != 2) return false;
        uint8_t hi = Wire.read();
        uint8_t lo = Wire.read();
        out = (uint16_t)((hi << 8) | lo);
        return true;
    }

    const Config& cfg_;
    uint8_t       addr_;
    uint8_t       last_output_       = 0;
    bool          last_output_valid_ = false;
};

#endif  // ARDUINO_ARCH_ESP32

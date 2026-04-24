#pragma once

// EspTimeSource — TimeSource backed by the ESP32's SNTP-synced system
// clock. Pure wrapper over POSIX `time()` + a validity heuristic: if
// the current epoch parses to a year before 2024, we assume SNTP
// hasn't completed yet (the chip boots with `time()` returning 0 ≈
// 1970), and the engine stays paused on the spinner.
//
// SNTP setup (server list, timezone) happens in the sketch via
// `configTime()` — keeping it out of this class lets the same shim
// serve future callers that want time from a different backend (RTC,
// cellular modem) without inheriting ESP32 SNTP goo.
//
// Timezone mirrors ParticleTimeSource's contract: stored as float
// hours, applied by callers (engine's local-minute math) via
// zone_offset_hours(). We do NOT lean on ESP-IDF's POSIX `TZ`
// environment — the engine's math assumes UTC from wall_now() and
// applies the offset itself, same contract on every platform.

#if defined(ARDUINO_ARCH_ESP32)

#include <Arduino.h>
#include <time.h>
#include "interface/TimeSource.h"

class EspTimeSource : public TimeSource {
public:
    explicit EspTimeSource(float zone_offset_hours = 0.0f)
        : zone_off_(zone_offset_hours) {}

    void set_zone(float hours) { zone_off_ = hours; }

    // Validity: SNTP sync leaves `time()` at a real epoch (> ~54 years
    // since 1970), so any year >= 2024 is a safe "we've talked to an
    // NTP server" proxy. Cheaper than polling SNTP sync state directly
    // and doesn't depend on the Arduino-ESP32 SNTP API shape, which
    // has shifted between core versions.
    bool valid() const override {
        time_t now = ::time(nullptr);
        if (now < 1704067200) return false;   // 2024-01-01T00:00:00Z
        return true;
    }

    time_t wall_now() const override { return ::time(nullptr); }
    float  zone_offset_hours() const override { return zone_off_; }

private:
    float zone_off_;
};

#endif  // ARDUINO_ARCH_ESP32

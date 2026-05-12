#pragma once

// Daylight Saving Time helper. Computes whether DST is currently in
// effect under a configurable rule, given UTC time and the device's
// base (standard-time) UTC offset.
//
// Why a separate module: the math is small but worth testing in
// isolation (boundary days, year transitions, leap years), and it
// needs to be reachable from both platform shims (ESP32 today,
// Particle when that mirror lands).
//
// Rule enum mirrors the `dst_enabled` catalog values:
//   0 — disabled (caller should not consult this module; included as
//        a sentinel so a switch on the enum covers it cleanly)
//   1 — US: 2nd Sunday of March → 1st Sunday of November, transition
//        at 02:00 standard local time
//
// More rules (EU, AU, ...) plug in here as enum values + matching
// is_*_dst_active functions. Do NOT pre-stub them; add when a real
// deployment requires it. See TODO.md "Timezone offset + DST."

#include <time.h>
#include <stdint.h>


namespace critterchron {
namespace dst {

// Scoped (`enum class`) deliberately: arduino-esp32's
// `cores/esp32/esp32-hal-gpio.h` `#define`s `DISABLED` as `0x00` for
// interrupt-mode constants. An unscoped enum value `DISABLED` would
// get macro-replaced everywhere this header is pulled in and break
// the gpio.h enum too. Scoped values (`Rule::Disabled`) don't
// collide. Backing type pinned to int so KV `get_int` deserializes
// without a cast.
enum class Rule : int {
    Disabled = 0,
    US       = 1,
};

// Compute the UTC time_t of the Nth occurrence of `dow` (0=Sunday,
// 1=Monday, …) in (year, month_1_12), at `seconds_into_day_utc`
// seconds past midnight UTC.
//
// `nth` is 1-based: nth=1 returns the first matching day-of-week in
// the month, nth=2 the second, etc. `nth=-1` returns the LAST
// matching dow in the month (handy for EU rules later).
//
// Returns 0 on bogus inputs (month outside 1..12, dow outside 0..6,
// nth==0). Caller should treat 0 as "unknown — don't trust this
// result"; it's distinguishable from any reasonable transition
// moment (which is always >= 2024-01-01).
time_t nth_dow_of_month_utc(int year, int month_1_12, int dow_0_6,
                            int nth, int seconds_into_day_utc);

// Returns true if US-rule DST is in effect at `now_utc`, given that
// the device's STANDARD-time (no-DST) UTC offset is
// `base_offset_hours` (e.g. -5.0 for US-Eastern STD, -7.0 for
// US-Mountain STD).
//
// Bounds (UTC):
//   spring_forward = 2nd Sunday of March  @ 02:00 standard local
//   fall_back      = 1st Sunday of November @ 02:00 standard local
//
// Both moments are converted to UTC by subtracting `base_offset_hours`
// from 02:00 — that keeps all comparisons in UTC and avoids the
// chicken-and-egg "what's the local time during the missing hour"
// ambiguity at the transition itself.
bool is_us_dst_active(time_t now_utc, float base_offset_hours);

// Convenience dispatch by Rule. Returns 0.0 unless the rule is
// recognized AND DST is currently active under that rule, in which
// case returns +1.0 (the hours to add to the base offset).
//
// Future-proofs the caller — adding a new Rule value here is a
// one-line change that propagates everywhere.
float dst_adjustment_hours(Rule rule, time_t now_utc,
                           float base_offset_hours);

}  // namespace dst
}  // namespace critterchron

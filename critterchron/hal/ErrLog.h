#pragma once
#include <cstdint>

// Device-side error ring buffer feeding the telemetry heartbeat.
//
// Today, OTA failures (oversized blob, sha mismatch, parse error) and
// boot/reinit failures only surface as Log.error on serial — useless
// for remote troubleshooting on deployed devices. ErrLog captures the
// most recent few error strings on the device and the heartbeat
// publisher drains them, one per cycle, into the telemetry payload.
//
// Design contract:
//   * Producer-side: `g_errlog.record(cat, "fmt", args...)` from any
//     thread, any time. Internally vsnprintf'd into a 56-byte slot
//     plus also Log.error()'d to serial so the existing dev workflow
//     is unchanged.
//   * Consumer-side: telemetry_cycle() calls `peek_oldest_unsent()`
//     before publish and `mark_sent(millis)` after a successful HTTP
//     200. A failed publish leaves the entry queued for retry.
//   * Sized for ~256 B RAM total (4 entries × 64 B). Fits even on the
//     P1 with NO_IR_OTA — see the heartbeat ricardo/rico note in
//     TODO.md Completed for the RAM budget context.
//   * Wire format: ` err=<cat>:<msg>` appended to the heartbeat k=v
//     payload. One error per heartbeat (rate-limit; 10s heartbeats
//     drain a full ring in 40s, plenty fast for the use cases below).
//
// Threading: a single mutex guards the ring. Producers and consumers
// can race (telemetry thread writes ir_poll errors AND consumes the
// ring; main thread can hit boot/reinit errors). Critical section is
// tiny — a memcpy of one entry — so contention is irrelevant.

namespace critterchron {

enum class ErrCat : uint8_t {
    Other     = 0,
    OtaFetch  = 1,   // network, oversized, malformed, sha mismatch
    OtaApply  = 2,   // parse failed, reinit failed
    Boot      = 3,   // engine.begin() failed, rescue-hold trigger
    Sensor    = 4,   // future — light sensor calibration drift, etc.
    Net       = 5,   // WiFi/cloud reconnect kicks, transient connectivity events
};

// Single-token snake_case names for the heartbeat wire format. Server
// can split on `err=` and bucket without a schema change. Keep stable.
const char* err_cat_tag(ErrCat cat);

struct ErrEntry {
    // Monotonic per-instance sequence number. Used as the identity for
    // mark_sent — `millis` was tried first and broke when multiple
    // entries got recorded within the same millisecond (peek walks
    // oldest-first; mark_sent walks linearly; same-millis ties caused
    // mark_sent to flag the wrong entry, leaking some records).
    uint32_t seq;
    uint32_t millis;        // capture time, for log/diagnostic display
    ErrCat   cat;
    bool     sent;
    char     msg[56];       // NUL-terminated, hard-truncated
};

class ErrLog {
public:
    ErrLog();

    // printf-style. Truncates safely on overflow. Always pushes (drops
    // oldest if ring is full). Also emits to serial via Log.error so
    // dev workflow is unchanged.
    void record(ErrCat cat, const char* fmt, ...);

    // Copies the oldest unsent entry into `out`, returns true if one
    // exists. Doesn't mutate the ring — call mark_sent() afterward
    // *only on successful publish* so a network blip retries.
    bool peek_oldest_unsent(ErrEntry& out);

    // Mark an entry as sent by its sequence number. No-op if no match
    // (already overwritten or already sent — both fine). Use the `seq`
    // field of an entry returned by peek_oldest_unsent.
    void mark_sent(uint32_t seq);

    // For tests / dumps. Returns total entries ever recorded (wraps).
    uint32_t total_recorded() const { return total_; }

private:
    static constexpr int N = 4;
    ErrEntry ring_[N];
    uint8_t  head_  = 0;   // next write slot
    uint8_t  count_ = 0;   // number of valid entries (capped at N)
    uint32_t total_ = 0;

    // Platform-specific lock primitive. Defined in ErrLog.cpp behind
    // an #ifdef so the header stays clean. Use lock_guard pattern:
    // construct a Lock to acquire, destructor releases.
    class Lock { public: Lock(); ~Lock(); };
};

extern ErrLog g_errlog;

}  // namespace critterchron

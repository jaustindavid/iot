#pragma once

// SnapshotBuffer — on-device ring of recent engine state, dumped to a
// cloud topic when a trigger fires. Closes the loop with the §3
// analyzer's `dump_now` writeback path. See FAILURE_TRIAGE.md §1.
//
// Design choices:
//
// * **Static arena, no heap.** Frame storage is a compile-time
//   `g_arena[SNAPSHOT_BUFFER_BYTES]`. Number of slots actually used
//   is a runtime knob (`snapshot_buffer_frames` KV); 0 → fully
//   dormant, no append() cost, no publish path engaged. Knobbing
//   down is safe; knobbing up beyond the compile-time arena clamps.
//
// * **Generation-number trigger semantics for `dump_now`.** Operator
//   or analyzer writes any nonzero value; device dumps once and
//   remembers the value. Subsequent same-value reads no-op; new
//   value (smaller, larger, rolled-over — any change) dumps again.
//   `0` is the rearm primitive: clears the device's last-seen so
//   the same number can be re-fired. Avoids needing device-side
//   KV-write capability.
//
// * **ASCII wire format.** Same `k=v k=v` shape as the existing
//   heartbeat, so the analyzer's `parse_heartbeat` works on both
//   header and frames. One publish per dump, multi-line body:
//
//     v=1 device=X ts=Y trigger=Z detail=... frames=N
//     tick=A millis=B agents=C heap=D seeks_d=E phys_max=F rend_max=G
//     tick=A+1 ...
//     ...
//
//   Costs ~60% more bytes than msgpack but costs zero new encoder
//   code in firmware and is human-readable on the wire.
//
// * **Cross-thread shape.** append() and evaluate() are called from
//   the engine thread (each physics tick). dump_pending() / encode()
//   / clear_pending() are called from the telemetry thread. The
//   single shared mutable state is `dump_pending_` (one volatile
//   bool, single-writer/single-reader) and the ring (single-writer
//   from engine, single-reader from tel-on-dump). When a dump fires
//   the ring may continue being written by engine while tel encodes;
//   in the worst case the oldest frames in the dump are slightly
//   stale (overwritten by ring wraparound mid-encode). 32 frames at
//   8Hz = 4 seconds; one TCP publish < 1s; acceptable race.
//
// * **Compile-time elision.** Per-device headers can `#define
//   NO_SNAPSHOT_BUFFER` to drop the entire feature from the binary.
//   Used on the OG Photon (rico) where flash + static-RAM cost of
//   even-dormant code matters. Same idiom as `NO_IR_OTA`.

#include <stdint.h>
#include <stddef.h>


namespace critterchron {
namespace snapshot {

#ifdef NO_SNAPSHOT_BUFFER

// Stub everything to no-ops so call sites in the .ino/.cpp don't have
// to be #ifdef'd. Compiler optimizes the calls away entirely.
inline void configure(uint16_t /*frames*/) {}
inline void append(uint32_t /*tick*/, uint32_t /*millis*/,
                   uint16_t /*agents*/, uint32_t /*heap_free*/,
                   uint32_t /*seeks_fail_total*/,
                   uint32_t /*phys_max_us*/, uint32_t /*rend_max_us*/) {}
inline void set_thresholds(int /*seeks_spike*/, int /*heap_low*/,
                           int /*agent_drop_pct*/) {}
inline void check_dump_now_kv(int /*kv_value*/) {}
inline bool dump_pending() { return false; }
inline size_t encode(char* /*out*/, size_t /*cap*/,
                     const char* /*device*/, uint32_t /*now_unix*/) { return 0; }
inline void clear_pending() {}
inline void force_dump(const char* /*trigger*/, const char* /*detail*/) {}
inline const char* pending_trigger() { return ""; }

#else  // !NO_SNAPSHOT_BUFFER

// Compile-time upper bound on per-frame storage. Overridable per
// device. ~80 ASCII bytes is comfortable headroom for the v1 frame
// shape; bump if v2 adds per-agent state.
#ifndef SNAPSHOT_FRAME_BYTES
#define SNAPSHOT_FRAME_BYTES 96
#endif

// Compile-time upper bound on number of frames. Runtime
// `snapshot_buffer_frames` KV picks the active count, clamped to
// this maximum. Total static RAM cost = SNAPSHOT_FRAME_BYTES *
// SNAPSHOT_MAX_FRAMES bytes. Default sized for the doc's "32 frames
// = 4 seconds at 8Hz" target.
#ifndef SNAPSHOT_MAX_FRAMES
#define SNAPSHOT_MAX_FRAMES 32
#endif

// Configure ring depth from the runtime KV value. Safe to call any
// time; clamps to [0, SNAPSHOT_MAX_FRAMES]. 0 disables append() and
// trigger evaluation entirely.
void configure(uint16_t frames);

// Per-device trigger thresholds. Pull from KV in the tel cycle
// alongside other knobs and push them in. -1 on any field means
// "leave current value." Defaults baked in (see .cpp).
void set_thresholds(int seeks_spike_per_sec,
                    int heap_low,
                    int agent_drop_pct);

// Engine-thread call: record one frame. Cheap when configured frames
// == 0 (early return, no work).
//
// `seeks_fail_total` is the running engine counter (not a delta); the
// snapshot module computes the per-tick delta internally and stores
// that. Same for the cumulative tick counter.
void append(uint32_t tick, uint32_t millis,
            uint16_t agents, uint32_t heap_free,
            uint32_t seeks_fail_total,
            uint32_t phys_max_us, uint32_t rend_max_us);

// Engine-thread call (or tel-thread call from the dump_now path):
// fire the dump request if `kv_value` represents a new generation.
// Generation-number semantics:
//   * kv_value == 0 → clear last_seen (rearm). No dump.
//   * kv_value != 0 AND kv_value != last_seen → dump pending; set
//                                                last_seen = kv_value.
//   * else → no-op.
void check_dump_now_kv(int kv_value);

// Tel-thread call: is there a dump waiting to be encoded + published?
bool dump_pending();

// Tel-thread call: encode the pending dump into `out`. Returns bytes
// written (excluding NUL terminator), or 0 if nothing to encode or
// `cap` was too small. After encode, caller should publish then
// invoke clear_pending().
size_t encode(char* out, size_t cap,
              const char* device, uint32_t now_unix);

// Tel-thread call: mark the pending dump as handled. Idempotent.
void clear_pending();

// Trigger names the encoder writes into the header `trigger=` field.
// Public so callers (e.g. an explicit `force_dump` path) can pass a
// recognizable label.
namespace trigger_name {
constexpr const char* MANUAL            = "manual";
constexpr const char* SEEKS_FAIL_SPIKE  = "seeks_fail_spike";
constexpr const char* HEAP_LOW          = "heap_low";
constexpr const char* AGENT_COUNT_DROP  = "agent_count_drop";
constexpr const char* ASSERTION         = "assertion";
// FAILURE_TRIAGE.md §2: continuous trace-mode dumps. Same frame format
// as event-driven dumps; platform code checks this label after
// dump_pending() to decide whether to publish to the per-device trace
// topic instead of the shared snapshot topic.
constexpr const char* TRACE             = "trace";
}  // namespace trigger_name

// Force a dump with a caller-supplied trigger label + detail string.
// Useful for explicit asserts / external trigger paths.
void force_dump(const char* trigger, const char* detail);

// Inspect the currently-pending dump's trigger label. Returns a
// pointer to the internal NUL-terminated buffer (zero-length string
// if no dump is pending). Lets the publish-path route to a different
// topic for trace-mode dumps without snapshot needing to know about
// platform topics.
const char* pending_trigger();

#endif  // NO_SNAPSHOT_BUFFER

}  // namespace snapshot
}  // namespace critterchron

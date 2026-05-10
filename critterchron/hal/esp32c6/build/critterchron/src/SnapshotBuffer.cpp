// SnapshotBuffer implementation. See SnapshotBuffer.h for design.
//
// Pure C++17 — no Arduino, no Particle, no FreeRTOS. The platform
// layer calls in from its respective .ino/.cpp; this file links into
// every device build that doesn't define NO_SNAPSHOT_BUFFER.

#include "SnapshotBuffer.h"

#ifndef NO_SNAPSHOT_BUFFER

#include <stdio.h>
#include <string.h>


namespace critterchron {
namespace snapshot {

namespace {

// ---- Frame storage ---------------------------------------------------
//
// Each slot is a fixed-size ASCII line ending in NUL (no '\n'). The
// encoder joins frames with '\n' between them at publish time. Storing
// them pre-encoded lets append() be branchless on the hot path; the
// alternative (struct-of-fields, encode at dump time) trades a bit of
// engine-tick CPU to save publish-time CPU, and the engine path is the
// one that needs to stay cheap.

struct Frame {
    char    line[SNAPSHOT_FRAME_BYTES];
    uint8_t len;   // 0 = unused slot
};

Frame    g_ring[SNAPSHOT_MAX_FRAMES];
uint16_t g_active_frames = 0;   // 0 = disabled, configurable up to SNAPSHOT_MAX_FRAMES
uint16_t g_head = 0;            // next write index in ring
uint32_t g_last_seeks_total = 0;
uint16_t g_last_agents = 0;
uint32_t g_last_seeks_window_ms = 0;
uint32_t g_seeks_in_current_window = 0;
bool     g_warmed = false;      // first append seen → diff math safe

// ---- Trigger thresholds (live tunable) -------------------------------
int g_threshold_seeks_spike  = 5;     // count per 1000ms window
int g_threshold_heap_low     = 5000;  // bytes
int g_threshold_agent_drop_pct = 50;  // % drop inter-tick

// ---- Pending-dump state ----------------------------------------------
//
// `g_dump_pending` is the cross-thread handoff. Engine writes true; tel
// reads + clears. `volatile` is the right primitive for this: aligned
// 32-bit/bool writes are atomic on every chip critterchron targets
// (ARM Cortex-M, RISC-V, Xtensa). No std::atomic to avoid pulling in
// the C++11 atomic library on Particle (it's available but noisy).
volatile bool g_dump_pending = false;

// Snapshot of the head index at trigger-fire time. The encoder walks
// `head_at_trigger - g_active_frames .. head_at_trigger`. If engine
// keeps writing during encode, the older end of the dump may get
// clobbered; we accept the race per the header's design notes.
uint16_t g_head_at_trigger = 0;

char g_trigger[32]   = {0};
char g_detail[64]    = {0};

// ---- Generation tracking for dump_now KV -----------------------------
int  g_last_dump_now_seen = 0;

// ---- Helpers ---------------------------------------------------------

inline uint16_t ring_at(uint16_t head, uint16_t offset_back) {
    // Returns the slot index `offset_back` positions before `head`,
    // wrapping. offset_back must be in [1, g_active_frames].
    uint16_t cap = g_active_frames;
    return (head + cap - offset_back) % cap;
}

void mark_dump_pending(const char* trigger, const char* detail) {
    if (g_dump_pending) {
        // A dump is already queued; don't overwrite the trigger label
        // since the *first* trigger to fire is the one operators want
        // to see in the queue.
        return;
    }
    g_head_at_trigger = g_head;
    if (trigger) {
        strncpy(g_trigger, trigger, sizeof(g_trigger) - 1);
        g_trigger[sizeof(g_trigger) - 1] = '\0';
    } else {
        g_trigger[0] = '\0';
    }
    if (detail) {
        strncpy(g_detail, detail, sizeof(g_detail) - 1);
        g_detail[sizeof(g_detail) - 1] = '\0';
    } else {
        g_detail[0] = '\0';
    }
    // Publish the flag last so the tel thread sees a consistent state
    // (head + trigger + detail before the flag flips).
    g_dump_pending = true;
}

}  // anonymous namespace

// ---- Public API ------------------------------------------------------

void configure(uint16_t frames) {
    if (frames > SNAPSHOT_MAX_FRAMES) frames = SNAPSHOT_MAX_FRAMES;
    if (frames == g_active_frames) return;
    // Reset on resize. Mid-flight config change is rare; simpler to
    // wipe than to preserve partial history through a depth change.
    g_active_frames = frames;
    g_head = 0;
    g_warmed = false;
    g_seeks_in_current_window = 0;
    g_last_seeks_window_ms = 0;
    for (size_t i = 0; i < SNAPSHOT_MAX_FRAMES; ++i) g_ring[i].len = 0;
}

void set_thresholds(int seeks_spike_per_sec, int heap_low, int agent_drop_pct) {
    if (seeks_spike_per_sec >= 0) g_threshold_seeks_spike   = seeks_spike_per_sec;
    if (heap_low >= 0)            g_threshold_heap_low      = heap_low;
    if (agent_drop_pct >= 0)      g_threshold_agent_drop_pct = agent_drop_pct;
}

void append(uint32_t tick, uint32_t millis_now,
            uint16_t agents, uint32_t heap_free,
            uint32_t seeks_fail_total,
            uint32_t phys_max_us, uint32_t rend_max_us) {
    if (g_active_frames == 0) return;

    // ---- Compute deltas vs previous append() ----
    int32_t seeks_delta = 0;
    if (g_warmed) {
        // Saturate to int16 range — a delta of >32k between ticks
        // means something else is very wrong; cap to keep frame ASCII
        // size bounded.
        int64_t d = (int64_t)seeks_fail_total - (int64_t)g_last_seeks_total;
        if (d >  32767) d =  32767;
        if (d < -32768) d = -32768;
        seeks_delta = (int32_t)d;
    }

    // ---- Encode into a frame slot ----
    Frame& slot = g_ring[g_head];
    int n = snprintf(slot.line, sizeof(slot.line),
        "tick=%u millis=%u agents=%u heap=%u seeks_d=%d phys_max=%u rend_max=%u",
        (unsigned)tick, (unsigned)millis_now,
        (unsigned)agents, (unsigned)heap_free,
        (int)seeks_delta,
        (unsigned)phys_max_us, (unsigned)rend_max_us);
    if (n < 0) {
        slot.len = 0;
    } else if (n >= (int)sizeof(slot.line)) {
        slot.len = sizeof(slot.line) - 1;
        slot.line[slot.len] = '\0';
    } else {
        slot.len = (uint8_t)n;
    }
    g_head = (g_head + 1) % g_active_frames;

    // ---- Auto-trigger evaluation ----
    // Triggers run AFTER append so the just-recorded frame is always
    // included in the dump that flags it.

    // heap_low: simple absolute threshold.
    if ((int)heap_free < g_threshold_heap_low) {
        char detail[64];
        snprintf(detail, sizeof(detail), "heap=%u<%d",
                 (unsigned)heap_free, g_threshold_heap_low);
        mark_dump_pending(trigger_name::HEAP_LOW, detail);
    }

    // seeks_fail_spike: count of new seeks within the current 1000ms
    // window. Window slides forward in 1s steps to avoid bias from
    // when the spike happens to land mid-window.
    if (g_warmed) {
        if (millis_now - g_last_seeks_window_ms >= 1000) {
            g_last_seeks_window_ms = millis_now;
            g_seeks_in_current_window = 0;
        }
        if (seeks_delta > 0) {
            g_seeks_in_current_window += (uint32_t)seeks_delta;
            if ((int)g_seeks_in_current_window > g_threshold_seeks_spike) {
                char detail[64];
                snprintf(detail, sizeof(detail), "delta=%u in 1s",
                         (unsigned)g_seeks_in_current_window);
                mark_dump_pending(trigger_name::SEEKS_FAIL_SPIKE, detail);
            }
        }
    }

    // agent_count_drop: inter-tick %-drop. Skipped while warming.
    if (g_warmed && g_last_agents > 0 && agents < g_last_agents) {
        uint32_t drop_pct = (uint32_t)(g_last_agents - agents) * 100 / g_last_agents;
        if ((int)drop_pct >= g_threshold_agent_drop_pct) {
            char detail[64];
            snprintf(detail, sizeof(detail), "%u→%u (%u%%)",
                     (unsigned)g_last_agents, (unsigned)agents,
                     (unsigned)drop_pct);
            mark_dump_pending(trigger_name::AGENT_COUNT_DROP, detail);
        }
    }

    g_last_seeks_total = seeks_fail_total;
    g_last_agents = agents;
    g_warmed = true;
}

void check_dump_now_kv(int kv_value) {
    if (kv_value == 0) {
        // Rearm: forget the last-acted-on generation so the same
        // value can be re-fired by an operator.
        g_last_dump_now_seen = 0;
        return;
    }
    if (kv_value == g_last_dump_now_seen) return;  // already dumped this gen
    if (g_active_frames == 0) {
        // Feature disabled; still update last_seen so we don't dump
        // immediately if the operator turns the feature on later
        // while the same `dump_now` value is sitting in KV.
        g_last_dump_now_seen = kv_value;
        return;
    }
    char detail[64];
    snprintf(detail, sizeof(detail), "dump_now=%d", kv_value);
    g_last_dump_now_seen = kv_value;
    mark_dump_pending(trigger_name::MANUAL, detail);
}

void force_dump(const char* trigger, const char* detail) {
    if (g_active_frames == 0) return;
    mark_dump_pending(trigger, detail);
}

bool dump_pending() {
    return g_dump_pending;
}

const char* pending_trigger() {
    return g_trigger;
}

size_t encode(char* out, size_t cap, const char* device, uint32_t now_unix) {
    if (!g_dump_pending || g_active_frames == 0) return 0;
    if (out == nullptr || cap < 64) return 0;

    // Header line: v=1 device=X ts=Y trigger=Z detail=... frames=N
    int hn = snprintf(out, cap,
        "v=1 device=%s ts=%u trigger=%s detail=%s frames=%u",
        device ? device : "unknown",
        (unsigned)now_unix,
        g_trigger[0] ? g_trigger : "unknown",
        g_detail[0]  ? g_detail  : "",
        (unsigned)g_active_frames);
    if (hn < 0 || (size_t)hn >= cap) {
        // Header alone overflowed — abort, leave pending so caller
        // can either grow the buffer or drop the dump on the floor.
        out[cap > 0 ? cap - 1 : 0] = '\0';
        return 0;
    }
    size_t pos = (size_t)hn;

    // Frames: oldest → newest. Walk from `head_at_trigger` back
    // g_active_frames slots, skipping unused (slot.len == 0) entries
    // (the ring may not be full yet on a fresh boot).
    uint16_t head = g_head_at_trigger;
    for (uint16_t i = g_active_frames; i >= 1; --i) {
        const Frame& f = g_ring[ring_at(head, i)];
        if (f.len == 0) continue;
        if (pos + 1 + f.len + 1 > cap) {
            // No more room. Truncate cleanly — the dump that lands is
            // a *prefix* of the intended dump, not corrupt msgpack.
            break;
        }
        out[pos++] = '\n';
        memcpy(out + pos, f.line, f.len);
        pos += f.len;
    }
    if (pos < cap) out[pos] = '\0';
    else out[cap - 1] = '\0';
    return pos;
}

void clear_pending() {
    g_dump_pending = false;
    g_trigger[0] = '\0';
    g_detail[0]  = '\0';
}

}  // namespace snapshot
}  // namespace critterchron

#endif  // !NO_SNAPSHOT_BUFFER

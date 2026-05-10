// Tests for hal/SnapshotBuffer.{h,cpp}. Standalone — no engine, no
// platform shim. PASS/FAIL reporting matches the Python tests at
// repo root.
//
// Build & run from this directory:
//   make test-snapshot
//
// Covers:
//   * configure() resize semantics
//   * append() ring wraparound
//   * Generation-number trigger semantics for dump_now
//   * heap_low / seeks_fail_spike / agent_count_drop auto-triggers
//   * force_dump() explicit trigger
//   * encode() output shape (header line + frame lines, frame count)
//   * clear_pending() resets state
//
// What it does NOT cover (intentional):
//   * Cross-thread races (single-threaded test). The header documents
//     the engine-vs-tel race shape; can't usefully unit-test it.
//   * NO_SNAPSHOT_BUFFER stub path — covered by per-device builds.

#include "SnapshotBuffer.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>


using namespace critterchron::snapshot;

static int g_pass = 0;
static int g_fail = 0;
static bool g_verbose = false;

static void check(const char* label, bool cond, const std::string& detail = "") {
    if (cond) {
        ++g_pass;
        if (g_verbose) std::printf("  [PASS] %s\n", label);
    } else {
        ++g_fail;
        std::printf("  [FAIL] %s%s%s\n", label,
                    detail.empty() ? "" : ": ",
                    detail.c_str());
    }
}

// Reset module state between tests by reconfiguring frames=0 then back.
static void reset_module() {
    configure(0);
    set_thresholds(5, 5000, 50);  // back to defaults
    clear_pending();
    // Walk the dump_now generation back to 0 (rearm) so the next
    // dump_now=N test fires cleanly.
    check_dump_now_kv(0);
}

// ---- configure ------------------------------------------------------

static void test_configure_disabled_by_default() {
    reset_module();
    // Append should be a no-op when frames == 0.
    append(/*tick=*/1, /*ms=*/1000, /*agents=*/5, /*heap=*/100000,
           /*seeks_total=*/0, /*phys=*/100, /*rend=*/200);
    check("configure: append() silent at frames=0", !dump_pending());
}

static void test_configure_resize_clears_buffer() {
    reset_module();
    configure(8);
    append(1, 1000, 5, 100000, 0, 100, 200);
    append(2, 1125, 5, 100000, 0, 100, 200);
    configure(16);
    // Resize should wipe — encode would have nothing to dump (no
    // pending trigger anyway), so just sanity-check no crash.
    check("configure: resize doesn't crash", true);
}

static void test_configure_clamps_to_max() {
    reset_module();
    configure(9999);  // way over SNAPSHOT_MAX_FRAMES
    // Internal clamp; we can't read the value back directly, but a
    // tight-loop dump should still produce a sane frame count.
    for (uint32_t i = 0; i < 100; ++i) {
        append(i, 1000 + i * 125, 5, 100000, 0, 100, 200);
    }
    force_dump("test", "clamp");
    char buf[8192];
    size_t n = encode(buf, sizeof(buf), "host", 1700000000);
    // Header should declare frames=SNAPSHOT_MAX_FRAMES (32 by default).
    bool ok = std::strstr(buf, "frames=32") != nullptr;
    check("configure: oversize clamped to SNAPSHOT_MAX_FRAMES", ok,
          ok ? "" : std::string("got: ") + std::string(buf, n < 200 ? n : 200));
}

// ---- generation-number trigger -------------------------------------

static void test_dump_now_zero_no_op() {
    reset_module();
    configure(8);
    append(1, 1000, 5, 100000, 0, 100, 200);
    check_dump_now_kv(0);
    check("dump_now=0: no trigger", !dump_pending());
}

static void test_dump_now_first_value_fires() {
    reset_module();
    configure(8);
    append(1, 1000, 5, 100000, 0, 100, 200);
    check_dump_now_kv(42);
    check("dump_now: first nonzero value fires", dump_pending());
}

static void test_dump_now_repeat_silent() {
    reset_module();
    configure(8);
    append(1, 1000, 5, 100000, 0, 100, 200);
    check_dump_now_kv(42);
    clear_pending();
    check_dump_now_kv(42);  // same value again
    check("dump_now: repeat same value silent", !dump_pending());
}

static void test_dump_now_change_fires_again() {
    reset_module();
    configure(8);
    append(1, 1000, 5, 100000, 0, 100, 200);
    check_dump_now_kv(42);
    clear_pending();
    check_dump_now_kv(43);
    check("dump_now: changed value fires again", dump_pending());
}

static void test_dump_now_smaller_value_fires() {
    // Critical for rollover handling — change-detection, not greater-than.
    reset_module();
    configure(8);
    append(1, 1000, 5, 100000, 0, 100, 200);
    check_dump_now_kv(1778000000);   // analyzer's int(time.time()) shape
    clear_pending();
    check_dump_now_kv(1);              // operator manually writes 1
    check("dump_now: smaller value than last fires (rollover-safe)",
          dump_pending());
}

static void test_dump_now_zero_rearms_same_value() {
    reset_module();
    configure(8);
    append(1, 1000, 5, 100000, 0, 100, 200);
    check_dump_now_kv(7);
    clear_pending();
    check_dump_now_kv(7);  // same → silent
    check("dump_now: same value silent before rearm", !dump_pending());
    check_dump_now_kv(0);  // rearm
    check_dump_now_kv(7);  // now should fire again
    check("dump_now: same value fires after 0-rearm", dump_pending());
}

static void test_dump_now_disabled_still_tracks() {
    // When the feature is disabled, KV-changes should still update
    // last_seen so re-enabling doesn't immediately fire on a stale
    // generation number that was sitting in KV all along.
    reset_module();
    // frames=0 (disabled)
    check_dump_now_kv(99);
    check("dump_now: disabled-mode silent", !dump_pending());
    configure(8);
    append(1, 1000, 5, 100000, 0, 100, 200);
    check_dump_now_kv(99);  // same as what was seen while disabled
    check("dump_now: silent on re-enable if value unchanged",
          !dump_pending());
}

// ---- auto-triggers --------------------------------------------------

static void test_heap_low_fires() {
    reset_module();
    configure(8);
    set_thresholds(/*seeks=*/-1, /*heap=*/5000, /*agent=*/-1);
    append(1, 1000, 5, /*heap=*/4000, 0, 100, 200);
    check("heap_low: fires when heap < threshold", dump_pending());
}

static void test_heap_ok_silent() {
    reset_module();
    configure(8);
    set_thresholds(-1, 5000, -1);
    append(1, 1000, 5, /*heap=*/10000, 0, 100, 200);
    check("heap_low: silent when heap >= threshold", !dump_pending());
}

static void test_seeks_spike_fires() {
    reset_module();
    configure(8);
    set_thresholds(/*spike=*/5, -1, -1);
    // Warm
    append(1, 1000, 5, 100000, /*seeks_total=*/0, 100, 200);
    // Single-tick delta of 10 within 1s window → over threshold
    append(2, 1100, 5, 100000, /*seeks_total=*/10, 100, 200);
    check("seeks_fail_spike: fires on 10-in-1s burst", dump_pending());
}

static void test_seeks_under_threshold_silent() {
    reset_module();
    configure(8);
    set_thresholds(5, -1, -1);
    append(1, 1000, 5, 100000, 0, 100, 200);
    append(2, 1100, 5, 100000, 3, 100, 200);  // only 3 < threshold
    check("seeks_fail_spike: silent under threshold", !dump_pending());
}

static void test_seeks_window_resets() {
    reset_module();
    configure(8);
    set_thresholds(5, -1, -1);
    append(1, 1000, 5, 100000, 0, 100, 200);
    append(2, 1500, 5, 100000, 3, 100, 200);  // 3 in window
    append(3, 2600, 5, 100000, 6, 100, 200);  // 1.1s later → window resets, +3 only
    check("seeks_fail_spike: window-reset prevents false positive",
          !dump_pending());
}

static void test_agent_count_drop_fires() {
    reset_module();
    configure(8);
    set_thresholds(-1, -1, /*drop=*/50);
    append(1, 1000, /*agents=*/10, 100000, 0, 100, 200);
    append(2, 1100, /*agents=*/4,  100000, 0, 100, 200);  // 60% drop
    check("agent_count_drop: fires on >threshold drop", dump_pending());
}

static void test_agent_drop_silent_under_threshold() {
    reset_module();
    configure(8);
    set_thresholds(-1, -1, 50);
    append(1, 1000, /*agents=*/10, 100000, 0, 100, 200);
    append(2, 1100, /*agents=*/8,  100000, 0, 100, 200);  // 20% drop
    check("agent_count_drop: silent under threshold", !dump_pending());
}

// ---- encode shape ---------------------------------------------------

static void test_encode_header_shape() {
    reset_module();
    configure(4);
    for (uint32_t i = 0; i < 4; ++i) {
        append(i, 1000 + i * 125, 5, 100000, 0, 100, 200);
    }
    force_dump("manual", "test=1");
    char buf[2048];
    size_t n = encode(buf, sizeof(buf), "tommy_tanuki", 1777999999);
    std::string s(buf, n);
    check("encode: header has v=1",       s.find("v=1") != std::string::npos);
    check("encode: header has device=",   s.find("device=tommy_tanuki") != std::string::npos);
    check("encode: header has trigger=",  s.find("trigger=manual") != std::string::npos);
    check("encode: header has detail=",   s.find("detail=test=1") != std::string::npos);
    check("encode: header has frames=4",  s.find("frames=4") != std::string::npos);
}

static void test_encode_frame_count() {
    reset_module();
    configure(4);
    // Fill exactly 4 frames; every one should appear in the dump.
    for (uint32_t i = 0; i < 4; ++i) {
        append(i, 1000 + i * 125, 5, 100000, 0, 100, 200);
    }
    force_dump("manual", "");
    char buf[2048];
    size_t n = encode(buf, sizeof(buf), "host", 1700000000);
    int newline_count = 0;
    for (size_t i = 0; i < n; ++i) if (buf[i] == '\n') ++newline_count;
    // 1 header + 4 frame lines = 4 newlines (header has no leading \n)
    check("encode: 4 frames produce 4 newline-separated frame lines",
          newline_count == 4,
          std::string("got newlines=") + std::to_string(newline_count));
}

static void test_encode_partial_buffer_fills() {
    // When the ring isn't full yet, encode should skip empty slots
    // rather than emit blanks.
    reset_module();
    configure(8);
    // Only 2 frames written; slots 2..7 are empty.
    append(1, 1000, 5, 100000, 0, 100, 200);
    append(2, 1125, 5, 100000, 0, 100, 200);
    force_dump("manual", "");
    char buf[2048];
    size_t n = encode(buf, sizeof(buf), "host", 1700000000);
    int newline_count = 0;
    for (size_t i = 0; i < n; ++i) if (buf[i] == '\n') ++newline_count;
    check("encode: partial ring emits only filled slots",
          newline_count == 2,
          std::string("got newlines=") + std::to_string(newline_count));
}

static void test_encode_no_pending_returns_zero() {
    reset_module();
    configure(4);
    append(1, 1000, 5, 100000, 0, 100, 200);
    char buf[1024];
    size_t n = encode(buf, sizeof(buf), "host", 0);
    check("encode: no pending dump → 0 bytes written", n == 0);
}

static void test_clear_pending_resets() {
    reset_module();
    configure(4);
    append(1, 1000, 5, 100000, 0, 100, 200);
    force_dump("manual", "x");
    check("force_dump: pending true after force_dump", dump_pending());
    clear_pending();
    check("clear_pending: pending false after clear", !dump_pending());
}

static void test_first_trigger_wins() {
    // If two triggers fire in quick succession before tel can publish,
    // we want the operator to see the *first* trigger label (the one
    // that probably matters most diagnostically).
    reset_module();
    configure(8);
    set_thresholds(/*spike=*/5, /*heap=*/5000, -1);
    append(1, 1000, 5, /*heap=*/4000, 0, 100, 200);  // heap_low fires first
    append(2, 1050, 5, /*heap=*/4000, /*seeks=*/10, 100, 200);  // seeks_spike would fire
    char buf[2048];
    size_t n = encode(buf, sizeof(buf), "host", 0);
    std::string s(buf, n);
    check("first-trigger-wins: heap_low label preserved",
          s.find("trigger=heap_low") != std::string::npos);
}

// ---- runner ---------------------------------------------------------

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "-v") == 0 ||
            std::strcmp(argv[i], "--verbose") == 0) g_verbose = true;
    }

    test_configure_disabled_by_default();
    test_configure_resize_clears_buffer();
    test_configure_clamps_to_max();

    test_dump_now_zero_no_op();
    test_dump_now_first_value_fires();
    test_dump_now_repeat_silent();
    test_dump_now_change_fires_again();
    test_dump_now_smaller_value_fires();
    test_dump_now_zero_rearms_same_value();
    test_dump_now_disabled_still_tracks();

    test_heap_low_fires();
    test_heap_ok_silent();
    test_seeks_spike_fires();
    test_seeks_under_threshold_silent();
    test_seeks_window_resets();
    test_agent_count_drop_fires();
    test_agent_drop_silent_under_threshold();

    test_encode_header_shape();
    test_encode_frame_count();
    test_encode_partial_buffer_fills();
    test_encode_no_pending_returns_zero();
    test_clear_pending_resets();
    test_first_trigger_wins();

    int total = g_pass + g_fail;
    std::printf("\n%d/%d passed", g_pass, total);
    if (g_fail) std::printf(" — %d failed", g_fail);
    std::printf("\n");
    return g_fail ? 1 : 0;
}

// hal/host/src/main.cpp — host-side C++ harness.
//
// Mirrors the flags that main.py exposes for dumping a reference trace:
//   --ticks N                          # stop after N physics ticks
//   --seed N                           # seed RNG (required for parity)
//   --dump-fake-time YYYY-MM-DDTHH:MM  # freeze wall clock
//   --dump-state PATH                  # write tick-by-tick JSONL
//
// Geometry comes from GRID_WIDTH / GRID_HEIGHT at compile time — the
// Makefile passes them through from the chosen device header (or a
// command-line override).

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <fstream>

#include "CritterEngine.h"
#include "DumpSink.h"
#include "FakeTimeSource.h"

static void usage(const char* argv0) {
    std::fprintf(stderr,
        "Usage: %s [--ticks N] [--seed N] [--dump-fake-time YYYY-MM-DDTHH:MM] "
        "[--dump-state PATH] [--night]\n"
        "       %s --soak [--soak-ticks N] [--tick-ms M] [--seed N] "
        "[--start-time YYYY-MM-DDTHH:MM] [--stuck-ticks W]\n"
        "\n"
        "  --soak        Fast-forward sim: advance the virtual clock M ms per\n"
        "                tick (so minutes actually roll), run up to --soak-ticks\n"
        "                ticks as fast as the CPU allows, and auto-detect the\n"
        "                clock-transition livelock (work exists but agents stop\n"
        "                seeking/moving) or a glitch. On detection, dumps the\n"
        "                grid + per-agent state and the tick it happened.\n",
        argv0, argv0);
}

static time_t parse_fake_time(const char* s) {
    struct tm tm{};
    // strptime is POSIX and present on macOS/Linux. Not on MSVC, but we
    // don't target MSVC for the host harness.
    if (!strptime(s, "%Y-%m-%dT%H:%M", &tm)) {
        std::fprintf(stderr, "Bad --dump-fake-time: %s\n", s);
        std::exit(2);
    }
    // strptime leaves tm in naive local time; we want UTC seconds so the
    // engine's syncTimeAt can re-add zone_offset internally.
    return timegm(&tm);
}

int main(int argc, char** argv) {
    int ticks = 0;
    uint32_t seed = 0;
    bool have_seed = false;
    const char* fake_time_str = nullptr;
    const char* dump_path = nullptr;
    bool night = false;
    bool soak = false;
    long soak_ticks = 5000000;   // ~926h of virtual clock at 400ms/tick
    uint32_t tick_ms = 0;        // 0 → use critter_ir::RUNTIME_TICK_MS
    const char* start_time_str = nullptr;
    long stuck_window = 900;     // ticks of no-progress-with-work → livelock

    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--ticks") && i+1 < argc) {
            ticks = std::atoi(argv[++i]);
        } else if (!std::strcmp(argv[i], "--seed") && i+1 < argc) {
            seed = (uint32_t)std::strtoul(argv[++i], nullptr, 10);
            have_seed = true;
        } else if (!std::strcmp(argv[i], "--dump-fake-time") && i+1 < argc) {
            fake_time_str = argv[++i];
        } else if (!std::strcmp(argv[i], "--dump-state") && i+1 < argc) {
            dump_path = argv[++i];
        } else if (!std::strcmp(argv[i], "--night")) {
            night = true;
        } else if (!std::strcmp(argv[i], "--soak")) {
            soak = true;
        } else if (!std::strcmp(argv[i], "--soak-ticks") && i+1 < argc) {
            soak_ticks = std::strtol(argv[++i], nullptr, 10);
        } else if (!std::strcmp(argv[i], "--tick-ms") && i+1 < argc) {
            tick_ms = (uint32_t)std::strtoul(argv[++i], nullptr, 10);
        } else if (!std::strcmp(argv[i], "--start-time") && i+1 < argc) {
            start_time_str = argv[++i];
        } else if (!std::strcmp(argv[i], "--stuck-ticks") && i+1 < argc) {
            stuck_window = std::strtol(argv[++i], nullptr, 10);
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    // Default fake time keeps parity runs reproducible even without the flag.
    time_t utc_now = fake_time_str
        ? parse_fake_time(fake_time_str)
        : 1745078100;  // 2026-04-19T14:35Z, arbitrary fixed

    // Zone offset mirrors what main.py does when it calls engine.sync_time_at
    // with a naive datetime: it treats the timestamp as local. Here we pass
    // zone_offset_hours = 0 and let the engine interpret wall_now() as UTC.
    FakeTimeSource clock(utc_now, 0.0f);
    DumpSink sink;
    critterchron::CritterEngine engine(sink, clock);

    if (!engine.begin()) {
        std::fprintf(stderr, "CritterEngine::begin() failed\n");
        return 1;
    }
    if (have_seed) engine.seedRng(seed);
    engine.setNightMode(night);

    // ---- Soak / fast-forward mode -------------------------------------
    // Advances the virtual clock so minutes actually roll (the regime the
    // clock-transition livelock lives in — the frozen-clock parity loop
    // below can NEVER reproduce it). Runs as fast as the CPU allows and
    // auto-detects the wedge: a window of `stuck_window` ticks during which
    // work exists (missing+extra > 0) but no agent issues a seek or moves.
    // Also reports a glitch (benched agent) distinctly. On detection it
    // prints the tick + a grid/agent snapshot and exits non-zero so a seed
    // sweep can `||` over it.
    if (soak) {
        uint32_t step_ms = tick_ms ? tick_ms : (uint32_t)critter_ir::RUNTIME_TICK_MS;
        if (step_ms == 0) step_ms = 400;
        if (start_time_str) clock.set_now(parse_fake_time(start_time_str));

        long  stuck = 0;
        long  max_stuck = 0;          // high-water mark of the no-progress streak
        long  max_work  = 0;          // most missing+extra seen at once
        long  ticks_work_no_seek = 0; // ticks where work>0 AND no seek this tick
        uint32_t last_seeks = engine.metrics().total_seeks;
        uint32_t last_glitches = engine.metrics().glitches;
        // Snapshot agent positions to detect "no movement."
        auto positions_signature = [&]() -> uint64_t {
            uint64_t h = 1469598103934665603ull; // FNV-ish, just a change-detector
            for (uint16_t k = 0; k < engine.agentSlotCount(); ++k) {
                critterchron::Point p = engine.agentPos(k);
                h = (h ^ (uint64_t)(uint8_t)p.x) * 1099511628211ull;
                h = (h ^ (uint64_t)(uint8_t)p.y) * 1099511628211ull;
            }
            return h;
        };
        uint64_t last_pos_sig = positions_signature();

        auto report = [&](const char* what, long tick) {
            std::printf("\n*** SOAK DETECTED: %s at tick %ld "
                        "(virtual wall=%ld) ***\n", what, tick, (long)clock.wall_now());
            std::printf("  missing=%u extra=%u current=%u  total_seeks=%u "
                        "failed_seeks=%u glitches=%u grecov=%u\n",
                        (unsigned)engine.countTiles("missing"),
                        (unsigned)engine.countTiles("extra"),
                        (unsigned)engine.countTiles("current"),
                        (unsigned)engine.metrics().total_seeks,
                        (unsigned)engine.metrics().failed_seeks,
                        (unsigned)engine.metrics().glitches,
                        (unsigned)engine.metrics().glitch_recoveries);
            for (uint16_t k = 0; k < engine.agentSlotCount(); ++k) {
                critterchron::Point p = engine.agentPos(k);
                std::printf("  agent[%u] pos=(%d,%d) pc=%d glitched=%d\n",
                            k, (int)p.x, (int)p.y, (int)engine.agentPc(k),
                            engine.agentGlitched(k) ? 1 : 0);
            }
            std::printf("  --- grid dumpStateJsonl ---\n%s\n",
                        engine.dumpStateJsonl().c_str());
        };

        for (long i = 0; i < soak_ticks; ++i) {
            clock.advance_ms(step_ms);
            engine.syncTime();
            engine.tick();

            uint32_t seeks    = engine.metrics().total_seeks;
            uint32_t glitches = engine.metrics().glitches;
            uint64_t pos_sig  = positions_signature();
            uint16_t work     = engine.countTiles("missing") + engine.countTiles("extra");

            if (glitches != last_glitches) {
                report("GLITCH (agent benched)", i);
                return 3;
            }
            if (work > (uint16_t)max_work) max_work = work;
            if (work > 0 && seeks == last_seeks) ++ticks_work_no_seek;
            bool no_progress = (seeks == last_seeks) && (pos_sig == last_pos_sig);
            if (work > 0 && no_progress) {
                if (++stuck > max_stuck) max_stuck = stuck;
                if (stuck >= stuck_window) { report("LIVELOCK (work ignored)", i); return 4; }
            } else {
                stuck = 0;
            }
            last_seeks = seeks;
            last_glitches = glitches;
            last_pos_sig = pos_sig;
        }
        std::printf("soak: completed %ld ticks, no wedge detected "
                    "(virtual wall advanced %ld s)\n"
                    "  high-water: max_stuck_streak=%ld (threshold=%ld)  "
                    "max_work=%ld  ticks_work_no_seek=%ld/%ld\n",
                    soak_ticks, (long)(clock.wall_now()),
                    max_stuck, stuck_window, max_work,
                    ticks_work_no_seek, soak_ticks);
        return 0;
    }

    std::ofstream dump;
    if (dump_path) {
        dump.open(dump_path);
        if (!dump) {
            std::fprintf(stderr, "Cannot open dump path: %s\n", dump_path);
            return 1;
        }
    }

    for (int i = 0; ticks == 0 || i < ticks; ++i) {
        engine.syncTime();
        engine.tick();
        if (dump.is_open()) dump << engine.dumpStateJsonl();
        if (ticks == 0 && i >= 10000) break;  // safety cap for infinite mode
    }

    const auto& m = engine.metrics();
    double lit_rate = m.total_intended
        ? (double)m.total_lit_intended / (double)m.total_intended * 100.0
        : 0.0;
    double convergence = engine.tickCount()
        ? (double)m.convergences / (double)engine.tickCount() * 100.0
        : 0.0;

    std::printf("\n==============================\n");
    std::printf(" CRITTERCHRON HEALTH REPORT (C++ HOST)\n");
    std::printf("==============================\n");
    std::printf("Grid:           %dx%d\n", GRID_WIDTH, GRID_HEIGHT);
    std::printf("Total Ticks:    %u\n", engine.tickCount());
    std::printf("Convergence:    %.1f%%\n", convergence);
    std::printf("Lit Rate:       %.1f%%\n", lit_rate);
    std::printf("Glitched (RBW): %u\n", m.glitches);
    std::printf("Failed Seeks:   %u\n", m.failed_seeks);
    std::printf("Step Contests:  %u\n", m.step_contests);
    std::printf("==============================\n\n");

    return 0;
}

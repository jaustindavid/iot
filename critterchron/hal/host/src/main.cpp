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
        "[--dump-state PATH] [--night]\n", argv0);
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

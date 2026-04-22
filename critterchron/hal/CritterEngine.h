#pragma once
#include <cstdint>
#include <cstddef>
#include <random>
#include <string>
#include <utility>
#include <vector>
#include "interface/LedSink.h"
#include "interface/TimeSource.h"
#include "ir/IrRuntime.h"
// Generated per-build: supplies GRID_WIDTH / GRID_HEIGHT defines and the
// DEFAULT_IR_BLOB the loader parses at begin(). The fallback 16x16 defines
// below only fire on hosts that build without a generated header.
#include "ir/critter_ir.h"

#ifndef GRID_WIDTH
#define GRID_WIDTH 16
#endif
#ifndef GRID_HEIGHT
#define GRID_HEIGHT 16
#endif

namespace critterchron {

struct Point { int8_t x, y; };

struct Tile {
    uint8_t state    : 1;
    uint8_t intended : 1;
    int16_t claimant;            // -1 unclaimed
    uint8_t r, g, b;             // last draw color (only meaningful when state)
#if IR_MAX_MARKERS > 0
    // Per-tile marker counts (IR v5). Slot i holds the count for the marker
    // whose compiler-assigned index is i (see critter_ir::MARKERS[k].index).
    // Parallels engine.py's Tile.count dict-by-slot. Zeroed on engine
    // construction; modified by incr/decr opcodes and decayed in-tick.
    // Size is 0 (opt-out) on builds that -DIR_MAX_MARKERS=0.
    uint8_t count[IR_MAX_MARKERS];
#endif
};

// Engine-wide cap across all agent types. Must be >= the sum of per-type
// `up to N` limits in any .crit script. Sized to ants.crit's `up to 80`,
// which is itself tuned to the theoretical peak lit pixels for HH:MM
// (05:55 = 75 on MEGAFONT_5X6) plus a handful of non-painter agents. Agent
// is ~28B on-device, so the default costs ~2.2KB — meaningful on OG Photon
// where WICED wants every spare byte. Override with -DMAX_AGENTS=… in a
// device header if a future script needs more.
#ifndef MAX_AGENTS
#define MAX_AGENTS 80
#endif

struct Agent {
    int16_t  id;
    uint16_t type_idx;           // index into critter_ir::AGENT_TYPES
    uint16_t beh_idx;            // index into critter_ir::BEHAVIORS
    Point    pos, prev_pos;
    // Index into critter_ir::COLORS. Engine resolves to RGB via
    // resolveColor() at render and dump time — for static colors this is a
    // table lookup, for cycles it walks CYCLE_ENTRIES using render_frame_.
    uint8_t  color_idx;
    // Per-agent cycle phase offset. Added to the frame counter before the
    // modulo, so agents sharing a cycle color fall out of sync naturally
    // instead of pulsing in lockstep. Randomized at spawn.
    uint16_t color_phase;
    int16_t  remaining_ticks;
    int16_t  step_duration;
    int16_t  pc;
    int16_t  seek_ticks;
    uint8_t  state_str_id;       // index into this agent type's states[] + 1; 0 = "none"
    bool     glitched;
    bool     alive;
};

struct HealthMetrics {
    uint32_t convergences = 0;
    uint32_t glitches = 0;
    uint32_t step_contests = 0;
    uint32_t failed_seeks = 0;
    uint32_t total_intended = 0;
    uint32_t total_lit_intended = 0;
};

class CritterEngine {
public:
    CritterEngine(LedSink& sink, TimeSource& clock);

    bool begin();
    // Re-seat engine state against the *currently loaded* critter_ir tables
    // (no loadDefault call). Used by the OTA path: after Stra2usClient swaps
    // the tables via critter_ir::load(), the engine still holds agents that
    // reference indices in the old behavior/type pool — this clears them
    // and respawns from the new INITIAL_AGENTS. Returns false on IR version
    // mismatch (same gate as begin()).
    bool reinit();
    void seedRng(uint32_t seed) { rng_.seed(seed); }

    void tick();
    void render(float blend);
    void syncTime();
    void syncTimeAt(time_t utc_seconds, float zone_offset_hours);

    uint32_t             tickCount()     const { return tick_count_; }
    const HealthMetrics& metrics()       const { return metrics_; }
    uint16_t             liveAgentCount() const {
        uint16_t n = 0;
        for (uint16_t i = 0; i < agent_count_; ++i) if (agents_[i].alive) ++n;
        return n;
    }

    // Last-tick timing split, updated at tick() exit. Wrapper reads both to
    // report separate `interp` and `astar` budgets alongside the existing
    // whole-tick `phys`. `interp` excludes the astar cost it contains.
    uint32_t interpUsLastTick() const { return interp_us_; }
    uint32_t astarUsLastTick()  const { return astar_us_;  }

    // Dump post-tick state as one JSON-Lines record. Schema matches
    // engine.py's dump_state_jsonl so C++↔C++ (and eventually, with
    // RNG-matched inputs, C++↔Python) traces can diff cleanly.
    std::string dumpStateJsonl() const;

    static constexpr uint8_t SUPPORTED_IR_VERSION = 6;

    // Night-mode toggle. When true, resolveColor() consults the night
    // palette (NIGHT_COLORS / NIGHT_DEFAULT) before falling through to
    // the day palette. Blobs without a night palette are unaffected —
    // a missing override just falls through. Default off; platform glue
    // drives this off the LightSensor via a Schmitt trigger.
    void setNightMode(bool on) { night_mode_ = on; }
    bool nightMode() const      { return night_mode_; }

private:
    // ---------- lookup helpers ----------
    int  colorIndex(const char* name) const;
    bool colorRGB(const char* name, uint8_t& r, uint8_t& g, uint8_t& b) const;
    // Resolve a color by index to RGB at frame `frame`. Cycles walk using
    // `frame` mod total frames. Static colors ignore the frame argument.
    // render() calls with render_frame_ so display animates at 50Hz; the
    // state dump calls with tick_count_ so HW traces diff against the
    // Python sim cleanly (sim advances cycles per tick, not per render).
    // Guarantees an assignment even on out-of-range idx (writes 255,255,255).
    void resolveColor(int idx, uint32_t frame,
                      uint8_t& r, uint8_t& g, uint8_t& b) const;
    // Unconditional day-palette resolve — same walker as before, without
    // the night-mode preempt. Called by resolveColor() when night_mode_
    // is off, and recursively from the night branch when a night entry
    // refers back into the day palette (so night → day transitions
    // terminate cleanly).
    void resolveDayColor(int idx, uint32_t frame,
                         uint8_t& r, uint8_t& g, uint8_t& b) const;
    int  landmarkIndex(const char* name) const;
    int  agentTypeIndex(const char* name) const;
    int  behaviorIndex(const char* name) const;
    int  pfConfigIndex(const char* name) const;
    // Resolve a marker name to its Tile::count[] slot. Returns -1 if the
    // name is unknown or the build is compiled with IR_MAX_MARKERS=0.
    // Note: the slot is `MARKERS[k].index`, not `k` — the compiler can
    // assign a stable slot independent of declaration order, and the HAL
    // respects whatever slot the wire format carries.
    int  markerSlot(const char* name) const;

    // Pathfinding config resolution with the same fallback chain as engine.py.
    int32_t pfInt(uint16_t type_idx, const char* key, int32_t fallback) const;
    bool    pfDiagonal(uint16_t type_idx) const;
    float   pfDiagonalCost(uint16_t type_idx) const;

    bool isPainter(uint16_t type_idx) const { return painter_mask_ & (1u << type_idx); }

    // ---------- tick pipeline ----------
    void applySpawnRules();
    void processAgent(Agent& a);
    void computeConvergence();
    void clearClaims();

    bool evaluateIf(const Agent& a, const char* body) const;
    bool tileMatches(int x, int y, const char* kind) const;
    bool checkBoardCondition(const char* kind) const;
    bool occupiedByNonPainter(int x, int y, int16_t ignore_id = -1) const;

    // Bounded A* with best-so-far fallback; returns first step (dx,dy) or
    // {0,0} if no move. Populates `metrics_.failed_seeks` on budget exhaustion.
    bool aStarFirstStep(Point start, Point target, uint16_t type_idx,
                        Point& out_step);

    void despawn(Agent& a);

    // ---------- state ----------
    LedSink&       sink_;
    TimeSource&    clock_;
    Tile           grid_[GRID_WIDTH][GRID_HEIGHT];
    Agent          agents_[MAX_AGENTS];
    uint16_t       agent_count_ = 0;
    uint32_t       tick_count_ = 0;
    // Timing accumulators — reset at tick() start, updated during
    // processAgent/aStarFirstStep, finalized at tick() exit. Exposed via
    // interpUsLastTick()/astarUsLastTick(). Overhead is ~2 micros() calls
    // per agent + 2 per aStar invocation.
    uint32_t       interp_us_ = 0;
    uint32_t       astar_us_  = 0;
    // Counter incremented once per render() call (~50Hz). Drives cycle-color
    // walk so animations advance at display rate independent of physics tick
    // rate. Wraps naturally at 2^32; cycle total * frame_count fits.
    uint32_t       render_frame_ = 0;
    int16_t        next_agent_id_ = 1;
    HealthMetrics  metrics_;
    uint32_t       painter_mask_ = 0;   // bit i = agent type i is a painter
    bool           night_mode_ = false;
    std::mt19937   rng_{0xC0FFEE};

    // A* scratch — owned by the engine so each call reuses storage instead
    // of allocating three vectors on entry. Photon 2's heap fragments under
    // fast scripts (ants @ 50ms × 32 agents). Sized to GRID_WIDTH*GRID_HEIGHT
    // on first use; heap is bounded by max_nodes*8 in practice.
    std::vector<float>                            pf_cost_;
    std::vector<int16_t>                          pf_from_;
    std::vector<std::pair<float, int>>            pf_heap_;

    // Render scratch — owned so each render() reuses the same bytes. Composed
    // as (landmarks, lit tiles, agent-lerp) then flushed to the sink. Agents
    // overwrite any tile underneath with `agent_rgb * weight` per HAL_SPEC §6.
    uint8_t                                       fb_[GRID_WIDTH * GRID_HEIGHT * 3];
};

}  // namespace critterchron

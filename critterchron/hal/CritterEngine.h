#pragma once
#include <cstdint>
#include <cstddef>
#include <random>
#include <string>
#include <utility>
#include <vector>
#include "interface/LedSink.h"
#include "interface/CritTimeSource.h"
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

// One step of a cached A* plan. `cost` is the g-cost delta A* charged at
// plan time (1.0 on a clear ortho edge, up to ~1 + pen_lit + pen_occ on a
// lit+occupied one, scaled by the diagonal multiplier for corner moves).
// Stored so the runtime "did reality drift off A*'s estimate?" check can
// compare the current edge cost against what A* budgeted — see
// stepTowardTarget() for the reactivity mechanism. 8B with alignment;
// PLAN_MAX copies live on each Agent.
struct PlannedStep {
    int8_t dx;
    int8_t dy;
    // (2B padding here for 4B alignment of cost)
    float  cost;
};

// Cap on cached plan depth per agent. The `plan_horizon` pfInt knob
// clamps to this. 4 is a deliberately modest upper bound: the whole point
// of caching is to skip replans, but committing to more than ~4 steps
// blind outruns the reactivity the cost-budget check gives back. Sized in
// bytes, too: 4 × 8B = 32B/agent, × MAX_AGENTS=80 = 2.5KB of plan
// scratch on-device. Bumpable if a future agent type wants longer
// horizons and the RAM budget is there.
#ifndef PLAN_MAX
#define PLAN_MAX 4
#endif

struct Tile {
    uint8_t state    : 1;
    uint8_t intended : 1;
    // Cliff-fade flag (IR v6): when set alongside a non-sentinel
    // age_max, the render path skips the brightness lerp — the tile
    // sits at full intensity for the whole countdown then vanishes in
    // one frame. Sibling to the `fade` flavor (lerp) and the no-fade
    // sentinel (persist forever). Fits in existing bitfield slack;
    // zero footprint. Mirrored on the Python side as Tile.hold_mode.
    uint8_t hold_mode : 1;
    int16_t claimant;            // -1 unclaimed
    uint8_t r, g, b;             // last draw color (only meaningful when state)
    // Index into critter_ir::COLORS of the color name that produced r/g/b.
    // Sentinel 0xFF means "no name on file" (virgin tile, or `draw` with no
    // color arg whose prior RGB was set by something other than `draw
    // <name>`). Consulted by setNightMode() on a day/night transition so
    // tiles painted before the flip re-resolve to the other palette —
    // without this, a `draw brick` from an hour ago would keep its day RGB
    // forever even though the panel is now in night mode. Costs 1B/tile
    // (256B on a 16×16, 256B on a 32×8 — negligible). Only consulted when
    // state == 1, so the post-memset zero value is harmless (we skip it).
    uint8_t color_ref;
#if IR_MAX_MARKERS > 0
    // Per-tile marker counts (IR v5). Slot i holds the count for the marker
    // whose compiler-assigned index is i (see critter_ir::MARKERS[k].index).
    // Parallels engine.py's Tile.count dict-by-slot. Zeroed on engine
    // construction; modified by incr/decr opcodes and decayed in-tick.
    // Size is 0 (opt-out) on builds that -DIR_MAX_MARKERS=0.
    uint8_t count[IR_MAX_MARKERS];
#endif
    // Tile-paint fade (IR v6). `draw <color> fade N` sets age=age_max=N
    // and the per-tick scheduler decrements age; at age=0 state flips
    // back to 0 (the tile auto-expires). Sentinel 0xFFFF means "no fade"
    // — a plain `draw <color>` paints a tile that persists until cleaned,
    // identical to pre-v6 behavior. Render-time lerp: rgb *= age/age_max
    // (Q8 fixed-point) before marker composite. 4 bytes/tile; on a 16×16
    // that's 1 KiB, manageable on every platform including OG Photon
    // (TODO.md §369-374). Mirrored on the Python side as Tile.age/age_max.
    uint16_t age;
    uint16_t age_max;
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

// Ticks a glitched agent stays benched before the tick loop auto-rearms
// it (clears `glitched`, restarts its behavior at pc=0). Recovery exists
// so a transient runaway self-heals instead of bricking the clock until
// reboot — but it does NOT mask the bug: every glitch still bumps
// metrics_.glitches and every rearm bumps metrics_.glitch_recoveries, so
// a persistent cause shows up as a visible flap in telemetry rather than
// a silent freeze. ~256 ticks ≈ 25s at a 100ms tick, ~100s at 400ms.
// Reuses the agent's `remaining_ticks` field as the countdown (unused
// while benched) — deliberately NOT a new Agent field; growing the
// struct crashloops the ESP32-C3 (see 2026-05-23 canary investigation).
#ifndef GLITCH_RECOVERY_TICKS
#define GLITCH_RECOVERY_TICKS 256
#endif

struct Agent {
    int16_t  id;
    uint16_t type_idx;           // index into critter_ir::AGENT_TYPES
    uint16_t beh_idx;            // index into critter_ir::BEHAVIORS
    Point    pos, prev_pos;
    // Delta of the most recent move (pos - prev-before-move). Fed back into
    // aStarFirstStep as a backtrack-bias hint: the planner adds a small
    // penalty to the edge that would reverse this step, breaking the A↔B
    // oscillation that otherwise shows up under fresh-plan-per-step
    // semantics when local geometry makes both neighbors look equally
    // promising. prev_pos can't carry this signal because processAgent
    // resets prev_pos = pos when the interpolation animation completes —
    // which is exactly when the next A* call happens. Zero-initialized
    // (by the memset at spawn) so the very first step has no bias.
    Point    last_step;
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

    // ---------- cached A* plan (plan_horizon knob) ----------
    // When plan_horizon > 1, A* returns up to PLAN_MAX steps; the agent
    // consumes them one per tick, replanning only when (a) plan exhausted,
    // (b) target drifted (seek re-picked a different nearest), or (c) the
    // next step's *current* cost exceeds what A* budgeted at plan time.
    // (c) is the reactivity layer: an agent appearing in our path or a
    // tile becoming lit after we planned both show up as cost overshoots.
    // Default plan_horizon=1 keeps the current replan-every-tick behavior
    // and makes all these fields no-ops (plan_len always 0→1→0).
    PlannedStep plan[PLAN_MAX];
    Point       plan_target;  // seek target at plan time; replan on drift
    float       plan_slack;   // cumulative under-budget credit; non-negative
                              // by construction (see stepTowardTarget)
    uint8_t     plan_len;
    uint8_t     plan_cursor;

    // NOTE: WEDGE_DIAG canary used to live here as a trailing field. On
    // ESP32-C3 the 4-byte struct-size bump caused a boot crashloop (root
    // cause not nailed down — see investigation notes 2026-05-23). Moved
    // out to a sibling static array `g_agent_canaries[MAX_AGENTS]` in
    // CritterEngine.cpp to keep this struct's layout invariant across
    // diag-on/diag-off builds. Diagnostic value is slightly reduced —
    // catches writes into the array region but not within-struct
    // overflows past plan[PLAN_MAX] — but irst= has exonerated the
    // IR-buffer-corruption theory the canary was primarily built for, so
    // the remaining "did the array region get hit" use is still useful.
};

struct HealthMetrics {
    uint32_t convergences = 0;
    uint32_t glitches = 0;
    uint32_t step_contests = 0;
    uint32_t failed_seeks = 0;
    // Counts every seek opcode entry (both classic and gradient forms),
    // success or fail. Diagnostic-only — paired with failed_seeks lets the
    // analyzer compute successful_seeks = total - failed and spot the
    // "agent is reaching seek opcodes but always succeeding trivially"
    // case (state machine cycles, seeks count climbs, but failed_seeks
    // flat) vs the "agent never even gets to a seek opcode" case (both
    // counters frozen while state cycles). The wedge ricky_raccoon hit
    // 2026-05-29 looks like the former; this field disambiguates.
    uint32_t total_seeks = 0;
    // Count of auto-rearm events (a benched/glitched agent restored to
    // active after GLITCH_RECOVERY_TICKS). Paired with `glitches`: if both
    // climb together over time, an agent is repeatedly glitching and
    // self-healing — a persistent corruption cause flapping, not a
    // one-shot. Surfaced as `grecov=` in the WEDGE_DIAG heartbeat.
    uint32_t glitch_recoveries = 0;
    uint32_t total_intended = 0;
    uint32_t total_lit_intended = 0;
};

class CritterEngine {
public:
    CritterEngine(LedSink& sink, CritTimeSource& clock);

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
    uint16_t             agentSlotCount() const { return agent_count_; }

    // Per-agent state-string name (from `state { ... }` declaration in the
    // script). Returns "none" for dead/OOB agents or agents whose state_str_id
    // hasn't been set. Pointer is into the loaded IR string pool — stable
    // across the lifetime of the loaded blob, do not free.
    //
    // Heartbeat builders use this for the `ast=(s1,s2,...)` field; combined
    // with the seek/astar staleness signature it distinguishes "both agents
    // wedged in the same state forever" (state machine stuck) from "both
    // agents legitimately taking the idle branch" (clock-face complete /
    // missing-cell set empty).
    const char* agentStateName(uint16_t idx) const;

    // Raw per-agent identifiers. Used by the WEDGE_DIAG heartbeat field
    // (`agid=(t<N>/s<N>,...)`) to expose the underlying byte values so we
    // can localize the corruption that surfaces as empty `ast=()`. These
    // bypass agentStateName's fallback-to-"none" guards — operators want
    // the raw value, not a sanitized one. Returns 0xFFFF / 0xFF for
    // OOB/dead agents (UINT_MAX sentinels distinguish "no agent here"
    // from "agent here with id 0").
    uint16_t agentTypeIdxRaw(uint16_t idx) const;
    uint8_t  agentStateIdRaw(uint16_t idx) const;

    // Walk all alive agents' canaries; returns the value of the first one
    // that's NOT 0xDEADBEEF, or 0xDEADBEEF if all intact. 0 if no agents.
    // Heartbeat builder emits `canary=ok` or `canary=BAD/<hex>` based on
    // this. Reads from the sibling `g_agent_canaries[]` static array in
    // CritterEngine.cpp (see comment in Agent struct re: why it's not
    // inside Agent anymore). Returns 0xDEADBEEF on non-WEDGE_DIAG builds.
    uint32_t firstBadCanary() const;

    // Per-agent position + script PC, used by the WEDGE_DIAG heartbeat
    // fields `apos=` and `apc=`. Together they tell us: are the agents
    // physically moving on the grid? Are they sitting at a specific
    // script PC indefinitely (suggesting a stuck control-flow branch)?
    // Returns {0, 0} / 0 for OOB/dead slots — caller distinguishes "no
    // agent" from "agent at origin" via the agid= field.
    Point   agentPos(uint16_t idx) const;
    int16_t agentPc (uint16_t idx) const;

    // Per-agent glitched flag. An agent trips this when processAgent runs
    // away past the 100-opcode guard; once set it's NEVER cleared at
    // runtime and the tick loop skips the agent permanently (CritterEngine
    // .cpp:626). This is the wedge mechanism we chased for weeks — the
    // engine-wide count is in metrics().glitches, but per-agent visibility
    // tells the analyzer WHICH slot died and lets us correlate with the
    // frozen agid=/apos=/apc= snapshot. Returns false for OOB/dead slots.
    bool agentGlitched(uint16_t idx) const;

    // Pack the grid's `intended` and `state` (lit) bits into two
    // bitmaps, one bit per cell, index = y*GRID_WIDTH + x. Each needs
    // ceil(GRID_WIDTH*GRID_HEIGHT / 8) bytes (32 on a 32x8 grid). The
    // WEDGE_DIAG heartbeat hex-encodes these as `gint=`/`glit=` so we can
    // SEE the actual board at a wedge — missing = intended & !state,
    // extra = state & !intended — and settle whether the agents' "no
    // work" world-view matches reality or `cells=` is misreporting.
    // Reads the live grid; nbytes must be >= the per-bitmap size or the
    // call no-ops (leaves buffers zeroed).
    void gridBitmaps(uint8_t* intended_out, uint8_t* state_out,
                     size_t nbytes) const;

    // Within-tick board counts (wedge diagnostic). `pre` = missing/extra
    // as the agents saw them this tick (captured before processAgent);
    // `post` = after all post-agent grid mutation (fade-expiry, decay).
    // Divergence between pre/post — or between pre and the tel thread's
    // `cells=` — localizes whether the grid changes mid-tick or across
    // threads. Surfaced as `bpre=`/`bpost=` in the heartbeat and the
    // "dbg" object in dumpStateJsonl.
    uint16_t dbgBoardMissPre()  const { return dbg_miss_pre_;  }
    uint16_t dbgBoardExtraPre() const { return dbg_extra_pre_; }
    uint16_t dbgBoardMissPost() const { return dbg_miss_post_; }
    uint16_t dbgBoardExtraPost()const { return dbg_extra_post_;}

    // Fill `out` with the `marker` count at each point of the named
    // `landmark`, in landmark-point order, up to `out_max` entries.
    // Returns the number written (0 if landmark or marker is unknown, or
    // the build has IR_MAX_MARKERS=0). WEDGE_DIAG heartbeat dumps the
    // boober "piles" landmark's "heap" counts as `pileheap=(N,N,...)`.
    // The 2026-05-29 wedge hypothesis predicts ALL counts >= 1 at the
    // moment of wedge — the return-deposit seek `< 1` then has no
    // candidate, so a `returning` agent can never deposit and the system
    // livelocks. A snapshot showing every cell >= 1 (especially all == cap)
    // confirms it; any cell == 0 refutes it.
    uint16_t landmarkMarkerCounts(const char* landmark, const char* marker,
                                  uint8_t* out, uint16_t out_max) const;

    // Copy `out_len` bytes from the IR-string-pool memory pointed to by
    // the first state name of AGENT_TYPES[0]. Heartbeat dumps these as
    // hex so we can spot zero-byte corruption in the IR text buffer.
    // Caller passes a buffer of at least `out_len` bytes; we copy from
    // the underlying string region, NOT from any sanitized representation.
    // Reads up to NUL or out_len, padded with zeros — the corruption
    // signature is precisely the first-byte-is-zero case we want to spot.
    void irStateNameBytes(uint16_t type_idx, uint16_t state_slot,
                          uint8_t* out, size_t out_len) const;

    // Walk the grid and count tiles matching `kind`. `kind` is anything
    // tileMatches() accepts: "missing", "extra", "current", or a landmark
    // name. Returns 0 for unrecognized kinds. O(GRID_WIDTH*GRID_HEIGHT);
    // cheap on physical-display sizes (~70 cells), do not call from the
    // engine hot loop. Heartbeat-cycle use only.
    uint16_t countTiles(const char* kind) const;

    // UTC seconds passed to the most recent syncTimeAt() call, or 0 if
    // the engine has never synced. The heartbeat builder publishes the
    // delta (wall - lastSyncedUnix) as `esync_lag=`. Healthy fleet hovers
    // at <=60s (syncs once per virtual minute on minute-edge). A frozen
    // engine that's no longer receiving time pulses shows the gap growing
    // without bound — the discriminator for the "engine time view stuck"
    // wedge flavor observed on c3b 2026-05-17.
    time_t lastSyncedUnix() const { return last_synced_unix_; }

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
    // Toggle night mode. Out-of-line so we can sweep the grid on the
    // transition edge: tiles painted before the flip have their RGB
    // re-resolved against the target palette via their stashed
    // `color_ref`, so the whole panel swaps in lockstep instead of
    // leaving stragglers painted in yesterday's palette forever.
    // Idempotent — a set that doesn't change the state skips the sweep.
    // Caveat: cycle colors re-resolve at frame 0 (phase reset). If a
    // per-tile phase snapshot ever becomes important, it's another
    // uint16_t per tile; deferred until it shows up as a visible problem.
    void setNightMode(bool on);
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
    // Drunkenness ∈ [0.0, 1.0]; 0.0 (default) = sober planner-optimal walk.
    // See drunkenPerturb() in CritterEngine.cpp for the effect on step.
    float   pfDrunkenness(uint16_t type_idx) const;

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

    // Bounded A* with best-so-far fallback. Writes up to `max_steps` moves
    // from the start of the reconstructed path into `out_steps[0..*out_len)`
    // with their A*-budgeted g-cost deltas. Returns true iff at least one
    // step was produced. `max_steps` is clamped to PLAN_MAX by the caller.
    // Populates `metrics_.failed_seeks` on budget exhaustion.
    bool aStarPlan(Point start, Point target, uint16_t type_idx,
                   PlannedStep* out_steps, int max_steps, int& out_len,
                   Point last_step = {0, 0});

    // One-step convenience wrapper preserving the pre-horizon call shape.
    // Not used by seek anymore (that path goes through stepTowardTarget for
    // the plan-reuse machinery) but left in place for any future caller
    // that genuinely wants a fresh one-shot first step.
    bool aStarFirstStep(Point start, Point target, uint16_t type_idx,
                        Point& out_step, Point last_step = {0, 0}) {
        PlannedStep one;
        int n = 0;
        bool ok = aStarPlan(start, target, type_idx, &one, 1, n, last_step);
        if (!ok || n == 0) { out_step = {0, 0}; return false; }
        out_step = {one.dx, one.dy};
        return true;
    }

    // Produce one move toward target: consume a cached plan step if valid,
    // else replan via aStarPlan and execute its first step. Handles all
    // agent-side bookkeeping (prev_pos, pos, last_step, step_duration,
    // remaining_ticks, plan cache). Returns true on move, false on "no
    // step available" (caller reverts to 1-tick retry). The reactivity
    // mechanism (cost-budget surprise → replan) lives here, not inside
    // aStarPlan — plans themselves are pure A* output.
    bool stepTowardTarget(Agent& a, Point target);

    void despawn(Agent& a);

    // ---------- state ----------
    LedSink&       sink_;
    CritTimeSource&    clock_;
    Tile           grid_[GRID_WIDTH][GRID_HEIGHT];
    Agent          agents_[MAX_AGENTS];
    uint16_t       agent_count_ = 0;
    uint32_t       tick_count_ = 0;
    // UTC seconds passed to the most recent syncTimeAt() call. Surfaced via
    // lastSyncedUnix() for the `esync_lag=` heartbeat field.
    time_t         last_synced_unix_ = 0;
    // Within-tick board-count diagnostics (see dbgBoard*() getters). Stamped
    // in tick(): *_pre_ before processAgent, *_post_ after fade-expiry.
    uint16_t       dbg_miss_pre_  = 0;
    uint16_t       dbg_extra_pre_ = 0;
    uint16_t       dbg_miss_post_ = 0;
    uint16_t       dbg_extra_post_= 0;
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
    // Path reconstruction scratch — walked backward from the end node
    // through from[] once per aStarPlan call to extract the first N steps.
    // Bounded by grid size in worst case.
    std::vector<int>                              pf_trail_;

    // Render scratch — owned so each render() reuses the same bytes. Composed
    // as (landmarks, lit tiles, agent-lerp) then flushed to the sink. Agents
    // overwrite any tile underneath with `agent_rgb * weight` per HAL_SPEC §6.
    uint8_t                                       fb_[GRID_WIDTH * GRID_HEIGHT * 3];
};

}  // namespace critterchron

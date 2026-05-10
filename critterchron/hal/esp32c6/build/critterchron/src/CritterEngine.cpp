#include "CritterEngine.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <functional>
#include <queue>
#include <utility>
#include <vector>

#if defined(PLATFORM_ID)
#include "Particle.h"            // micros()
#else
#include <chrono>
#endif

namespace critterchron {
namespace {

// Monotonic microsecond clock. On Particle we use `micros()` (32-bit wrap
// every ~71 min — fine because we only ever subtract two adjacent samples).
// Elsewhere (host harness, tests) we fall back to std::chrono::steady_clock
// truncated to 32 bits so diag numbers look the same across platforms.
inline uint32_t now_us() {
#if defined(PLATFORM_ID)
    return micros();
#else
    using clk = std::chrono::steady_clock;
    return static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            clk::now().time_since_epoch()).count());
#endif
}

}  // namespace  (local helpers)
namespace {

// Sine-dip weights for the render lerp. Instead of a linear cross-fade
// (prev_w + pos_w = 1 at every t), we use `cos(π·t)` so prev_w falls to 0
// at t=0.5 and pos_w emerges from 0 there — modeling a point light behind
// a perforated screen (light pipes with gaps between them: the agent
// genuinely goes dark mid-transit instead of smoothly handing brightness
// to the next cell). `cos_q8_lut[i]` stores `round(sqrt(cos(π·i/256)) * 256)`
// for i ∈ [0, 128]; the second half of the cycle reuses the same table via
// `cos_q8_lut[256 - alpha_q8]` since cos is symmetric about π/2. Hardcoded
// constexpr so we don't drag libm's trig into flash for a one-time boot init.
//
// Shape note: the sqrt makes the curve "shallower" — prev_w stays near the top
// longer and plunges to zero faster near the midpoint, so the agent dwells
// near full brightness for more of the transit with a briefer blackout at
// t=0.5. Midpoint is still true zero, so the pipe-gap physics hold; only the
// dwell pattern changes. Revert to pure cos by regenerating the table from
// `round(256 * cos(π·i/256))`.
constexpr int16_t cos_q8_lut[129] = {
     256,  256,  256,  256,  256,  256,  256,  256,
     255,  255,  255,  255,  255,  254,  254,  254,
     254,  253,  253,  253,  252,  252,  251,  251,
     250,  250,  249,  249,  248,  248,  247,  247,
     246,  245,  245,  244,  243,  243,  242,  241,
     240,  240,  239,  238,  237,  236,  235,  234,
     233,  232,  231,  230,  229,  228,  227,  226,
     225,  224,  223,  222,  220,  219,  218,  217,
     215,  214,  213,  211,  210,  208,  207,  205,
     204,  202,  201,  199,  198,  196,  194,  193,
     191,  189,  187,  185,  184,  182,  180,  178,
     176,  174,  172,  170,  167,  165,  163,  161,
     158,  156,  154,  151,  149,  146,  143,  141,
     138,  135,  132,  129,  126,  123,  120,  117,
     113,  110,  106,  102,   98,   94,   90,   85,
      80,   75,   69,   63,   57,   49,   40,   28,
       0,
};

// HH:MM font used by syncTime — mirrors MEGAFONT_5X6 in main.py.
const uint8_t MEGAFONT_5X6[10][6] = {
    {0x70,0x98,0x98,0x98,0x98,0x70}, // 0
    {0x30,0x70,0x30,0x30,0x30,0x78}, // 1
    {0xF0,0x18,0x70,0xC0,0xC0,0xF8}, // 2
    {0xF8,0x18,0x70,0x18,0x98,0x70}, // 3
    {0x80,0x98,0xF8,0x18,0x18,0x18}, // 4
    {0xF8,0xC0,0xF0,0x18,0x98,0x70}, // 5
    {0x70,0xC0,0xF0,0xC8,0xC8,0x70}, // 6
    {0xF8,0x18,0x30,0x60,0x60,0x60}, // 7
    {0x70,0x98,0x70,0x98,0x98,0x70}, // 8
    {0x70,0x98,0x78,0x18,0x98,0x70}, // 9
};

// Per-script pathfinding fallback defaults (mirrors engine.py _PF_DEFAULTS).
constexpr int32_t PF_DEF_MAX_NODES        = 512;
constexpr int32_t PF_DEF_PENALTY_LIT      = 10;
constexpr int32_t PF_DEF_PENALTY_OCCUPIED = 20;
constexpr int32_t PF_DEF_STEP_RATE        = 2;

bool startsWith(const char* s, const char* prefix) {
    return std::strncmp(s, prefix, std::strlen(prefix)) == 0;
}

bool strEq(const char* a, const char* b) { return std::strcmp(a, b) == 0; }

// Tokenize instruction text by whitespace into up to 16 pointers into a
// caller-owned scratch buffer (mutates scratch by writing NULs).
int tokenize(char* scratch, const char** toks, int max_toks) {
    int n = 0;
    char* p = scratch;
    while (*p && n < max_toks) {
        while (*p == ' ' || *p == '\t') ++p;
        if (!*p) break;
        toks[n++] = p;
        while (*p && *p != ' ' && *p != '\t') ++p;
        if (*p) { *p = 0; ++p; }
    }
    return n;
}

int parseIntOr(const char* s, int fallback) {
    char* end = nullptr;
    long v = std::strtol(s, &end, 10);
    return (end == s) ? fallback : static_cast<int>(v);
}

// Manhattan distance (tie-breaker heuristic target selection).
int manhattan(Point a, Point b) {
    return std::abs(a.x - b.x) + std::abs(a.y - b.y);
}

}  // namespace

// =======================================================================
// Construction + lookup helpers
// =======================================================================

CritterEngine::CritterEngine(LedSink& sink, CritTimeSource& clock)
    : sink_(sink), clock_(clock) {
    // Value-init the whole grid so every field (including count[]) is zero.
    // Bit-fields + an array member rule out the Tile{...} aggregate init we
    // used pre-v5.
    for (int x = 0; x < GRID_WIDTH; ++x)
        for (int y = 0; y < GRID_HEIGHT; ++y)
            grid_[x][y] = Tile{};
}

int CritterEngine::colorIndex(const char* name) const {
    for (uint16_t i = 0; i < critter_ir::COLOR_COUNT; ++i)
        if (strEq(critter_ir::COLORS[i].name, name)) return i;
    return -1;
}

bool CritterEngine::colorRGB(const char* name, uint8_t& r, uint8_t& g, uint8_t& b) const {
    int i = colorIndex(name);
    if (i < 0) { r = g = b = 255; return false; }
    resolveColor(i, render_frame_, r, g, b);
    return true;
}

void CritterEngine::setNightMode(bool on) {
    // No-op on unchanged state — skip the sweep. This matters because the
    // platform glue pulls the Schmitt trigger every 200ms light sample and
    // reasserts night_mode_ every time; a naive sweep on every call would
    // walk the grid 5×/sec for no reason.
    if (on == (bool)night_mode_) return;
    night_mode_ = on;
    // Re-resolve every named lit tile against the now-active palette. Tiles
    // painted before this edge stored their color_ref alongside the
    // snapshot RGB (see the `draw` handler); walking the grid and rewriting
    // r/g/b from resolveColor(color_ref, 0, ...) makes the whole panel
    // swap in lockstep with the marker composite (which already renders
    // live per-frame) and with the landmark background (also resolved live).
    // Without this, `draw brick` from an hour ago would keep its day RGB
    // forever — visible for persistent tiles like clock digits that don't
    // get overpainted for minutes or hours.
    //
    // Frame is pinned to 0 deliberately: a per-tile cycle phase would cost
    // another 1-2 bytes/tile, and a phase reset on the relatively rare
    // day/night edge is below the visibility bar right now. If it ever
    // isn't, promote color_ref to a { idx, phase } pair.
    //
    // color_ref == 0xFF means "no name on file" (virgin tile, or `draw`
    // that hit the unknown-name fallback) — skip; we have no way to
    // re-resolve it. state == 0 tiles are dark in the composite anyway.
    for (int x = 0; x < GRID_WIDTH; ++x) {
        for (int y = 0; y < GRID_HEIGHT; ++y) {
            Tile& t = grid_[x][y];
            if (!t.state) continue;
            if (t.color_ref == 0xFF) continue;
            resolveColor((int)t.color_ref, 0, t.r, t.g, t.b);
        }
    }
}

void CritterEngine::resolveColor(int idx, uint32_t frame,
                                 uint8_t& r, uint8_t& g, uint8_t& b) const {
    // Night-mode preempt: if the active day color has a NIGHT_COLORS
    // override (or if NIGHT_DEFAULT applies), resolve through the night
    // palette instead. Night entries that reference day colors (kind 2
    // "ref", and each cycle sub-entry) bounce back into the day-palette
    // walker below — `skip_night` parameter on the recursive call
    // prevents an infinite bounce if someone authored night_default as
    // a day-color name.
    if (night_mode_ && idx >= 0 && idx < (int)critter_ir::COLOR_COUNT) {
        uint8_t night_ref = critter_ir::NIGHT_FOR[idx];
        const critter_ir::NightEntry* N = nullptr;
        if (night_ref < critter_ir::NIGHT_COLOR_COUNT) {
            N = &critter_ir::NIGHT_COLORS[night_ref];
        } else if (critter_ir::HAS_NIGHT_DEFAULT) {
            N = &critter_ir::NIGHT_DEFAULT;
        }
        if (N) {
            if (N->kind == 0) {                       // static RGB
                r = N->r; g = N->g; b = N->b;
                return;
            }
            if (N->kind == 2) {                       // ref → walk day palette
                resolveDayColor(N->target_day_idx, frame, r, g, b);
                return;
            }
            if (N->kind == 1 && N->cycle_count > 0) { // cycle → pick sub-entry, then day walk
                uint32_t total = 0;
                for (uint8_t k = 0; k < N->cycle_count; ++k) {
                    uint8_t f = critter_ir::NIGHT_CYCLE_ENTRIES[N->cycle_start + k].frames;
                    if (f == 0) f = 1;
                    total += f;
                }
                if (total == 0) { r = N->r; g = N->g; b = N->b; return; }
                uint32_t t = frame % total;
                for (uint8_t k = 0; k < N->cycle_count; ++k) {
                    uint8_t f = critter_ir::NIGHT_CYCLE_ENTRIES[N->cycle_start + k].frames;
                    if (f == 0) f = 1;
                    if (t < f) {
                        int sub = critter_ir::NIGHT_CYCLE_ENTRIES[N->cycle_start + k].color_idx;
                        resolveDayColor(sub, frame, r, g, b);
                        return;
                    }
                    t -= f;
                }
                r = N->r; g = N->g; b = N->b;
                return;
            }
        }
        // No night override for this day color and no NIGHT_DEFAULT —
        // fall through to the day palette unchanged.
    }
    resolveDayColor(idx, frame, r, g, b);
}

void CritterEngine::resolveDayColor(int idx, uint32_t frame,
                                    uint8_t& r, uint8_t& g, uint8_t& b) const {
    // Nested cycles aren't a language feature — a cycle's entries always
    // reference static colors — but guard against a pathological table
    // anyway. Depth 4 is plenty.
    int depth = 0;
    while (idx >= 0 && idx < (int)critter_ir::COLOR_COUNT && depth < 4) {
        const auto& C = critter_ir::COLORS[idx];
        if (C.kind == 0 || C.cycle_count == 0) {
            r = C.r; g = C.g; b = C.b;
            return;
        }
        uint32_t total = 0;
        for (uint8_t k = 0; k < C.cycle_count; ++k) {
            uint8_t f = critter_ir::CYCLE_ENTRIES[C.cycle_start + k].frames;
            if (f == 0) f = 1;
            total += f;
        }
        if (total == 0) { r = C.r; g = C.g; b = C.b; return; }
        uint32_t t = frame % total;
        for (uint8_t k = 0; k < C.cycle_count; ++k) {
            uint8_t f = critter_ir::CYCLE_ENTRIES[C.cycle_start + k].frames;
            if (f == 0) f = 1;
            if (t < f) {
                idx = critter_ir::CYCLE_ENTRIES[C.cycle_start + k].color_idx;
                break;
            }
            t -= f;
        }
        ++depth;
    }
    r = 255; g = 255; b = 255;
}

int CritterEngine::landmarkIndex(const char* name) const {
    for (uint16_t i = 0; i < critter_ir::LANDMARK_COUNT; ++i)
        if (strEq(critter_ir::LANDMARKS[i].name, name)) return i;
    return -1;
}

int CritterEngine::agentTypeIndex(const char* name) const {
    for (uint16_t i = 0; i < critter_ir::AGENT_TYPE_COUNT; ++i)
        if (strEq(critter_ir::AGENT_TYPES[i].name, name)) return i;
    return -1;
}

int CritterEngine::behaviorIndex(const char* name) const {
    for (uint16_t i = 0; i < critter_ir::BEHAVIOR_COUNT; ++i)
        if (strEq(critter_ir::BEHAVIORS[i].agent_name, name)) return i;
    return -1;
}

int CritterEngine::pfConfigIndex(const char* name) const {
    for (uint16_t i = 0; i < critter_ir::PF_CONFIG_COUNT; ++i)
        if (strEq(critter_ir::PF_CONFIGS[i].agent_name, name)) return i;
    return -1;
}

int CritterEngine::markerSlot(const char* name) const {
#if IR_MAX_MARKERS > 0
    for (uint16_t i = 0; i < critter_ir::MARKER_COUNT; ++i) {
        if (strEq(critter_ir::MARKERS[i].name, name)) {
            uint16_t slot = critter_ir::MARKERS[i].index;
            if (slot >= IR_MAX_MARKERS) return -1;
            return (int)slot;
        }
    }
#else
    (void)name;
#endif
    return -1;
}

int32_t CritterEngine::pfInt(uint16_t type_idx, const char* key, int32_t fallback) const {
    const char* name = critter_ir::AGENT_TYPES[type_idx].name;
    int i = pfConfigIndex(name);
    if (i >= 0) {
        const auto& c = critter_ir::PF_CONFIGS[i];
        if (strEq(key, "max_nodes")        && c.has_max_nodes)        return c.max_nodes;
        if (strEq(key, "penalty_lit")      && c.has_penalty_lit)      return c.penalty_lit;
        if (strEq(key, "penalty_occupied") && c.has_penalty_occupied) return c.penalty_occupied;
        if (strEq(key, "step_rate")        && c.has_step_rate)        return c.step_rate;
        if (strEq(key, "plan_horizon")     && c.has_plan_horizon)     return c.plan_horizon;
    }
    int all = pfConfigIndex("all");
    if (all >= 0) {
        const auto& c = critter_ir::PF_CONFIGS[all];
        if (strEq(key, "max_nodes")        && c.has_max_nodes)        return c.max_nodes;
        if (strEq(key, "penalty_lit")      && c.has_penalty_lit)      return c.penalty_lit;
        if (strEq(key, "penalty_occupied") && c.has_penalty_occupied) return c.penalty_occupied;
        if (strEq(key, "step_rate")        && c.has_step_rate)        return c.step_rate;
        if (strEq(key, "plan_horizon")     && c.has_plan_horizon)     return c.plan_horizon;
    }
    return fallback;
}

bool CritterEngine::pfDiagonal(uint16_t type_idx) const {
    const char* name = critter_ir::AGENT_TYPES[type_idx].name;
    int i = pfConfigIndex(name);
    if (i >= 0 && critter_ir::PF_CONFIGS[i].has_diagonal)
        return critter_ir::PF_CONFIGS[i].diagonal;
    int all = pfConfigIndex("all");
    if (all >= 0 && critter_ir::PF_CONFIGS[all].has_diagonal)
        return critter_ir::PF_CONFIGS[all].diagonal;
    return critter_ir::PF_TOP_DIAGONAL != 0;
}

float CritterEngine::pfDiagonalCost(uint16_t type_idx) const {
    const char* name = critter_ir::AGENT_TYPES[type_idx].name;
    int i = pfConfigIndex(name);
    if (i >= 0 && critter_ir::PF_CONFIGS[i].has_diagonal_cost)
        return critter_ir::PF_CONFIGS[i].diagonal_cost;
    int all = pfConfigIndex("all");
    if (all >= 0 && critter_ir::PF_CONFIGS[all].has_diagonal_cost)
        return critter_ir::PF_CONFIGS[all].diagonal_cost;
    return critter_ir::PF_TOP_HAS_DIAGONAL_COST ? critter_ir::PF_TOP_DIAGONAL_COST : 1.0f;
}

float CritterEngine::pfDrunkenness(uint16_t type_idx) const {
    // Mirrors pfDiagonalCost's resolution chain: per-agent → 'all' →
    // default. No top-level `drunkenness:` entry is allowed (compiler
    // rejects) since the knob is explicitly per-script personality; so
    // there's no top-level branch here, unlike diagonal_cost.
    const char* name = critter_ir::AGENT_TYPES[type_idx].name;
    int i = pfConfigIndex(name);
    if (i >= 0 && critter_ir::PF_CONFIGS[i].has_drunkenness)
        return critter_ir::PF_CONFIGS[i].drunkenness;
    int all = pfConfigIndex("all");
    if (all >= 0 && critter_ir::PF_CONFIGS[all].has_drunkenness)
        return critter_ir::PF_CONFIGS[all].drunkenness;
    return 0.0f;  // sober default — old IR blobs without the key land here.
}

// =======================================================================
// begin(): version gate + initial agents + painter mask
// =======================================================================

bool CritterEngine::begin() {
    // Materialize the runtime IR tables from the compiled-in blob. Same
    // parser we'll later use for OTA blobs — exercised every boot so
    // latent format breakage surfaces on-device before it surfaces in the
    // field. Caller gets no useful recovery path on failure beyond a log,
    // so we just refuse to come up.
    if (!critter_ir::loadDefault()) return false;
    return reinit();
}

// Rebuild engine state from whatever is currently in the critter_ir tables.
// Called by begin() after loadDefault(), and by the OTA path after
// critter_ir::load() has replaced the tables under us.
bool CritterEngine::reinit() {
    if (critter_ir::IR_VERSION > SUPPORTED_IR_VERSION) return false;

    // Agents hold indices into BEHAVIORS/AGENT_TYPES/COLORS, so any cached
    // state from a prior IR is garbage the moment we swap tables. Zero
    // everything and respawn from INITIAL_AGENTS. Metrics reset too — they
    // measured a different script.
    for (uint16_t i = 0; i < agent_count_; ++i) agents_[i].alive = false;
    agent_count_   = 0;
    painter_mask_  = 0;
    metrics_       = {};
    // tick_count_ and render_frame_ deliberately preserved so cycle-color
    // phase continuity survives a swap — the visible script is new, but the
    // clock shouldn't jitter.
    std::memset(grid_, 0, sizeof(grid_));
    // memset zeroes color_ref to 0 — a valid color index. That would be
    // fine in steady state (we gate the night-swap re-resolve on state==1
    // and no virgin tile is lit) but defensive: if any code path ever
    // sets state=1 without going through the draw handler, we don't want
    // a stray "re-resolve COLORS[0]" to overwrite the RGB on a night
    // transition. Pin to the 0xFF "no name" sentinel up front.
    for (int x = 0; x < GRID_WIDTH; ++x)
        for (int y = 0; y < GRID_HEIGHT; ++y) {
            grid_[x][y].color_ref = 0xFF;
            // Pin fade fields to the no-fade sentinel post-memset. The
            // memset zero would mean "expire on next tick" if state ever
            // flipped to 1 without going through the draw handler — same
            // defensive reasoning as color_ref above. Tiles painted by
            // `draw <color>` (no fade) re-write these to 0xFFFF anyway.
            grid_[x][y].age = 0xFFFF;
            grid_[x][y].age_max = 0xFFFF;
            // hold_mode is a 1-bit field already zeroed by the memset
            // above; explicit write here is belt-and-suspenders, matches
            // the posture for color_ref/age, and keeps the init story
            // self-documenting if the bitfield is ever rearranged.
            grid_[x][y].hold_mode = 0;
        }
    clearClaims();

    // Painter mask: agent types whose behavior contains draw/erase are
    // excluded from the occupied-cell lookup (they *become* the pixel).
    for (uint16_t i = 0; i < critter_ir::BEHAVIOR_COUNT; ++i) {
        const auto& b = critter_ir::BEHAVIORS[i];
        int type_idx = agentTypeIndex(b.agent_name);
        if (type_idx < 0) continue;
        for (uint16_t j = 0; j < b.insn_count; ++j) {
            const char* t = b.insns[j].text;
            if (startsWith(t, "draw") || startsWith(t, "erase")) {
                painter_mask_ |= (1u << type_idx);
                break;
            }
        }
    }

    for (uint16_t i = 0;
         i < critter_ir::INITIAL_AGENT_COUNT && agent_count_ < MAX_AGENTS;
         ++i) {
        const auto& ia = critter_ir::INITIAL_AGENTS[i];
        Agent& a = agents_[agent_count_++];
        std::memset(&a, 0, sizeof(a));
        a.id = next_agent_id_++;
        int t = agentTypeIndex(ia.name);
        a.type_idx = (t < 0) ? 0 : static_cast<uint16_t>(t);
        int bi = behaviorIndex(ia.name);
        a.beh_idx = (bi < 0) ? 0 : static_cast<uint16_t>(bi);
        a.pos = {ia.x, ia.y};
        a.prev_pos = a.pos;
        int ci = colorIndex(ia.name);
        a.color_idx = (ci < 0) ? 0 : static_cast<uint8_t>(ci);
        a.color_phase = (uint16_t)(rng_() & 0xFFFF);
        a.alive = true;

        // Resolve initial state string into state_str_id (0 = "none").
        if (ia.state && ia.state[0] && !strEq(ia.state, "none")) {
            const auto& type = critter_ir::AGENT_TYPES[a.type_idx];
            for (uint16_t j = 0; j < type.state_count; ++j) {
                if (strEq(type.states[j], ia.state)) {
                    a.state_str_id = static_cast<uint8_t>(j + 1);
                    break;
                }
            }
        }
    }
    return true;
}

// =======================================================================
// sync_time
// =======================================================================

void CritterEngine::syncTime() {
    if (!clock_.valid()) return;
    syncTimeAt(clock_.wall_now(), clock_.zone_offset_hours());
}

void CritterEngine::syncTimeAt(time_t utc_seconds, float zone_offset_hours) {
    time_t local = utc_seconds + static_cast<time_t>(zone_offset_hours * 3600.0f);
    std::tm tm;
#if defined(_WIN32)
    gmtime_s(&tm, &local);
#else
    gmtime_r(&local, &tm);
#endif
    char digits[5];
    std::snprintf(digits, sizeof(digits), "%02d%02d", tm.tm_hour, tm.tm_min);

    for (int x = 0; x < GRID_WIDTH; ++x)
        for (int y = 0; y < GRID_HEIGHT; ++y)
            grid_[x][y].intended = 0;

    // Anchor layout mirrors engine.py sync_time_at: wide grids (width >=
    // 2*height) lay HH MM in a single row; square-ish grids stack HH over MM.
    int coords[4][2];
    if (GRID_WIDTH >= 2 * GRID_HEIGHT) {
        constexpr int row_width = 24;  // 5+1+5+2+5+1+5
        int x0 = (GRID_WIDTH - row_width) / 2; if (x0 < 0) x0 = 0;
        int y0 = (GRID_HEIGHT - 6) / 2;        if (y0 < 0) y0 = 0;
        coords[0][0] = x0;      coords[0][1] = y0;
        coords[1][0] = x0 + 6;  coords[1][1] = y0;
        coords[2][0] = x0 + 13; coords[2][1] = y0;
        coords[3][0] = x0 + 19; coords[3][1] = y0;
    } else {
        coords[0][0] = 2; coords[0][1] = 1;
        coords[1][0] = 8; coords[1][1] = 1;
        coords[2][0] = 2; coords[2][1] = 8;
        coords[3][0] = 8; coords[3][1] = 8;
    }
    for (int i = 0; i < 4; ++i) {
        int d = digits[i] - '0';
        if (d < 0 || d > 9) continue;
        const uint8_t* glyph = MEGAFONT_5X6[d];
        int ox = coords[i][0], oy = coords[i][1];
        for (int row = 0; row < 6; ++row)
            for (int col = 0; col < 5; ++col)
                if (glyph[row] & (0x80 >> col))
                    if (ox+col < GRID_WIDTH && oy+row < GRID_HEIGHT)
                        grid_[ox+col][oy+row].intended = 1;
    }
}

// =======================================================================
// tick
// =======================================================================

void CritterEngine::clearClaims() {
    for (int x = 0; x < GRID_WIDTH; ++x)
        for (int y = 0; y < GRID_HEIGHT; ++y)
            grid_[x][y].claimant = -1;
}

bool CritterEngine::occupiedByNonPainter(int x, int y, int16_t ignore_id) const {
    for (uint16_t i = 0; i < agent_count_; ++i) {
        const Agent& a = agents_[i];
        if (!a.alive || a.id == ignore_id) continue;
        if (isPainter(a.type_idx)) continue;
        if (a.pos.x == x && a.pos.y == y) return true;
    }
    return false;
}

void CritterEngine::applySpawnRules() {
    for (uint16_t r = 0; r < critter_ir::SPAWN_RULE_COUNT; ++r) {
        const auto& rule = critter_ir::SPAWN_RULES[r];
        if (tick_count_ % rule.interval != 0) continue;

        bool cond_ok = strEq(rule.condition, "always") ||
                       checkBoardCondition(rule.condition);
        if (!cond_ok) continue;

        int type_idx = agentTypeIndex(rule.agent_type);
        if (type_idx < 0) continue;
        uint16_t limit = critter_ir::AGENT_TYPES[type_idx].limit;
        uint16_t count = 0;
        for (uint16_t i = 0; i < agent_count_; ++i)
            if (agents_[i].alive && agents_[i].type_idx == (uint16_t)type_idx) ++count;
        if (count >= limit) continue;

        int lm = landmarkIndex(rule.landmark);
        if (lm < 0) continue;
        uint16_t pn = critter_ir::LANDMARKS[lm].point_count;
        if (pn == 0) continue;
        std::uniform_int_distribution<uint16_t> dist(0, pn - 1);
        auto ip = critter_ir::LANDMARKS[lm].points[dist(rng_)];
        Point p{ip.x, ip.y};

        if (agent_count_ >= MAX_AGENTS) continue;
        Agent& a = agents_[agent_count_++];
        std::memset(&a, 0, sizeof(a));
        a.id = next_agent_id_++;
        a.type_idx = (uint16_t)type_idx;
        int bi = behaviorIndex(rule.agent_type);
        a.beh_idx = (bi < 0) ? 0 : (uint16_t)bi;
        a.pos = p;
        a.prev_pos = p;
        int ci = colorIndex(rule.agent_type);
        a.color_idx = (ci < 0) ? 0 : static_cast<uint8_t>(ci);
        a.color_phase = (uint16_t)(rng_() & 0xFFFF);
        a.alive = true;
    }
}

void CritterEngine::tick() {
    ++tick_count_;
    clearClaims();
    applySpawnRules();

    // Reset timing accumulators. astar_us_ is added to inside processAgent
    // at each aStarFirstStep call site; interp_us_ captures total
    // processAgent time and gets astar subtracted out at the end so the
    // two buckets don't double-count.
    astar_us_ = 0;
    uint32_t proc_total_us = 0;

    for (uint16_t i = 0; i < agent_count_; ++i) {
        Agent& a = agents_[i];
        if (!a.alive || a.glitched) continue;

        if (a.remaining_ticks > 0) {
            --a.remaining_ticks;
            if (a.remaining_ticks == 0) a.prev_pos = a.pos;
            else continue;
        }
        uint32_t t0 = now_us();
        processAgent(a);
        proc_total_us += now_us() - t0;
    }

    // interp = total-in-processAgent minus the astar portion it contained.
    interp_us_ = (proc_total_us > astar_us_) ? (proc_total_us - astar_us_) : 0;

    // Compact dead agents (stable by id ordering is preserved because
    // agents_ was appended in id order and we only remove).
    uint16_t w = 0;
    for (uint16_t i = 0; i < agent_count_; ++i)
        if (agents_[i].alive) agents_[w++] = agents_[i];
    agent_count_ = w;

    std::sort(agents_, agents_ + agent_count_,
              [](const Agent& x, const Agent& y) { return x.id < y.id; });

#if IR_MAX_MARKERS > 0
    // Eager marker decay. For each declared marker with decay_k > 0 and
    // decay_t > 0, subtract K from every non-zero count every T ticks.
    // Runs after agent action so the tick's deposits land before they
    // bleed off (parity with engine.py). Zero decay_k or decay_t → no
    // decay, same short-circuit.
    for (uint16_t m = 0; m < critter_ir::MARKER_COUNT; ++m) {
        const critter_ir::Marker& M = critter_ir::MARKERS[m];
        if (M.decay_k == 0 || M.decay_t == 0) continue;
        if (tick_count_ % M.decay_t != 0) continue;
        uint16_t slot = M.index;
        if (slot >= IR_MAX_MARKERS) continue;
        for (int x = 0; x < GRID_WIDTH; ++x)
            for (int y = 0; y < GRID_HEIGHT; ++y) {
                uint8_t c = grid_[x][y].count[slot];
                if (!c) continue;
                grid_[x][y].count[slot] =
                    (c > M.decay_k) ? (uint8_t)(c - M.decay_k) : 0;
            }
    }
#endif

    // Tile-paint fade decay (IR v6). For every lit tile painted with
    // `draw <color> fade N`, decrement age once per tick; when age
    // hits zero, flip state back to 0 (the tile auto-expires). The
    // 0xFFFF sentinel skips both the decrement and the expiry — pre-v6
    // paints and plain `draw <color>` keep their persistent behavior.
    // Runs after agent action and after marker decay so a tick's fresh
    // `draw ... fade N` doesn't pre-decrement (parity with engine.py).
    for (int x = 0; x < GRID_WIDTH; ++x) {
        for (int y = 0; y < GRID_HEIGHT; ++y) {
            Tile& t = grid_[x][y];
            if (!t.state) continue;
            if (t.age_max == 0xFFFF) continue;
            if (t.age == 0) continue;
            if (--t.age == 0) t.state = 0;
        }
    }

    computeConvergence();
}

void CritterEngine::computeConvergence() {
    uint32_t matches = 0;
    uint32_t intended = 0;
    uint32_t lit_intended = 0;
    const uint32_t total = GRID_WIDTH * GRID_HEIGHT;

    for (int x = 0; x < GRID_WIDTH; ++x) {
        for (int y = 0; y < GRID_HEIGHT; ++y) {
            bool eff = grid_[x][y].state || occupiedByNonPainter(x, y);
            bool want = grid_[x][y].intended;
            if (eff == want) ++matches;
            if (want) { ++intended; if (eff) ++lit_intended; }
        }
    }
    if (matches == total) ++metrics_.convergences;
    metrics_.total_intended     += intended;
    metrics_.total_lit_intended += lit_intended;
}

void CritterEngine::despawn(Agent& a) {
    a.alive = false;
}

// =======================================================================
// Conditions
// =======================================================================

bool CritterEngine::tileMatches(int x, int y, const char* kind) const {
    const Tile& t = grid_[x][y];
    if (strEq(kind, "current"))  return t.intended && t.state;
    if (strEq(kind, "extra"))    return !t.intended && t.state;
    if (strEq(kind, "missing"))  return t.intended && !t.state;
    int lm = landmarkIndex(kind);
    if (lm >= 0) {
        const auto& L = critter_ir::LANDMARKS[lm];
        for (uint16_t i = 0; i < L.point_count; ++i)
            if (L.points[i].x == x && L.points[i].y == y) return true;
    }
    return false;
}

bool CritterEngine::checkBoardCondition(const char* kind) const {
    for (int x = 0; x < GRID_WIDTH; ++x)
        for (int y = 0; y < GRID_HEIGHT; ++y)
            if (tileMatches(x, y, kind)) return true;
    return false;
}

bool CritterEngine::evaluateIf(const Agent& a, const char* body) const {
    // Accepts the text AFTER "if ", with any trailing ':' already stripped.

    // Generic `not <rest>` peel: invert and recurse. A single peel
    // covers every supported form symmetrically (board-scope tile
    // predicates, standing-on-color, agent/landmark existence, on-
    // agent colocation, etc.). Compiler-side marker canonicalization
    // (`if not on <m>` → `if on no marker <m>`) has already fired by
    // the time we get here, so this peel doesn't re-handle markers.
    while (*body == ' ') ++body;
    if (body[0] == 'n' && body[1] == 'o' && body[2] == 't' && body[3] == ' ')
        return !evaluateIf(a, body + 4);

    char buf[256];
    std::strncpy(buf, body, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;

    const char* tok[16];
    int n = tokenize(buf, tok, 16);
    if (n == 0) return false;

    // state {==,!=} X
    if (n == 3 && strEq(tok[0], "state") &&
        (strEq(tok[1], "==") || strEq(tok[1], "!="))) {
        // compare agent.state_str_id against the type's state table entry
        const auto& type = critter_ir::AGENT_TYPES[a.type_idx];
        const char* agent_state = (a.state_str_id == 0) ? "none"
            : (a.state_str_id <= type.state_count ? type.states[a.state_str_id - 1] : "none");
        bool eq = strEq(agent_state, tok[2]);
        return strEq(tok[1], "==") ? eq : !eq;
    }

    // random N[%]
    if (n == 2 && strEq(tok[0], "random")) {
        int pct = parseIntOr(tok[1], 0);
        std::uniform_int_distribution<int> d(1, 100);
        return d(const_cast<std::mt19937&>(rng_)) <= pct;
    }

    // on (current|extra|missing) — bare form with no kind prefix
    if (n == 2 && strEq(tok[0], "on")) {
        const char* kind = tok[1];
        if (strEq(kind, "current") || strEq(kind, "extra") || strEq(kind, "missing"))
            return tileMatches(a.pos.x, a.pos.y, kind);
    }
    // standing on (current|extra|missing)
    if (n == 3 && strEq(tok[0], "standing") && strEq(tok[1], "on")) {
        const char* kind = tok[2];
        if (strEq(kind, "current") || strEq(kind, "extra") || strEq(kind, "missing"))
            return tileMatches(a.pos.x, a.pos.y, kind);
    }

    // standing on landmark <name> — same semantics as `on landmark <name>`.
    if (n == 4 && strEq(tok[0], "standing") && strEq(tok[1], "on")
            && strEq(tok[2], "landmark")) {
        int lm = landmarkIndex(tok[3]);
        if (lm < 0) return false;
        const auto& L = critter_ir::LANDMARKS[lm];
        for (uint16_t i = 0; i < L.point_count; ++i)
            if (L.points[i].x == a.pos.x && L.points[i].y == a.pos.y) return true;
        return false;
    }

    // standing on color <name> — tile at agent pos is lit AND its current
    // resolved RGB matches the named color. Cycle colors compare against
    // the color's live phase (same resolution path used by painting).
    if (n == 4 && strEq(tok[0], "standing") && strEq(tok[1], "on")
            && strEq(tok[2], "color")) {
        const Tile& t = grid_[a.pos.x][a.pos.y];
        if (!t.state) return false;
        uint8_t r, g, b;
        if (!colorRGB(tok[3], r, g, b)) return false;
        return t.r == r && t.g == g && t.b == b;
    }

    // on landmark <name>
    if (n == 3 && strEq(tok[0], "on") && strEq(tok[1], "landmark")) {
        int lm = landmarkIndex(tok[2]);
        if (lm < 0) return false;
        const auto& L = critter_ir::LANDMARKS[lm];
        for (uint16_t i = 0; i < L.point_count; ++i)
            if (L.points[i].x == a.pos.x && L.points[i].y == a.pos.y) return true;
        return false;
    }

    // on agent <name> [with state == <value>]
    // 3 tokens: no state filter.  7 tokens: "with state == <value>" suffix.
    if ((n == 3 || n == 7) && strEq(tok[0], "on") && strEq(tok[1], "agent")) {
        const char* want_state = nullptr;
        if (n == 7) {
            if (!strEq(tok[3], "with") || !strEq(tok[4], "state") || !strEq(tok[5], "=="))
                return false;
            want_state = tok[6];
        }
        for (uint16_t i = 0; i < agent_count_; ++i) {
            const Agent& o = agents_[i];
            if (!o.alive || o.id == a.id) continue;
            if (!strEq(critter_ir::AGENT_TYPES[o.type_idx].name, tok[2])) continue;
            if (o.pos.x != a.pos.x || o.pos.y != a.pos.y) continue;
            if (want_state) {
                const auto& otype = critter_ir::AGENT_TYPES[o.type_idx];
                const char* os = (o.state_str_id == 0) ? "none"
                    : (o.state_str_id <= otype.state_count ? otype.states[o.state_str_id - 1] : "none");
                if (!strEq(os, want_state)) continue;
            }
            return true;
        }
        return false;
    }

    // Board-level (no pos prefix): current/extra/missing
    if (n == 1 && (strEq(tok[0], "current") || strEq(tok[0], "extra") || strEq(tok[0], "missing")))
        return checkBoardCondition(tok[0]);

    // agent <name>
    if (n == 2 && strEq(tok[0], "agent")) {
        for (uint16_t i = 0; i < agent_count_; ++i) {
            const Agent& o = agents_[i];
            if (!o.alive || o.id == a.id) continue;
            if (strEq(critter_ir::AGENT_TYPES[o.type_idx].name, tok[1])) return true;
        }
        return false;
    }

    // landmark <name> — defined-ness test
    if (n == 2 && strEq(tok[0], "landmark"))
        return landmarkIndex(tok[1]) >= 0;

#if IR_MAX_MARKERS > 0
    // --- Marker predicates. Compiler canonicalizes sugared forms
    //     (`if trail > 0`, `if on trail`, `if no trail`) into these
    //     `marker`-keyed shapes, so the HAL only has to handle canon.
    //     All evaluate to false when the named marker is unknown —
    //     matches the engine.py "unrecognized → false" posture.

    // Self-cell: "on marker <m> (>|<) N"  (5 tokens)
    if (n == 5 && strEq(tok[0], "on") && strEq(tok[1], "marker")
            && (strEq(tok[3], ">") || strEq(tok[3], "<"))) {
        int slot = markerSlot(tok[2]);
        if (slot < 0) return false;
        int nn = parseIntOr(tok[4], 0);
        int cnt = grid_[a.pos.x][a.pos.y].count[slot];
        return strEq(tok[3], ">") ? (cnt > nn) : (cnt < nn);
    }
    // Self-cell: "on no marker <m>"  (4 tokens)
    if (n == 4 && strEq(tok[0], "on") && strEq(tok[1], "no")
            && strEq(tok[2], "marker")) {
        int slot = markerSlot(tok[3]);
        if (slot < 0) return false;
        return grid_[a.pos.x][a.pos.y].count[slot] == 0;
    }

    // Scoped predicate helpers: evaluate over all grid cells, or only
    // cells belonging to a given landmark. `op`: '>', '<', or 0 for
    // the `no` form (universal: all cells must be zero).
    auto eval_marker_scope = [&](int slot, char op, int nn,
                                 const char* landmark) -> bool {
        if (slot < 0) return false;
        if (landmark) {
            int lm = landmarkIndex(landmark);
            if (lm < 0) return false;
            const auto& L = critter_ir::LANDMARKS[lm];
            if (op == 0) {
                for (uint16_t i = 0; i < L.point_count; ++i)
                    if (grid_[L.points[i].x][L.points[i].y].count[slot] != 0)
                        return false;
                return true;
            }
            for (uint16_t i = 0; i < L.point_count; ++i) {
                int c = grid_[L.points[i].x][L.points[i].y].count[slot];
                if (op == '>' ? (c > nn) : (c < nn)) return true;
            }
            return false;
        }
        // Board-wide scan.
        if (op == 0) {
            for (int x = 0; x < GRID_WIDTH; ++x)
                for (int y = 0; y < GRID_HEIGHT; ++y)
                    if (grid_[x][y].count[slot] != 0) return false;
            return true;
        }
        for (int x = 0; x < GRID_WIDTH; ++x)
            for (int y = 0; y < GRID_HEIGHT; ++y) {
                int c = grid_[x][y].count[slot];
                if (op == '>' ? (c > nn) : (c < nn)) return true;
            }
        return false;
    };

    // Scoped: "no marker <m>"                 (3 tokens)
    //         "no marker <m> on landmark <l>" (6 tokens)
    if ((n == 3 || n == 6) && strEq(tok[0], "no") && strEq(tok[1], "marker")) {
        if (n == 6 && !(strEq(tok[3], "on") && strEq(tok[4], "landmark")))
            return false;
        const char* lm = (n == 6) ? tok[5] : nullptr;
        return eval_marker_scope(markerSlot(tok[2]), 0, 0, lm);
    }
    // Scoped: "marker <m> (>|<) N"                    (4 tokens)
    //         "marker <m> (>|<) N on landmark <l>"    (7 tokens)
    if ((n == 4 || n == 7) && strEq(tok[0], "marker")
            && (strEq(tok[2], ">") || strEq(tok[2], "<"))) {
        if (n == 7 && !(strEq(tok[4], "on") && strEq(tok[5], "landmark")))
            return false;
        int nn = parseIntOr(tok[3], 0);
        char op = tok[2][0];
        const char* lm = (n == 7) ? tok[6] : nullptr;
        return eval_marker_scope(markerSlot(tok[1]), op, nn, lm);
    }
#endif

    return false;
}

// =======================================================================
// A*
// =======================================================================

bool CritterEngine::aStarPlan(Point start, Point target, uint16_t type_idx,
                              PlannedStep* out_steps, int max_steps, int& out_len,
                              Point last_step) {
    out_len = 0;
    if (max_steps < 1) return false;
    if (start.x == target.x && start.y == target.y) return false;

    const bool diag = pfDiagonal(type_idx);
    const float dcost = diag ? pfDiagonalCost(type_idx) : 1.0f;
    const int32_t max_nodes   = pfInt(type_idx, "max_nodes", PF_DEF_MAX_NODES);
    const int32_t pen_lit     = pfInt(type_idx, "penalty_lit", PF_DEF_PENALTY_LIT);
    const int32_t pen_occ     = pfInt(type_idx, "penalty_occupied", PF_DEF_PENALTY_OCCUPIED);

    struct Move { int dx, dy; float cost; };
    Move moves_ortho[4] = {{0,1,1.0f},{0,-1,1.0f},{1,0,1.0f},{-1,0,1.0f}};
    Move moves_diag[8]  = {{0,1,1.0f},{0,-1,1.0f},{1,0,1.0f},{-1,0,1.0f},
                           {1,1,dcost},{1,-1,dcost},{-1,1,dcost},{-1,-1,dcost}};
    const Move* moves = diag ? moves_diag : moves_ortho;
    const int   nmoves = diag ? 8 : 4;

    auto hfn = [&](Point p) -> float {
        int dx = std::abs(target.x - p.x);
        int dy = std::abs(target.y - p.y);
        if (diag) return dcost * std::min(dx, dy) + std::abs(dx - dy);
        return static_cast<float>(dx + dy);
    };

    const int W = GRID_WIDTH, H = GRID_HEIGHT, N = W * H;
    auto idx = [&](int x, int y) { return y * W + x; };

    // Reuse engine-owned scratch. Resize (no-op after first call) + reset.
    pf_cost_.assign(N, std::numeric_limits<float>::infinity());
    pf_from_.assign(N, -1);
    pf_heap_.clear();
    auto& cost = pf_cost_;
    auto& from = pf_from_;
    cost[idx(start.x, start.y)] = 0.0f;

    using QE = std::pair<float, int>;
    // priority_queue-on-existing-container idiom: min-heap by std::greater.
    auto q_push = [&](QE v) {
        pf_heap_.push_back(v);
        std::push_heap(pf_heap_.begin(), pf_heap_.end(), std::greater<QE>{});
    };
    auto q_pop = [&]() -> QE {
        std::pop_heap(pf_heap_.begin(), pf_heap_.end(), std::greater<QE>{});
        QE v = pf_heap_.back();
        pf_heap_.pop_back();
        return v;
    };
    q_push({hfn(start), idx(start.x, start.y)});

    int   nodes     = 0;
    const int start_idx = idx(start.x, start.y);
    // best_node/best_h track the best partial progress if A* doesn't reach
    // target before running out of budget. Deliberately *not* pre-seeded with
    // start — that was a latent bug: if no popped node ever had h strictly
    // below hfn(start) (e.g. agent at a local heuristic minimum inside the
    // budget-reachable neighborhood, which is easy to hit with small
    // max_nodes + occupancy/lit penalties steering A* sideways), best_node
    // stayed at start, firstStepFrom(start) returned {0,0}, aStarFirstStep
    // returned false, caller held the agent in place with 1-tick retry, and
    // the next tick re-derived the same stuck state. Agent hangs forever.
    // Now: only non-start pops are candidates, and if no pop ever beats the
    // heuristic, we fall back to the frontier scan below.
    int   best_node = -1;
    float best_h    = std::numeric_limits<float>::infinity();

    // Reconstruct the planned path from `end` back to start, writing up to
    // max_steps entries of (dx, dy, per-edge cost) into out_steps starting
    // at out_steps[0]. Returns the count written. The per-edge cost comes
    // straight from A*'s cost[] array (g-cost delta between predecessor
    // and successor) — this is exactly what the runtime cost-budget check
    // in stepTowardTarget compares against. Used for both the "reached
    // target" and "best-partial-progress" exits; partial-progress callers
    // cap to 1 step because speculative plans past the partial-endpoint
    // are more guess than plan.
    auto planFromEnd = [&](int end, int cap) -> int {
        pf_trail_.clear();
        int cur = end;
        // Guard both by start arrival and by total steps — a malformed
        // from[] (shouldn't happen, but…) can't loop us forever.
        while (cur != start_idx && from[cur] != -1
                && (int)pf_trail_.size() < N) {
            pf_trail_.push_back(cur);
            int next = from[cur];
            if (next == cur) break;
            cur = next;
        }
        if (pf_trail_.empty() || cur != start_idx) return 0;

        int path_len = (int)pf_trail_.size();
        int n = std::min(path_len, cap);
        // pf_trail_ is ordered end→start; reverse while emitting so
        // out_steps[0] is the first move the agent takes from `start`.
        for (int i = 0; i < n; ++i) {
            int node = pf_trail_[path_len - 1 - i];
            int prev = (i == 0) ? start_idx : pf_trail_[path_len - i];
            int nx = node % W, ny = node / W;
            int px = prev % W, py = prev / W;
            out_steps[i].dx   = static_cast<int8_t>(nx - px);
            out_steps[i].dy   = static_cast<int8_t>(ny - py);
            // cost delta is pure A*-charged edge cost; includes the
            // backtrack bias iff i == 0 and this edge reversed last_step,
            // which is harmless (runtime check stays consistent with
            // plan-time accounting either way).
            out_steps[i].cost = cost[node] - cost[prev];
        }
        return n;
    };

    // Precompute the "backtrack tile" — the neighbor of start that would
    // reverse last_step. -1 when we have no last-step signal (fresh agent,
    // just spawned, just wandered with dx=dy=0). When partial-progress
    // mode has to pick *any* step and two alternatives look equal-ish by
    // heuristic, preferring a non-backtrack tile is what actually breaks
    // the A↔B cycle — the edge-cost bias above only shapes A* internal
    // expansion, not the best-partial-progress selection below.
    const int backtrack_idx =
        (last_step.x != 0 || last_step.y != 0)
          ? ((start.y - last_step.y) * W + (start.x - last_step.x))
          : -1;
    auto is_backtrack_step = [&](PlannedStep s) {
        return backtrack_idx >= 0
            && s.dx == -last_step.x && s.dy == -last_step.y;
    };

    // Shared fallback for "A* gave up before reaching target." Prefers the
    // best_node the main loop tracked; if that's missing (no heuristic
    // progress at all), scan the start's adjacent tiles for whichever one
    // A* managed to relax (cost < infinity) with the lowest h. "Step
    // wherever A* at least reached" beats standing still.
    //
    // Anti-oscillation: if the first step toward the chosen node reverses
    // last_step, rescan the start-adjacent frontier for any non-backtrack
    // neighbor A* relaxed. Accept the backtrack only when literally no
    // other neighbor was reached — no-walls grid + tight budget can still
    // technically hit that case if everything around start is lit+occupied.
    // Returns false only when no neighbor at all was explored.
    auto step_from_best = [&]() -> bool {
        auto frontier_pick = [&](bool allow_backtrack) -> int {
            int bn = -1;
            float fb_h = std::numeric_limits<float>::infinity();
            for (int m = 0; m < nmoves; ++m) {
                int nx = start.x + moves[m].dx, ny = start.y + moves[m].dy;
                if (nx < 0 || nx >= W || ny < 0 || ny >= H) continue;
                int ni = idx(nx, ny);
                if (cost[ni] == std::numeric_limits<float>::infinity()) continue;
                if (!allow_backtrack && ni == backtrack_idx) continue;
                float nh = hfn({static_cast<int8_t>(nx), static_cast<int8_t>(ny)});
                if (bn < 0 || nh < fb_h) { fb_h = nh; bn = ni; }
            }
            return bn;
        };

        // Partial-progress exits always emit a *single* step. Committing
        // to a multi-step plan past a speculative endpoint would amplify
        // guess error — the first step beyond it was never A*-validated
        // at all.
        int bn = best_node;
        if (bn < 0) {
            bn = frontier_pick(/*allow_backtrack=*/false);
            if (bn < 0) bn = frontier_pick(/*allow_backtrack=*/true);
            if (bn < 0) return false;
        }
        int n = planFromEnd(bn, 1);
        if (n == 0 || is_backtrack_step(out_steps[0])) {
            // best_node's first step walks us backward. Retry via the
            // adjacency scan with backtrack excluded.
            int alt = frontier_pick(/*allow_backtrack=*/false);
            if (alt >= 0) {
                int an = planFromEnd(alt, 1);
                if (an > 0 && !is_backtrack_step(out_steps[0])) {
                    out_len = 1;
                    return true;
                }
            }
            // No clean alternative. If the original step is a legitimate
            // (non-zero) backtrack, take it — staying put is worse than
            // oscillating one tick while the world changes around us.
            if (n > 0) { out_len = 1; return true; }
            return false;
        }
        out_len = n;   // n == 1 in the partial-progress path
        return true;
    };

    while (!pf_heap_.empty()) {
        auto [pri, ci] = q_pop();
        (void)pri;
        int cx = ci % W, cy = ci / W;
        ++nodes;

        if (cx == target.x && cy == target.y) {
            int n = planFromEnd(ci, max_steps);
            if (n == 0) return false;
            out_len = n;
            return true;
        }

        float h = hfn({static_cast<int8_t>(cx), static_cast<int8_t>(cy)});
        // Best-partial-progress eligibility excludes both start itself
        // (covered above — see the latent-bug comment) AND the backtrack
        // tile. Skipping backtrack here means the partial-progress return
        // path can't pick "retreat one square" as its answer — which was
        // the oscillation: both ends of an A↔B pair saw the opposite as
        // the closest-h neighbor within budget and flipped each tick.
        if (ci != start_idx && ci != backtrack_idx && h < best_h) {
            best_h = h; best_node = ci;
        }

        if (nodes >= max_nodes) {
            ++metrics_.failed_seeks;
            return step_from_best();
        }

        for (int m = 0; m < nmoves; ++m) {
            int nx = cx + moves[m].dx, ny = cy + moves[m].dy;
            if (nx < 0 || nx >= W || ny < 0 || ny >= H) continue;
            float nc = cost[ci] + moves[m].cost;
            if (grid_[nx][ny].state) nc += pen_lit;
            // engine.py:208 — A* penalizes occupancy by ANY agent (painters
            // included), even though _effective_state excludes them.
            bool any_agent = false;
            for (uint16_t k = 0; k < agent_count_; ++k) {
                if (!agents_[k].alive) continue;
                if (agents_[k].pos.x == nx && agents_[k].pos.y == ny) { any_agent = true; break; }
            }
            if (any_agent) nc += pen_occ;
            // Backtrack bias: only applied to edges leaving the start node,
            // and only against the tile the agent just came from. 2.0 is
            // tuned to beat the symmetric-geometry tie (lateral move costs
            // 1; backtrack costs 1+2=3) while still losing to genuinely
            // blocked-elsewhere paths (a lit+occupied neighbor at 1+10+20
            // dwarfs the bias). Interior A* expansion is untouched so
            // longer planned paths that happen to wrap back past the start
            // still cost correctly. See Agent::last_step for the signal.
            if (ci == start_idx
                    && (last_step.x != 0 || last_step.y != 0)
                    && nx == start.x - last_step.x
                    && ny == start.y - last_step.y) {
                nc += 2.0f;
            }
            int ni = idx(nx, ny);
            if (nc < cost[ni]) {
                cost[ni] = nc;
                from[ni] = static_cast<int16_t>(ci);
                q_push({nc + hfn({static_cast<int8_t>(nx), static_cast<int8_t>(ny)}), ni});
            }
        }
    }

    // Heap drained without reaching target. Rare on a connected no-walls
    // grid (target eventually pops even with heavy penalties), but same
    // fallback applies if it ever does.
    return step_from_best();
}

// Commit one move toward `target` for agent `a`, reusing a cached plan
// when it's still valid or replanning via aStarPlan. The two validity
// checks are:
//   (1) Target drift — seek re-picks nearest each tick; if that moved,
//       the old plan heads somewhere we're no longer chasing.
//   (2) Cost-budget surprise — the next cached step's *current* edge cost
//       (lit / occupied by another agent now, even if it wasn't at plan
//       time) is compared against what A* charged when the plan was made,
//       with a running `plan_slack` accumulator soaking up small under-
//       budget windfalls. A step whose real cost exceeds planned + slack
//       means reality drifted off model → replan.
//
// The mechanism composes cleanly: at plan_horizon=1 the plan is always
// consumed in one tick and (2) never fires (slack starts at 0, plan is
// fresh). At higher horizons, (2) is what gives back the reactivity the
// horizon otherwise trades away.
bool CritterEngine::stepTowardTarget(Agent& a, Point target) {
    const bool diag = pfDiagonal(a.type_idx);
    const float dcost = diag ? pfDiagonalCost(a.type_idx) : 1.0f;
    const int32_t pen_lit = pfInt(a.type_idx, "penalty_lit", PF_DEF_PENALTY_LIT);
    const int32_t pen_occ = pfInt(a.type_idx, "penalty_occupied", PF_DEF_PENALTY_OCCUPIED);
    const int32_t step_rate = pfInt(a.type_idx, "step_rate", PF_DEF_STEP_RATE);

    // Compute the *current* edge cost for stepping from `a.pos` by
    // (dx, dy). Mirrors the cost function inside aStarPlan exactly so
    // plan-time and runtime accounting stay on the same ruler.
    auto edge_cost_now = [&](int8_t dx, int8_t dy, int nx, int ny) -> float {
        float c = (dx != 0 && dy != 0) ? dcost : 1.0f;
        if (grid_[nx][ny].state) c += pen_lit;
        for (uint16_t k = 0; k < agent_count_; ++k) {
            if (!agents_[k].alive || agents_[k].id == a.id) continue;
            if (agents_[k].pos.x == nx && agents_[k].pos.y == ny) {
                c += pen_occ;
                break;
            }
        }
        return c;
    };

    auto commit_step = [&](int8_t dx, int8_t dy) {
        int nx = a.pos.x + dx;
        int ny = a.pos.y + dy;
        a.prev_pos = a.pos;
        a.pos = {static_cast<int8_t>(nx), static_cast<int8_t>(ny)};
        a.last_step = {dx, dy};
        a.step_duration   = (int16_t)step_rate;
        a.remaining_ticks = (int16_t)step_rate;
    };

    // Post-plan drunkenness — mirror of engine.py::_drunken_perturb.
    // Returns the (dx, dy) to actually commit. Sets `freeze=true` to
    // signal the caller to idle for a tick (no move). Called with the
    // planner-optimal (pdx, pdy); we decide sober/freeze/orthogonal
    // stagger based on a uniform roll vs pfDrunkenness.
    //
    // Short-circuits at drunk<=0 so the sober hot path (almost every
    // script, today) does a single float comparison and consumes no
    // RNG state — matches the Python sim's zero-overhead sober path.
    //
    // On orthogonal failure (both 90°-rotations out-of-bounds or
    // occupied), degrades to freeze rather than forcing the planned
    // step: a stagger into a wall is better-read as "drunk" than
    // "teleported through an obstacle."
    auto drunken_perturb = [&](int8_t pdx, int8_t pdy,
                               int8_t& odx, int8_t& ody,
                               bool& freeze) -> void {
        freeze = false;
        float drunk = pfDrunkenness(a.type_idx);
        if (drunk <= 0.0f) { odx = pdx; ody = pdy; return; }
        if (drunk > 1.0f)  drunk = 1.0f;
        std::uniform_real_distribution<float> ud(0.0f, 1.0f);
        float r = ud(rng_);
        if (r >= drunk) { odx = pdx; ody = pdy; return; }
        if (r < drunk * 0.5f) { freeze = true; return; }
        // Orthogonal stagger. 90° rotations of (pdx, pdy). For 4-conn
        // planned moves one component is 0 so orthos are the two
        // perpendicular cells; for diagonal planned moves orthos are
        // the other two diagonals — still reads as a swerve.
        const int8_t o1_dx = (int8_t)(-pdy), o1_dy = (int8_t)(pdx);
        const int8_t o2_dx = (int8_t)(pdy),  o2_dy = (int8_t)(-pdx);
        // Coin-flip order so stagger direction is unbiased.
        std::uniform_int_distribution<int> coin(0, 1);
        int8_t ax, ay, bx, by;
        if (coin(rng_)) { ax=o1_dx; ay=o1_dy; bx=o2_dx; by=o2_dy; }
        else            { ax=o2_dx; ay=o2_dy; bx=o1_dx; by=o1_dy; }
        auto try_ortho = [&](int8_t dx, int8_t dy) -> bool {
            int nx = a.pos.x + dx, ny = a.pos.y + dy;
            if (nx < 0 || nx >= GRID_WIDTH || ny < 0 || ny >= GRID_HEIGHT)
                return false;
            for (uint16_t k = 0; k < agent_count_; ++k) {
                if (!agents_[k].alive || agents_[k].id == a.id) continue;
                if (agents_[k].pos.x == nx && agents_[k].pos.y == ny)
                    return false;
            }
            return true;
        };
        if (try_ortho(ax, ay)) { odx = ax; ody = ay; return; }
        if (try_ortho(bx, by)) { odx = bx; ody = by; return; }
        freeze = true;  // both orthos blocked — stagger into a wall = freeze
    };

    // ---- try cached plan ----
    if (a.plan_len > 0 && a.plan_cursor < a.plan_len) {
        // Target drift: compare against stored plan_target.
        if (a.plan_target.x != target.x || a.plan_target.y != target.y) {
            a.plan_len = 0;  // fall through to replan
        } else {
            const PlannedStep& ps = a.plan[a.plan_cursor];
            int nx = a.pos.x + ps.dx;
            int ny = a.pos.y + ps.dy;
            if (nx < 0 || nx >= GRID_WIDTH || ny < 0 || ny >= GRID_HEIGHT) {
                // Shouldn't happen (A* bounds-checked at plan time) but
                // if the grid shrank mid-plan or the agent teleported via
                // a scripted `step`, bail out and replan.
                a.plan_len = 0;
            } else {
                float actual = edge_cost_now(ps.dx, ps.dy, nx, ny);
                if (actual > ps.cost + a.plan_slack) {
                    // Surprise. Replan.
                    a.plan_len = 0;
                } else {
                    // Drunkenness fires AFTER the surprise-cost check: we
                    // already know the planned step is still valid, we're
                    // just deciding whether to execute it or stagger.
                    int8_t odx, ody; bool freeze;
                    drunken_perturb(ps.dx, ps.dy, odx, ody, freeze);
                    if (freeze) {
                        // Don't advance the plan cursor — we'll retry the
                        // same step next tick. Cheap; no replan cost.
                        a.step_duration   = 1;
                        a.remaining_ticks = 1;
                        return true;
                    }
                    if (odx != ps.dx || ody != ps.dy) {
                        // Orthogonal stagger — we've walked off the plan.
                        // Invalidate so next tick replans from the new
                        // position (the cached plan's remaining steps
                        // assumed we were on the straight-line path).
                        a.plan_len = 0;
                        commit_step(odx, ody);
                        return true;
                    }
                    a.plan_slack += ps.cost - actual;  // stays >= 0 by construction
                    ++a.plan_cursor;
                    commit_step(ps.dx, ps.dy);
                    return true;
                }
            }
        }
    }

    // ---- replan ----
    int32_t horizon = pfInt(a.type_idx, "plan_horizon", 1);
    if (horizon < 1) horizon = 1;
    if (horizon > PLAN_MAX) horizon = PLAN_MAX;

    int plan_len = 0;
    bool ok = aStarPlan(a.pos, target, a.type_idx,
                        a.plan, (int)horizon, plan_len, a.last_step);
    if (!ok || plan_len <= 0) {
        a.plan_len = 0;
        a.step_duration   = 1;
        a.remaining_ticks = 1;
        return false;
    }
    const PlannedStep& first = a.plan[0];
    int8_t odx, ody; bool freeze;
    drunken_perturb(first.dx, first.dy, odx, ody, freeze);
    if (freeze) {
        // Discard the fresh plan: we didn't execute its first step, so
        // the cached cursor=1 position would be wrong. Replan next tick.
        a.plan_len = 0;
        a.step_duration   = 1;
        a.remaining_ticks = 1;
        return true;
    }
    if (odx != first.dx || ody != first.dy) {
        // Orthogonal stagger on a fresh plan — same invalidation as above.
        a.plan_len = 0;
        commit_step(odx, ody);
        return true;
    }
    a.plan_len     = static_cast<uint8_t>(plan_len);
    a.plan_cursor  = 1;                 // cursor advances over the step we execute below
    a.plan_target  = target;
    a.plan_slack   = 0.0f;
    commit_step(first.dx, first.dy);
    return true;
}

// =======================================================================
// processAgent (interpreter)
// =======================================================================

void CritterEngine::processAgent(Agent& a) {
    const auto& beh = critter_ir::BEHAVIORS[a.beh_idx];
    const auto* insns = beh.insns;
    const int16_t n = static_cast<int16_t>(beh.insn_count);
    if (n == 0) return;

    int16_t pc = a.pc;
    const int16_t original_pc = a.pc;
    int executed = 0;

    while (pc < n) {
        if (executed > 100) {
            a.glitched = true;
            ++metrics_.glitches;
            return;
        }
        ++executed;

        uint8_t indent  = insns[pc].indent;
        const char* inst = insns[pc].text;

        // ---- if ... ----
        if (startsWith(inst, "if ")) {
            const char* body = inst + 3;
            // Strip trailing ':'
            char buf[256];
            std::strncpy(buf, body, sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = 0;
            size_t L = std::strlen(buf);
            if (L && buf[L - 1] == ':') buf[L - 1] = 0;
            bool ok = evaluateIf(a, buf);
            if (ok) { ++pc; }
            else {
                ++pc;
                while (pc < n && insns[pc].indent > indent) ++pc;
            }
            continue;
        }

        // ---- else: ----
        if (strEq(inst, "else:")) {
            ++pc;
            while (pc < n && insns[pc].indent > indent) ++pc;
            continue;
        }

        // ---- done ----
        if (strEq(inst, "done")) { a.pc = 0; return; }

        // ---- set state = X ----
        if (startsWith(inst, "set state =")) {
            const char* v = inst + std::strlen("set state =");
            while (*v == ' ') ++v;
            // Resolve into state_str_id
            const auto& type = critter_ir::AGENT_TYPES[a.type_idx];
            uint8_t sid = 0;
            for (uint16_t i = 0; i < type.state_count; ++i)
                if (strEq(type.states[i], v)) { sid = (uint8_t)(i + 1); break; }
            a.state_str_id = sid;
            ++pc;
            continue;
        }

        // ---- set color = <name> ----
        if (startsWith(inst, "set color =")) {
            const char* v = inst + std::strlen("set color =");
            while (*v == ' ') ++v;
            int ci = colorIndex(v);
            if (ci >= 0) a.color_idx = (uint8_t)ci;
            ++pc;
            continue;
        }

        // ---- erase ----
        if (strEq(inst, "erase")) {
            grid_[a.pos.x][a.pos.y].state = 0;
            ++pc;
            continue;
        }

#if IR_MAX_MARKERS > 0
        // ---- incr / decr marker <name> [N] ----
        // Wire form post-compiler canonicalization: `incr marker trail [N]`
        // (default N=1). Clamped to [0, 255] like the sim — overflow just
        // saturates at the cap rather than wrapping. Unknown marker names
        // are silently skipped; the compiler rejects them up front, so
        // reaching this branch with a bad name means a hand-rolled blob
        // and we prefer "no effect" over crashing the tick.
        if (startsWith(inst, "incr marker") || startsWith(inst, "decr marker")) {
            bool is_incr = (inst[0] == 'i');
            char buf[64];
            std::strncpy(buf, inst, sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = 0;
            const char* tok[8]; int t = tokenize(buf, tok, 8);
            if (t >= 3) {
                int slot = markerSlot(tok[2]);
                if (slot >= 0) {
                    int n = (t >= 4) ? parseIntOr(tok[3], 1) : 1;
                    if (n < 0) n = 0;
                    uint8_t& c = grid_[a.pos.x][a.pos.y].count[slot];
                    if (is_incr) {
                        int nc = (int)c + n;
                        if (nc > 255) nc = 255;
                        c = (uint8_t)nc;
                    } else {
                        int nc = (int)c - n;
                        if (nc < 0) nc = 0;
                        c = (uint8_t)nc;
                    }
                }
            }
            ++pc;
            continue;
        }
#endif

        // ---- draw [color] [(fade|hold) N] ----
        if (startsWith(inst, "draw")) {
            grid_[a.pos.x][a.pos.y].state = 1;
            char buf[128];
            std::strncpy(buf, inst, sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = 0;
            const char* tok[8]; int t = tokenize(buf, tok, 8);
            // Tile-paint fade (IR v6). Optional `(fade|hold) N` tail.
            // Compiler-validated and mutually exclusive, so trust the
            // structure: if the second-to-last token is "fade" or
            // "hold", the last token parses as a positive int. Strip
            // it from the visible token count so the color-name path
            // below sees the same shape as the pre-v6 form.
            uint16_t fade_n = 0xFFFF;
            uint8_t  hold_mode = 0;
            if (t >= 2 && (strEq(tok[t - 2], "fade") || strEq(tok[t - 2], "hold"))) {
                int n = parseIntOr(tok[t - 1], 0);
                if (n > 0 && n < 0xFFFF) fade_n = (uint16_t)n;
                if (strEq(tok[t - 2], "hold")) hold_mode = 1;
                t -= 2;
            }
            if (t > 1) {
                // Snapshot the agent's phase-adjusted color so painted tiles
                // match what the agent looked like at paint time. Tile stays
                // static after — no live animation on tiles.
                //
                // Also stash the color index itself in color_ref so a
                // day/night flip can re-resolve the tile against the other
                // palette (see setNightMode below). `ci < 0` (unknown
                // color, clamp to white) writes the sentinel 0xFF — there's
                // no name on file to re-resolve against, so the night
                // transition leaves the tile's RGB alone. Since IR_MAX_COLORS
                // is 32 by default and never approaches 255, uint8_t is
                // plenty; 0xFF is an unambiguous "not a color index."
                int ci = colorIndex(tok[1]);
                uint8_t r, g, b;
                if (ci >= 0) {
                    resolveColor(ci, render_frame_ + a.color_phase, r, g, b);
                    grid_[a.pos.x][a.pos.y].color_ref = (uint8_t)ci;
                } else {
                    r = g = b = 255;
                    grid_[a.pos.x][a.pos.y].color_ref = 0xFF;
                }
                grid_[a.pos.x][a.pos.y].r = r;
                grid_[a.pos.x][a.pos.y].g = g;
                grid_[a.pos.x][a.pos.y].b = b;
            }
            // Always reset age/age_max/hold_mode from the opcode — no
            // carry, no max-of (TODO §328-334). Plain `draw <color>`
            // clears any prior fade/hold by writing the no-fade
            // sentinel and a zero hold flag.
            grid_[a.pos.x][a.pos.y].age = fade_n;
            grid_[a.pos.x][a.pos.y].age_max = fade_n;
            grid_[a.pos.x][a.pos.y].hold_mode = hold_mode;
            ++pc;
            continue;
        }

        // ---- pause [N|LO-HI] ----
        if (startsWith(inst, "pause")) {
            char buf[64];
            std::strncpy(buf, inst, sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = 0;
            const char* tok[8]; int t = tokenize(buf, tok, 8);
            int ticks = 1;
            if (t > 1) {
                const char* dash = std::strchr(tok[1], '-');
                if (dash) {
                    int lo = std::atoi(tok[1]);
                    int hi = std::atoi(dash + 1);
                    if (hi < lo) std::swap(lo, hi);
                    std::uniform_int_distribution<int> d(lo, hi);
                    ticks = d(rng_);
                } else {
                    ticks = parseIntOr(tok[1], 1);
                }
            }
            a.remaining_ticks = ticks;
            a.step_duration   = 1;
            a.prev_pos        = a.pos;
            a.pc              = pc + 1;
            return;
        }

        // ---- despawn [name] ----
        if (startsWith(inst, "despawn")) {
            char buf[64];
            std::strncpy(buf, inst, sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = 0;
            const char* tok[8]; int t = tokenize(buf, tok, 8);
            if (t == 1) { despawn(a); return; }
            // despawn agent <name>  or legacy despawn <name>
            const char* target = tok[t - 1];
            for (uint16_t i = 0; i < agent_count_; ++i) {
                Agent& o = agents_[i];
                if (!o.alive || o.id == a.id) continue;
                if (!strEq(critter_ir::AGENT_TYPES[o.type_idx].name, target)) continue;
                if (o.pos.x == a.pos.x && o.pos.y == a.pos.y) { o.alive = false; break; }
            }
            ++pc;
            continue;
        }

        // ---- step (dx, dy) ----
        // Literal relative move. No collision check — caller (coati bobbing at
        // pool) accepts the fragility. Target clamped to grid bounds.
        if (startsWith(inst, "step")) {
            int dx = 0, dy = 0;
            std::sscanf(inst, "step ( %d , %d )", &dx, &dy);
            int nx = a.pos.x + dx;
            int ny = a.pos.y + dy;
            if (nx < 0) nx = 0; else if (nx >= GRID_WIDTH)  nx = GRID_WIDTH  - 1;
            if (ny < 0) ny = 0; else if (ny >= GRID_HEIGHT) ny = GRID_HEIGHT - 1;
            a.prev_pos = a.pos;
            a.pos = {static_cast<int8_t>(nx), static_cast<int8_t>(ny)};
            a.last_step = {static_cast<int8_t>(a.pos.x - a.prev_pos.x),
                           static_cast<int8_t>(a.pos.y - a.prev_pos.y)};
            // Scripted non-seek move invalidates any cached seek plan —
            // the plan's deltas were relative to a different starting
            // position and its cost accounting was against an older
            // world state. Cheap to clear; cheap to replan on next seek.
            a.plan_len = 0;
            int32_t sr = pfInt(a.type_idx, "step_rate", PF_DEF_STEP_RATE);
            a.step_duration   = (int16_t)sr;
            a.remaining_ticks = (int16_t)sr;
            a.pc = pc + 1;
            return;
        }

        // ---- wander [avoiding current|any] ----
        if (startsWith(inst, "wander")) {
            const bool diag = pfDiagonal(a.type_idx);
            const int deltas4[4][2] = {{0,1},{0,-1},{1,0},{-1,0}};
            const int deltas8[8][2] = {{0,1},{0,-1},{1,0},{-1,0},
                                       {1,1},{1,-1},{-1,1},{-1,-1}};
            const int (*D)[2] = diag ? deltas8 : deltas4;
            int nd = diag ? 8 : 4;
            bool av_cur = std::strstr(inst, "avoiding current") != nullptr;
            bool av_any = std::strstr(inst, "avoiding any") != nullptr;

            Point cands[8]; int nc = 0;
            for (int i = 0; i < nd; ++i) {
                int nx = a.pos.x + D[i][0], ny = a.pos.y + D[i][1];
                if (nx < 0 || nx >= GRID_WIDTH || ny < 0 || ny >= GRID_HEIGHT) continue;
                bool eff = grid_[nx][ny].state || occupiedByNonPainter(nx, ny);
                if (av_cur && grid_[nx][ny].intended && eff) continue;
                if (av_any && eff) continue;
                cands[nc++] = {static_cast<int8_t>(nx), static_cast<int8_t>(ny)};
            }
            if (nc > 0) {
                std::uniform_int_distribution<int> d(0, nc - 1);
                Point np = cands[d(rng_)];
                a.prev_pos = a.pos;
                a.pos = np;
                a.last_step = {static_cast<int8_t>(a.pos.x - a.prev_pos.x),
                               static_cast<int8_t>(a.pos.y - a.prev_pos.y)};
                a.plan_len = 0;  // see comment at `step` — scripted move invalidates plan
                int32_t sr = pfInt(a.type_idx, "step_rate", PF_DEF_STEP_RATE);
                a.step_duration = (int16_t)sr;
                a.remaining_ticks = (int16_t)sr;
            } else {
                a.step_duration = 1;
                a.remaining_ticks = 1;
            }
            a.pc = pc + 1;
            return;
        }

        // ---- seek [nearest] [free] [agent|landmark] <target>
        //           [with state == <name>]
        //           [(on|not on) <k>]
        //           [timeout N] ----
        //   `free`: drop candidates occupied by another live agent, and
        //   claim the winning cell even when <target> is `current`
        //   (classic seek only claims for `missing`/`extra`). Makes
        //   `seek nearest free current` behave right in swarms — without
        //   it every locust seeks the same Manhattan-nearest leaf and
        //   pile-ups saturate A*.
        //   OR gradient form:
        //       seek (highest|lowest) marker <m> [(>|<) N]
        //                                        [on landmark <l>]
        //                                        [timeout N]
        if (startsWith(inst, "seek")) {
            char buf[128];
            std::strncpy(buf, inst, sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = 0;
            const char* tok[16]; int t = tokenize(buf, tok, 16);

#if IR_MAX_MARKERS > 0
            // Gradient-seek branch. Detect by the second token; classic
            // seeks never use highest/lowest. Walks the marker field on
            // the current grid, selects best cell by count (+ Manhattan
            // tie-break), then reuses the same A*+step machinery the
            // classic path uses below.
            if (t >= 4 && (strEq(tok[1], "highest") || strEq(tok[1], "lowest"))
                    && strEq(tok[2], "marker")) {
                const bool want_high = strEq(tok[1], "highest");
                int slot = markerSlot(tok[3]);
                int gi = 4;
                char gop = 0;
                int  gn  = 0;
                if (gi + 1 < t && (strEq(tok[gi], ">") || strEq(tok[gi], "<"))) {
                    gop = tok[gi][0];
                    gn  = parseIntOr(tok[gi + 1], 0);
                    gi += 2;
                }
                const char* glandmark = nullptr;
                bool gland_negate = false;
                // `not on landmark X` inverts the candidate scope to the
                // board cells OUTSIDE the landmark. Detect the optional
                // `not` token first; the rest of the parse is identical.
                if (gi + 3 < t && strEq(tok[gi], "not") && strEq(tok[gi + 1], "on")
                        && strEq(tok[gi + 2], "landmark")) {
                    glandmark = tok[gi + 3];
                    gland_negate = true;
                    gi += 4;
                } else if (gi + 2 < t && strEq(tok[gi], "on")
                        && strEq(tok[gi + 1], "landmark")) {
                    glandmark = tok[gi + 2];
                    gi += 3;
                }
                int gtimeout = -1;
                if (gi + 1 < t && strEq(tok[gi], "timeout"))
                    gtimeout = parseIntOr(tok[gi + 1], -1);

                if (pc != original_pc) a.seek_ticks = 0;
                ++a.seek_ticks;
                if (gtimeout >= 0 && a.seek_ticks > gtimeout) {
                    ++metrics_.failed_seeks;
                    a.seek_ticks = 0;
                    ++pc;
                    continue;
                }

                // Resolve scope: either a landmark's points or the whole grid.
                const critter_ir::Point* lpts = nullptr;
                uint16_t lnpts = 0;
                if (glandmark) {
                    int lm = landmarkIndex(glandmark);
                    if (lm >= 0) {
                        lpts = critter_ir::LANDMARKS[lm].points;
                        lnpts = critter_ir::LANDMARKS[lm].point_count;
                    } else {
                        // Landmark unresolved: no candidates, fail the seek
                        // cleanly so the caller's else/timeout path runs.
                        a.seek_ticks = 0;
                        ++pc;
                        continue;
                    }
                }

                // Visit candidates and track the best. Inline the
                // filter+select instead of materializing a list so we
                // don't need a grid-sized scratch buffer here.
                bool have = false;
                Point best{0, 0};
                int best_cnt = 0, best_d = 0;

                auto consider = [&](int x, int y) {
                    if (slot < 0) return;
                    int c = grid_[x][y].count[slot];
                    // Bare "highest/lowest marker X" excludes zero-count
                    // cells — can't follow a gradient across flat zero.
                    // `< N` INCLUDES zero-count (the soft-cap deposit
                    // primitive). `> N` is strict.
                    if (gop == 0) {
                        if (c == 0) return;
                    } else if (gop == '>') {
                        if (!(c > gn)) return;
                    } else {
                        if (!(c < gn)) return;
                    }
                    int d = manhattan(a.pos, Point{(int8_t)x, (int8_t)y});
                    bool better;
                    if (!have) better = true;
                    else if (want_high) {
                        if (c != best_cnt) better = (c > best_cnt);
                        else               better = (d < best_d);
                    } else {
                        if (c != best_cnt) better = (c < best_cnt);
                        else               better = (d < best_d);
                    }
                    if (better) {
                        best = {(int8_t)x, (int8_t)y};
                        best_cnt = c; best_d = d;
                        have = true;
                    }
                };

                if (lpts && !gland_negate) {
                    for (uint16_t k = 0; k < lnpts; ++k)
                        consider(lpts[k].x, lpts[k].y);
                } else if (lpts && gland_negate) {
                    // Complement scan: visit every grid cell, skip those
                    // inside the landmark. O(W·H) outer × O(L) inner per
                    // skip check; landmarks are typically small so the
                    // inner loop is cheap. If a future landmark grows
                    // large enough that this matters, swap in a packed
                    // bitmap — the candidate-build is still warm cache
                    // either way.
                    for (int x = 0; x < GRID_WIDTH; ++x) {
                        for (int y = 0; y < GRID_HEIGHT; ++y) {
                            bool in_lm = false;
                            for (uint16_t k = 0; k < lnpts; ++k) {
                                if (lpts[k].x == x && lpts[k].y == y) {
                                    in_lm = true; break;
                                }
                            }
                            if (!in_lm) consider(x, y);
                        }
                    }
                } else {
                    for (int x = 0; x < GRID_WIDTH; ++x)
                        for (int y = 0; y < GRID_HEIGHT; ++y)
                            consider(x, y);
                }

                if (have && !(best.x == a.pos.x && best.y == a.pos.y)) {
                    // astar_us_ now covers the plan-reuse path too — the
                    // cost-budget check is cheap but inside the same
                    // envelope, and on replan it's dominated by aStarPlan
                    // the way it always was. Same timing semantics.
                    uint32_t astar_t0 = now_us();
                    (void)stepTowardTarget(a, best);
                    astar_us_ += now_us() - astar_t0;
                    a.pc = pc;   // stay on this seek
                    return;
                }

                // Already at the peak (or no candidates): advance past
                // the seek. Matches the classic-seek arrival semantics.
                a.seek_ticks = 0;
                ++pc;
                continue;
            }
#endif

            int i = 1;
            if (i < t && strEq(tok[i], "nearest")) ++i;
            bool want_free = false;
            if (i < t && strEq(tok[i], "free")) { want_free = true; ++i; }
            const char* target_kind = nullptr;  // "agent"/"landmark"/nullptr
            if (i < t && (strEq(tok[i], "agent") || strEq(tok[i], "landmark"))) {
                target_kind = tok[i]; ++i;
            }
            const char* target_type = (i < t) ? tok[i++] : nullptr;

            // Optional state filter (agent targets only). Four tokens:
            // with state == <value>. Ignored for non-agent targets.
            const char* state_filter = nullptr;
            if (i + 3 < t && strEq(tok[i], "with")
                    && strEq(tok[i + 1], "state") && strEq(tok[i + 2], "==")) {
                state_filter = tok[i + 3];
                i += 4;
            }

            const char* filter_mode = nullptr;
            const char* filter_kind = nullptr;
            if (i + 1 < t && strEq(tok[i], "on")) {
                filter_mode = "on"; filter_kind = tok[i + 1]; i += 2;
            } else if (i + 2 < t && strEq(tok[i], "not") && strEq(tok[i + 1], "on")) {
                filter_mode = "not on"; filter_kind = tok[i + 2]; i += 3;
            }

            int timeout = -1;
            if (i + 1 < t && strEq(tok[i], "timeout"))
                timeout = parseIntOr(tok[i + 1], -1);

            if (pc != original_pc) a.seek_ticks = 0;
            ++a.seek_ticks;

            if (timeout >= 0 && a.seek_ticks > timeout) {
                ++metrics_.failed_seeks;
                a.seek_ticks = 0;
                ++pc;
                continue;
            }

            // Build candidate list (up to grid size).
            Point cands[GRID_WIDTH * GRID_HEIGHT];
            int nc = 0;

            if (target_kind && strEq(target_kind, "agent") && target_type) {
                for (uint16_t k = 0; k < agent_count_ && nc < (int)(sizeof(cands)/sizeof(cands[0])); ++k) {
                    const Agent& o = agents_[k];
                    if (!o.alive || o.id == a.id) continue;
                    if (!strEq(critter_ir::AGENT_TYPES[o.type_idx].name, target_type)) continue;
                    if (state_filter) {
                        const auto& otype = critter_ir::AGENT_TYPES[o.type_idx];
                        const char* os = (o.state_str_id == 0) ? "none"
                            : (o.state_str_id <= otype.state_count ? otype.states[o.state_str_id - 1] : "none");
                        if (!strEq(os, state_filter)) continue;
                    }
                    cands[nc++] = o.pos;
                }
            } else if (target_kind && strEq(target_kind, "landmark") && target_type) {
                int lm = landmarkIndex(target_type);
                if (lm >= 0) {
                    const auto& L = critter_ir::LANDMARKS[lm];
                    for (uint16_t k = 0; k < L.point_count && nc < (int)(sizeof(cands)/sizeof(cands[0])); ++k)
                        cands[nc++] = Point{L.points[k].x, L.points[k].y};
                }
            } else if (target_type &&
                       (strEq(target_type, "missing") ||
                        strEq(target_type, "extra") ||
                        strEq(target_type, "current"))) {
                for (int x = 0; x < GRID_WIDTH; ++x)
                    for (int y = 0; y < GRID_HEIGHT; ++y) {
                        if (grid_[x][y].claimant >= 0) continue;
                        if (tileMatches(x, y, target_type))
                            cands[nc++] = {static_cast<int8_t>(x), static_cast<int8_t>(y)};
                    }
            }

            // `free` occupancy filter: drop any cell where another live
            // agent currently stands. Claims (above, in the tile-state
            // branch) block *pending arrivals*; this blocks *current
            // occupants*. Agent-targeted seeks skip this — the agent
            // IS the target.
            if (want_free && nc > 0 &&
                    !(target_kind && strEq(target_kind, "agent"))) {
                int w = 0;
                for (int k = 0; k < nc; ++k) {
                    bool occupied = false;
                    for (uint16_t ai = 0; ai < agent_count_; ++ai) {
                        const Agent& o = agents_[ai];
                        if (!o.alive || o.id == a.id) continue;
                        if (o.pos.x == cands[k].x && o.pos.y == cands[k].y) {
                            occupied = true; break;
                        }
                    }
                    if (!occupied) cands[w++] = cands[k];
                }
                nc = w;
            }

            if (filter_mode && filter_kind) {
                bool want_on = strEq(filter_mode, "on");
                int w = 0;
                for (int k = 0; k < nc; ++k) {
                    bool hit = tileMatches(cands[k].x, cands[k].y, filter_kind);
                    if (hit == want_on) cands[w++] = cands[k];
                }
                nc = w;
            }

            // Pick nearest by Manhattan from agent pos.
            Point best{0, 0};
            bool have = false;
            int best_d = 0;
            for (int k = 0; k < nc; ++k) {
                int d = manhattan(a.pos, cands[k]);
                if (!have || d < best_d) { best = cands[k]; best_d = d; have = true; }
            }

            if (have && !(best.x == a.pos.x && best.y == a.pos.y)) {
                if (target_type && (strEq(target_type, "missing") ||
                                    strEq(target_type, "extra") ||
                                    (want_free && strEq(target_type, "current"))))
                    grid_[best.x][best.y].claimant = a.id;

                uint32_t astar_t0 = now_us();
                (void)stepTowardTarget(a, best);
                astar_us_ += now_us() - astar_t0;
                a.pc = pc;   // stay on this seek
                return;
            }

            a.seek_ticks = 0;
            ++pc;
            continue;
        }

        // Unrecognized — skip.
        ++pc;
    }

    // Fell off the bottom: pause one tick, reset to top.
    a.remaining_ticks = 1;
    a.step_duration   = 1;
    a.prev_pos        = a.pos;
    a.pc              = 0;
}

// =======================================================================
// Rendering
// =======================================================================
//
// Layered into a scratch framebuffer, then flushed to the sink. Order:
//   1. landmarks (fixed background)
//   2. lit tiles (per-script `draw` / `fill` state)
//   3. agents, lerping `prev_pos → pos` by blend
//
// The lerp uses integer Q8 alpha (weight ∈ [0, 256]) so OG Photon — no
// FPU — doesn't take a software-float hit on every agent every frame. For
// a step of duration D with R remaining ticks, `alpha = (elapsed*256 +
// blend_q8) / D` where `elapsed = D - R`. A stationary agent (prev == pos)
// short-circuits to full-weight paint at pos.
//
// Per HAL_SPEC §6, agents overwrite tiles underneath (not alpha-blend) —
// `agent_rgb * weight` is written verbatim. A trailing cell at blend≈1.0
// will briefly write ~black before the next physics tick snaps
// `prev_pos := pos` and the tile re-emerges. One 20ms frame, below flicker.

void CritterEngine::render(float blend) {
    // Advance the render-frame counter for cycle-color animation. One frame
    // per render() call: at 50Hz a `cycle red 2, green 2, blue 2` script
    // cycles through in 120ms — fast enough to read as color animation but
    // authors can slow it down by padding the frame counts.
    ++render_frame_;
    std::memset(fb_, 0, sizeof(fb_));

    auto put = [&](int x, int y, uint8_t r, uint8_t g, uint8_t b) {
        if (x < 0 || x >= GRID_WIDTH || y < 0 || y >= GRID_HEIGHT) return;
        uint8_t* p = &fb_[(y * GRID_WIDTH + x) * 3];
        p[0] = r; p[1] = g; p[2] = b;
    };

    // --- Landmarks (fixed background) ---
    for (uint16_t i = 0; i < critter_ir::LANDMARK_COUNT; ++i) {
        const auto& L = critter_ir::LANDMARKS[i];
        uint8_t r = 0, g = 0, b = 0;
        if (!colorRGB(L.color, r, g, b)) continue;
        for (uint16_t k = 0; k < L.point_count; ++k) {
            put(L.points[k].x, L.points[k].y, r, g, b);
        }
    }

    // --- Lit tiles ---
    // Tile-paint fade (IR v6): when age_max is non-sentinel, scale RGB
    // by age/age_max in Q8 fixed-point. Marker composite below stacks
    // additively on the dimmed tile — markers on a fading tile still
    // burn through at full intensity, matching the spec (TODO §354-358).
    // 256 lerps/frame on a 16×16 grid is noise vs. existing render cost.
    //
    // The cliff-fade variant (`draw ... hold N`) shares the countdown
    // but skips the lerp: the tile renders at full intensity until the
    // tick where state flips back to 0. Used for heartbeat / "still
    // here" indicators that should disappear sharply rather than dim.
    for (int x = 0; x < GRID_WIDTH; ++x) {
        for (int y = 0; y < GRID_HEIGHT; ++y) {
            const Tile& t = grid_[x][y];
            if (!t.state) continue;
            if (t.age_max == 0xFFFF || t.age_max == 0 || t.hold_mode) {
                put(x, y, t.r, t.g, t.b);
            } else {
                // Fade-pass channel scale. Mirrors NeoPixelSink::scale_ch's
                // contract: a non-zero source channel stays at >=1 through
                // the entire fade window, falling to 0 only when state
                // flips off at age=0. Without the floor-clamp, low-channel
                // night-palette colors (e.g. brick (0, 1, 0)) truncate to
                // 0 for any q8 < 256 — the channel disappears one tick
                // into the fade rather than gracefully dimming. Observed
                // 2026-04-28 on fraggle's brick at night.
                uint16_t q8 = (uint16_t)(((uint32_t)t.age << 8) / t.age_max);
                auto fade_ch = [q8](uint8_t c) -> uint8_t {
                    if (c == 0) return 0;
                    uint16_t s = ((uint16_t)c * q8) >> 8;
                    return s ? (uint8_t)s : 1;
                };
                put(x, y, fade_ch(t.r), fade_ch(t.g), fade_ch(t.b));
            }
        }
    }

#if IR_MAX_MARKERS > 0
    // --- Marker ramp composite (additive over tile/landmark layer) ---
    // Per MARKERS_SPEC §5: each marker contributes `count × rgb_coef` to
    // the tile's per-channel sum, then clamp. Stacks with prior layers
    // and with other markers on the same cell. Agents paint on top of
    // this in the next pass.
    if (critter_ir::MARKER_COUNT > 0) {
        for (uint16_t mi = 0; mi < critter_ir::MARKER_COUNT; ++mi) {
            const auto& M = critter_ir::MARKERS[mi];
            if (M.index >= IR_MAX_MARKERS) continue;

            // Night-mode ramp override (IR v6). NIGHT_MARKERS is
            // indexed sparsely by tile-slot (`.index` mirrors the day
            // marker it replaces), so a linear scan of at most
            // IR_MAX_MARKERS entries is cheap and lets us fall through
            // to the day ramp when no night override is declared for
            // this marker. Decay stays on the day counter regardless —
            // we only swap the per-unit RGB here.
            uint8_t rr = M.r, gg = M.g, bb = M.b;
            if (night_mode_ && critter_ir::NIGHT_MARKER_COUNT > 0) {
                for (uint16_t ni = 0; ni < critter_ir::NIGHT_MARKER_COUNT; ++ni) {
                    const auto& N = critter_ir::NIGHT_MARKERS[ni];
                    if (N.index == M.index) {
                        rr = N.r; gg = N.g; bb = N.b;
                        break;
                    }
                }
            }
            if (rr == 0 && gg == 0 && bb == 0) continue;  // no-op ramp

            for (int x = 0; x < GRID_WIDTH; ++x) {
                for (int y = 0; y < GRID_HEIGHT; ++y) {
                    uint8_t c = grid_[x][y].count[M.index];
                    if (!c) continue;
                    uint8_t* p = &fb_[(y * GRID_WIDTH + x) * 3];
                    int r = (int)p[0] + (int)c * (int)rr;
                    int g = (int)p[1] + (int)c * (int)gg;
                    int b = (int)p[2] + (int)c * (int)bb;
                    p[0] = (uint8_t)(r > 255 ? 255 : r);
                    p[1] = (uint8_t)(g > 255 ? 255 : g);
                    p[2] = (uint8_t)(b > 255 ? 255 : b);
                }
            }
        }
    }
#endif

    // --- Agents (overwrite, with lerp on active steps) ---
    // Clamp blend outside the loop; repeat this many times per frame otherwise.
    if (blend < 0.0f) blend = 0.0f;
    if (blend > 1.0f) blend = 1.0f;
    const int blend_q8 = (int)(blend * 256.0f);  // [0, 256]

    auto paint = [&](int x, int y, uint8_t r, uint8_t g, uint8_t b, int w_q8) {
        if (x < 0 || x >= GRID_WIDTH || y < 0 || y >= GRID_HEIGHT) return;
        uint8_t* p = &fb_[(y * GRID_WIDTH + x) * 3];
        // Blend agent onto whatever the earlier layers painted (tile/landmark
        // or 0 for empty cells). This gives a smooth agent→tile transition
        // at prev_pos during a step — otherwise the tile flashes to black
        // through the lerp and pops back when the agent finishes arriving.
        // For empty cells (underneath = 0) the blend collapses to pure
        // agent*weight, preserving the pipe-gap darkness effect.
        int inv_w_q8 = 256 - w_q8;
        p[0] = (uint8_t)((r * w_q8 + p[0] * inv_w_q8 + 128) >> 8);
        p[1] = (uint8_t)((g * w_q8 + p[1] * inv_w_q8 + 128) >> 8);
        p[2] = (uint8_t)((b * w_q8 + p[2] * inv_w_q8 + 128) >> 8);
    };

    for (uint16_t i = 0; i < agent_count_; ++i) {
        const Agent& a = agents_[i];
        if (!a.alive) continue;

        uint8_t ar, ag, ab;
        resolveColor(a.color_idx, render_frame_ + a.color_phase, ar, ag, ab);

        const bool moving = (a.prev_pos.x != a.pos.x) || (a.prev_pos.y != a.pos.y);
        if (!moving) {
            paint(a.pos.x, a.pos.y, ar, ag, ab, 256);
            continue;
        }

        int dur = a.step_duration > 0 ? a.step_duration : 1;
        int elapsed = dur - a.remaining_ticks;  // 0..dur
        int alpha_q8 = (elapsed * 256 + blend_q8) / dur;
        if (alpha_q8 < 0)   alpha_q8 = 0;
        if (alpha_q8 > 256) alpha_q8 = 256;

        // Sine dip: prev fades out over [0, 0.5] on a cos curve, pos emerges
        // from 0 over [0.5, 1]. Agent goes dark at midpoint (intentional —
        // models the physical gap between light pipes).
        int prev_w_q8, pos_w_q8;
        if (alpha_q8 <= 128) {
            prev_w_q8 = cos_q8_lut[alpha_q8];
            pos_w_q8  = 0;
        } else {
            prev_w_q8 = 0;
            pos_w_q8  = cos_q8_lut[256 - alpha_q8];
        }

        paint(a.prev_pos.x, a.prev_pos.y, ar, ag, ab, prev_w_q8);
        paint(a.pos.x,      a.pos.y,      ar, ag, ab, pos_w_q8);
    }

    // --- Flush framebuffer to sink ---
    sink_.clear();
    for (int y = 0; y < GRID_HEIGHT; ++y) {
        for (int x = 0; x < GRID_WIDTH; ++x) {
            uint8_t* p = &fb_[(y * GRID_WIDTH + x) * 3];
            if (p[0] || p[1] || p[2]) sink_.set(x, y, p[0], p[1], p[2]);
        }
    }
    sink_.show();
}

// =======================================================================
// JSONL dump (schema matches engine.py dump_state_jsonl)
// =======================================================================

std::string CritterEngine::dumpStateJsonl() const {
    std::string out;
    out.reserve(4096);
    char buf[128];

    std::snprintf(buf, sizeof(buf), "{\"tick\":%u,\"w\":%d,\"h\":%d,\"state\":[",
                  tick_count_, GRID_WIDTH, GRID_HEIGHT);
    out += buf;
    for (int x = 0; x < GRID_WIDTH; ++x)
        for (int y = 0; y < GRID_HEIGHT; ++y) {
            if (x || y) out += ',';
            out += grid_[x][y].state ? '1' : '0';
        }
    out += "],\"intended\":[";
    for (int x = 0; x < GRID_WIDTH; ++x)
        for (int y = 0; y < GRID_HEIGHT; ++y) {
            if (x || y) out += ',';
            out += grid_[x][y].intended ? '1' : '0';
        }
    out += "],\"tile_colors\":[";
    bool first = true;
    for (int x = 0; x < GRID_WIDTH; ++x)
        for (int y = 0; y < GRID_HEIGHT; ++y) {
            const Tile& t = grid_[x][y];
            if (!t.state) continue;
            if (t.r == 0 && t.g == 0 && t.b == 0) continue;
            if (!first) out += ',';
            first = false;
            std::snprintf(buf, sizeof(buf), "[%d,%d,[%u,%u,%u]]", x, y, t.r, t.g, t.b);
            out += buf;
        }
    out += "],\"agents\":[";
    first = true;
    for (uint16_t i = 0; i < agent_count_; ++i) {
        const Agent& a = agents_[i];
        if (!a.alive) continue;
        if (!first) out += ',';
        first = false;
        uint8_t ar, ag, ab;
        resolveColor(a.color_idx, tick_count_ + a.color_phase, ar, ag, ab);
        std::snprintf(buf, sizeof(buf),
                      "{\"id\":%d,\"name\":\"%s\",\"pos\":[%d,%d],"
                      "\"color\":[%u,%u,%u],\"glitched\":%s}",
                      a.id, critter_ir::AGENT_TYPES[a.type_idx].name,
                      a.pos.x, a.pos.y, ar, ag, ab,
                      a.glitched ? "true" : "false");
        out += buf;
    }
    out += "]";

#if IR_MAX_MARKERS > 0
    // Marker header + sparse per-tile counts. Gated on MARKER_COUNT so
    // blobs without markers leave no key at all — pre-v5 diff consumers
    // stay byte-identical to the pre-v5 format.
    if (critter_ir::MARKER_COUNT > 0) {
        out += ",\"markers\":{";
        for (uint16_t i = 0; i < critter_ir::MARKER_COUNT; ++i) {
            if (i) out += ',';
            std::snprintf(buf, sizeof(buf), "\"%s\":%u",
                          critter_ir::MARKERS[i].name,
                          (unsigned)critter_ir::MARKERS[i].index);
            out += buf;
        }
        out += "},\"tile_markers\":[";
        bool mfirst = true;
        for (int x = 0; x < GRID_WIDTH; ++x)
            for (int y = 0; y < GRID_HEIGHT; ++y) {
                const Tile& t = grid_[x][y];
                for (uint16_t i = 0; i < critter_ir::MARKER_COUNT; ++i) {
                    uint16_t slot = critter_ir::MARKERS[i].index;
                    if (slot >= IR_MAX_MARKERS) continue;
                    uint8_t c = t.count[slot];
                    if (!c) continue;
                    if (!mfirst) out += ',';
                    mfirst = false;
                    std::snprintf(buf, sizeof(buf), "[%d,%d,%u,%u]",
                                  x, y, (unsigned)slot, (unsigned)c);
                    out += buf;
                }
            }
        out += "]";
    }
#endif

    char bigbuf[256];
    std::snprintf(bigbuf, sizeof(bigbuf),
                  ",\"metrics\":{\"convergences\":%u,\"glitches\":%u,"
                  "\"step_contests\":%u,\"failed_seeks\":%u,"
                  "\"total_intended\":%u,\"total_lit_intended\":%u}}\n",
                  metrics_.convergences, metrics_.glitches,
                  metrics_.step_contests, metrics_.failed_seeks,
                  metrics_.total_intended, metrics_.total_lit_intended);
    out += bigbuf;
    return out;
}

}  // namespace critterchron

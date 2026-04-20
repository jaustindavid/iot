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

CritterEngine::CritterEngine(LedSink& sink, TimeSource& clock)
    : sink_(sink), clock_(clock) {
    for (int x = 0; x < GRID_WIDTH; ++x)
        for (int y = 0; y < GRID_HEIGHT; ++y)
            grid_[x][y] = Tile{0, 0, -1, 0, 0, 0};
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

void CritterEngine::resolveColor(int idx, uint32_t frame,
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

int32_t CritterEngine::pfInt(uint16_t type_idx, const char* key, int32_t fallback) const {
    const char* name = critter_ir::AGENT_TYPES[type_idx].name;
    int i = pfConfigIndex(name);
    if (i >= 0) {
        const auto& c = critter_ir::PF_CONFIGS[i];
        if (strEq(key, "max_nodes")        && c.has_max_nodes)        return c.max_nodes;
        if (strEq(key, "penalty_lit")      && c.has_penalty_lit)      return c.penalty_lit;
        if (strEq(key, "penalty_occupied") && c.has_penalty_occupied) return c.penalty_occupied;
        if (strEq(key, "step_rate")        && c.has_step_rate)        return c.step_rate;
    }
    int all = pfConfigIndex("all");
    if (all >= 0) {
        const auto& c = critter_ir::PF_CONFIGS[all];
        if (strEq(key, "max_nodes")        && c.has_max_nodes)        return c.max_nodes;
        if (strEq(key, "penalty_lit")      && c.has_penalty_lit)      return c.penalty_lit;
        if (strEq(key, "penalty_occupied") && c.has_penalty_occupied) return c.penalty_occupied;
        if (strEq(key, "step_rate")        && c.has_step_rate)        return c.step_rate;
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

// =======================================================================
// begin(): version gate + initial agents + painter mask
// =======================================================================

bool CritterEngine::begin() {
    if (critter_ir::IR_VERSION > SUPPORTED_IR_VERSION) return false;

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

    // on landmark <name>
    if (n == 3 && strEq(tok[0], "on") && strEq(tok[1], "landmark")) {
        int lm = landmarkIndex(tok[2]);
        if (lm < 0) return false;
        const auto& L = critter_ir::LANDMARKS[lm];
        for (uint16_t i = 0; i < L.point_count; ++i)
            if (L.points[i].x == a.pos.x && L.points[i].y == a.pos.y) return true;
        return false;
    }

    // on agent <name>
    if (n == 3 && strEq(tok[0], "on") && strEq(tok[1], "agent")) {
        for (uint16_t i = 0; i < agent_count_; ++i) {
            const Agent& o = agents_[i];
            if (!o.alive || o.id == a.id) continue;
            if (!strEq(critter_ir::AGENT_TYPES[o.type_idx].name, tok[2])) continue;
            if (o.pos.x == a.pos.x && o.pos.y == a.pos.y) return true;
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

    return false;
}

// =======================================================================
// A*
// =======================================================================

bool CritterEngine::aStarFirstStep(Point start, Point target, uint16_t type_idx,
                                   Point& out_step) {
    out_step = {0, 0};
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

    int nodes = 0;
    int best_node = idx(start.x, start.y);
    float best_h = hfn(start);

    auto firstStepFrom = [&](int end) -> Point {
        int cur = end;
        int prev = idx(start.x, start.y);
        while (cur != prev && from[cur] != -1 && from[cur] != prev)
            cur = from[cur];
        if (cur == prev) return {0, 0};
        return {static_cast<int8_t>(cur % W - start.x),
                static_cast<int8_t>(cur / W - start.y)};
    };

    while (!pf_heap_.empty()) {
        auto [pri, ci] = q_pop();
        (void)pri;
        int cx = ci % W, cy = ci / W;
        ++nodes;

        if (cx == target.x && cy == target.y) {
            Point s = firstStepFrom(ci);
            if (s.x == 0 && s.y == 0) return false;
            out_step = s;
            return true;
        }

        float h = hfn({static_cast<int8_t>(cx), static_cast<int8_t>(cy)});
        if (h < best_h) { best_h = h; best_node = ci; }

        if (nodes >= max_nodes) {
            ++metrics_.failed_seeks;
            Point s = firstStepFrom(best_node);
            if (s.x == 0 && s.y == 0) return false;
            out_step = s;
            return true;
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
            int ni = idx(nx, ny);
            if (nc < cost[ni]) {
                cost[ni] = nc;
                from[ni] = static_cast<int16_t>(ci);
                q_push({nc + hfn({static_cast<int8_t>(nx), static_cast<int8_t>(ny)}), ni});
            }
        }
    }

    Point s = firstStepFrom(best_node);
    if (s.x == 0 && s.y == 0) return false;
    out_step = s;
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

        // ---- draw [color] ----
        if (startsWith(inst, "draw")) {
            grid_[a.pos.x][a.pos.y].state = 1;
            char buf[128];
            std::strncpy(buf, inst, sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = 0;
            const char* tok[8]; int t = tokenize(buf, tok, 8);
            if (t > 1) {
                // Snapshot the agent's phase-adjusted color so painted tiles
                // match what the agent looked like at paint time. Tile stays
                // static after — no live animation on tiles.
                int ci = colorIndex(tok[1]);
                uint8_t r, g, b;
                if (ci >= 0) {
                    resolveColor(ci, render_frame_ + a.color_phase, r, g, b);
                } else {
                    r = g = b = 255;
                }
                grid_[a.pos.x][a.pos.y].r = r;
                grid_[a.pos.x][a.pos.y].g = g;
                grid_[a.pos.x][a.pos.y].b = b;
            }
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

        // ---- seek [nearest] [agent|landmark] <target> [(on|not on) <k>] [timeout N] ----
        if (startsWith(inst, "seek")) {
            char buf[128];
            std::strncpy(buf, inst, sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = 0;
            const char* tok[16]; int t = tokenize(buf, tok, 16);

            int i = 1;
            if (i < t && strEq(tok[i], "nearest")) ++i;
            const char* target_kind = nullptr;  // "agent"/"landmark"/nullptr
            if (i < t && (strEq(tok[i], "agent") || strEq(tok[i], "landmark"))) {
                target_kind = tok[i]; ++i;
            }
            const char* target_type = (i < t) ? tok[i++] : nullptr;

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
                if (target_type && (strEq(target_type, "missing") || strEq(target_type, "extra")))
                    grid_[best.x][best.y].claimant = a.id;

                Point step;
                uint32_t astar_t0 = now_us();
                bool moved = aStarFirstStep(a.pos, best, a.type_idx, step);
                astar_us_ += now_us() - astar_t0;
                if (moved) {
                    a.prev_pos = a.pos;
                    a.pos = {static_cast<int8_t>(a.pos.x + step.x),
                             static_cast<int8_t>(a.pos.y + step.y)};
                    int32_t sr = pfInt(a.type_idx, "step_rate", PF_DEF_STEP_RATE);
                    a.step_duration = (int16_t)sr;
                    a.remaining_ticks = (int16_t)sr;
                } else {
                    a.step_duration = 1;
                    a.remaining_ticks = 1;
                }
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
    for (int x = 0; x < GRID_WIDTH; ++x) {
        for (int y = 0; y < GRID_HEIGHT; ++y) {
            const Tile& t = grid_[x][y];
            if (t.state) put(x, y, t.r, t.g, t.b);
        }
    }

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
    char bigbuf[256];
    std::snprintf(bigbuf, sizeof(bigbuf),
                  "],\"metrics\":{\"convergences\":%u,\"glitches\":%u,"
                  "\"step_contests\":%u,\"failed_seeks\":%u,"
                  "\"total_intended\":%u,\"total_lit_intended\":%u}}\n",
                  metrics_.convergences, metrics_.glitches,
                  metrics_.step_contests, metrics_.failed_seeks,
                  metrics_.total_intended, metrics_.total_lit_intended);
    out += bigbuf;
    return out;
}

}  // namespace critterchron

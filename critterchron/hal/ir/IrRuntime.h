// hal/ir/IrRuntime.h — runtime IR tables + blob parser.
//
// Step 5 of the OTA IR rollout: the compiled-in default IR is now the
// same text wire format as an OTA blob (see hal/ir/ir_text.py /
// OTA_IR.md). Parsing happens at boot from DEFAULT_IR_BLOB (or any
// mutable char buffer the caller supplies), filling the critter_ir::*
// tables the engine has always indexed into.
//
// Struct shapes match the old compile-time layout in the generated
// critter_ir.h, so CritterEngine.cpp's access patterns
// (`critter_ir::COLORS[i].name`, `PF_CONFIGS[i].has_diagonal`, etc.)
// keep working unchanged — only the storage moves from constexpr tables
// to parser-populated pools.
//
// Memory model:
//   - Caller passes load() a writable char buffer. Parser flips runs of
//     whitespace to NUL in place so every struct's `const char*` points
//     into that buffer. Buffer must outlive the engine.
//   - Pools (COLORS, LANDMARKS, …) are static globals sized by the
//     MAX_* caps below. Overflow is a load-time error, not a silent
//     truncation.

#pragma once
#include <cstdint>
#include <cstddef>

// --- Capacity caps. Override via -D at build time if a script outgrows them.
#ifndef IR_MAX_COLORS
#define IR_MAX_COLORS 32
#endif
#ifndef IR_MAX_CYCLE_ENTRIES
#define IR_MAX_CYCLE_ENTRIES 64
#endif
#ifndef IR_MAX_LANDMARKS
#define IR_MAX_LANDMARKS 16
#endif
#ifndef IR_MAX_LANDMARK_POINTS
#define IR_MAX_LANDMARK_POINTS 256
#endif
#ifndef IR_MAX_AGENT_TYPES
#define IR_MAX_AGENT_TYPES 16
#endif
#ifndef IR_MAX_AGENT_STATES
#define IR_MAX_AGENT_STATES 64
#endif
#ifndef IR_MAX_INITIAL_AGENTS
#define IR_MAX_INITIAL_AGENTS 64
#endif
#ifndef IR_MAX_SPAWN_RULES
#define IR_MAX_SPAWN_RULES 16
#endif
#ifndef IR_MAX_PF_CONFIGS
#define IR_MAX_PF_CONFIGS 16
#endif
#ifndef IR_MAX_BEHAVIORS
#define IR_MAX_BEHAVIORS 16
#endif
#ifndef IR_MAX_INSNS
#define IR_MAX_INSNS 384
#endif
// Night palette caps — sparse override keyed by day color index. Cap at
// IR_MAX_COLORS so every day color can have a per-color night override;
// cycle entries live in their own small pool. Blobs without a night
// palette don't touch these tables.
#ifndef IR_MAX_NIGHT_COLORS
#define IR_MAX_NIGHT_COLORS IR_MAX_COLORS
#endif
#ifndef IR_MAX_NIGHT_CYCLE_ENTRIES
#define IR_MAX_NIGHT_CYCLE_ENTRIES 32
#endif
// Backing buffer used by loadDefault(). Sized so thyme-plus-source (~4.4KB)
// fits with headroom. OTA loads bring their own buffer.
#ifndef IR_DEFAULT_BUFFER_BYTES
#define IR_DEFAULT_BUFFER_BYTES 6144
#endif

namespace critter_ir {

// ---------- runtime struct shapes (match the old constexpr layout) ----------

struct CycleEntry { uint8_t color_idx; uint8_t frames; };

// kind: 0 = static RGB; 1 = cycle (walks CYCLE_ENTRIES[cycle_start ..
// cycle_start+cycle_count]). For static colors the r/g/b fields hold the
// literal RGB; for cycles they hold the first entry's resolved RGB as a
// fallback.
struct Color {
    const char* name;
    uint8_t kind;
    uint8_t r, g, b;
    uint8_t cycle_start, cycle_count;
};

// Night palette entry. Kind semantics mirror the engine's resolver:
//   0 = static RGB (r/g/b hold the literal color).
//   1 = cycle (walks NIGHT_CYCLE_ENTRIES[cycle_start .. +cycle_count]).
//       Cycle sub-entries reference DAY colors by index; resolution
//       recurses into COLORS[] with skip_night, so night cycles are
//       composed of unambiguous day-palette primitives.
//   2 = ref (points at a day color by index — used for `pulse: warm`
//       style night overrides where the night appearance is just another
//       existing day color).
// Fallback r/g/b fields hold the first cycle sub-entry's RGB (parallel
// to Color.r/g/b's fallback role), so a depth-limited resolve still
// produces a sensible pixel.
struct NightEntry {
    uint8_t kind;
    uint8_t r, g, b;
    uint8_t target_day_idx;     // kind==2: day COLORS index; else unused
    uint8_t cycle_start;        // kind==1: offset into NIGHT_CYCLE_ENTRIES
    uint8_t cycle_count;
};

struct Point { int8_t x, y; };

struct Landmark {
    const char* name;
    const char* color;
    const Point* points;
    uint16_t point_count;
};

struct AgentType {
    const char* name;
    uint16_t limit;
    const char* const* states;
    uint16_t state_count;
};

struct InitialAgent {
    const char* name;
    int8_t x, y;
    const char* state;
};

struct SpawnRule {
    const char* agent_type;
    const char* landmark;
    uint32_t interval;
    const char* condition;
};

struct PFConfig {
    const char* agent_name;
    int32_t max_nodes;
    int32_t penalty_lit;
    int32_t penalty_occupied;
    uint8_t diagonal;          // 0/1
    float   diagonal_cost;
    int32_t step_rate;
    uint8_t has_max_nodes, has_penalty_lit, has_penalty_occupied,
            has_diagonal, has_diagonal_cost, has_step_rate;
};

struct Insn { uint8_t indent; const char* text; };

struct Behavior {
    const char* agent_name;
    const Insn* insns;
    uint16_t insn_count;
};

// ---------- tables (populated by load()) ----------

extern Color         COLORS[IR_MAX_COLORS];
extern uint16_t      COLOR_COUNT;

extern CycleEntry    CYCLE_ENTRIES[IR_MAX_CYCLE_ENTRIES];
extern uint16_t      CYCLE_ENTRY_COUNT;

// Night palette — optional, populated only when the blob carries
// NIGHT_COLORS / NIGHT_DEFAULT sections. Engine indexes via
// NIGHT_FOR[day_color_idx]: a value < NIGHT_COLOR_COUNT points into
// NIGHT_COLORS[], 0xFF means no per-color override (fall through to
// NIGHT_DEFAULT if HAS_NIGHT_DEFAULT, else day palette).
extern NightEntry    NIGHT_COLORS[IR_MAX_NIGHT_COLORS];
extern uint16_t      NIGHT_COLOR_COUNT;
extern uint8_t       NIGHT_FOR[IR_MAX_COLORS];

extern CycleEntry    NIGHT_CYCLE_ENTRIES[IR_MAX_NIGHT_CYCLE_ENTRIES];
extern uint16_t      NIGHT_CYCLE_ENTRY_COUNT;

extern NightEntry    NIGHT_DEFAULT;
extern uint8_t       HAS_NIGHT_DEFAULT;

extern Landmark      LANDMARKS[IR_MAX_LANDMARKS];
extern uint16_t      LANDMARK_COUNT;

extern AgentType     AGENT_TYPES[IR_MAX_AGENT_TYPES];
extern uint16_t      AGENT_TYPE_COUNT;

extern InitialAgent  INITIAL_AGENTS[IR_MAX_INITIAL_AGENTS];
extern uint16_t      INITIAL_AGENT_COUNT;

extern SpawnRule     SPAWN_RULES[IR_MAX_SPAWN_RULES];
extern uint16_t      SPAWN_RULE_COUNT;

extern PFConfig      PF_CONFIGS[IR_MAX_PF_CONFIGS];
extern uint16_t      PF_CONFIG_COUNT;

extern Behavior      BEHAVIORS[IR_MAX_BEHAVIORS];
extern uint16_t      BEHAVIOR_COUNT;

// Top-level pathfinding (engine consults this when no per-agent override
// matches). Same semantics as the old constexpr slots.
extern uint8_t       PF_TOP_DIAGONAL;
extern float         PF_TOP_DIAGONAL_COST;
extern uint8_t       PF_TOP_HAS_DIAGONAL_COST;

// Parsed from the blob's `ir_version` metadata. Engine's begin() compares
// to SUPPORTED_IR_VERSION and bails on mismatch.
extern uint8_t       IR_VERSION;

// Runtime tick rate (ms) from the blob's TICK section. Compile-time
// TICK_RATE_MS still lives in critter_ir.h for particle.cpp's constexpr
// main-loop budgets; this is the value the engine actually schedules on.
extern uint32_t      RUNTIME_TICK_MS;

// ---------- loader ----------

// Parse a mutable char buffer in place. `buf` must be null-terminated or
// exactly `len` bytes; parser flips whitespace to NUL and stores interior
// pointers into the buffer, which must outlive the engine. Verifies the
// Fletcher-16 checksum on the END line and rejects blobs whose
// `ir_version` > SUPPORTED_IR_VERSION.
bool load(char* buf, size_t len);

// Convenience: copy DEFAULT_IR_BLOB into a static scratch buffer, then
// call load(). Safe to call more than once (repeats the copy).
bool loadDefault();

// Optional: short human-readable reason for the last failed load(). One
// line, no newline. Returns a zero-length string on success.
const char* lastLoadError();

}  // namespace critter_ir

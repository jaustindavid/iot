// hal/ir/IrRuntime.cpp — blob parser + runtime IR tables.
//
// See IrRuntime.h for the design. Everything here is cross-platform C++
// (no Particle APIs) so hosting builds get the same parser.

#include "IrRuntime.h"
#include "critter_ir.h"   // GRID_WIDTH / GRID_HEIGHT for max_x/max_y resolution
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <cstdio>

namespace critter_ir {

// ---------- storage ----------

Color         COLORS[IR_MAX_COLORS];
uint16_t      COLOR_COUNT = 0;

CycleEntry    CYCLE_ENTRIES[IR_MAX_CYCLE_ENTRIES];
uint16_t      CYCLE_ENTRY_COUNT = 0;

NightEntry    NIGHT_COLORS[IR_MAX_NIGHT_COLORS];
uint16_t      NIGHT_COLOR_COUNT = 0;
uint8_t       NIGHT_FOR[IR_MAX_COLORS];     // filled with 0xFF on reset

CycleEntry    NIGHT_CYCLE_ENTRIES[IR_MAX_NIGHT_CYCLE_ENTRIES];
uint16_t      NIGHT_CYCLE_ENTRY_COUNT = 0;

NightEntry    NIGHT_DEFAULT = {};
uint8_t       HAS_NIGHT_DEFAULT = 0;

Landmark      LANDMARKS[IR_MAX_LANDMARKS];
uint16_t      LANDMARK_COUNT = 0;

AgentType     AGENT_TYPES[IR_MAX_AGENT_TYPES];
uint16_t      AGENT_TYPE_COUNT = 0;

InitialAgent  INITIAL_AGENTS[IR_MAX_INITIAL_AGENTS];
uint16_t      INITIAL_AGENT_COUNT = 0;

SpawnRule     SPAWN_RULES[IR_MAX_SPAWN_RULES];
uint16_t      SPAWN_RULE_COUNT = 0;

PFConfig      PF_CONFIGS[IR_MAX_PF_CONFIGS];
uint16_t      PF_CONFIG_COUNT = 0;

Behavior      BEHAVIORS[IR_MAX_BEHAVIORS];
uint16_t      BEHAVIOR_COUNT = 0;

Marker        MARKERS[IR_MAX_MARKERS];
uint16_t      MARKER_COUNT = 0;

Marker        NIGHT_MARKERS[IR_MAX_MARKERS];
uint16_t      NIGHT_MARKER_COUNT = 0;

uint8_t       PF_TOP_DIAGONAL          = 0;
float         PF_TOP_DIAGONAL_COST     = 0.0f;
uint8_t       PF_TOP_HAS_DIAGONAL_COST = 0;

uint8_t       IR_VERSION      = 0;
uint32_t      RUNTIME_TICK_MS = 500;

char          SCRIPT_NAME[IR_SCRIPT_NAME_MAX] = {0};
char          SCRIPT_SHA[65]                  = {0};

// ---------- sliced pools (owned by tables above) ----------

static Point        LANDMARK_POINT_POOL[IR_MAX_LANDMARK_POINTS];
static uint16_t     lm_pt_used = 0;

static const char*  AGENT_STATE_POOL[IR_MAX_AGENT_STATES];
static uint16_t     ag_state_used = 0;

static Insn         INSN_POOL[IR_MAX_INSNS];
static uint16_t     insn_used = 0;

// Scratch buffer used by loadDefault() so the caller doesn't have to
// supply one. For OTA loads the caller owns the buffer.
static char         DEFAULT_BLOB_BUF[IR_DEFAULT_BUFFER_BYTES];

// ---------- error reporting ----------

static char last_error[128] = "";

static void set_err(const char* msg) {
    size_t i = 0;
    while (msg[i] && i + 1 < sizeof(last_error)) { last_error[i] = msg[i]; ++i; }
    last_error[i] = '\0';
}

static void set_errf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(last_error, sizeof(last_error), fmt, ap);
    va_end(ap);
}

const char* lastLoadError() { return last_error; }

// ---------- helpers ----------

static uint16_t fletcher16(const uint8_t* data, size_t n) {
    uint32_t s1 = 0, s2 = 0;
    for (size_t i = 0; i < n; ++i) {
        s1 = (s1 + data[i]) % 255;
        s2 = (s2 + s1) % 255;
    }
    return (uint16_t)((s2 << 8) | s1);
}

// In-place line cursor. Each call returns a null-terminated pointer to
// the next line (may be an empty string). Returns nullptr at end.
struct LineCursor {
    char* p;
    char* end;
    char* next() {
        if (p >= end) return nullptr;
        char* start = p;
        while (p < end && *p != '\n') ++p;
        if (p < end) { *p = '\0'; ++p; }
        return start;
    }
    // Skip blank lines, then return next non-empty line (or nullptr).
    char* next_nonempty() {
        char* ln;
        while ((ln = next()) != nullptr) {
            if (ln[0] != '\0') return ln;
        }
        return nullptr;
    }
};

// Split a line on runs of ' ' / '\t', null-terminating each token. Fills
// `out[]` up to `out_cap` and returns the token count actually consumed
// (never more than out_cap).
static int tokenize(char* line, char** out, int out_cap) {
    int n = 0;
    char* p = line;
    while (*p) {
        while (*p == ' ' || *p == '\t') ++p;
        if (!*p) break;
        if (n < out_cap) out[n] = p;
        ++n;
        while (*p && *p != ' ' && *p != '\t') ++p;
        if (*p) { *p = '\0'; ++p; }
    }
    return n;
}

// Skip `n` whitespace-separated tokens in `line` (null-terminating each),
// returning the pointer to the rest of the line (whitespace-trimmed on
// the leading edge). Use this when the trailing content is free-form
// text: SPAWNS condition, BEHAVIORS instruction text.
static char* skip_tokens(char* line, int n, char** toks_out) {
    char* p = line;
    for (int i = 0; i < n; ++i) {
        while (*p == ' ' || *p == '\t') ++p;
        if (!*p) { toks_out[i] = p; continue; }
        toks_out[i] = p;
        while (*p && *p != ' ' && *p != '\t') ++p;
        if (*p) { *p = '\0'; ++p; }
    }
    while (*p == ' ' || *p == '\t') ++p;
    return p;
}

// Resolve a coordinate token against this device's grid. Accepts:
//   "N"            → integer literal N
//   "max_x"        → GRID_WIDTH - 1
//   "max_y"        → GRID_HEIGHT - 1
//   "max_x-N"      → GRID_WIDTH - 1 - N
//   "max_y-N"      → GRID_HEIGHT - 1 - N
// Geometry lives on the device — the wire IR carries symbolic tokens so
// one blob works across 16x16 / 32x8 / etc. Baking max_x at publish time
// would silently encode the publisher's assumed geometry into the blob.
static int8_t parseCoord(const char* s) {
    if (!s || !*s) return 0;
    int axis_max = 0;
    const char* rest = nullptr;
    if (strncmp(s, "max_x", 5) == 0) {
        axis_max = GRID_WIDTH - 1;
        rest = s + 5;
    } else if (strncmp(s, "max_y", 5) == 0) {
        axis_max = GRID_HEIGHT - 1;
        rest = s + 5;
    } else {
        return (int8_t)atoi(s);
    }
    if (*rest == '\0') return (int8_t)axis_max;
    // Only subtraction is supported (`max_x-1`, `max_y-2`). A plus or
    // malformed trailer falls through to atoi of the trailer, which
    // would be wrong — reject by returning axis_max.
    if (*rest != '-') return (int8_t)axis_max;
    int off = atoi(rest + 1);
    return (int8_t)(axis_max - off);
}

// Look up a color by name in the currently-populated COLORS table.
// Returns -1 if not found. Used to resolve cycle-entry color refs.
static int colorIndexByName(const char* name) {
    for (uint16_t i = 0; i < COLOR_COUNT; ++i) {
        if (strcmp(COLORS[i].name, name) == 0) return (int)i;
    }
    return -1;
}

// ---------- reset ----------

static void reset_tables() {
    COLOR_COUNT = CYCLE_ENTRY_COUNT = 0;
    NIGHT_COLOR_COUNT = NIGHT_CYCLE_ENTRY_COUNT = 0;
    HAS_NIGHT_DEFAULT = 0;
    for (uint16_t i = 0; i < IR_MAX_COLORS; ++i) NIGHT_FOR[i] = 0xFF;
    LANDMARK_COUNT = AGENT_TYPE_COUNT = 0;
    INITIAL_AGENT_COUNT = SPAWN_RULE_COUNT = 0;
    PF_CONFIG_COUNT = BEHAVIOR_COUNT = 0;
    MARKER_COUNT = 0;
    NIGHT_MARKER_COUNT = 0;
    PF_TOP_DIAGONAL = 0;
    PF_TOP_DIAGONAL_COST = 0.0f;
    PF_TOP_HAS_DIAGONAL_COST = 0;
    IR_VERSION = 0;
    RUNTIME_TICK_MS = 500;
    SCRIPT_NAME[0] = '\0';
    SCRIPT_SHA[0] = '\0';
    lm_pt_used = 0;
    ag_state_used = 0;
    insn_used = 0;
}

// Fill `out` with a parsed NightEntry from `toks` (starting at the kind
// token). `with_name` controls whether a day-color-name slot follows
// kind — true for NIGHT_COLORS entries, false for NIGHT_DEFAULT.
// Returns the day-color index the entry overrides (0xFFFF if with_name
// is false / the entry is default-scoped), or 0xFFFE on error.
static uint16_t parseNightEntry(char** toks, int nt, bool with_name,
                                NightEntry& out) {
    if (nt < 1) { set_err("night entry empty"); return 0xFFFE; }
    const char* kind = toks[0];
    int idx = 1;
    uint16_t day_idx = 0xFFFF;
    if (with_name) {
        if (idx >= nt) {
            set_err("night entry missing day-color name");
            return 0xFFFE;
        }
        int day = colorIndexByName(toks[idx++]);
        if (day < 0) {
            set_errf("night override refers to unknown day color '%s'",
                     toks[idx - 1]);
            return 0xFFFE;
        }
        day_idx = (uint16_t)day;
    }
    memset(&out, 0, sizeof(out));
    if (strcmp(kind, "static") == 0) {
        if (nt - idx < 3) { set_err("night static malformed"); return 0xFFFE; }
        out.kind = 0;
        out.r = (uint8_t)atoi(toks[idx]);
        out.g = (uint8_t)atoi(toks[idx + 1]);
        out.b = (uint8_t)atoi(toks[idx + 2]);
    } else if (strcmp(kind, "ref") == 0) {
        if (nt - idx < 1) { set_err("night ref malformed"); return 0xFFFE; }
        int tgt = colorIndexByName(toks[idx]);
        if (tgt < 0) {
            set_errf("night ref target '%s' not in day palette", toks[idx]);
            return 0xFFFE;
        }
        out.kind = 2;
        out.target_day_idx = (uint8_t)tgt;
        out.r = COLORS[tgt].r;
        out.g = COLORS[tgt].g;
        out.b = COLORS[tgt].b;
    } else if (strcmp(kind, "cycle") == 0) {
        if (nt - idx < 1) { set_err("night cycle malformed"); return 0xFFFE; }
        int m = atoi(toks[idx++]);
        if (nt - idx < 2 * m) {
            set_err("night cycle entries truncated"); return 0xFFFE;
        }
        out.kind = 1;
        out.cycle_start = (uint8_t)NIGHT_CYCLE_ENTRY_COUNT;
        out.cycle_count = (uint8_t)m;
        uint8_t fb_r = 255, fb_g = 255, fb_b = 255;
        for (int j = 0; j < m; ++j) {
            if (NIGHT_CYCLE_ENTRY_COUNT >= IR_MAX_NIGHT_CYCLE_ENTRIES) {
                set_err("IR_MAX_NIGHT_CYCLE_ENTRIES exceeded"); return 0xFFFE;
            }
            int sub_idx = colorIndexByName(toks[idx]);
            if (sub_idx < 0) {
                set_errf("night cycle ref to unknown day color '%s'", toks[idx]);
                return 0xFFFE;
            }
            int frames = atoi(toks[idx + 1]);
            NIGHT_CYCLE_ENTRIES[NIGHT_CYCLE_ENTRY_COUNT].color_idx = (uint8_t)sub_idx;
            NIGHT_CYCLE_ENTRIES[NIGHT_CYCLE_ENTRY_COUNT].frames    = (uint8_t)frames;
            NIGHT_CYCLE_ENTRY_COUNT++;
            if (j == 0) {
                fb_r = COLORS[sub_idx].r;
                fb_g = COLORS[sub_idx].g;
                fb_b = COLORS[sub_idx].b;
            }
            idx += 2;
        }
        out.r = fb_r; out.g = fb_g; out.b = fb_b;
    } else {
        set_errf("unknown night entry kind '%s'", kind);
        return 0xFFFE;
    }
    return day_idx;
}

// ---------- parse ----------

bool load(char* buf, size_t len) {
    reset_tables();
    last_error[0] = '\0';

    if (!buf || len == 0) { set_err("empty buffer"); return false; }

    // --- find and verify END line ---
    // END is "\nEND <hex>\n" near the tail. We checksum every byte up to
    // and including the trailing \n before the "END " token.
    size_t scan = len;
    // Trim trailing NULs / whitespace in case the buffer is sized bigger
    // than the blob (loadDefault passes the full buffer).
    while (scan > 0 && (buf[scan - 1] == '\0' || buf[scan - 1] == '\n' ||
                        buf[scan - 1] == '\r' || buf[scan - 1] == ' '))
        --scan;
    // Find the last "\nEND " before `scan`.
    size_t end_pos = (size_t)-1;
    for (size_t i = scan; i >= 5; --i) {
        if (buf[i - 5] == '\n' && buf[i - 4] == 'E' && buf[i - 3] == 'N' &&
            buf[i - 2] == 'D' && buf[i - 1] == ' ') {
            end_pos = i - 5;  // position of the '\n' itself
            break;
        }
        if (i == 5) break;
    }
    if (end_pos == (size_t)-1) { set_err("missing END line"); return false; }

    // Parse the checksum from the END line.
    const char* end_line = buf + end_pos + 1;  // skip the leading \n
    // end_line is "END XXXX\n" or "END XXXX"
    unsigned expected = 0;
    if (sscanf(end_line, "END %x", &expected) != 1) {
        set_err("malformed END line"); return false;
    }

    // Verify checksum over [0, end_pos] inclusive (the \n before END).
    uint16_t actual = fletcher16((const uint8_t*)buf, end_pos + 1);
    if (actual != (uint16_t)expected) {
        set_errf("checksum mismatch: got %04x expected %04x",
                 actual, (unsigned)expected);
        return false;
    }

    // --- line-cursor parse over [0, end_pos] ---
    // Include the trailing '\n' at end_pos in the cursor range so the last
    // content line gets null-terminated like every other line. Without this
    // +1, a section that immediately precedes END with no blank line (e.g.
    // BEHAVIORS, whose encoder emits the END line right after the last
    // instruction) would leave its last line un-NUL'd, and the stored
    // const char* would contain the rest of the buffer as a suffix —
    // breaking every strEq/startsWith lookup against that line's opcode.
    LineCursor cur{buf, buf + end_pos + 1};

    // 1. Magic
    char* ln = cur.next_nonempty();
    if (!ln || strncmp(ln, "CRIT ", 5) != 0) {
        set_err("missing CRIT magic"); return false;
    }
    int fmt_ver = atoi(ln + 5);
    if (fmt_ver != 1) {
        set_errf("unsupported format version %d", fmt_ver);
        return false;
    }

    // 2. Metadata — key/value lines until "---". `ir_version` drives the
    //    compatibility gate in begin(); `name` and `src_sha256` are copied
    //    into SCRIPT_NAME / SCRIPT_SHA so the heartbeat can report a real
    //    identity even on flash-only devices (NO_IR_OTA) or before the
    //    first successful OTA apply on an OTA-capable device. The rest of
    //    the metadata (encoded_at, encoder_version, …) is tooling-only and
    //    intentionally dropped.
    while ((ln = cur.next()) != nullptr) {
        if (strcmp(ln, "---") == 0) break;
        if (ln[0] == '\0') continue;
        // Split on first space.
        char* sp = strchr(ln, ' ');
        if (!sp) continue;
        *sp = '\0';
        const char* key = ln;
        const char* val = sp + 1;
        if (strcmp(key, "ir_version") == 0) {
            IR_VERSION = (uint8_t)atoi(val);
        } else if (strcmp(key, "name") == 0) {
            // Copy (not alias) — the buffer may be reused/freed after parse.
            size_t n = 0;
            while (val[n] && n + 1 < sizeof(SCRIPT_NAME)) {
                SCRIPT_NAME[n] = val[n]; ++n;
            }
            SCRIPT_NAME[n] = '\0';
        } else if (strcmp(key, "src_sha256") == 0) {
            size_t n = 0;
            while (val[n] && n + 1 < sizeof(SCRIPT_SHA)) {
                SCRIPT_SHA[n] = val[n]; ++n;
            }
            SCRIPT_SHA[n] = '\0';
        }
    }
    if (!ln) { set_err("missing --- after metadata"); return false; }

    // 3. Sections. Section kind is the first token; a count comes next,
    //    then that many body lines. Order of sections matches ir_text.py's
    //    encoder, but parse is kind-dispatched so small reorderings don't
    //    break us.
    while ((ln = cur.next_nonempty()) != nullptr) {
        // Sized to fit the widest header we emit: `NIGHT_DEFAULT cycle M
        // <sub1> <f1> ...` with M up to IR_MAX_NIGHT_CYCLE_ENTRIES (32),
        // so 2 + 1 + 64 = 67 tokens worst-case. Bump if a new section
        // grows the header.
        char* toks[72];
        int nt = tokenize(ln, toks, 72);
        if (nt < 1) continue;
        const char* kind = toks[0];

        if (strcmp(kind, "TICK") == 0) {
            if (nt < 2) { set_err("TICK missing value"); return false; }
            RUNTIME_TICK_MS = (uint32_t)atol(toks[1]);

        } else if (strcmp(kind, "COLORS") == 0) {
            if (nt < 2) { set_err("COLORS missing count"); return false; }
            int n = atoi(toks[1]);
            for (int i = 0; i < n; ++i) {
                char* body = cur.next_nonempty();
                if (!body) { set_err("COLORS truncated"); return false; }
                if (COLOR_COUNT >= IR_MAX_COLORS) {
                    set_err("IR_MAX_COLORS exceeded"); return false;
                }
                char* ctok[64];
                int cnt = tokenize(body, ctok, 64);
                if (cnt < 1) { set_err("COLORS empty line"); return false; }
                Color& C = COLORS[COLOR_COUNT];
                if (strcmp(ctok[0], "static") == 0) {
                    if (cnt < 5) { set_err("static color malformed"); return false; }
                    C.name = ctok[1];
                    C.kind = 0;
                    C.r = (uint8_t)atoi(ctok[2]);
                    C.g = (uint8_t)atoi(ctok[3]);
                    C.b = (uint8_t)atoi(ctok[4]);
                    C.cycle_start = 0;
                    C.cycle_count = 0;
                } else if (strcmp(ctok[0], "cycle") == 0) {
                    if (cnt < 3) { set_err("cycle malformed"); return false; }
                    C.name = ctok[1];
                    C.kind = 1;
                    int m = atoi(ctok[2]);
                    if (cnt < 3 + 2 * m) {
                        set_err("cycle entries truncated"); return false;
                    }
                    C.cycle_start = CYCLE_ENTRY_COUNT;
                    C.cycle_count = (uint8_t)m;
                    uint8_t fb_r = 255, fb_g = 255, fb_b = 255;
                    for (int j = 0; j < m; ++j) {
                        if (CYCLE_ENTRY_COUNT >= IR_MAX_CYCLE_ENTRIES) {
                            set_err("IR_MAX_CYCLE_ENTRIES exceeded"); return false;
                        }
                        const char* sub = ctok[3 + 2 * j];
                        int frames = atoi(ctok[4 + 2 * j]);
                        int sub_idx = colorIndexByName(sub);
                        if (sub_idx < 0) {
                            set_errf("cycle ref to unknown color '%s'", sub);
                            return false;
                        }
                        CYCLE_ENTRIES[CYCLE_ENTRY_COUNT].color_idx = (uint8_t)sub_idx;
                        CYCLE_ENTRIES[CYCLE_ENTRY_COUNT].frames    = (uint8_t)frames;
                        CYCLE_ENTRY_COUNT++;
                        if (j == 0) {
                            fb_r = COLORS[sub_idx].r;
                            fb_g = COLORS[sub_idx].g;
                            fb_b = COLORS[sub_idx].b;
                        }
                    }
                    C.r = fb_r; C.g = fb_g; C.b = fb_b;
                } else {
                    set_errf("unknown color kind '%s'", ctok[0]);
                    return false;
                }
                COLOR_COUNT++;
            }

        } else if (strcmp(kind, "NIGHT_COLORS") == 0) {
            if (nt < 2) { set_err("NIGHT_COLORS missing count"); return false; }
            int n = atoi(toks[1]);
            for (int i = 0; i < n; ++i) {
                char* body = cur.next_nonempty();
                if (!body) { set_err("NIGHT_COLORS truncated"); return false; }
                if (NIGHT_COLOR_COUNT >= IR_MAX_NIGHT_COLORS) {
                    set_err("IR_MAX_NIGHT_COLORS exceeded"); return false;
                }
                char* ntok[64];
                int cnt = tokenize(body, ntok, 64);
                NightEntry& N = NIGHT_COLORS[NIGHT_COLOR_COUNT];
                uint16_t day_idx = parseNightEntry(ntok, cnt, /*with_name=*/true, N);
                if (day_idx == 0xFFFE) return false;
                if (day_idx < IR_MAX_COLORS) NIGHT_FOR[day_idx] = (uint8_t)NIGHT_COLOR_COUNT;
                NIGHT_COLOR_COUNT++;
            }

        } else if (strcmp(kind, "NIGHT_DEFAULT") == 0) {
            // Rest of the line is the body (`<kind> [args]`). We already
            // have `toks[1..nt-1]` for it from the top-level tokenize.
            if (nt < 2) { set_err("NIGHT_DEFAULT missing body"); return false; }
            if (parseNightEntry(&toks[1], nt - 1, /*with_name=*/false,
                                NIGHT_DEFAULT) == 0xFFFE) {
                return false;
            }
            HAS_NIGHT_DEFAULT = 1;

        } else if (strcmp(kind, "NIGHT_MARKERS") == 0) {
            // v6 section. Format per entry: `<name> <r> <g> <b>`. Index
            // and decay K/T are inherited from the day MARKERS entry with
            // the matching name (a single counter governs decay in both
            // modes — see IrRuntime.h NIGHT_MARKERS comment). We require
            // MARKERS to have been parsed first so the name→index lookup
            // works; the encoder emits MARKERS before NIGHT_MARKERS, so
            // that ordering is guaranteed by the wire format.
            if (nt < 2) { set_err("NIGHT_MARKERS missing count"); return false; }
            int n = atoi(toks[1]);
            for (int i = 0; i < n; ++i) {
                char* body = cur.next_nonempty();
                if (!body) { set_err("NIGHT_MARKERS truncated"); return false; }
                if (NIGHT_MARKER_COUNT >= IR_MAX_MARKERS) {
                    set_err("IR_MAX_MARKERS exceeded (night)"); return false;
                }
                char* ntok[5];
                int ncnt = tokenize(body, ntok, 5);
                if (ncnt < 4) { set_err("night marker malformed"); return false; }
                // Resolve against day markers so the engine can index
                // Tile::count[] without a name lookup per cell.
                uint16_t day_idx = 0xFFFF;
                for (uint16_t j = 0; j < MARKER_COUNT; ++j) {
                    if (strcmp(MARKERS[j].name, ntok[0]) == 0) {
                        day_idx = MARKERS[j].index;
                        break;
                    }
                }
                if (day_idx == 0xFFFF) {
                    set_errf("night marker '%s' has no day counterpart",
                             ntok[0]);
                    return false;
                }
                Marker& M = NIGHT_MARKERS[NIGHT_MARKER_COUNT];
                M.name    = ntok[0];
                M.index   = day_idx;
                M.r       = (uint8_t)atof(ntok[1]);
                M.g       = (uint8_t)atof(ntok[2]);
                M.b       = (uint8_t)atof(ntok[3]);
                M.decay_k = 0;   // unused for night entries
                M.decay_t = 0;
                NIGHT_MARKER_COUNT++;
            }

        } else if (strcmp(kind, "MARKERS") == 0) {
            // v5 section. Format per entry:
            //   <name> <index> <r> <g> <b> <decay_k> <decay_t>
            // Ramp rgb are floats on the wire (preserving sim precision
            // for the round-trip test) but truncated to uint8 on load —
            // the device doesn't need fractional ramp coefficients per
            // MARKERS_SPEC §12 ("ramp Q8 math" — integer-only is fine).
            if (nt < 2) { set_err("MARKERS missing count"); return false; }
            int n = atoi(toks[1]);
            for (int i = 0; i < n; ++i) {
                char* body = cur.next_nonempty();
                if (!body) { set_err("MARKERS truncated"); return false; }
                if (MARKER_COUNT >= IR_MAX_MARKERS) {
                    set_err("IR_MAX_MARKERS exceeded"); return false;
                }
                char* mtok[8];
                int mcnt = tokenize(body, mtok, 8);
                if (mcnt < 7) { set_err("marker entry malformed"); return false; }
                Marker& M = MARKERS[MARKER_COUNT];
                M.name    = mtok[0];
                M.index   = (uint16_t)atoi(mtok[1]);
                M.r       = (uint8_t)atof(mtok[2]);
                M.g       = (uint8_t)atof(mtok[3]);
                M.b       = (uint8_t)atof(mtok[4]);
                M.decay_k = (uint8_t)atoi(mtok[5]);
                M.decay_t = (uint16_t)atoi(mtok[6]);
                if (M.index >= IR_MAX_MARKERS) {
                    set_errf("marker '%s' index %u >= IR_MAX_MARKERS",
                             M.name, (unsigned)M.index);
                    return false;
                }
                MARKER_COUNT++;
            }

        } else if (strcmp(kind, "LANDMARKS") == 0) {
            if (nt < 2) { set_err("LANDMARKS missing count"); return false; }
            int n = atoi(toks[1]);
            for (int i = 0; i < n; ++i) {
                char* body = cur.next_nonempty();
                if (!body) { set_err("LANDMARKS truncated"); return false; }
                if (LANDMARK_COUNT >= IR_MAX_LANDMARKS) {
                    set_err("IR_MAX_LANDMARKS exceeded"); return false;
                }
                char* ltok[4];
                int lcnt = tokenize(body, ltok, 4);
                if (lcnt < 3) { set_err("landmark malformed"); return false; }
                Landmark& L = LANDMARKS[LANDMARK_COUNT];
                L.name  = ltok[0];
                L.color = ltok[1];
                int pc  = atoi(ltok[2]);
                L.point_count = (uint16_t)pc;
                if (lm_pt_used + pc > IR_MAX_LANDMARK_POINTS) {
                    set_err("IR_MAX_LANDMARK_POINTS exceeded"); return false;
                }
                L.points = &LANDMARK_POINT_POOL[lm_pt_used];
                for (int j = 0; j < pc; ++j) {
                    char* pl = cur.next_nonempty();
                    if (!pl) { set_err("landmark points truncated"); return false; }
                    char* ptok[2];
                    int pcnt = tokenize(pl, ptok, 2);
                    if (pcnt < 2) { set_err("landmark point malformed"); return false; }
                    LANDMARK_POINT_POOL[lm_pt_used].x = parseCoord(ptok[0]);
                    LANDMARK_POINT_POOL[lm_pt_used].y = parseCoord(ptok[1]);
                    ++lm_pt_used;
                }
                LANDMARK_COUNT++;
            }

        } else if (strcmp(kind, "AGENTS") == 0) {
            if (nt < 2) { set_err("AGENTS missing count"); return false; }
            int n = atoi(toks[1]);
            for (int i = 0; i < n; ++i) {
                char* body = cur.next_nonempty();
                if (!body) { set_err("AGENTS truncated"); return false; }
                if (AGENT_TYPE_COUNT >= IR_MAX_AGENT_TYPES) {
                    set_err("IR_MAX_AGENT_TYPES exceeded"); return false;
                }
                char* atok[3];
                int acnt = tokenize(body, atok, 3);
                if (acnt < 3) { set_err("agent malformed"); return false; }
                AgentType& A = AGENT_TYPES[AGENT_TYPE_COUNT];
                A.name         = atok[0];
                A.limit        = (uint16_t)atoi(atok[1]);
                int sc         = atoi(atok[2]);
                A.state_count  = (uint16_t)sc;
                if (ag_state_used + sc > IR_MAX_AGENT_STATES) {
                    set_err("IR_MAX_AGENT_STATES exceeded"); return false;
                }
                A.states = sc > 0 ? &AGENT_STATE_POOL[ag_state_used] : nullptr;
                for (int j = 0; j < sc; ++j) {
                    char* sl = cur.next_nonempty();
                    if (!sl) { set_err("agent states truncated"); return false; }
                    // state is a single token; strip any leading/trailing
                    // whitespace by tokenizing.
                    char* stok[1];
                    int scnt = tokenize(sl, stok, 1);
                    if (scnt < 1) { set_err("agent state empty"); return false; }
                    AGENT_STATE_POOL[ag_state_used++] = stok[0];
                }
                AGENT_TYPE_COUNT++;
            }

        } else if (strcmp(kind, "INITIAL") == 0) {
            if (nt < 2) { set_err("INITIAL missing count"); return false; }
            int n = atoi(toks[1]);
            for (int i = 0; i < n; ++i) {
                char* body = cur.next_nonempty();
                if (!body) { set_err("INITIAL truncated"); return false; }
                if (INITIAL_AGENT_COUNT >= IR_MAX_INITIAL_AGENTS) {
                    set_err("IR_MAX_INITIAL_AGENTS exceeded"); return false;
                }
                char* itok[4];
                int icnt = tokenize(body, itok, 4);
                if (icnt < 4) { set_err("initial agent malformed"); return false; }
                InitialAgent& IA = INITIAL_AGENTS[INITIAL_AGENT_COUNT];
                IA.name  = itok[0];
                IA.x     = parseCoord(itok[1]);
                IA.y     = parseCoord(itok[2]);
                IA.state = itok[3];
                INITIAL_AGENT_COUNT++;
            }

        } else if (strcmp(kind, "SPAWNS") == 0) {
            if (nt < 2) { set_err("SPAWNS missing count"); return false; }
            int n = atoi(toks[1]);
            for (int i = 0; i < n; ++i) {
                char* body = cur.next_nonempty();
                if (!body) { set_err("SPAWNS truncated"); return false; }
                if (SPAWN_RULE_COUNT >= IR_MAX_SPAWN_RULES) {
                    set_err("IR_MAX_SPAWN_RULES exceeded"); return false;
                }
                // agent_type landmark interval <rest-of-line is condition>
                char* stok[3];
                char* rest = skip_tokens(body, 3, stok);
                SpawnRule& S = SPAWN_RULES[SPAWN_RULE_COUNT];
                S.agent_type = stok[0];
                S.landmark   = stok[1];
                S.interval   = (uint32_t)atol(stok[2]);
                S.condition  = rest;   // may be "" if no condition present
                SPAWN_RULE_COUNT++;
            }

        } else if (strcmp(kind, "PF_TOP") == 0) {
            if (nt < 4) { set_err("PF_TOP malformed"); return false; }
            PF_TOP_DIAGONAL          = (uint8_t)atoi(toks[1]);
            PF_TOP_HAS_DIAGONAL_COST = (uint8_t)atoi(toks[2]);
            PF_TOP_DIAGONAL_COST     = (float)atof(toks[3]);

        } else if (strcmp(kind, "PF") == 0) {
            if (nt < 2) { set_err("PF missing count"); return false; }
            int n = atoi(toks[1]);
            for (int i = 0; i < n; ++i) {
                char* body = cur.next_nonempty();
                if (!body) { set_err("PF truncated"); return false; }
                if (PF_CONFIG_COUNT >= IR_MAX_PF_CONFIGS) {
                    set_err("IR_MAX_PF_CONFIGS exceeded"); return false;
                }
                char* ptok[16];
                int pcnt = tokenize(body, ptok, 16);
                if (pcnt < 1) { set_err("PF empty line"); return false; }
                PFConfig& P = PF_CONFIGS[PF_CONFIG_COUNT];
                memset(&P, 0, sizeof(P));
                P.agent_name = ptok[0];
                for (int j = 1; j < pcnt; ++j) {
                    char* eq = strchr(ptok[j], '=');
                    if (!eq) continue;
                    *eq = '\0';
                    const char* k = ptok[j];
                    const char* v = eq + 1;
                    if (strcmp(k, "max_nodes") == 0)           { P.max_nodes        = atoi(v); P.has_max_nodes        = 1; }
                    else if (strcmp(k, "penalty_lit") == 0)    { P.penalty_lit      = atoi(v); P.has_penalty_lit      = 1; }
                    else if (strcmp(k, "penalty_occupied") == 0){ P.penalty_occupied = atoi(v); P.has_penalty_occupied = 1; }
                    else if (strcmp(k, "diagonal") == 0)       { P.diagonal         = (uint8_t)atoi(v); P.has_diagonal    = 1; }
                    else if (strcmp(k, "diagonal_cost") == 0)  { P.diagonal_cost    = (float)atof(v); P.has_diagonal_cost = 1; }
                    else if (strcmp(k, "step_rate") == 0)      { P.step_rate        = atoi(v); P.has_step_rate        = 1; }
                    else if (strcmp(k, "plan_horizon") == 0)   { P.plan_horizon     = atoi(v); P.has_plan_horizon     = 1; }
                    // Unknown keys: ignored. Forward-compat with future PF
                    // fields; the old encoder would have rejected them too
                    // but we're on the receiver end here.
                }
                PF_CONFIG_COUNT++;
            }

        } else if (strcmp(kind, "BEHAVIORS") == 0) {
            if (nt < 2) { set_err("BEHAVIORS missing count"); return false; }
            int n = atoi(toks[1]);
            for (int i = 0; i < n; ++i) {
                char* body = cur.next_nonempty();
                if (!body) { set_err("BEHAVIORS truncated"); return false; }
                if (BEHAVIOR_COUNT >= IR_MAX_BEHAVIORS) {
                    set_err("IR_MAX_BEHAVIORS exceeded"); return false;
                }
                char* btok[2];
                int bcnt = tokenize(body, btok, 2);
                if (bcnt < 2) { set_err("behavior header malformed"); return false; }
                Behavior& B = BEHAVIORS[BEHAVIOR_COUNT];
                B.agent_name = btok[0];
                int ic       = atoi(btok[1]);
                B.insn_count = (uint16_t)ic;
                if (insn_used + ic > IR_MAX_INSNS) {
                    set_err("IR_MAX_INSNS exceeded"); return false;
                }
                B.insns = &INSN_POOL[insn_used];
                for (int j = 0; j < ic; ++j) {
                    char* il = cur.next();   // keep empty-line case as an error
                    if (!il) { set_err("behavior insns truncated"); return false; }
                    // Trim leading whitespace; remainder is "<indent> <text...>".
                    while (*il == ' ' || *il == '\t') ++il;
                    char* itok[1];
                    char* rest = skip_tokens(il, 1, itok);
                    INSN_POOL[insn_used].indent = (uint8_t)atoi(itok[0]);
                    INSN_POOL[insn_used].text   = rest;
                    ++insn_used;
                }
                BEHAVIOR_COUNT++;
            }

        } else if (strcmp(kind, "SOURCE") == 0) {
            // Skip N lines; source is tooling-only and never parsed on-device.
            if (nt < 2) { set_err("SOURCE missing count"); return false; }
            int n = atoi(toks[1]);
            for (int i = 0; i < n; ++i) {
                if (!cur.next()) { set_err("SOURCE truncated"); return false; }
            }

        } else {
            // Unknown section: forward-compat. We have no length prefix for
            // arbitrary sections, so we can't safely skip. Fail loudly.
            set_errf("unknown section kind '%s'", kind);
            return false;
        }
    }

    return true;
}

}  // namespace critter_ir

// DEFAULT_IR_BLOB lives in the generated ir/critter_ir.h. Pulling it into
// this TU keeps the parser self-contained and makes CritterEngine.cpp's
// only job at init-time a single call to loadDefault(). Relative spelling
// so the file resolves next to IrRuntime.cpp under src/ir/ on Particle;
// host builds pass -I <build>/<script>/ir to mirror that layout.
#include "critter_ir.h"

namespace critter_ir {

bool loadDefault() {
    // The compiled-in blob may carry a trailing SOURCE block after the
    // END line (debug/audit payload). The device parser stops at END, so
    // we only need to buffer bytes up to and including the END line — the
    // rest stays in flash, never in RAM. Find the END-line terminator by
    // scanning for the first column-0 "\nEND " marker. Source lines are
    // `| `-prefixed so they can't produce a false match.
    size_t body_len = 0;
    if (DEFAULT_IR_BLOB_LEN >= 5 && memcmp(DEFAULT_IR_BLOB, "CRIT ", 5) == 0) {
        // Header is at offset 0; scan forward from there for "\nEND ".
        for (size_t i = 0; i + 5 < DEFAULT_IR_BLOB_LEN; ++i) {
            if (DEFAULT_IR_BLOB[i] == '\n'
                    && DEFAULT_IR_BLOB[i+1] == 'E'
                    && DEFAULT_IR_BLOB[i+2] == 'N'
                    && DEFAULT_IR_BLOB[i+3] == 'D'
                    && DEFAULT_IR_BLOB[i+4] == ' ') {
                // Include the END line itself up to (and including) its \n.
                size_t j = i + 5;
                while (j < DEFAULT_IR_BLOB_LEN && DEFAULT_IR_BLOB[j] != '\n') ++j;
                body_len = (j < DEFAULT_IR_BLOB_LEN) ? j + 1 : DEFAULT_IR_BLOB_LEN;
                break;
            }
        }
    }
    if (body_len == 0) body_len = DEFAULT_IR_BLOB_LEN;  // no END found; hand full blob to parser (will error)
    if (body_len + 1 > IR_DEFAULT_BUFFER_BYTES) {
        set_err("DEFAULT_IR_BLOB (up to END) too large for IR_DEFAULT_BUFFER_BYTES");
        return false;
    }
    memcpy(DEFAULT_BLOB_BUF, DEFAULT_IR_BLOB, body_len);
    DEFAULT_BLOB_BUF[body_len] = '\0';
    return load(DEFAULT_BLOB_BUF, body_len);
}

}  // namespace critter_ir

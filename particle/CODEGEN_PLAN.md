# Coati → Particle Photon 2 Codegen: Implementation Plan

## Goal

Build `coati/codegen.py`: a Python module that reads a parsed `CoatiIR` and emits
a `CoatiEngine.h` / `CoatiEngine.cpp` pair for the Particle Photon 2. The generated
files replace the hand-written engine entirely. The rest of the firmware HAL
(`coaticlock.cpp`, `WobblyTime.h`, `Stra2usClient`, `LightSensor.h`) is kept as-is
with minimal field-name updates.

**Invocation:**
```
python -m coati codegen scripts/coaticlock.coati --output-dir coaticlock/src/
```

---

## Deferred (out of scope for this implementation)

The following features are explicitly deferred. They are additive and will not block
what is built here:

- Landmark colors driven by IR (currently hardcoded in `coaticlock.cpp`)
- Rendering `pulse` / `shimmer` effects on device
- `static` and `sequence` goal types
- OTA behavior update (IR over Stra2us)

---

## Key Design Decisions

### `done` → lambda

The behavior tree uses `done` to early-exit the entire rule evaluation for an agent.
The generated C++ wraps each agent's behavior in a **lambda**, making `done` → `return;`.
The outer physics loop retains its own `continue` for movement/pause skipping.
This avoids `goto` and keeps the generated code readable.

```cpp
auto eval_agent = [&]() {
    // when carrying and not washed:
    if (a.carrying && !a.washed) {
        if (a.wash_counter > 0) {
            a.wash_counter--;
            // ...
            return;   // ← done
        }
        // seek pool — no-op if already there
        Point pool_dest = pool[i % pool_count];
        if (a.pos == pool_dest) return;
        if (!path_calculated) {
            a.path = find_path(a.pos, pool_dest, i);
            a.claimed = pool_dest;
            path_calculated = true;
        }
        return;       // ← end of when-block
    }
    // when carrying and washed:
    if (a.carrying && a.washed) { ... }
};
eval_agent();
```

### Pathfind budget

- `one pathfind per tick` → `bool path_calculated = false;` in behavior phase loop;
  set to `true` after any `seek` / `seek_nearest` that runs A*
- `unlimited pathfinds per tick` → omit the flag
- `seek` when already at dest → emit `if (a.pos == dest) return;` guard before A*
  (mirrors the Python engine fix for pool starvation)

### Agent struct

Script properties declared in `--- agents ---` become typed fields in the generated
`AgentState` struct. Built-in fields (`pos`, `last_pos`, `active`, `path`, `claimed`,
`wait_counter`) are always present. Example for `coaticlock.coati`:

```cpp
struct AgentState {
    int          index        = 0;
    Point        pos          = {0, 0};
    Point        last_pos     = {0, 0};
    bool         active       = true;
    std::vector<Point> path;
    Point        claimed      = {-1, -1};
    int          wait_counter = 0;
    // Script-defined properties:
    bool         carrying     = false;
    bool         washed       = false;
    bool         bored        = false;
    int          wash_counter = 0;
    int          pause_counter = 0;
    int          bored_counter = 0;
};
```

---

## IR → C++ Mapping Reference

### Conditions

| IR op | Generated C++ |
|---|---|
| `and` | `(A && B)` |
| `or` | `(A \|\| B)` |
| `prop_true` | `a.prop` |
| `prop_false` | `!a.prop` |
| `gt` / `gte` / `eq` | `a.prop > N` etc. |
| `gt_expr` | `a.prop > (3 + (int)i * 2)` |
| `at_landmark` | `a.pos == lm[i % lm_count]` |
| `not_at_landmark` | `a.pos != lm[i % lm_count]` |
| `at_claimed` | `a.claimed.x >= 0 && a.pos == a.claimed` |
| `landmark_free` | `lm_user == -1 \|\| lm_user == (int)i` |
| `standing_on_term` | `_in_vec(tk_term, a.pos)` |
| `cell_is_lit` / `on_lit` | `current_board[a.pos.x][a.pos.y]` |
| `at_any_landmark` | `_is_landmark(a.pos)` |
| `near_lit` | `_near_lit(a.pos)` |
| `term_exists` | `!tk_avail_term.empty()` |
| `term_empty` | `tk_term.empty()` |

### Actions

| IR action | Generated C++ |
|---|---|
| `done` | `return;` |
| `set prop val` | `a.prop = val;` |
| `increment prop` | `a.prop++;` |
| `decrement prop` | `a.prop--;` |
| `reset prop` | `a.prop = default_val;` |
| `clear_claimed` | `a.claimed = {-1, -1};` |
| `clear_path` | `a.path.clear();` |
| `pause N` | `a.pause_counter = N;` |
| `claim lm` | `lm_user = (int)i;` |
| `release lm` | `if (lm_user == (int)i) lm_user = -1;` |
| `pickup board` | `current_board[x][y] = false; fade_board[x][y] = 0; a.carrying = true;` |
| `pickup dumpster` | `a.carrying = true;` |
| `place` | `current_board[x][y] = true; fade_board[x][y] = 0; a.carrying = false;` |
| `discard` | `/* no board change */` |
| `despawn` | `a.active = false; a.path.clear(); a.claimed = {-1,-1}; return;` |
| `seek lm` | dest guard + `a.path = find_path(...); a.claimed = dest; path_calculated = true;` |
| `seek_nearest term` | dest guard + `_available(tk_term, i, avail); dest = _closest(...); ...` |
| `bob lm` | `if (a.pos.y == spot.y) a.path = {{spot.x, spot.y-1}}; else a.path = {spot};` |
| `step_up lm` | `a.path = {{a.pos.x, a.pos.y - 1}};` |
| `wander` | random-walk block: safe neighbors, near_lit guard, chance% roll |
| `become bored` (set bored true) | `a.bored = true;` |
| `set_display_color` | `// simulator only — omitted on device` |

---

## Implementation Steps

### Step 1 — Codegen scaffold: header + struct + class declaration

**Files:** `coati/codegen.py` [NEW]

- `class Codegen` with `generate(ir) → (header, source)` entry point
- `_emit_header(ir)` → emits `CoatiEngine.h`
  - `#pragma once`, Particle/STL includes, `Point` struct
  - `AgentState` struct (built-ins + all script properties with defaults)
  - `CoatiEngine` class declaration: boards, agents vector, public methods,
    private pathfinding scratch arrays, landmark lock ints
- `_emit_constructor(ir)` → initializes agents at start positions (fixed) or
  spawn_point inactive (pool mode)

**Verification:**
```bash
python -c "
from coati.parser import parse_file
from coati.codegen import Codegen
ir = parse_file('scripts/coaticlock.coati')
h, _ = Codegen().generate(ir)
print(h)
" | head -60
```
Expected: clean C++ header with correct struct fields.

---

### Step 2 — Condition and simple action emitters

**Files:** `coati/codegen.py`

- `_emit_condition(cond, index_var)` → recursive C++ boolean string
- `_emit_action(action, index_var, ir)` → C++ action statement(s)
  - Cover: `done`, `set`, `increment`, `decrement`, `reset`, `clear_claimed`,
    `clear_path`, `pause`, `claim`, `release`, `pickup`, `place`, `discard`,
    `despawn`, `become_bored`, `set_display_color` (skip)

**Verification:**
```bash
python -c "
from coati.codegen import Codegen
from coati.parser import parse_file
ir = parse_file('scripts/coaticlock.coati')
cg = Codegen()
# Spot-check a few conditions from the IR
for rule in ir.behavior.rules[:2]:
    print(cg._emit_condition(rule.condition, 'i'))
"
```
Expected: valid C++ boolean expressions for `carrying && !washed` etc.

---

### Step 3 — Behavior tree emitter (when / if / else / done)

**Files:** `coati/codegen.py`

- `_emit_body(body, index_var, indent)` → recursive if/else/action list
- `_emit_when_block(rule, index_var, indent)` → `if (condition) { body }`
- `_emit_behavior(ir)` → wraps all when-blocks in an `auto eval_agent = [&]() { ... }; eval_agent();` lambda per agent

**Verification:**
```bash
python -m coati codegen scripts/coaticlock.coati 2>&1 | head -120
```
Expected: readable C++ behavior tree in the output, matching coaticlock.coati logic.
Manually check that `when carrying and not washed` → correct nested if/done structure.

---

### Step 4 — seek, seek_nearest, wander, bob, step_up actions

**Files:** `coati/codegen.py`

- `seek lm` → dest-guard + A* call + `path_calculated = true`
- `seek_nearest term` → `_available()` call + closest + A* call
- `wander` → safe-neighbor enumeration, near_lit check, chance% roll, forced condition
- `bob lm` and `step_up lm` → direct path assignment

**Verification:**
Compare the generated `tick()` wander block against `CoatiEngine.cpp` line 359–385
(bored wandering) — should be semantically identical.

---

### Step 5 — tick(): terms, movement phase, collision, fade

**Files:** `coati/codegen.py`

- `_emit_compute_terms(ir)` → per tick: `std::vector<Point> tk_extras, tk_missing`
  populated by scanning the board, excluding landmark cells
- `_emit_movement_phase(ir)` → for each active agent with a path: collision check,
  advance one step or increment `wait_counter` and run collision rules
- `_emit_collision_rules(ir)` → emit the `--- collision ---` body inline
- `_emit_fade_update(ir)` → increment fade for all lit cells

**Verification:**
```bash
python -m coati codegen scripts/coaticlock.coati > /tmp/CoatiEngine.cpp
wc -l /tmp/CoatiEngine.cpp
```
Expected: ~250–400 lines, similar length to hand-written engine.

---

### Step 6 — Clock goal: update_target, apply_pending_target, megafont

**Files:** `coati/codegen.py`

- Emit megafont bitmaps array (copy from existing `CoatiEngine.cpp`)
- `_emit_update_target(ir)` → `update_target(time_t virtual_now)` method:
  snprintf hour/min, call `draw_digit()`, set `target_pending = true`
- `_emit_apply_pending_target(ir)` → copy pending_target → target_board,
  reset agent state, release landmark locks

**Verification:**
Run `python -m coati codegen scripts/coaticlock.coati` and confirm:
- `update_target()` and `apply_pending_target()` present with correct signatures
- Megafont data block present
- `draw_digit()` helper emitted with correct bit-scanning logic

---

### Step 7 — Pool mode: spawn phase, despawn, active flag

**Files:** `coati/codegen.py`

Only emitted when `ir.agents.pool_mode == True`.

- Agents initialized as `active = false`
- `_emit_spawn_phase(ir)` → `spawn_timer++; if (spawn_timer >= N) { spawn_timer=0; eval spawn condition; activate first inactive agent; }`
- `despawn` action → `a.active = false; a.path.clear(); a.claimed = {-1,-1}; return;`
- All movement/behavior loops gate on `if (!a.active) continue;`

**Verification:**
```bash
python -m coati codegen scripts/ladybugs.coati > /tmp/LadybugEngine.cpp
grep -n "spawn\|active\|despawn" /tmp/LadybugEngine.cpp
```
Expected: spawn timer logic, active checks in all loops, despawn handler.

---

### Step 8 — CLI wiring

**Files:** `coati/__main__.py` [MODIFY]

Add `codegen` subcommand:
```
python -m coati codegen <script.coati> [--output-dir DIR]
```
- Parses the script → IR
- Calls `Codegen().generate(ir)` → `(header, source)`
- Writes `CoatiEngine.h` and `CoatiEngine.cpp` to `--output-dir` (default: cwd)
- Prints a summary: grid size, agent count/mode, rule count, actions emitted

**Verification:**
```bash
python -m coati codegen scripts/coaticlock.coati --output-dir /tmp/gen/
ls /tmp/gen/
# CoatiEngine.h  CoatiEngine.cpp
```

---

### Step 9 — Update coaticlock.cpp field names

**Files:** `coaticlock/src/coaticlock.cpp` [MODIFY]

Three field renames to match generated struct (all in the render loop ~line 311):

```diff
-  CoatiAgent& a = engine.agents[i];
+  AgentState& a = engine.agents[i];

-  if (a.is_bored) {
+  if (a.bored) {

-  if (a.wait_ticks > 1) {
+  if (a.wait_counter > 1) {
```

No other changes. `engine.current_board`, `engine.fade_board`, `a.pos`,
`a.last_pos`, `a.carrying` all keep their names.

**Verification:** File compiles cleanly after swapping in generated engine files.

---

### Step 10 — Build, flash, and behavioral verification

**Files:** generated `CoatiEngine.h` / `CoatiEngine.cpp` in `coaticlock/src/`

1. Generate:
   ```bash
   python -m coati codegen scripts/coaticlock.coati --output-dir coaticlock/src/
   ```
2. Build:
   ```bash
   cd coaticlock && particle compile photon2 src/ --saveTo firmware.bin
   ```
   Expected: zero errors, zero warnings.

3. Flash:
   ```bash
   particle flash <device-name> firmware.bin
   ```

4. Behavioral checks:
   - Display renders current time correctly within 30s of boot
   - Both agents actively rearrange pixels on minute change
   - No crash / watchdog reset after 10 minutes of runtime
   - `System.freeMemory()` (via serial log) shows > 500KB free

5. Python sim parity check (optional but recommended):
   - Set both sim and device to same static time
   - Compare pixel state after N ticks — should converge to identical pattern

---

## File Summary

| File | Status | Notes |
|---|---|---|
| `coati/codegen.py` | NEW | ~500–700 lines |
| `coati/__main__.py` | MODIFY | add `codegen` subcommand |
| `coaticlock/src/CoatiEngine.h` | REPLACE (generated) | |
| `coaticlock/src/CoatiEngine.cpp` | REPLACE (generated) | |
| `coaticlock/src/coaticlock.cpp` | MODIFY (3 lines) | field renames |
| All other HAL files | UNCHANGED | |

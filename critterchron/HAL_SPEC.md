# CritterChron Hardware Abstraction Layer — Spec & Phased Plan

Scope: run the CritterChron IR (as produced by `compiler.py`) on a Particle
Photon 2 driving a WS2812B matrix (16×16 or 32×8, zigzag wiring), with a
clean seam for a future ESP32 port. The Python sim in this directory is the
behavioral gold master — the HAL must reproduce its tick semantics.

The `particle/` sibling project is reference, not source. Where the coati
reference and the newer sim disagree, **the newer sim wins**.

---

## 1. Design principles

1. **Interpreter, not codegen.** The CritterChron IR is already a flat token
   stream (`engine.py:_process_agent_behavior`) — string opcodes, per-agent
   `pc` continuation, yields on `pause`/`wander`/`seek`. That's nearly a VM
   spec. Porting it 1:1 to C++ is cheaper than codegen and keeps behavior
   parity to a single source of truth. It also leaves the door open to OTA
   IR pushes later without re-flashing firmware.
2. **One minimal platform seam.** Copy coati's `LedSink` pattern: the engine
   knows nothing about NeoPixel, FastLED, SPI, pins, or Particle APIs. It
   calls `clear / set(idx, r, g, b) / show` and returns. Everything else —
   time, WiFi, analog, OTA — lives in the per-platform `main`.
3. **Per-device header owns identity + geometry.** Each physical device gets
   its own header (e.g. `ranier_raccoon.h`) defining grid, rotation, pin,
   Stra2us ID/secret. The Makefile symlinks `creds.h` to the chosen device
   header. This mirrors the coati convention and is the right shape.
4. **Deferred is deferred.** WobblyTime, Stra2us, and light-sensor autobright
   are explicit phase-2+ items. Phase 1 uses system time and a fixed
   brightness. Their seams exist from day one so we don't rewrite `loop()`
   later.
5. **Don't let hand-written code shape the engine.** Coati §7.9 applies: if
   a platform shim wants a field the IR doesn't declare, the shim is wrong.
   The engine is the contract.

---

## 2. Component layout

```
critterchron/
  hal/
    CritterEngine.h / .cpp        ← IR interpreter, rendering, A*
    interface/
      LedSink.h                   ← the one engine↔platform seam
      CritTimeSource.h            ← abstract "now()" (RTC, WobblyTime later)
    photon2/
      critterchron_photon2.cpp    ← setup/loop, NeoPixel sink, light sensor
    esp32/                        ← future; empty for now
    devices/
      <name>.h                    ← per-device header (geometry + Stra2us)
    ir/
      critter_ir.h                ← generated: IR as C++ initializers
      ir_encoder.py               ← compiler.py IR → critter_ir.h
  compiler.py / engine.py / …     ← unchanged
```

The IR is baked in as a C++ header via `ir_encoder.py` in phase 1. Phase 3+
can optionally load it from flash or Stra2us — same on-device interpreter.

---

## 3. Per-device header

Single source for everything that varies per physical unit. Phase-1
required fields are marked *(P1)*; everything else stays `#undef`-tolerant.

```cpp
// devices/ranier_raccoon.h
#pragma once

// --- Identity ---
#define DEVICE_NAME        "ranier_raccoon"     // (P1)
#define STRA2US_CLIENT_ID  "ranier_raccoon"     // (P2)
#define STRA2US_SECRET_HEX "<64 hex>"           // (P2)
#define STRA2US_HOST       "stra2us.austindavid.com"
#define STRA2US_PORT       8153
#define STRA2US_APP        "critterchron"

// --- Hardware geometry ---                     (all P1)
#define GRID_WIDTH    16
#define GRID_HEIGHT   16
#define GRID_ROTATION 0        // 0 / 90 / 180 / 270 (matters on 16×16)
#define MATRIX_PIN    SPI      // overrideable; default SPI (MOSI on Photon 2)
#define PIXEL_TYPE    WS2812B

// --- Tuning ---                                (P1, optional)
// #define MAX_BRIGHTNESS  0.8f
// #define TIMEZONE_OFFSET_HOURS -5.0f
//
// Physics tick rate is NOT a device knob — it's baked into the .crit
// script and comes through as critter_ir::TICK_RATE_MS. Render tick is
// a firmware constant in the platform shim (~30fps). Don't add either
// here; they'd silently diverge hardware from sim.
```

The Makefile copies/symlinks the chosen device header to `creds.h`, matching
coati's convention so nothing in `src/` references a device by name.

---

## 4. Platform seam: `LedSink` (+ `CritTimeSource`)

### 4.1 `LedSink` — brightness + geometry ownership on the sink
```cpp
struct LedSink {
    virtual void clear() = 0;
    virtual void set(int x, int y, uint8_t r, uint8_t g, uint8_t b) = 0;
    virtual void show() = 0;
    virtual ~LedSink() = default;
};
```
The sink owns **rotation + zigzag mapping → linear index** and
**brightness scaling**. The engine hands it logical pre-rotation `(x, y)`
coordinates and full-brightness colors; each platform's sink adapts those
to its physical wiring. Keeps the engine fully hardware-agnostic — same
`.o` runs on Photon2 and ESP32 regardless of their serpentine/rotation
conventions. That means:

- **Phase 1** sinks multiply by a fixed `MAX_BRIGHTNESS` constant.
- **Phase 2** the light-sensor low-pass feeds the sink; engine unchanged.
- **Future** per-agent self-coloring or effects land in the engine
  without re-threading brightness logic.

This also keeps the coati-style loading spinner out of the engine: the
platform shim can draw directly into the sink pre-sync, since the sink
is both the brightness and output boundary.

### 4.2 `CritTimeSource` — new, small
```cpp
struct CritTimeSource {
    virtual bool   valid() const = 0;         // cloud sync state
    virtual time_t wall_now() const = 0;      // UTC seconds
    virtual float  zone_offset_hours() const = 0;
    virtual ~CritTimeSource() = default;
};
```
Phase 1 impl = Particle `Time.*`. Phase 2 wraps it in `WobblyTime`. The
engine calls only through this interface for `sync_time()`.

Originally named `TimeSource`; renamed 2026-04-25 after Particle DeviceOS
6.x added `enum class TimeSource : uint8_t` to `spark_wiring_time.h`,
which collided with our struct on Photon 2 / Argon (both DeviceOS 6.x;
Photon Gen 2 was untouched because it's pinned to a 3.x LTS without the
enum). The `Crit` prefix is the namespace marker we use anywhere a
plausibly-generic name might collide with vendor headers.

The key is to keep both interfaces **pure virtual + platform-provided** so
the engine is unit-testable on host.

---

## 5. The engine

### 5.1 State (mirrors `Tile` / `Agent` in `engine.py`)
```cpp
struct Tile {
    uint8_t state     : 1;   // painter-driven
    uint8_t intended  : 1;   // font oracle
    int16_t claimant;        // -1 = unclaimed
    uint8_t r, g, b;         // color written by last draw
};

struct Agent {
    int16_t id;
    uint8_t name_id;         // index into IR's agent-type table
    Point   pos, prev_pos;
    uint8_t color[3];
    int16_t remaining_ticks;
    int16_t pc;
    int16_t seek_ticks;
    uint8_t state_str_id;    // interned
    bool    glitched;
    int16_t counters[4];     // reserved; unused in phase 1
};
```
No per-agent `config` map — the sim ignores it anyway (README §gotchas).
Drop it rather than faithfully porting dead state. The `counters` slab is
dead weight in phase 1 but reserves space so adding numeric agent-local
state later (incr/decr/test-and-set) doesn't re-lay the struct.

### 5.2 Tick loop
One-to-one with `CritterEngine.tick()`:

1. clear claims
2. apply spawn rules (`interval` + global cond check)
3. for each agent: decrement `remaining_ticks`; if zero, run
   `_process_agent_behavior` until a yield or the 100-instruction glitch
   trip
4. sort by id (deterministic conflict resolution)
5. convergence metric

The 100-instruction safety cap is **load-bearing** and must be ported. It
is the on-device guarantee that a bad script can't hang the display loop.

### 5.3 Interpreter
String opcodes in the IR become a small tagged-union opcode table at encode
time. `ir_encoder.py` rewrites `"seek missing timeout 20"` into
`{OP_SEEK, target=MISSING, timeout=20, …}`. Keeps the device interpreter
cache-friendly and lets us validate opcodes at build time instead of at
runtime on a Photon with no useful stdout.

All opcodes from `engine.py` port: `if on <x>`, `else`, `done`,
`set state =`, `erase`, `draw <color>`, `pause [N|LO-HI]`, `despawn [name]`,
`wander [avoiding current|any]`, `seek [nearest] <tgt> [(not) on <kind>]
[timeout N]`.

**Name resolution happens at parse time.** `if on <name>:` and
`seek <name>` take a bare identifier; the parser resolves it against the
declared landmark and agent sets and emits distinct opcodes
(`IF_ON_LANDMARK <id>` vs `IF_ON_AGENT <id>`, same for `SEEK_LANDMARK` /
`SEEK_AGENT`). Collisions (a name declared as both) are a parse error
with a suggestion; the optional disambiguating forms
`if on landmark <name>:` and `if on agent <name>:` exist as escape
hatches. The interpreter never does string matching against names at
runtime.

**Termination rule.** The parser rejects any statically reachable path
through the top-level instruction sequence that falls off the end
without yielding (`done`, `pause`, `wander`, `seek`, `despawn`). `if:`
bodies — true *or* false branch — are free to fall through into the
sibling instruction at the same indent; the straight-line "try each
condition in turn" pattern is the intended shape. Only top-level
fall-off is an error.

**`done` semantics.** `done` means "this agent is finished for this
tick; next tick, restart evaluation from the top of its behavior
block." Equivalent to the sim's `pc = 0; return`. This differs from
coati's `done` (which is "stop for this tick only"); documented here so
readers don't carry over the wrong mental model.

**IR version byte.** The encoded IR carries a single-byte format
version at its head. The interpreter refuses anything newer than it
knows. Phase 1 writes `0x01`; future additions (counters, new opcodes,
new condition kinds) bump it.

### 5.4 Pathfinder
Bounded A* with Manhattan heuristic and `penalty_lit=10` /
`penalty_occupied=20`. Node cap default 512, configurable per-script in
`--- pathfinding ---`.

**Diagonal movement — opt-in per script.** The `--- pathfinding ---`
section gains a `diagonal: allowed | disallowed` toggle, default
`disallowed`. When allowed, the 8-neighbor expansion applies to both A*
and `wander` (diagonals are a movement rule, not a pathfinder feature).
Pass-through is allowed — an agent stepping diagonally from (x,y) to
(x+1,y+1) does not trigger any `on <agent>` / same-tile collision at
(x,y+1) or (x+1,y); encounters still require strict same-tile.

Parser enforces mutual presence of `diagonal` and `diagonal_cost`:

- `diagonal: allowed` with no `diagonal_cost` → parse error.
- `diagonal_cost` set without `diagonal: allowed` → parse error.

Both-or-neither. Default cost we'd encourage in scripts is `1.414`
(preserves "straight is preferred" unless `penalty_lit` dominates);
`1.0` lets diagonals win freely. The cost applies to that move's
contribution to A* `g_cost`; nothing else.

Node cap may need a modest bump (512 → 768) when diagonal is allowed,
since 8-neighbor expansion doubles the fringe; left as a measurement
once the dainty-agent interior-of-`8` test case exists. See §10 Engine
budget.

---

## 6. Rendering

CritterChron's tile model already carries per-tile color (set by `draw
<color>`). Unlike coati, there is no separate `--- rendering ---` section
— the `.crit` script declares colors in `--- colors ---` and agents/tiles
use them directly. Render path:

1. `sink.clear()`
2. For each tile where `state` is true and no agent stands on it this
   frame: `sink.set(map(x,y), tile.rgb)`
3. For each agent: interpolate `prev_pos → pos` by `blend` (smooth motion
   at render rate), write `agent.rgb` to both `prev_pos` (at `1−blend`
   weight) and `pos` (at `blend` weight). **Agent color dominates** —
   painted-tile color underneath is not visible while the agent stands
   on it. (Author reserves the right to add agent self-coloring / tile
   show-through later.)
4. Glitched agents render as a rainbow cycle keyed off `now_ms` (the
   only on-device signal that a script is broken — matches the sim's
   diagnostic state in `renderer.py`).
5. `sink.show()`

**Lerp is mandatory, not optional.** The render tick blends between
`prev_pos` and `pos` using `blend = (now - last_physics_tick) /
critter_ir::TICK_RATE_MS` (clamped to 1.0). This is what makes the clock look
alive rather than turn-based, and it's load-bearing for the project's
intent.

Rotation + zigzag are applied inside the engine's `map(x, y)`:
```cpp
int phys_h = (rotation == 90 || rotation == 270) ? GRID_WIDTH : GRID_HEIGHT;
// rotate, then zigzag by physical column parity
```
(Same shape as `CoatiEngine::render`'s `_map` lambda.)

Brightness is not multiplied here — the engine writes full-intensity
colors to the sink, which scales on its way to the strip (§4.1).

Phase 1 has no pulse / shimmer / fade — the sim doesn't use them. We can
add a `fade` layer later if the language adopts it.

---

## 7. Phased plan

### Phase 1A — MVP on Photon 2, 16×16 *(1–2 weekends)*
**Goal:** a `.crit` file flashes to a Photon 2 and updates the clock.

- `ir_encoder.py`: dump a parsed IR to `critter_ir.h` (C++ initializer).
- `CritterEngine` (header + .cpp): tile/agent structs, tick loop, A\*
  (with dynamic cap — see §10), interpreter for every opcode in
  `engine.py`, renderer with rotation + zigzag. No fades.
- `LedSink` + `NeoPixelSink` on Particle; sink owns brightness scaling
  against a fixed `MAX_BRIGHTNESS`.
- `CritTimeSource` backed by `Time.*`; local-wall-clock shift identical to the
  coati fix (`local_now = utc + zone*3600`).
- `photon2/critterchron_photon2.cpp`: setup/loop with physics tick +
  render tick (ms constants from device header), pre-sync spinner
  (drawn directly to sink), fixed brightness.
- `devices/<name>.h` + Makefile symlink.
- One known-good 16×16 device header checked in.
- Smoke test: run `ants.crit` and `thyme.crit`; compare tick-by-tick tile
  state with the Python sim for the first 500 ticks using a seeded RNG
  and a serial log dump.
- Budget telemetry: physics-tick duration, A\* nodes explored,
  pathfinds attempted/served/skipped, current per-agent-type caps.
  Exposed as Particle.variable so the designer can watch from the
  cloud console without Stra2us.

**Exit criteria:** HH over MM renders correctly on a 16×16 panel at
`GRID_ROTATION=0` and `=90`; ants converge within the same ~tick budget
as the sim.

### Phase 1B — 32×8 support *(small, follows 1A)*
Bumped up from later phases because 32×8 exercises a different
pathfinding shape — very wide, very short, different font layout
(`HH  MM` horizontal, vertically centered) — and finding bugs here
before Stra2us / WobblyTime land is cheaper than finding them later.

- Sim-first: the `--widescreen` TODO in `TODO.md` lands in `engine.py`
  and the renderer, with a 32×8 HH MM horizontal layout.
- HAL: nothing new mechanically — grid constants come from the device
  header. One 32×8 device header checked in.
- Diagonal pathfinding stress test: 32×8 with `diagonal: allowed`
  through a dense `ants` population exercises the longest-expected
  paths on the narrower axis. Good budget calibration target for §10.

**Exit criteria:** same `.crit` scripts that ran on 16×16 (modulo
landmark coordinates using `max_x`/`max_y`) run on 32×8 with visibly
correct font layout and convergence.

### Phase 2 — Environmental polish *(1 weekend)*
- Light sensor + brightness smoothing (port `LightSensor.h`). Hooks
  into the sink's brightness multiplier — the engine does not learn
  about ambient light.
- Pre-sync loading spinner formalized (system-coupled, per coati §7.10
  — belongs in the shim, not the engine).
- Stra2us KV overrides for tuning knobs (`MAX_BRIGHTNESS`,
  `TIMEZONE_OFFSET_HOURS`). Physics tick stays script-owned.

### Phase 3 — WobblyTime *(small)*
- Implement `WobblyTime` as a `CritTimeSource` decorator around the Particle
  time source. Same parameters (`wobble_min/max_seconds`, `fast_rate`,
  `slow_rate`) as the coati impl; source can be lifted mostly verbatim.

### Phase 4 — Stra2us telemetry *(1 weekend)*
- Port `Stra2usClient` + HMAC headers from coati.
- Telemetry thread: heartbeep every `N` seconds with uptime, RSSI, free
  memory, brightness calibration, IR version hash.
- KV poll: global then per-device keys, same as coati's `poll_config`.
- Live-tunable params: brightness bounds, tick rates, wobbly params,
  timezone offset.

### Phase 5 — ESP32 port *(stretch)*
- New `esp32/critterchron_esp32.cpp` with a FastLED-backed `LedSink`.
- WiFi / NTP / mbedTLS-HMAC for Stra2us.
- Engine and device-header format unchanged.

### Phase 6 — OTA IR *(stretch)*
- Ship IR bytes as a Stra2us KV payload, swap the static `critter_ir.h`
  blob for a flash-loaded buffer. Requires stable IR encoding — good
  reason to lock it down in phase 1.

---

## 8. Sim-first prerequisites

Changes that must land in `engine.py` before the HAL can mirror them
without breaking gold-master parity:

- **Diagonal movement toggle.** `--- pathfinding ---` parses
  `diagonal: allowed | disallowed` and `diagonal_cost: <float>`; A* and
  `wander` both respect it. Verify `ants.crit` / `thyme.crit` /
  `tortoise.crit` still converge unchanged with default `disallowed`.
  Add one new example script that uses `diagonal: allowed` inside a
  closed digit (the "dainty agent" test).
- **Parser: name resolution.** Drop the hardcoded `nest`/`pool`/`trash`
  specials in `_process_agent_behavior`; resolve `if on <name>:` and
  `seek <name>` against the declared landmark/agent sets at parse time
  and record the distinct IR opcodes.
- **Parser: top-level termination check.** Reject scripts whose
  top-level sequence can fall off the end without yielding. (Already
  partly in place via the loop-without-yield test; tighten the
  reachability analysis.)
- **IR schema version byte.** Start at `0x01`.

None of these change script syntax except the new pathfinding fields.
Existing `.crit` files should continue to parse and run unchanged.

---

## 9. Decisions locked in

- Interpreter over codegen.
- Per-device header + Makefile symlink to `creds.h`.
- HAL tree lives at `critterchron/hal/` with `hal/photon2/` (and later
  `hal/esp32/`) platform subdirs. Single repo, small number of HALs.
- `LedSink` + `CritTimeSource` platform seams. **Sink owns brightness
  scaling**; the engine writes full-intensity colors.
- `done` = restart-next-tick; parser enforces top-level yield
  termination; `if:` bodies may fall through.
- Bare-identifier resolution at parse time (landmark vs agent);
  optional disambiguating `if on landmark <x>:` / `if on agent <x>:`.
- Counter slab + IR version byte reserved in phase 1; numeric
  properties deferred to a later language pass.
- Diagonal movement: script-wide opt-in in `--- pathfinding ---`;
  applies to A* *and* `wander`; pass-through allowed; cost and toggle
  are both-or-neither (parser enforces); default off.
- **Rendering priorities:** agent color dominates over tile color
  (agent on top). Motion-blur lerp between `prev_pos` and `pos` at
  render rate — mandatory, not optional.
- **Glitch rendering:** rainbow cycle on device, same as the sim.
  Silent failure is a non-starter — this is the only signal the user
  has that a script has broken.
- **Phase-1 brightness:** fixed `MAX_BRIGHTNESS` in the sink;
  autobrightness is a phase-2 replacement that does not touch the
  engine.
- **32×8 support bumped into Phase 1B** (was phase 2) — different
  pathfinding shape, good early stress test.

---

## 10. Engine budget (dynamic bang-bang cap)

Physics tick at 10 Hz = 100 ms. Render tick at 60 Hz = 16.7 ms. Photon 2
is Cortex-M33 @ 200 MHz. Rough per-tick consumers on a 16×16 grid:

| Work | Cost estimate | Notes |
|---|---|---|
| Clear claims + convergence scan | ~50 µs | 256 tiles, trivial |
| Spawn rules | <10 µs | few rules, list walk |
| Per-agent behavior (100-insn cap) | ~1 µs/insn × N×100 | negligible vs A\* |
| A\* pathfind, 512 nodes, 8-neighbor | ~3–5 ms per call | priority-queue heavy; measure |
| `sink.show()` for 256 px (WS2812B) | ~7.7 ms | at render tick, not physics |

The sim pathfinds every seeker every tick. On device, 20 agents × 5 ms
= 100 ms = budget gone. At 80 ants (`ants.crit`), catastrophic. "Floor
is lava" with 180 agents is real.

### 10.1 Design: dynamic cap per agent-type

Rather than pick a static per-tick limit and hope it fits, the engine
measures its own overrun and **bang-bangs** the A\* node cap. The
script author's declared `max_nodes` is an **upper bound**, not a
target — the engine may shrink it per-agent-type when physics ticks are
running hot, and grow it back when they're not. The script's job is to
say "this agent type never needs a path longer than X"; the engine's
job is to stay inside its physics budget.

**Policy (phase 1, single global cap):**

- Track `current_cap` starting at `script_max_nodes` (default 512, or
  768 if diagonal).
- After each physics tick, record wall-clock duration `T_tick`.
- Rolling window of last `K = 4` ticks.
- If `max(window) > 0.7 × TICK_RATE_MS` (hot): `current_cap /= 2`,
  floor at `MIN_CAP = 32`.
- If `max(window) < 0.4 × TICK_RATE_MS` and `current_cap <
  script_max_nodes`: `current_cap *= 1.25`, ceiling at script max.
- Hysteresis bands (`0.4` vs `0.7`) prevent ping-ponging.

**Per-type shrink (phase 1B or later):** track nodes consumed per
agent type each tick; when shrinking, halve only the type that
contributed most. When growing, grow the type furthest from its script
max. Not in phase 1A — single global cap is simpler and will be enough
for `ants` / `tortoise` / `thyme`; per-type matters more when you run
coati-style long-haul seekers alongside busy short-path types.

### 10.2 Best-so-far pathfind (sim-first prerequisite)

When A\* exhausts its node cap, it currently returns `None` and the
agent doesn't step. Change the sim so exhaustion returns the path to
the **closest-explored fringe node** (coati does this, see §7.8 in
`particle/DESIGN.md`). With this change:

- A shrunken cap still produces *a* step in roughly the right
  direction, every tick.
- Agents make progress even while the engine is recovering from a
  spike.
- The "dainty agent" that can't route around a lit tile gets a
  wander-like step toward its target instead of freezing.

This is what makes the bang-bang cap safe to shrink aggressively: the
worst outcome is a slower/dumber path, never a stuck agent.

### 10.3 Telemetry for the designer

Exposed as Particle.variable in phase 1A, wrapped into Stra2us publish
in phase 4. All already cheap to compute:

- `tick_ms_avg`, `tick_ms_max` (over last `K` ticks)
- `pf_attempts`, `pf_served`, `pf_exhausted_cap` (per tick)
- `pf_nodes_total` (per tick)
- `current_cap` (and per-type once that lands)
- `glitches`, `failed_seeks` (already tracked)

Designer reads these to understand how close their script is to
starving. "Busy sims can't have expensive routing, and complex sims
can't have a lot of agents" — the numbers make that trade-off legible
instead of mysterious.

### 10.4 Knob exposure

All caps, tick rates, and bang-bang thresholds are compile-time
`constexpr` in phase 1A, pulled from the device header where
customizable. No Stra2us wiring day-1; phase 4 exposes whatever
subset makes sense for runtime tuning. Particle.variable/function
covers the development-console case until then.

---

## 11. Still-open, ask before we start phase 1

None. All the shape-affecting questions are answered. Remaining items
are just *implementation* order within phase 1A — I'll start with the
sim-first prerequisites (diagonal movement, name resolution at parse,
best-so-far A\*, termination check, IR version byte), then the IR
encoder, then the C++ engine, then the Photon shim.

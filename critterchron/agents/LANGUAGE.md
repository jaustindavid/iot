# The .crit language

A .crit file describes a tiny multi-agent system that paints the "intended"
pixels of a clock display. The compiler (`compiler.py`) turns it into IR; the
Python engine runs it under `renderer.py`, and `hal/ir/ir_encoder.py` emits a
C++ header for the same IR to execute on-device.

The language is intentionally small. Every opcode must parse to one of the
canonical shapes in `compiler.py`'s `_OPCODE_PATTERNS`; unknown instructions
are rejected, not silently ignored.

## Hello world

A single agent sitting in one spot, cycling red → green → blue:

```
--- colors ---
red:     (255, 0, 0)
green:   (0, 255, 0)
blue:    (0, 0, 255)
rainbow: cycle red 2, green 2, blue 2
bug:     (128, 128, 128)

--- agents ---
bug: state {none}
bug starts at (8, 4)

--- simulation ---
tick rate: 100ms

--- behavior ---
bug:
  set color = rainbow
  pause 10
  done
```

See [agents/rainbow.crit](rainbow.crit).

## Worked examples

Four annotated scripts covering most of the language. Read them in this order:

- [tortoise.crit](tortoise.crit) — simplest painter; `seek nearest`, `draw`, `erase`, `wander`.
- [thyme.crit](thyme.crit) — predator/prey; states, `spawn`, inter-agent `seek` + `despawn`.
- [coati.crit](coati.crit) — multi-state scavenger; diagonal pathfinding, `step`, nested `if`s.
- [ants.crit](ants.crit) — swarm; 80 agents at 50ms, `seek ... timeout N`, self-`despawn`.

## File structure

A file is a sequence of `--- header ---` sections. Sections can appear in any
order. Recognized headers: `colors`, `landmarks`, `agents`, `spawning`,
`simulation`, `pathfinding`, `behavior`. Anything outside a section is
ignored (use it for a file header comment).

Lines starting with `#` are comments. `#` mid-line also starts a comment in
behavior blocks.

Indent with spaces only. A tab anywhere in the file fails compilation with a
line number — mixed indentation has bitten too many scripts.

### Name spaces and collisions

Colors, landmarks, agent types, and markers share a flat namespace with one
deliberate overlap:

- **Agent name == color name is required.** Every agent type must have a
  color or cycle of the same name; that's the default body color at
  spawn. This is the *one* overlap the compiler demands.
- **Landmark name == agent name is forbidden.** `if on <name>` /
  `seek <name>` would be ambiguous.
- **Landmark name == color name is forbidden.** `if standing on <name>`
  would be ambiguous.
- **Marker name == landmark / agent / color name is forbidden.** `if <m>`
  (marker-count predicate) and `if <landmark>` (cell-on-landmark) reach
  for the same sugar; keep them distinct so the canonicalizer's dispatch
  stays unambiguous.

Pick landmark names that aren't already colors, agents, or markers. If a
pile-of-thing agent collides with its tile color (e.g. `brick` tiles and a
`brick` agent), rename one — `bricks` plural for the pile reads naturally.

## Blocks

### `--- colors ---`

Named RGB tuples (and a few related entities that share this section
syntactically). Every agent type and every landmark must have a color or
cycle by the same name, or compilation fails.

```
red: (255, 0, 0)
```

A `cycle` is a color that animates at render time, walking a list of
`(color_name, frames)` entries. Frames are counted against render ticks
(~50Hz on hardware). Colors referenced by a cycle must be static — cycles
don't nest.

```
rainbow: cycle red 2, green 2, blue 2
```

Agents can bind to a cycle at spawn (by naming the agent type as the cycle)
or via `set color = <name>` at runtime. Agents spawned sharing a cycle get
a randomized per-agent phase offset so they drift out of lockstep. Tiles
snapshot the current cycle value at the moment of `draw` — painted tiles
don't animate.

#### Markers (`ramp`)

A marker declaration lives syntactically inside `--- colors ---` but is a
different beast: it's a per-tile `uint8_t` count, rendered as a scalar-
scaled RGB contribution on top of the agent/tile layer. Agents modify it
with `incr marker` / `decr marker`; it decays on a declared schedule.

```
trail: ramp (0.3, 0.2, 0.0) decay 1/20     # -1 unit every 20 ticks
pile:  ramp (0, 2, 0)       decay 0/1       # no decay
```

- `ramp (r, g, b)` — per-unit coefficients. Rendered contribution at count
  `N` is `(r*N, g*N, b*N)` clamped to `[0, 255]` per channel.
- `decay K/T` — subtract `K` from every non-zero marker cell every `T`
  ticks. `K = 0` disables decay (a permanent pile); `T` must be positive.
- Cap: **4 markers per script** (`MAX_MARKERS`). The runtime stores counts
  as a fixed-width array per tile on device; raising the cap costs 1 B per
  tile per extra slot.
- Markers don't imply `lit`. A tile can carry a non-zero marker count while
  `state` is off.

Marker names go in the same namespace as colors / landmarks / agent
types — the compiler rejects collisions on principle (`if <pile>` would
be ambiguous between a landmark and a marker count).

See `MARKERS_SPEC.md` for the deeper design rationale and
`agents/ants.crit` for a worked example.

#### Night palette (`night:`)

An optional nested block inside `--- colors ---` declares overrides that
apply when the device enters night mode (hardware: Schmitt hysteresis on
smoothed brightness; sim: `--night` flag). Absent entries fall through to
the day palette, so a night block is purely additive.

```
--- colors ---
orange:   (255, 128, 0)
warmwhite:(255, 200, 160)
brick:    (180, 60, 30)
trail:    ramp (0.3, 0.2, 0.0) decay 1/20

night:
  orange:   (0, 255, 0)       # static override
  brick:    warmwhite          # name-ref into the day palette
  default:  (8, 8, 8)          # catch-all for unlisted colors
  trail:    ramp (0, 1, 0)     # per-marker ramp override
```

Inside `night:`:

- Static RGB overrides the matching day color.
- A bare identifier on the right is a reference into the day palette and
  resolves recursively (cycles and name-refs are fine; the resolver
  terminates on day-palette lookups).
- `default:` is a catch-all used when a color has no explicit night entry.
  Without both a specific entry and a `default:`, night mode falls all the
  way through to the day color.
- A cycle is legal on the RHS (`foo: cycle a 2, b 2`). Sub-names resolve
  through the day palette.
- A `ramp (r, g, b)` entry overrides a day marker's per-unit coefficients
  for night mode. **Decay (`K/T`) is *not* overridable** and staying on
  the day declaration is enforced — the decay scheduler is shared across
  modes. This is a v6 addition.

Tiles painted before a day↔night flip re-resolve against the target
palette automatically on the transition (the tile remembers the color
*name* it was painted with). Cycle colors reset phase on the flip —
a one-frame lockstep pulse that's been deemed acceptable for now.

### `--- landmarks ---`

Named point sets drawn as a static background layer. Each has a color name
and an indented list of `(x, y)` coordinates. `max_x` and `max_y` expand to
`width-1` and `height-1`.

```
nest: sand
  (0, max_y)
  (1, max_y)
```

### `--- agents ---`

Declares agent types and their initial placements.

```
up to 8 aphid                         # type + population cap
ladybug: state {starting, running}    # type + named states
ladybug starts at (max_x, 0), state = running   # initial agent
coati starts at (0, 0)                # type is auto-registered if novel
ant starts at nest                    # spawn at a landmark point
```

`starts at <landmark>` picks one point from the landmark's point set at seed
time (deterministic under a fixed RNG seed). Use it to colocate initial
agents with their home without duplicating coordinates.

If an agent type appears only in `starts at` with no `up to` or `state {}`,
it's auto-registered with limit 0 and no named states. `set state = X`
against an undeclared state is a runtime no-op.

### `--- spawning ---`

Spawn rules fire each tick if their condition is true and the agent-type
limit allows more. Only one grammar is recognized:

```
spawn <type> at <landmark> every N ticks when <cond>
```

`<cond>` is one of `missing`, `extra`, `current`, `always`.

### `--- simulation ---`

```
tick rate: 100ms
```

Physics tick period. Render ticks are decoupled (50Hz on hardware).

### `--- pathfinding ---`

A\* options live at two scopes:

- **Top-level** (unindented): only `diagonal` and `diagonal_cost`. Apply
  to every agent unless overridden.
- **Per-agent block** (`agent_name:` header, indented body): any key from
  the table below. The header `all:` is a reserved pseudo-agent — its
  values act as a fallback for any agent that doesn't have its own block,
  so it's the right place for fleet-wide defaults that aren't legal at
  top level (`max nodes`, `step rate`, etc.).

Lookup order, first hit wins: `per_agent[<name>]` → `per_agent["all"]`
→ top-level (for `diagonal` / `diagonal_cost` only) → hardcoded engine
default.

```
all:
  max nodes: 256
  step rate: 1
  diagonal: allowed
  diagonal_cost: 1.414

ladybug:
  step rate: 2
  penalty lit cell: 1.5
  penalty occupied cell: 3
  plan horizon: 8
  drunkenness: 0.2
```

`diagonal: allowed` requires `diagonal_cost`; setting `diagonal_cost`
without `diagonal: allowed` is a compile error. The pair must appear at
the same scope (both top-level or both inside the same per-agent block).

| Key | Form | Range / values | Notes |
|---|---|---|---|
| `max nodes` (alias `max_nodes`) | int | positive | A\* node-expansion budget per seek. Higher = farther reach, more CPU. |
| `step rate` (alias `step_rate`) | int | positive | Ticks between agent moves. `1` = move every physics tick; `2` = every other; etc. |
| `diagonal` | `allowed` \| `disallowed` | — | Whether 8-connected moves are legal. Top-level OR per-agent. |
| `diagonal cost` (alias `diagonal_cost`) | float | positive | A\* cost weight on diagonal steps. Required iff `diagonal: allowed`. Typically `1.414` (≈√2). Top-level OR per-agent. |
| `penalty lit cell` (alias `penalty_lit`) | numeric | ≥ 0 | Extra A\* cost for stepping onto a lit (state=1) tile. Used to discourage walking through the clock face. |
| `penalty occupied cell` (alias `penalty_occupied`) | numeric | ≥ 0 | Extra A\* cost for stepping onto a non-painter agent's cell. Encourages flowing around peers instead of through them. |
| `plan horizon` (alias `plan_horizon`) | int | ≥ 1 | Number of cached A\* steps the agent consumes before replanning. `1` (default) replans every move; higher amortizes A\* across multiple steps. Clamped to `[1, PLAN_MAX]` on-device — values above the cap silently saturate. |
| `drunkenness` | float | `0.0`–`1.0` | Probability of perturbing a planned step. Half the perturbations freeze the agent for one tick; the other half stagger orthogonally to the planned move. A\* itself stays deterministic, so goal convergence is preserved — only the walk is noisy. `0.0` (default) is sober planner-optimal motion; `~0.2` reads as "looks biological"; `1.0` perturbs every tick. Top-level not allowed (per-script personality knob). |

### `--- behavior ---`

One block per agent type, indentation-structured like Python. Each tick, an
agent resumes at its saved PC and runs opcodes until it **yields** (pause,
step, wander, seek-with-timeout, bare despawn) or hits a terminator (`done`
or bare `despawn`).

Every reachable path through an agent's behavior **must** yield before
reaching `done`, or the compiler rejects the script — otherwise an agent
could spin forever in one tick.

## Opcodes

| Opcode                    | Effect                                                   |
|---------------------------|----------------------------------------------------------|
| `if <cond>:`              | See conditions below. `else:` is not supported — put fallback code at the if's indent. |
| `done`                    | End of this tick's execution; PC resets to 0.            |
| `set state = <name>`      | Change named state (declared in `agents` block).         |
| `set color = <name>`      | Rebind agent color; cycles start animating immediately.  |
| `draw [<color>]`          | Light this tile. Color is snapshotted at paint time; the color name is remembered for day↔night re-resolution. |
| `erase`                   | Unlight this tile.                                       |
| `pause [N\|LO-HI]`        | Yield for N ticks (default 1). Range picks uniformly.    |
| `step (dx, dy)`           | Literal relative move. Clamps to grid edges. No guards.  |
| `wander [avoiding current\|any]` | One-step random walk; avoiding clause is optional. |
| `seek <target> [...filters...] [timeout N]` | A\* toward target. See below.              |
| `incr marker <m> [N]`     | Add N (default 1) to this cell's count for marker `<m>`. Clamped to 255. Doesn't yield. |
| `decr marker <m> [N]`     | Subtract N (default 1) from this cell's count for marker `<m>`. Clamped to 0. Doesn't yield. |
| `despawn [agent <name>]`  | Bare: remove self (terminator, yields). With name: remove a colocated other agent; falls through. |

The `incr`/`decr` opcodes accept a sugared bare form — `incr trail` works
when `trail` is a declared marker — the compiler pre-pass rewrites it to
the canonical `incr marker trail`. Same for `decr`.

### `seek`

Two top-level forms: a **target seek** (go to a named landmark / agent
type / tile predicate), and a **gradient seek** (climb or descend a
marker's scalar field).

#### Target seek

```
seek [nearest] [agent|landmark] <name>
     [with state == <stateName>]
     [on <pred> | not on <pred>
      | on marker <m> [op N] | not on marker <m>]
     [timeout N]
```

- `<name>` is a landmark, an agent type, or a tile predicate (`current` /
  `extra` / `missing`). Ambiguity between a landmark and an agent type
  forces you to disambiguate with `agent <name>` / `landmark <name>`.
- `nearest` only matters when the target is multi-cell (e.g. a landmark
  with many points, or an agent type with many instances).
- `with state == <stateName>` restricts candidates to agents currently in
  that named state. Only meaningful when the target is an agent type.
- `on <pred>` / `not on <pred>` filters candidates by tile state. Mostly
  useful with tile predicates: `seek aphid on extra` = go to an aphid
  standing on an unintended lit cell.
- `on marker <m> [op N]` / `not on marker <m>` filters by tile marker
  count (e.g. `seek nearest landmark food on marker trail > 0`). The
  `marker` keyword is required in this clause; the canonicalizer pass
  doesn't rewrite bare names inside the `on` tail.
- `timeout N`: if the seek can't reach its target within N ticks, the
  agent yields and tries again next tick. Without a timeout, a failed
  seek does **not** yield — the script keeps running, which is why solo
  `seek`s don't count as a yield for the path-validator.

#### Gradient seek

```
seek (highest | lowest) marker <m> [op N]
     [on landmark <l>]
     [timeout N]
```

- Walks A\* toward the cell maximizing (`highest`) or minimizing
  (`lowest`) marker `<m>`'s count within scope. Ties break by distance.
- Optional `[op N]` clause (`> N` / `< N`) prunes candidate cells by
  count before selection — `seek highest marker trail > 0` ignores cells
  where no trail has been laid.
- `on landmark <l>` restricts scope to the landmark's point set. Without
  it, scope is the whole grid.
- Gradient seeks only take a `marker` target; the `agent` / `landmark` /
  tile-predicate targets are for the target-seek form above.
- Same `timeout N` semantics as target-seek.

Bare-sugar forms the compiler rewrites to the canonical `marker` keyword
form: `seek highest trail > 0 on landmark food` (when `trail` is a
declared marker) → `seek highest marker trail > 0 on landmark food`.

## Conditions (`if <cond>:`)

| Form                                 | Meaning                                      |
|--------------------------------------|----------------------------------------------|
| `if state == <name>` / `!= <name>`   | Compare agent's state.                       |
| `if random N%`                       | True N% of the time.                         |
| `if on <pred>`                       | Agent's cell matches tile predicate.         |
| `if on landmark <name>`              | Agent stands on any point of that landmark.  |
| `if on agent <name> [with state == <s>]` | Another agent of that type (optionally in state `<s>`) shares the cell. |
| `if <pred>`                          | Board-wide: any cell matches the predicate.  |
| `if agent <name>`                    | Any other agent of that type exists.         |
| `if landmark <name>`                 | Landmark is defined (compile-time constant). |
| `if standing on <pred>`              | Same as `if on <pred>` but only for tile predicates. |
| `if standing on <color>`             | Agent's cell is lit with that color (walks cycle entries for cycle colors). |
| `if standing on landmark <name>`     | Alias for `if on landmark <name>`.           |
| `if marker <m> > N` / `< N`          | Any cell on the board has count in that range. |
| `if marker <m> > N on landmark <l>`  | Any cell of landmark `<l>` has count in that range. |
| `if no marker <m> [on landmark <l>]` | All cells (or all of landmark `<l>`) have count 0. Canonical form of the `== 0` alias. |
| `if on marker <m> > N` / `< N`       | Agent's cell has count in that range.        |
| `if on no marker <m>`                | Agent's cell has count 0.                    |

Tile predicates: `current` (intended AND lit), `missing` (intended AND
unlit), `extra` (unintended AND lit).

**Marker sugar.** When `<m>` is a declared marker, the compiler's
canonicalization pass rewrites these shorthand forms to the canonical
`marker <m>` versions above, so scripts can read more naturally:

- `if <m>` / `if <m> > N` / `if <m> == 0` → board-scope.
- `if <m> [op N] on landmark <l>` → landmark-scope.
- `if on <m>` / `if on <m> > N` / `if not on <m>` / `if not standing on <m>` → self-cell scope.
- `if no <m>` / `if <m> == 0` → zero-alias for `if no marker <m>`.

Only `<` and `>` are supported; `== 0` is aliased as `no`. General `== N`
would require runtime equality that the engine intentionally doesn't
carry.

Bare names after `if on`, `if <name>`, `seek`, and `despawn` are resolved
at compile time: if `<name>` is a landmark it becomes `if on landmark
<name>`; an agent type → `if on agent <name>`; a marker → one of the
marker forms above. A name matching multiple kinds is a compile error —
disambiguate explicitly.

`if on <name>` resolves agent/landmark/marker first, so it never matches
a bare color. Use `if standing on <color>` to test the tile's paint
color.

## Runtime model

- **Tick pipeline**: spawning → each agent runs until yield/terminator →
  conflict resolution (lower ID wins a contested `step`) → convergence
  check.
- **Painters**: any agent type whose behavior contains `draw` or `erase`
  is flagged as a painter. Painters don't count as "occupied" for other
  agents' pathfinding — they *become* the pixel they're on.
- **Cycles vs ticks**: cycle colors advance once per render frame (~50Hz
  on hardware, once per physics tick in the Python sim so traces stay
  diff-able). `pause 1` and `cycle ... 1` mean different things.
- **No floats in logic**: positions, timers, and counters are all ints.
  The only float in the IR is `diagonal_cost`.

## Compile-time validation

The compiler rejects:

- Tab characters anywhere in the source (spaces-only indentation).
- Unknown opcodes (typos in `state = running` miss the leading `set`).
- `else:` — not supported; put fallback code at the if's indent.
- Paths through a behavior that can fall off the bottom without `done`.
- Paths to `done` that never yield.
- Agent names without a color of the same name.
- Landmark name colliding with an agent type, color, or marker.
- Marker name colliding with a color, landmark, or agent type.
- Ambiguous bare names (matching more than one of landmark / agent type / marker).
- `diagonal`/`diagonal_cost` present without the other.
- Cycle colors referencing unknown colors or empty cycle lists.
- Night-palette entries referring to an unknown day color.
- Night-marker entries referring to an unknown day marker, or carrying
  their own `decay K/T` clause (decay is shared with the day entry).
- More than `MAX_MARKERS` (=4) marker declarations.
- `incr` / `decr` naming an undeclared marker.
- `ramp (r, g, b)` components outside `[0, 255]`. Components in `(0, 1)`
  compile with a truncation warning — they round to zero as `uint8_t` on
  device, which almost always means the author meant `1`.

## Versioning

Current: **IR v6**, encoder `ir_encoder/2`. Devices refuse blobs with
`ir_version` above what they support, so bumping `IR_VERSION` is the gate
for any wire-breaking change.

| Version | Added                                                     |
|---------|-----------------------------------------------------------|
| v3      | Baseline of this doc: cycles, landmarks, spawn rules, pathfinding per-agent blocks. |
| v4      | Night palette (`night:` block inside `--- colors ---`).   |
| v5      | Tile markers (`ramp` declarations, `incr`/`decr marker`, marker conditions, gradient seeks). |
| v6      | Per-marker night-mode ramp overrides. Night ramps carry rgb only; decay stays on the day declaration. |

Wire format: body ends at an `END <fletcher16>` line; an optional
`SOURCE <n>` trailer with the original `.crit` text lives after END, so
devices parse up to END and skip the rest. See `OTA_IR.md` for the full
section list.

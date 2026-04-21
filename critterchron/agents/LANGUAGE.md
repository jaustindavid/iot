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

Colors, landmarks, and agent types share a flat namespace with one deliberate
overlap:

- **Agent name == color name is required.** Every agent type must have a
  color of the same name; that's the default body color at spawn. This is
  the *one* overlap the compiler demands.
- **Landmark name == agent name is forbidden.** `if on <name>` /
  `seek <name>` would be ambiguous.
- **Landmark name == color name is forbidden.** `if standing on <name>`
  would be ambiguous.

Pick landmark names that aren't already colors or agents. If a pile-of-thing
agent collides with its tile color (e.g. `brick` tiles and a `brick` agent),
rename one — `bricks` plural for the pile reads naturally.

## Blocks

### `--- colors ---`

Named RGB tuples. Every agent type and every landmark must have a color by
the same name, or compilation fails.

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
or via `set color = <name>` at runtime. Tiles snapshot the current cycle
value at the moment of `draw` — painted tiles don't animate.

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

Top-level and per-agent A\* options. Top-level accepts only `diagonal` and
`diagonal_cost`. Per-agent blocks (indented under `agent_name:` or `all:`)
accept everything:

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
```

Lookup chain: `per_agent[name]` → `per_agent["all"]` → top-level (for
`diagonal`/`diagonal_cost` only) → hardcoded default. `diagonal: allowed`
requires `diagonal_cost`; setting `diagonal_cost` without `diagonal:
allowed` is a compile error.

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
| `draw [<color>]`          | Light this tile. Color is snapshotted at paint time.     |
| `erase`                   | Unlight this tile.                                       |
| `pause [N\|LO-HI]`        | Yield for N ticks (default 1). Range picks uniformly.    |
| `step (dx, dy)`           | Literal relative move. Clamps to grid edges. No guards.  |
| `wander [avoiding current\|any]` | One-step random walk; avoiding clause is optional. |
| `seek <target> [timeout N]` | A\* toward target. See below.                           |
| `despawn [agent <name>]`  | Bare: remove self (terminator, yields). With name: remove a colocated other agent; falls through. |

### `seek`

```
seek [nearest] [agent|landmark] <name> [on <pred>|not on <pred>] [timeout N]
```

- `<name>` is a landmark, an agent type, or a tile predicate (`current` /
  `extra` / `missing`). Ambiguity between a landmark and an agent type
  forces you to disambiguate with `agent <name>` / `landmark <name>`.
- `nearest` only matters when the target is multi-cell (e.g. a landmark
  with many points, or an agent type with many instances).
- `on <pred>` / `not on <pred>` filters candidates by tile state. Mostly
  useful with tile predicates: `seek aphid on extra` = go to an aphid
  standing on an unintended lit cell.
- `timeout N`: if the seek can't reach its target within N ticks, the
  agent yields and tries again next tick. Without a timeout, a failed seek
  does **not** yield — the script keeps running, which is why solo
  `seek`s don't count as a yield for the path-validator.

## Conditions (`if <cond>:`)

| Form                                 | Meaning                                      |
|--------------------------------------|----------------------------------------------|
| `if state == <name>` / `!= <name>`   | Compare agent's state.                       |
| `if random N%`                       | True N% of the time.                         |
| `if on <pred>`                       | Agent's cell matches tile predicate.         |
| `if on landmark <name>`              | Agent stands on any point of that landmark.  |
| `if on agent <name>`                 | Another agent of that type shares the cell.  |
| `if <pred>`                          | Board-wide: any cell matches the predicate.  |
| `if agent <name>`                    | Any other agent of that type exists.         |
| `if landmark <name>`                 | Landmark is defined (compile-time constant). |
| `if standing on <pred>`              | Same as `if on <pred>` but only for tile predicates. |
| `if standing on <color>`             | Agent's cell is lit with that color (walks cycle entries for cycle colors). |
| `if standing on landmark <name>`     | Alias for `if on landmark <name>`.           |

Tile predicates: `current` (intended AND lit), `missing` (intended AND
unlit), `extra` (unintended AND lit).

Bare names after `if on`, `if <name>`, `seek`, and `despawn` are resolved
at compile time: if `<name>` is a landmark it becomes `if on landmark
<name>`, if it's an agent type it becomes `if on agent <name>`. A name
that's both is a compile error — disambiguate explicitly.

`if on <name>` resolves agent/landmark first, so it never matches a bare
color. Use `if standing on <color>` to test the tile's paint color.

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
- Landmark name colliding with an agent type or a color.
- Ambiguous bare names (both a landmark and an agent type).
- `diagonal`/`diagonal_cost` present without the other.
- Cycle colors referencing unknown colors or empty cycle lists.

## Versioning

The compiler emits IR v3 (encoder `ir_encoder/2`). Devices refuse blobs with
`ir_version` above what they support, so bumping IR_VERSION is the gate for
any wire-breaking change. Wire format: body ends at a `END <fletcher16>`
line; an optional `SOURCE <n>` trailer with the original `.crit` text lives
after END, so devices parse up to END and skip the rest.

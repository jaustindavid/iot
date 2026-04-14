# Coati: A Behavior Language for Grid-Based Particle Simulations

## Overview

Coati is a domain-specific language for describing autonomous agent behavior on LED matrix displays. A `.coati` script defines a grid, agents, landmarks, pathfinding rules, and a prioritized behavior tree — all in structured English prose. The toolchain compiles scripts into a compact intermediate representation (IR), which can be run on a host-side terminal simulator or flashed to an embedded device.

The motivating use case is the **coaticlock**: two agents that autonomously rearrange pixels on a WS2812 LED matrix to display the current time. But the language is general enough to describe any grid-based multi-agent simulation — weather displays, message scrollers, game-of-life variants, or purely aesthetic particle effects.

```
┌──────────────┐      ┌──────────┐      ┌────────────────┐
│ .coati file  │─────▶│  Parser  │─────▶│   IR (JSON)    │
│ (English)    │      │ (Python) │      │                │
└──────────────┘      └──────────┘      └───────┬────────┘
                                                │
                          ┌─────────────────────┼─────────────────────┐
                          ▼                     ▼                     ▼
                   ┌──────────────┐    ┌────────────────┐    ┌───────────────┐
                   │  Simulator   │    │  IR → C++ Gen  │    │  IR → JSON    │
                   │  (terminal)  │    │  (codegen)     │    │  (flash to    │
                   │  python -m   │    │  for Particle  │    │   MCU VM)     │
                   │  coati run   │    │  firmware       │    │               │
                   └──────────────┘    └────────────────┘    └───────────────┘
                    iterate fast         deploy to HW          future: runtime
```

---

## 1. The Language

### 1.1 Design Principles

**Readable over writable.** A `.coati` file should read like stage directions for a play. Someone unfamiliar with the codebase should be able to read the script and predict what they'd see on the display. This means:

- No braces, semicolons, or symbolic operators beyond basic math.
- Indentation is significant only for grouping within a behavior block.
- Comments use `##`.
- Keywords are English words: `when`, `if`, `seek`, `place`, `pickup`, `done`.

**Declarative where possible, imperative where necessary.** The grid, layers, landmarks, pathfinding costs, and rendering are all declarative configuration. Agent behavior is imperative (a prioritized rule list) because the charm of the coaticlock comes from the specific choreography — the washing ritual, the dunking bob, the bored wandering — and those are hard to express as constraints.

**Pixel conservation.** The simulation model has an implicit conservation law: pixels are not created or destroyed, only moved. An agent picks up a pixel from one location (an "extra" on the board, or virtually from the dumpster) and places it elsewhere (a "missing" target cell, or back to the dumpster). The dumpster acts as an infinite reservoir — it's where surplus pixels go and where new pixels come from when the target has more lit cells than the board. This model is not enforced by the engine, but it's a core design assumption of the coaticlock scripts. A script that calls `place` without a prior `pickup` will create pixels from nothing, which works but breaks the conservation metaphor.

**Separate concerns.** A script has named sections (`--- grid ---`, `--- behavior ---`, `--- rendering ---`, etc.) that can be independently parsed and validated. Behavior doesn't know about colors. Rendering doesn't know about pathfinding costs. The goal generator doesn't know about agents.

### 1.2 Script Structure

A `.coati` file is divided into sections, each introduced by a `--- name ---` header. Sections can appear in any order. All sections are optional except `grid` and `agents`.

```
--- grid ---          # Dimensions, wiring
--- layers ---        # Named typed 2D arrays
--- landmarks ---     # Special named locations
--- terms ---         # Vocabulary shortcuts (computed grid queries)
--- agents ---        # Agent count, starting positions, properties
--- pathfinding ---   # Algorithm, costs, limits
--- behavior ---      # Prioritized rule list (the core logic)
--- spawning ---      # When to spawn new agents (pool mode only)
--- rendering ---     # State → color mappings, tick rates
--- goal ---          # What drives target changes (clock, weather, etc.)
--- collision ---     # What happens when agents collide
```

### 1.3 Section Reference

#### `--- grid ---`

```
32 wide, 8 tall, zigzag wiring
```

Defines the simulation space. `zigzag wiring` is metadata for the physical LED mapping (alternating row direction on odd columns). The simulator ignores wiring; the device codegen uses it for pixel index calculation.

Supported wiring types: `zigzag`, `linear`, `custom` (with a mapping table).

#### `--- layers ---`

```
current: bool, default off
target: bool, default off
fade: float, default 0.0
```

Named 2D arrays the same size as the grid. Layers are the shared state that agents read and write. The parser validates that behavior rules only reference declared layers.

Supported types: `bool`, `int`, `float`.

#### `--- landmarks ---`

```
dumpster at (0, 7) and (1, 7), color red (64, 0, 0)
pool at (30, 7) and (31, 7), color blue (0, 0, 64)
```

Named sets of grid positions with special meaning. Landmarks are:
- Excluded from term calculations (extras/missing lists).
- Referenceable by name in behavior rules (`seek pool`, `at dumpster`).
- Rendered with their declared color.

Each landmark occupies one or more cells. Agents that reference landmarks by name are assigned a specific cell by index: `agent 0` uses `landmark[0]`, `agent 1` uses `landmark[1]`, wrapping with modulo.

#### `--- terms ---`

```
extras: current is on and target is off
missing: current is off and target is on
```

Named boolean queries over the grid. Terms produce a list of matching cells each tick. They're the vocabulary that behavior rules use to find work. The parser expands term references in behavior rules to the underlying layer comparisons.

Terms support: `is on`, `is off`, `and`, `or`, `not`, and layer references.

#### `--- agents ---`

Agents can be declared in two modes:

**Fixed mode** — a specific number of agents, all active from the start:

```
2 coati agents
coati 0 starts at (8, 4)
coati 1 starts at (24, 4)
```

**Pool mode** — a maximum number of agents that are spawned and despawned dynamically:

```
up to 12 ant agents
spawn at (0, 7)
```

In pool mode, all agents start inactive. The `--- spawning ---` section controls when new agents are activated (see below). Agents can remove themselves from the simulation with the `despawn` action. When an agent despawns, she returns to the inactive pool and can be reactivated by a future spawn.

Both modes support per-agent properties:

```
properties:
  carrying: bool = false
  washed: bool = false
  ...
```

Properties are typed and initialized with defaults.

Built-in properties (always present, don't need declaration):
- `position` — current grid cell
- `path` — current A* path (list of points)
- `claimed` — the cell this agent has claimed (or none)
- `wait_counter` — collision wait ticks

#### `--- pathfinding ---`

```
algorithm: a*
heuristic: euclidean
max nodes: 256
orthogonal cost: 1.0
diagonal cost: 1.414
penalty lit cell: 50
penalty occupied cell: 20
```

Configures the pathfinding engine. `lit cell` means any cell where `current` is on. `occupied cell` means any cell where another agent is standing. These penalties are additive to the base movement cost.

The pathfinding section is pure configuration — the engine provides A* as a built-in. Future extensions could support other algorithms (BFS, Dijkstra, or "direct line" for simple agents).

#### `--- behavior ---`

This is the heart of the language. It's an ordered list of `when` rules. Each tick, the engine evaluates rules top-to-bottom for each idle agent (no path, no pause, no wait). The first matching rule fires.

```
when carrying and not washed:
  if wash_counter > 0:
    decrement wash_counter
    if wash_counter is 0:
      set washed to true
      release pool
      clear claimed
    else:
      bob at pool
    done
  if at pool and pool is free:
    claim pool
    set wash_counter to 5
    step up from pool
    done
  seek pool
```

**Structure and evaluation model:**

`when` blocks are **first-match-wins**. The engine evaluates each `when`
condition top-to-bottom. The first `when` whose condition is true has its body
executed; all subsequent `when` blocks are skipped. There is **no fallthrough**
between `when` blocks — you never need `done` to prevent one `when` from
running into the next.

Within a `when` body, statements execute **sequentially** from top to bottom.
`if`/`else` branches are evaluated in order: the first matching `if` runs its
body, and execution continues with whatever comes after the `if`/`else` chain
(not the next `when` — still the same body).

`done` means **stop processing this agent for this tick**. It exits the entire
behavior evaluation, not just the current `if` block. Use it when an `if`
branch is self-contained and the agent shouldn't fall through to later
statements in the same `when` body:

```
when carrying:
  if at pool:
    set washed to true
    done              ← agent is finished this tick
  if at dumpster:
    discard
    done              ← also finished
  seek pool           ← only reached if neither `if` matched
```

Without those `done` statements, an agent at the pool would set `washed`, then
fall through to `seek pool` (a wasted pathfind). `done` is the early exit.

Summary of keywords:

- `when <condition>:` — top-level rule (first-match-wins across `when` blocks).
- `if <condition>:` / `else:` — nested conditionals within a rule body (sequential).
- `done` — stop evaluating all behavior for this agent this tick (early exit).
- Actions are single-line imperative statements.

**Conditions:**

| Syntax | Meaning |
|---|---|
| `carrying` | Property is truthy |
| `not washed` | Property is falsy |
| `wash_counter > 0` | Numeric comparison |
| `wash_counter is 0` | Equality check |
| `at pool` | Agent position matches any cell in the landmark |
| `not at pool` | Agent position does NOT match any cell in the landmark |
| `at claimed` | Agent position matches its claimed target |
| `pool is free` | No other agent has claimed the landmark's resource lock |
| `standing on extras` | Agent position is in the named term's cell list |
| `cell is lit` | `current` layer is on at agent's position |
| `available extras exist` | At least one unclaimed cell in the term's list |
| `no missing` | The term's cell list is empty |
| `on lit` | Shorthand for `cell is lit` |
| `at any landmark` | Agent is on any declared landmark |
| `near lit` | Any of the 8 neighbors has `current` on |

**Actions:**

| Syntax | Meaning |
|---|---|
| `seek <landmark>` | Pathfind to agent's assigned cell in the landmark |
| `seek nearest available <term>` | Pathfind to closest unclaimed cell matching term, claim it |
| `seek dumpster` | Pathfind to agent's assigned dumpster cell |
| `pickup from board` | Turn off `current` at position, reset `fade`, set carrying |
| `pickup from dumpster` | Set carrying (no board change — dumpster is virtual) |
| `place` | Turn on `current` at position, set `fade` to 0.0 |
| `discard` | Drop carried pixel (no board change) |
| `despawn` | Deactivate agent and return to inactive pool (pool mode only) |
| `set <prop> to <value>` | Assign a property |
| `clear claimed` | Set claimed target to none |
| `pause <n>` | Freeze agent for N ticks |
| `increment <prop>` | Add 1 to a numeric property |
| `decrement <prop>` | Subtract 1 from a numeric property |
| `reset <prop>` | Set numeric property to 0, bool to false |
| `force <prop> to <n>` | Assign a specific numeric value |
| `claim <landmark>` | Acquire exclusive lock on a landmark resource |
| `release <landmark>` | Release the lock |
| `bob at <landmark>` | Alternate between landmark cell and cell above it |
| `step up from <landmark>` | Move one cell up from current position |
| `become bored` | Set `bored` to true |
| `wander avoiding <terms>, chance <n>%, forced if near lit` | Random walk with constraints |

**The `each tick:` block:**

```
each tick:
  for each lit cell in current:
    increase fade by 0.032, cap 1.0
```

Actions under `each tick:` run unconditionally every physics tick, after all agent behavior has been evaluated. This is used for effects that aren't agent-driven — fade animations, decay timers, global state updates. The block is parsed into `BehaviorIR.tick_actions`.

> **Implementation note (v0):** The fade update is currently hardcoded in the engine's `_update_fades()` method rather than interpreted from the IR's tick actions. The `each tick:` block is parsed and stored but not yet executed by the engine.

**The `available` qualifier on terms:**

When a condition references a term with the `available` prefix (e.g., `available extras exist`, `seek nearest available missing`), the engine filters the term's cell list to exclude cells claimed by other agents. Without `available`, the raw unfiltered list is used.

> **Implementation note (v0):** The engine's `term_exists` condition currently *always* checks availability (filters by claims), even when the script says `missing exist` without the `available` qualifier. This is arguably the right default for the coaticlock but is technically a semantic mismatch. If a future script needs to ask "do *any* missing pixels exist regardless of claims?", we'd need a separate `term_any_exist` op or a way to opt out of filtering.

**The `one pathfind per tick` directive:**

```
one pathfind per tick
```

This global constraint means only one agent can compute an A* path per physics tick. It mirrors the MCU's CPU budget. In the original C++, this is the `path_calculated` flag. The simulator respects it for behavioral fidelity; it can be relaxed with `unlimited pathfinds per tick` for faster convergence during testing.

#### `--- rendering ---`

```
physics: 10 hz
display: 60 hz, interpolated

board pixel: green (0, 64, 0) * fade
agent carrying not washed: yellow (64, 64, 0) pulse 150ms
agent idle index 0: white (64, 64, 64)
```

Rendering rules map simulation state to visual output. Each rule has a selector (what it matches) and a color expression.

**Color expressions:**

- `(r, g, b)` — static color
- `* fade` — multiply by a layer value
- `pulse <period>` — sinusoidal brightness oscillation: `0.7 + 0.3 * sin(t / period)`
- `shimmer <period>` — hard on/off toggle: `sin(t / period) > 0`
- `solid` — no modulation

**Selectors:**

Selectors match by state and optionally by agent index. More specific selectors win (e.g., `agent bored index 0` overrides `agent idle index 0`). The evaluation order matches the coaticlock's C++ render logic.

In the terminal simulator, colors are mapped to ANSI escape codes or Unicode block characters. On the device, they drive NeoPixel RGB values.

#### `--- goal ---`

```
type: clock
font: megafont 5x6
layout: horizontal
time: wobbly 120s to 300s, fast 1.3x, slow 0.7x
```

Defines what generates target patterns. The goal is external to the agent behavior — agents react to target changes, they don't cause them.

**Goal types:**

- `clock` — renders time digits into the target layer. Supports `wobbly` time (virtual offset with asymmetric tick rates) or `real` time.
- `static` — loads a fixed bitmap (for testing).
- `sequence` — cycles through a list of bitmaps on a timer.
- Future: `http`, `serial`, `random`.

The goal's `font` references a bitmap font definition. `megafont 5x6` is the built-in coaticlock font (hardcoded bitmasks for digits 0-9 and colon). The `layout` controls glyph placement: `horizontal` for 32x8, `grid` for 16x16.

#### `--- spawning ---`

Only used with pool mode agents (`up to N`).

```
spawn every 10 ticks when missing exist
```

Each tick, a spawn timer increments. When it reaches the `every` threshold, the timer resets and the `when` condition is evaluated. If true (or if no condition is given), one inactive agent is activated at the `spawn at` position declared in the agents section. If all agent slots are active, the spawn is silently skipped.

The condition is evaluated globally, not per-agent — it uses the same condition syntax as behavior rules but doesn't reference any specific agent's properties. In practice this means it works with term-based conditions (`missing exist`, `extras exist`) but not with agent property checks (`carrying`, `at landmark`).

#### `--- collision ---`

```
on collision:
  increment wait_counter
  if wait_counter > 3 + index * 2:
    clear path
    clear claimed
    reset wait_counter
```

Defines what happens when an agent's next path step is occupied by another agent. This is separated from behavior because it fires during the movement phase, before behavior evaluation.

`index` refers to the agent's numeric index. The expression `3 + index * 2` means agent 0 waits up to 3 ticks, agent 1 waits up to 5.

---

## 2. Intermediate Representation (IR)

The parser produces a JSON IR that the simulator and (future) device runtime consume. The IR is a direct, unambiguous encoding of the script — no English, no ambiguity, no comments.

### 2.1 Top-Level Structure

```json
{
  "grid": { "width": 32, "height": 8, "wiring": "zigzag" },
  "layers": [
    { "name": "current", "type": "bool", "default": false },
    { "name": "target", "type": "bool", "default": false },
    { "name": "fade", "type": "float", "default": 0.0 }
  ],
  "landmarks": [
    {
      "name": "dumpster",
      "cells": [[0, 7], [1, 7]],
      "color": [64, 0, 0]
    },
    {
      "name": "pool",
      "cells": [[30, 7], [31, 7]],
      "color": [0, 0, 64]
    }
  ],
  "terms": [
    { "name": "extras", "expr": { "op": "and", "args": [{"op": "eq", "layer": "current", "value": true}, {"op": "eq", "layer": "target", "value": false}] }},
    { "name": "missing", "expr": { "op": "and", "args": [{"op": "eq", "layer": "current", "value": false}, {"op": "eq", "layer": "target", "value": true}] }}
  ],
  "agents": {
    "type_name": "coati",
    "count": 2,
    "starts": [[8, 4], [24, 4]],
    "properties": [
      { "name": "carrying", "type": "bool", "default": false },
      { "name": "washed", "type": "bool", "default": false },
      { "name": "bored", "type": "bool", "default": false },
      { "name": "wash_counter", "type": "int", "default": 0 },
      { "name": "pause_counter", "type": "int", "default": 0 },
      { "name": "bored_counter", "type": "int", "default": 0 }
    ]
  },
  "pathfinding": {
    "algorithm": "astar",
    "heuristic": "euclidean",
    "max_nodes": 256,
    "orthogonal_cost": 1.0,
    "diagonal_cost": 1.414,
    "penalties": [
      { "condition": "lit", "cost": 50 },
      { "condition": "occupied", "cost": 20 }
    ]
  },
  "behavior": {
    "pathfind_limit": 1,
    "rules": [ ... ]
  },
  "rendering": { ... },
  "goal": { ... },
  "collision": { ... }
}
```

### 2.2 Behavior Rule IR

Each rule is a tree of condition → action nodes:

```json
{
  "when": { "op": "and", "args": [
    { "op": "prop_true", "prop": "carrying" },
    { "op": "prop_false", "prop": "washed" }
  ]},
  "body": [
    {
      "if": { "op": "gt", "prop": "wash_counter", "value": 0 },
      "then": [
        { "action": "decrement", "prop": "wash_counter" },
        {
          "if": { "op": "eq", "prop": "wash_counter", "value": 0 },
          "then": [
            { "action": "set", "prop": "washed", "value": true },
            { "action": "release", "landmark": "pool" },
            { "action": "clear_claimed" }
          ],
          "else": [
            { "action": "bob", "landmark": "pool" }
          ]
        },
        { "action": "done" }
      ]
    },
    {
      "if": { "op": "and", "args": [
        { "op": "at_landmark", "name": "pool" },
        { "op": "landmark_free", "name": "pool" }
      ]},
      "then": [
        { "action": "claim", "landmark": "pool" },
        { "action": "set", "prop": "wash_counter", "value": 5 },
        { "action": "step_up", "landmark": "pool" },
        { "action": "done" }
      ]
    },
    { "action": "seek", "landmark": "pool" }
  ]
}
```

This is a straightforward AST. No control flow beyond `if/else` and `done`. The engine walks the tree top-to-bottom, evaluating conditions and executing actions.

---

## 3. The Parser

### 3.1 Architecture

The parser is a two-pass Python module:

**Pass 1: Section splitting.** Scan for `--- name ---` headers and split the file into named text blocks. Strip comments (`##`) and blank lines.

**Pass 2: Section-specific parsing.** Each section has its own parser function that reads its text block and produces the corresponding IR fragment.

- `parse_grid(text)` → `{ width, height, wiring }`
- `parse_layers(text)` → `[{ name, type, default }]`
- `parse_landmarks(text)` → `[{ name, cells, color }]`
- `parse_terms(text)` → `[{ name, expr }]`
- `parse_agents(text)` → `{ type_name, count, starts, properties }`
- `parse_pathfinding(text)` → `{ algorithm, heuristic, ... }`
- `parse_behavior(text)` → `{ pathfind_limit, rules }`
- `parse_rendering(text)` → `{ physics_hz, display_hz, rules }`
- `parse_goal(text)` → `{ type, font, layout, time_config }`
- `parse_collision(text)` → `{ rules }`

### 3.2 Behavior Parsing

The behavior section is the most complex. It's parsed with a recursive descent approach:

1. **Tokenize** lines by indentation level (2-space indent = one nesting level).
2. **Top-level `when` blocks** are split at zero-indent `when` lines.
3. Within each `when` block, **`if`/`else`/`done` structures** are parsed recursively.
4. **Action lines** are pattern-matched against known action templates.
5. **Conditions** are parsed with a small expression parser supporting `and`, `or`, `not`, comparisons (`>`, `<`, `is`, `>=`), and property/term/landmark references.

The parser does **not** try to understand arbitrary English. It recognizes a fixed vocabulary of keywords and patterns. If a line doesn't match any known pattern, it's a parse error with a line number and suggestion.

> **Implementation note (v0):** Unrecognized action lines are currently silently dropped (`_parse_action` returns `None`). This should become a hard error with suggestions — see Section 3.4.

### 3.3 Validation

> **Status:** Not yet implemented. The parser currently accepts invalid references without error. This is the highest-priority gap — a script that references a nonexistent landmark or property will fail silently at runtime instead of at parse time.

After parsing, the IR should be validated:

- All property references in behavior rules exist in the agent property list.
- All term references exist in the terms section.
- All landmark references exist in the landmarks section.
- Layer references in terms are valid layer names.
- Landmark cells are within grid bounds.
- Agent start positions are within grid bounds and not on landmarks.
- Pathfinding costs are non-negative.
- Rendering selectors don't reference undefined agent states.

Errors should be reported with the originating section name and line number.

### 3.4 Error Messages

> **Status:** Not yet implemented. The parser currently raises generic `ParseError` exceptions. Unrecognized action lines are silently dropped.

Parser errors should be helpful, not cryptic:

```
coaticlock.coati:47: unknown action "grab pixel"
  did you mean: "pickup from board"?

coaticlock.coati:23: unknown property "washed_up" in condition
  known properties: carrying, washed, bored, wash_counter, pause_counter, bored_counter

coaticlock.coati:12: landmark "trash" not defined
  known landmarks: dumpster, pool
```

### 3.5 Known Parser Pitfalls

Issues discovered during implementation that need attention:

**Keyword disambiguation.** The condition parser matches patterns in order. If a general pattern appears before a specific one, the specific case is silently misclassified. Example: `at claimed` was matched by `at <landmark>` (producing `at_landmark("claimed")`) before the special-case `at_claimed` check. **Rule: specific patterns must always precede general ones.** This is easy to get wrong when adding new keywords. Consider a registry/dispatch approach instead of ordered if-chains.

**Naive `and`/`or` splitting.** Conditions are split on `" and "` and `" or "` as literal substrings. This works for current conditions but will break on any future multi-word phrase containing "and" (e.g., `"standing on supply and demand"`). A proper tokenizer or parenthesized grouping would fix this, but hasn't been needed yet.

**`pickup` sets `carrying` implicitly.** The engine's `pickup` action automatically sets `carrying = true`. The scripts also do `set carrying to true` after `pickup from board`, making the explicit set redundant. We should decide: either `pickup` is a composite action (doc and enforce that it sets carrying, remove redundant script lines) or it's a primitive (remove the implicit set from the engine, require scripts to do it). Current recommendation: keep it composite, but the doc must be clear about what side effects each action has.

---

## 4. The Simulator

### 4.1 Purpose

The simulator lets you iterate on `.coati` scripts without flashing hardware. It runs the same IR that the device would run, at the same tick rate, with an ASCII or Unicode terminal display.

### 4.2 Display Modes

**ASCII mode** (default, works in any terminal):

```
tick 47  |  12:34  |  agents: [C0 carrying→wash] [C1 seeking extra]

. . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . .
. . . . . # . . . . # # . . . . . # . . . . . # # # . . . . . .
. . . . # . # . . . . # . . . . # . . . . . . . . # . . . . . .
. . . . . . # . . . . # . 0 . . # # . . . . . # # . . . . . . .
. . . . # # . . . . . # . . . . . . # . . . # . . . . . . . . .
. . . . # . # . . . . # . . . . . . # . . . # . . . . . . . . .
. . . . . # . . . . # # # . . . # # . . . . . # # # . 1 . . . .
D D . . . . . . . . . . . . . . . . . . . . . . . . . . . . P P
```

Where:
- `.` = empty cell
- `#` = lit pixel (current layer on), intensity shown by character: `·` `░` `▒` `▓` `█` (tracks fade)
- `0`, `1` = agent positions (colored by state with ANSI codes)
- `D` = dumpster, `P` = pool
- Target-only cells shown as dim `○` (missing pixels agents need to place)

**Unicode block mode** (for terminals with color support):

> **Status:** Not yet implemented. Only ANSI color mode is built.

Uses half-block characters (`▀`, `▄`, `█`) and 24-bit ANSI color to render at 2 cells per grid row, closely approximating the actual LED display colors.

### 4.3 Interaction

The simulator runs in one of three modes:

**`run`** — Full speed (10 ticks/sec physics). The terminal display updates in place using ANSI cursor control. Press `q` to quit, `space` to pause/resume, `+`/`-` to speed up or slow down.

**`step`** — One tick per keypress. Shows full state dump after each tick: agent positions, properties, paths, claimed targets, extras/missing counts.

**`fast`** — No rendering, runs as fast as possible. Useful for testing convergence: "how many ticks until `current == target`?" Prints summary stats at the end.

### 4.4 Engine Architecture

```python
class CoatiEngine:
    def __init__(self, ir: CoatiIR):
        self.ir = ir
        self.grid_w = ir.grid.width
        self.grid_h = ir.grid.height
        self.layers = {l.name: 2D_array(l.default) for l in ir.layers}
        self.landmarks = {lm.name: lm.cells for lm in ir.landmarks}
        self.agents = [Agent(i, ir.agents) for i in range(ir.agents.count)]
        self.landmark_locks = {lm.name: -1 for lm in ir.landmarks}
        self.tick_count = 0

    def tick(self):
        # 1. Update goal (may change target layer)
        self._update_goal()

        # 2. Evaluate terms (recompute cell lists from layer state)
        self._compute_terms()

        # 3. Movement phase (with collision handling)
        for agent in self.agents:
            if agent.path:
                # check next step for collision, apply collision rules
                ...

        # 4. Behavior phase
        pathfinds_used = 0
        for agent in self.agents:
            if agent.path or agent.wait_counter > 0:
                continue
            # walk behavior rules top-to-bottom, first match fires
            for rule in self.ir.behavior.rules:
                if self._eval_condition(rule.condition, agent):
                    done, used_pathfind = self._exec_body(rule.body, agent)
                    if used_pathfind:
                        pathfinds_used += 1
                    break
            if pathfinds_used >= self.ir.behavior.pathfind_limit:
                break

        # 5. Fade updates
        self._update_fades()
        self.tick_count += 1
```

The key design choice: the engine is **IR-driven, not hardcoded**. The behavior tree walker reads the IR rule list and evaluates conditions/actions generically. There's no coaticlock-specific code in the engine — it's all in the `.coati` script.

> **Implementation note (v0):** The rendering rules from the IR are parsed and stored but not yet consumed by the simulator. Agent colors are currently hardcoded in `simulator.py`'s `_agent_color()` function based on property state, rather than being driven by the `--- rendering ---` section. This means changing colors in the `.coati` script won't affect the simulator display yet.

### 4.5 Pathfinding

The A* implementation is a direct port of the C++ version:

```python
class AStarPathfinder:
    def find_path(self, start, dest, agent_index, agents, layers, config):
        # Priority queue with (f_cost, point)
        # g_cost = movement cost so far
        # h_cost = heuristic (euclidean distance to dest)
        # Penalties applied per config: lit cells, occupied cells
        # Max nodes explored = config.max_nodes
        # Fallback: if dest unreachable, return path to closest explored node
```

The pathfinder is parameterized entirely by the IR's `pathfinding` section. Different scripts can use different cost functions without changing the engine.

### 4.6 Goal: Clock with Wobbly Time

For the coaticlock, the goal generator manages a virtual clock:

```python
class WobblyClock:
    def __init__(self, min_offset, max_offset, fast_rate, slow_rate):
        self.target_offset = random.randint(min_offset, max_offset)
        self.virtual_time = time.time() + self.target_offset
        # ...

    def advance(self, real_now):
        current_offset = self.virtual_time - real_now
        rate = self.fast_rate if current_offset < self.target_offset else self.slow_rate
        self.virtual_time += (elapsed * rate)
        # Re-pick target when within 5 seconds
        # Re-seed if drift > 2x max
```

The font renderer uses the same 5x6 bitmasks from the C++ source, encoding digits 0-9 and colon as byte arrays.

---

## 5. Device Runtime (Future)

### 5.1 Two Approaches

**Option A: C++ Codegen.** The parser emits a `.cpp` file that implements the behavior tree as a series of `if/else` chains — essentially regenerating `CoatiEngine.cpp` from the IR. This is the simplest approach and produces identical performance to the handwritten code.

**Option B: Embedded VM.** Flash a generic interpreter that reads IR (msgpack-encoded) from flash storage or receives it over-the-air via the Stra2us backend. The VM walks the behavior tree each tick, looking up conditions and dispatching actions. This is more flexible (update behavior without reflashing) but has higher per-tick overhead.

### 5.2 Recommended Path

Start with **Option A** (codegen). The IR-to-C++ translation is mechanical, the output is debuggable, and it works with the existing Particle build toolchain. Option B becomes worth it when we want OTA behavior updates — at that point the VM is a ~2KB addition to the firmware and the IR msgpack payload is under 1KB.

### 5.3 Shared Components

Regardless of approach, the device firmware keeps its existing:
- NeoPixel driver with SPI/DMA
- Stra2us telemetry client
- Light sensor + brightness curve
- WobblyTime (or the codegen can emit an equivalent)

The behavior engine is the only thing that changes. Grid dimensions, landmark positions, rendering colors, pathfinding costs — all come from the IR instead of being hardcoded.

---

## 6. Testing Strategy

### 6.1 Parser Tests

- Round-trip: parse a `.coati` file → IR → re-emit a normalized `.coati` file → parse again → IR should be identical.
- Error cases: undefined properties, out-of-bounds landmarks, type mismatches.
- Edge cases: empty behavior section, zero agents, single-cell grid.

### 6.2 Simulator Tests

- **Convergence test:** Load a static target, run in `fast` mode, assert `current == target` within N ticks.
- **Determinism test:** Run the same script with the same random seed twice, assert identical tick-by-tick state.
- **Behavior parity test:** Run the Python simulator and the C++ engine side-by-side with the same initial state and random seed. Compare agent positions and board state at each tick. This validates that the simulator faithfully reproduces the device behavior.

### 6.3 Scenario Tests

Specific scenarios extracted from the coaticlock:

- **Minute change:** Set target to "12:34", let agents converge. Change target to "12:35" (only last digit changes). Assert only the changed pixels are moved.
- **Pool contention:** Both agents carrying at the same time. Assert only one washes at a time; the other waits.
- **Dumpster retrieval:** Target has more pixels than current. Assert agents visit the dumpster to "create" new pixels.
- **Bored wandering:** No work to do. Assert agents wander without stepping on lit pixels or landmarks.
- **Collision resolution:** Two agents pathing toward each other. Assert one gives up after its timeout.

---

## 7. Lessons from v0 Implementation

These issues were discovered while building the parser, engine, and simulator. They inform what to fix next and what to watch for as the language evolves.

### 7.1 The Parser Needs a Formal Grammar

The current parser uses ad-hoc regex matching and string splitting. This works for the coaticlock vocabulary but is fragile:

- **Keyword priority bugs** are easy to introduce. The `at claimed` vs `at <landmark>` ordering bug was caught in testing, but similar issues will recur as new conditions are added. A dispatch table keyed on the first word (or a proper PEG/recursive-descent tokenizer) would prevent this class of bug.
- **`and`/`or` splitting** is naive string splitting, not real boolean expression parsing. Any future condition containing the substring " and " will break. A proper expression parser with precedence would be more robust.
- **Unrecognized lines are silently dropped.** If `_parse_action()` returns `None`, the line vanishes. This made debugging harder — a typo in a script produces no error, just missing behavior.

### 7.2 The Script and Engine Have Redundant Semantics

The `pickup` action in the engine implicitly sets `carrying = true`, but both scripts also explicitly `set carrying to true` afterward. This redundancy isn't harmful but signals a design tension: should actions be high-level composites with side effects, or low-level primitives that scripts compose manually?

Recommendation: define a clear "action contract" for each action — document exactly what state it modifies — and enforce it in one place. Either `pickup` is a composite that sets carrying (and scripts shouldn't repeat it), or it's a primitive that only touches the board (and scripts must set carrying).

### 7.3 Rendering Is Not Yet IR-Driven

The simulator's `_agent_color()` function hardcodes the color logic (yellow when carrying, cyan when bored, etc.) rather than reading from the parsed `--- rendering ---` rules. This means editing the rendering section of a `.coati` script has no effect on the simulator. Closing this gap requires:

1. A selector matching system that evaluates render rules against agent state.
2. Priority/specificity logic (more specific selectors override general ones).
3. Color modulation evaluation (pulse, shimmer, fade multiply).

### 7.4 The `each tick:` Block Is Parsed But Not Executed

The fade update logic is hardcoded in `engine.py`'s `_update_fades()` rather than interpreted from the IR's `tick_actions`. The `each tick:` block is parsed and stored in `BehaviorIR.tick_actions` but the engine ignores it. For now this is fine — fade updates are the only use case — but it should be wired up before the language supports other per-tick effects.

### 7.5 Terminal Raw Mode Breaks Output Newlines

The live simulator (`run` mode) needs non-blocking single-keypress input for controls (`q`, `space`, `+`/`-`). The initial implementation used `tty.setraw()`, which disables *all* terminal processing — including the kernel's automatic `\n` → `\r\n` output translation. This caused every `\n` in the rendered frame to move the cursor down without returning to column 0, producing a staircase effect where each grid row was offset further to the right.

**Fix:** Use `tty.setcbreak()` instead of `tty.setraw()`. Cbreak mode gives us single-character input without echo (same keypress behavior) but preserves the terminal's output post-processing, so `\n` works normally. The only difference from raw mode is that cbreak still interprets a few input signals (like Ctrl-C for SIGINT), which is actually desirable — it means the simulator can be killed even if the input loop hangs.

**Rule of thumb:** Only use `setraw()` when you need full control of both input *and* output byte streams (e.g., implementing a terminal emulator). For TUI applications that just need non-blocking keypress detection, `setcbreak()` is almost always the right choice.

### 7.6 Simulator Must Read Physics Hz from IR

The simulator's `run_live()` initially hardcoded `tick_interval = 0.1 / speed` instead of reading `ir.rendering.physics_hz`. Changing the `--- rendering ---` section's physics rate had no effect. Fixed to compute `base_interval = 1.0 / ir.rendering.physics_hz` so scripts can control their own simulation speed.

### 7.7 Agent Evaluation Order Matters

Because of the `one pathfind per tick` constraint, agent 0 always gets first priority for pathfinding. If both agents need new paths on the same tick, agent 1 must wait until the next tick. This matches the C++ behavior but can cause asymmetric convergence. The design doc should be explicit that **agent evaluation order is index order** and that the pathfind budget is shared.

---

## 8. Open Questions (Unchanged from Pre-Implementation)

**How far should the English go?** The current language is a curated vocabulary, not natural language processing. But the line between "structured English" and "real parser keywords" is blurry. Should we support `pick up the pixel` as well as `pickup from board`? Aliases increase learnability but complicate parsing. Current stance: no aliases in v1, maybe add a synonym table later.

**Should the IR be the distribution format?** For OTA updates, shipping IR (as msgpack) to the device is elegant. But it means the IR becomes a stable API — changes to the IR format would break deployed devices. Alternative: always ship via codegen, treat IR as an internal compiler artifact.

**Multi-grid support?** Some installations use multiple panels (e.g., two 16x16 panels side by side acting as one 32x16). Should the language support multi-grid layouts natively, or is that a deployment concern below the language level?

**Agent-to-agent communication?** The current coaticlock agents coordinate implicitly through the `claimed` mechanism and the `pool_user` lock. Should the language support explicit messaging between agents, or is implicit coordination through shared state sufficient?

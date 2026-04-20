# CritterChron

Agent-based pixel clock on a 16×16 or 32×8 LED grid. Agents (defined in `.crit` files) collectively converge the grid's state to the current wall-clock time, rendered as `HH` over `MM` (square grids) or `HH MM` (wide grids) via a 5×6 font.

Runs in two places: the Python sim (gold master, pygame-visualized) and on a Particle Photon 2 via the C++ HAL in `hal/`. See `HAL_SPEC.md` for the hardware architecture and `handoff.md` for current status. `handoff.txt` is the original project vision.

## Running

```bash
python3 main.py agents/ants.crit                  # visual
python3 main.py agents/ants.crit --headless       # no pygame, just health report
python3 main.py agents/ants.crit --ticks 100      # stop after N ticks
python3 main.py agents/ants.crit --tick-rate 50   # override ms/tick (ignores .crit value)
```

## Grid & tile semantics

Each tile has two booleans:

- `intended` — set by the font oracle (`sync_time`). True if this pixel should be lit to display the current time.
- `state` — whether the tile is currently painted (by a painter agent). Starts False.

Derived from the pair:

- **current**: `intended and state` — tile is painted where it should be.
- **missing**: `intended and not state` — tile should be painted but isn't.
- **extra**: `not intended and state` — tile is painted where it shouldn't be (typically after the minute rolls over).

**Principle: missing/extra are pure tile semantics.** Agent occupancy does *not* contribute to the effective state of a tile for missing/extra checks. A non-painter agent standing on an unintended tile does not make that tile "extra". (The convergence metric in `tick()` still uses effective state — it's a display-correctness signal, separate from missing/extra logic.)

## Agent types

Two implicit categories based on whether the behavior block contains `draw` or `erase`:

- **Painter** (has draw/erase): contributes to `tile.state` via its primitives. Excluded from `_occupied_positions` so painters don't collide-block each other at the tile level.
- **Non-painter** (no draw/erase): physical presence only. Counts in `_occupied_positions` and in A*'s `penalty_occupied`.

Non-painters do not create phantom extras/missings. The aphid (painter) and the ladybug (non-painter predator) in `thyme.crit` are the canonical examples.

## Behavior DSL

Sections in a `.crit` file:

- `--- colors ---` — named RGB tuples.
- `--- landmarks ---` — named point sets on the grid. `max_x` / `max_y` are resolved at parse time.
- `--- agents ---` — `up to N <name>` (population cap) or `<name> starts at (x,y)` (one-off initial placement). Each agent type must have a matching entry in colors.
- `--- spawning ---` — `spawn <agent> at <landmark> every N ticks when <missing|extra|always>`.
- `--- simulation ---` — `tick rate: Nms`.
- `--- pathfinding ---` — per-agent-type config: `max nodes`, `penalty lit cell`, `penalty occupied cell`, `step rate`, `diagonal: allowed|disallowed`, `diagonal_cost: <float>`. Fields are honored by both the sim and HAL A*. Diagonal is opt-in script-wide (parse error if `diagonal` and `diagonal_cost` aren't both set or both absent).
- `--- behavior ---` — indentation-sensitive blocks, one per agent name.

### Behavior primitives

- `pause [N|LOW-HIGH]` — yield for N ticks (default 1). Writes `remaining_ticks`.
- `wander [avoiding current|avoiding any]` — step to a random neighbor (possibly filtered). Unknown modifiers (e.g. `prefering current`) are silently ignored and behave as plain `wander`.
- `step (dx, dy)` — literal relative move to `(pos + dx, pos + dy)`, clamped to grid bounds. No collision check — caller owns the fragility. Yields for `step_rate` ticks.
- `draw <color>` / `erase` — set/clear `tile.state` at the agent's position. Marks the agent as a painter.
- `despawn` — remove self.
- `despawn <agent_name>` — remove a co-located agent of that type. Self continues.
- `seek ...` — see grammar below.

### `if` conditions

Indentation defines blocks. `if ... :` on True advances one instruction; on False, skips to the next instruction at or below its indent. `else:` is handled by skipping its block when reached naturally (after a True branch).

Conditions:

- `if on current:` / `if standing on current:` — tile under me is intended+painted.
- `if on extra:` / `if standing on extra:` — painted where not intended.
- `if on missing:` / `if standing on missing:` — intended but not painted.
- `if on <landmark>:` — any landmark name declared in the `--- landmarks ---` block. Resolved at parse time to `if on landmark <name>:` (explicit form also accepted).
- `if on <agent_name>:` — another agent of that type shares my tile. Resolved at parse time to `if on agent <name>:`.
- `if missing:` / `if extras:` — a missing/extra tile exists anywhere on the grid.
- `if random N%:` — probabilistic.
- `if state == <value>:` — agent-local state string.

Every block (including nested `if`s) must terminate in a `done` that's statically reachable — the compiler rejects scripts that can fall off the bottom without yielding.

### `seek` grammar

```
seek [nearest] <target> [(on | not on) <kind>] [timeout N]
```

- `<target>` is `missing`, `extra`, `current`, a landmark name, or an agent name.
- `<kind>` (filter) is `current`, `extra`, `missing`, or a landmark name. Filters the candidate tiles by `_tile_matches(pos, kind)`. The filter is parsed and applied uniformly to any target; combinations that are semantically useless (e.g. `seek missing on current`) are the author's problem, not the parser's.
- `timeout N` gives up after N consecutive ticks on this seek and advances past the instruction. Without a timeout, a seek with an unreachable target spins forever by design — explicit opt-in only.
- `nearest` is accepted as a noise word for readability; seek always picks the nearest candidate.

Seek stays on its own instruction across ticks: it sets `agent.pc = pc` and `remaining_ticks = 1` after each step. On arrival (target reached), it falls through to `pc += 1` within the same tick, so the following instruction runs immediately.

## The claim mechanic

When an agent begins seeking a missing/extra tile, it sets `claimant_id` on the target tile for that tick. Other agents' missing/extra candidate scans skip claimed tiles. Claims are cleared at the start of every tick.

**Claims are only set for missing/extra targets**, not for landmark or agent targets. Rationale: the claim was designed to prevent multiple painters dogpiling the same missing tile. Claiming an agent's position poisoned that tile for any nearby painter trying to target the same location as a missing tile (the painter would skip its own standing tile and bounce instead of painting). Predator chasing prey doesn't need this kind of coordination — multiple predators chasing the same target is fine.

## Agent execution flow per tick

1. Clear all claims.
2. Spawning: each spawn rule fires if `tick_count % interval == 0` and condition holds.
3. For each agent (in list order — lower ids first after the initial sort):
   - If `remaining_ticks > 0`, decrement and possibly skip.
   - Else run `_process_agent_behavior` from `agent.pc` until an instruction yields (`pause`/`wander`/`seek` move/`done`) or the 100-instruction safety cap trips (sets `glitched`, agent enters rainbow state).
4. Sort agents by id (deterministic conflict resolution — lower id wins).
5. Convergence check: compare `_effective_state` to `intended` across the grid.

## Gotchas / limitations

- **Convergence metric uses `_effective_state`** (includes agent occupancy). Inconsistent with the pure-tile semantics elsewhere but kept deliberately — it's a display-correctness signal, and ants historically count as display pixels when they squat.
- **No seek radius.** Seek scans the whole grid. Fine for 16×16 and 32×8.
- **`timeout` is not magic.** No implicit default; seek without a timeout is an infinite commitment.
- **`penalty_occupied` counts all agents**, including painters. Painters don't block each other physically but they do raise path cost.
- **`ladybug starts at (0,0), red`** — the trailing `, red` isn't parsed (the regex expects `state = <value>`). Color comes from the colors block lookup by agent name, so this tends to work by accident.
- **`step_contests` metric is never incremented.** Dead field in the health report.
- **Unknown `wander` modifiers are silent no-ops** (e.g. `wander prefering current` behaves as plain `wander`).
- **`step (dx, dy)` is unguarded.** Clamps to grid bounds but doesn't check occupancy — the caller (e.g. coati bobbing at the pool) owns the fragility. See `TODO.md`.

## Example agents

- `agents/ants.crit` — painters that squat on missing tiles to light them. After minute rollover, the tile they painted becomes extra; they detect this via `if missing:` globally, erase, and seek a new missing. When no missings remain, they seek the nest and despawn.
- `agents/thyme.crit` — predator/prey. Aphids paint clock digits green and pause 100 ticks on current. Ladybugs seek `aphid not on current` so they only eat idle/traveling aphids, not ones actively maintaining the display. On same-tile encounter the ladybug uses `despawn aphid` to remove the prey.
- `agents/tortoise.crit` — single slow painter that seeks extras and missings sequentially.
- `agents/coati.crit` — (see file).

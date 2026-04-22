# Tile Markers — Design Spec

Status: design. Not implemented.

Scope: add a **scalar-field layer** to the tile grid, orthogonal to the
existing `lit` / `intended` bits. Each declared marker is a named
`uint8_t` field per cell. Agents `incr` / `decr` it. It renders via a
special `ramp` color whose RGB scales linearly with count. It decays
eagerly on a declared schedule. Predicates extend to ask about markers
the same way they ask about tile state today.

This is the hook for pheromone trails, stackable piles, wear maps,
resource nodes — any script where agents coordinate via environmental
state that *accumulates*, not just lit/unlit.

---

## 1. Motivation

Today's tile state is binary (`lit`) plus a compile-time `intended`
mask. That's enough for clock-painting and picture-drawing, which is
what CritterChron was built for. It's not enough for:

- **Pheromone trails** (ants classic) — "follow the strongest signal."
- **Stackable piles** — brick pile with height, not just presence.
- **Wear / heat maps** — how often is this cell traversed?
- **Resource nodes** — forest with count 5, decremented by harvest.

Workarounds today smuggle state through agent count, landmark redefs,
or parallel "tracker" agents. All ugly. A scalar field per tile is the
right primitive.

Decision from discussion: **orthogonal** (not a replacement for
`lit`/`draw`), **ramp color** (RGB is a float multiple of count), and
**eager decay** (per-tick sweep, not lazy read-time).

---

## 2. Grammar additions

### 2.1 Marker declaration (in `--- colors ---`)

A marker looks like a color with a new `ramp` kind:

```
trail: ramp (0.3, 0.2, 0.0) decay 1/20
pile:  ramp (0.0, 2.0, 0.0) decay 0/1        # no decay
```

- `ramp (r, g, b)` — floats. Rendered RGB at count `N` is
  `(r*N, g*N, b*N)` clamped to `[0, 255]` per channel. Floats in the
  source, fixed-point Q8 or Q16 on device (TBD — see §8).
- `decay K/T` — decrement every cell's count by `K` every `T` physics
  ticks. `0/anything` = no decay. `1/1` = fastest (drain one unit
  per tick).

Markers share the name space with colors / landmarks / agents per
existing rules (no collisions). A marker is a color in that a tile can
be `draw`n with a non-ramp color without the two interacting — see
§5 render order.

### 2.2 Opcodes

| Opcode               | Effect                                                 |
|----------------------|--------------------------------------------------------|
| `incr <marker> [N]`  | Add N (default 1) to this cell's marker count. Clamp at 255. |
| `decr <marker> [N]`  | Subtract N (default 1). Clamp at 0.                    |

Both fall through (don't yield, don't terminate). Match `draw`/`erase`
in that regard. A script that *only* does `incr` + `done` still needs
a yield on every path, same as today.

### 2.3 Conditions

Extend the tile-predicate family with marker predicates:

| Form                                     | Meaning                                 |
|------------------------------------------|-----------------------------------------|
| `if <marker>`                            | Sugar for `if <marker> > 0`.            |
| `if <marker> > N`                        | Board-wide: any cell has marker > N.    |
| `if <marker> < N`                        | Board-wide: any cell has marker < N.    |
| `if no <marker>`                         | Board-wide: all cells have marker == 0. |
| `if on <marker>`                         | Sugar for `if on <marker> > 0`.         |
| `if on <marker> > N`                     | Agent's cell has marker > N.            |
| `if on <marker> < N`                     | Agent's cell has marker < N.            |
| `if on no <marker>`                      | Agent's cell has marker == 0.           |
| `if <marker> on landmark <name>`         | Sugar for `if <marker> > 0 on landmark`.|
| `if <marker> > N on landmark <name>`     | Any landmark cell has marker > N.       |
| `if <marker> < N on landmark <name>`     | Any landmark cell has marker < N.       |
| `if no <marker> on landmark <name>`      | All landmark cells have marker == 0.    |

Comparison operators for v1 are `>` and `<` only (strict). `>=`, `<=`,
`==` can come later; the soft-cap pattern (§3.1) needs strict `<` to
work cleanly, and strict `>` was already the shape in the original
spec. **`if <marker>` without an operator is sugar for
`if <marker> > 0`** (and likewise `if on <marker>` = `if on <marker>
> 0`). Decided yes — `on heap > 0` is the 90% case and the bare form
reads cleanly in worked scripts. `if marker <name>` is reserved for a
future "is this marker declared" compile-time check if ever needed.

One semantic note: every tile has an implicit marker count of 0 until
an agent `incr`s it. So `if <marker> < N` matches every never-touched
cell for any N > 0 — which makes the board-wide form almost always
true and not very useful. The landmark-scoped and self-cell forms are
the ones that carry their weight.

This same extension folds in the landmark-scoped tile-predicate form
we were going to add for brick pile: `if extra on landmark pile`
becomes the bool sibling of `if <marker> > 0 on landmark pile`.

### 2.4 Seek

Two new primitives and one filter extension:

| Form                                          | Meaning                                           |
|-----------------------------------------------|---------------------------------------------------|
| `seek highest <marker>`                       | A\* toward highest-count cell (candidates: count > 0). |
| `seek highest <marker> > N`                   | Gradient with floor filter (candidates: count > N).    |
| `seek highest <marker> < N`                   | Gradient with cap filter (candidates: count < N, incl. 0). |
| `seek lowest  <marker>`                       | A\* toward lowest-count cell (candidates: all cells).  |
| `seek lowest  <marker> > N`                   | Lowest-count with floor filter.                        |
| `seek lowest  <marker> < N`                   | Lowest-count with cap filter.                          |
| `seek highest <marker> [cmp N] on landmark X` | Scope any of the above to a landmark's points.         |
| `seek landmark X on <marker> > N`             | Existing: nearest landmark point, marker-filtered.     |
| `seek landmark X on <marker> < N`             | Same, strict-less-than filter.                         |

`seek highest <marker>` is the pheromone follower — cost function is
"maximize count, break ties by distance." `seek lowest` is the
anti-pheromone (avoid the worn path). Both keep the `max nodes`
budget; if the candidate set is empty (no cells match the filter),
the seek fails like today.

**Candidate set semantics matter, especially for the cap form.** Bare
`seek highest <marker>` excludes zero-count cells on purpose — if
no agent has dropped a marker anywhere, there's no gradient to
follow, and the seek fails so the caller can fall through to
`wander`. The `< N` form *includes* zero-count cells because it's the
deposit primitive: on an empty pile all 16 cells tie at 0, distance
wins, agent lands on the nearest one. That's the correct behavior
for "find somewhere to put this brick," even on a fresh board.

The strict-less-than filter is the **soft-cap primitive**. Instead of
enforcing a per-cell ceiling inside `incr` (which would require a max
clause on the ramp declaration and silently lose bricks on overflow),
scripts guard the deposit site explicitly: `seek highest heap < 5 on
landmark piles` finds the best-available pile cell with room, and the
agent `incr`s where it lands. Conservation becomes the script
author's invariant, not the engine's — the engine just provides the
count. If the candidate set exhausts (all pile cells at cap), the
agent wanders and retries; transient races (cell fills between seek
start and arrival) can overshoot the cap by 1 and self-correct on
the next fetch cycle. Design choice from discussion: **workers are
tolerant**, engine doesn't cap — the author chooses when to be
strict.

**`on landmark X` scope** can be suffixed to any `seek highest` /
`seek lowest` form to restrict candidates to a landmark's points.
Essential for the doozer-style deposit flow: without scoping,
`seek highest heap < 5` on an empty board considers every cell
(all tied at 0), and the nearest-tie-break usually resolves to the
cell the agent is standing on, producing no motion. Scoping to the
pile's landmark fixes it.

---

## 3. Worked examples

### 3.1 Brick pile, marker-based

```
--- colors ---
doozer: (255, 255, 0)
brick:  (200, 60, 20)
heap:   ramp (0.0, 2.0, 0.0) decay 0/1    # persistent

--- landmarks ---
pile: brick
  (0, 7) (1, 7) (2, 7)

--- behavior ---
doozer:
  if no heap on landmark pile:
    # pile empty — deliver a brick
    seek landmark pile timeout 20
    if on landmark pile:
      incr heap
      draw brick
      pause 3
      done
    pause 1
    done

  if heap > 0 on landmark pile:
    # pile non-empty — take one
    seek landmark pile on heap > 0 timeout 20
    if on heap > 0:
      decr heap
      erase
      pause 3
      done

  wander
  pause 2
  done
```

Heap count tracks "bricks on pile cell." Render shows pile greener
with more bricks stacked — the brightness ramp doubles as a visible
fill level.

### 3.2 Pheromone ants

```
--- colors ---
ant:   (255, 180, 80)
trail: ramp (0.2, 0.1, 0.0) decay 1/15

--- agents ---
up to 12 ant

--- behavior ---
ant:
  incr trail 2
  if trail > 0:
    seek highest trail timeout 5
    pause 1
    done
  wander
  pause 1
  done
```

Ants drop pheromone wherever they walk, follow the gradient. Trail
fades 1 unit every 15 ticks; at 10 ticks/sec that's a 6s half-life
at count 2. Emergent foraging without explicit coordination.

---

## 4. Compile-time validation

- Marker name collides with agent/landmark/color → error (existing
  rule covers this).
- `incr` / `decr` / `if <marker>` referring to an undeclared marker →
  error.
- `ramp (r, g, b)` with all-zero RGB → warning (invisible, probably a
  bug).
- `decay K/T` with `T == 0` → error (division by zero).
- `decay K/T` with `K > T` doesn't technically collapse but warn
  (drains too fast to be useful).
- Cycles referencing a ramp color → error (ramps aren't static colors;
  cycles expect static entries).
- `draw <marker>` → error (markers aren't drawable; use `incr`).

---

## 5. Render model

Order per frame, inserting marker compositing between landmarks and
lit state:

1. Clear
2. Landmark backgrounds
3. **Marker ramps** — for each declared marker, for each cell with
   count > 0, blend `(r*N, g*N, b*N)` *additively* onto the tile.
   Multiple markers on the same cell add.
4. Lit state (`draw`'s color)
5. Agents

"Additively" means `tile_rgb = clamp(tile_rgb + marker_rgb, 0, 255)`
per channel. This keeps the clock-painter model intact (`draw` still
wins for its cell) and lets markers show through on un-drawn cells.

Open: should `draw` *replace* or *add on top of* the marker layer at
its cell? Replace is simpler and matches today's mental model. Add-on-
top lets pile markers show through even as bricks are drawn. I'd lean
replace for v1.

SYRP lerp interaction: the sinusoidal blend runs between "before" and
"after" frames for moving agents. Marker ramps are part of the
background — they just participate in both endpoints like landmarks
do. No special case.

---

## 6. Eager decay

Every physics tick, after the agent pass, before convergence check:

```
for each marker m:
    m.decay_counter += 1
    if m.decay_counter >= m.decay_T:
        m.decay_counter = 0
        for each tile:
            tile.count[m] = max(0, tile.count[m] - m.decay_K)
```

Cost: `O(W * H * M)` per decay fire. 16×16 × 4 markers × (1-in-T) ≈
~1024 / T ops per tick. Trivial on Photon 2; negligible even on OG
Photon.

Eager was the explicit call — reads are O(1), state is always current,
dumps are trivially reproducible. Lazy (read-time decay from
last-touched-tick) was the alternative; cheaper on big grids with
sparse reads, but the reasoning cost is higher and our grids are
small.

---

## 7. Memory footprint

Per-tile cost: `uint8_t count[MAX_MARKERS]`. Global cap `MAX_MARKERS
= 4` (can revisit). On a 16×16 grid that's 256×4 = **1024 B** of
extra RAM.

Fine on Photon 2 / Argon. Tight on OG Photon — `rico_raccoon` already
carries the smallest IR_OTA_BUFFER. Consider per-device
`MAX_MARKERS` override in the device header if rico ever needs to
skip markers entirely. Default to 4; `#define MAX_MARKERS 0` on rico
to compile out the feature → tile struct shrinks, ramp code dead-
stripped, scripts using markers fail `load()` with a clear error.

Per-marker global state: `ramp_r, ramp_g, ramp_b` (as Q8 ints or Q16
floats), `decay_K, decay_T, decay_counter`. ~8-12 B per marker ×
4 = 32-48 B. Rounding error.

---

## 8. Ramp fixed-point representation

Two options for on-device ramp RGB:

1. **Q8** — 8.8 fixed-point. Values in `[0.0, 255.996]`. Enough
   precision for our use (smallest useful increment is ~1/count, i.e.
   0.004 for 255). `uint16_t r_q8` per channel. Render is `(r_q8 *
   count) >> 8` then clamp.
2. **Float** — native `float` per channel. More uniform, simpler
   compiler emission, no fixed-point arithmetic on the device.

I'd pick **Q8**. We've avoided floats in the hot path everywhere else
(the only float in the IR today is `diagonal_cost`, and it's used
once per seek setup). A per-render-frame multiply per marker per tile
is a lot of float ops if we're not careful.

Wire format emits as `%.4f` strings; parser converts to Q8.

---

## 9. Wire format changes

New section, optional:

```
--- MARKERS <count>
MARKER <name> <r> <g> <b> <decay_K> <decay_T>
MARKER <name> <r> <g> <b> <decay_K> <decay_T>
...
```

Emitted only when the source declares at least one marker. Absent
section → no markers, zero-cost for existing scripts (byte-identical
output to pre-feature blobs, same guarantee the night palette gave).

**IR_VERSION bump**: 4 → 5.

Dump format: add one row per tile listing non-zero marker counts only:
`tile <x> <y> markers trail=3 heap=1`. Omits zero-count markers to
keep traces slim.

---

## 10. Sim ↔ HAL parity

Same contract as night palette: compiler validates → sim executes →
HAL reproduces. Dump diffs are gold — any divergence in decay timing,
clamp behavior, or render compositing shows up cell-by-cell.

Tests to add:
- `agents/tests/markers_basic.crit` — hello-world: one ant, one
  marker, pause 5, confirm decay fires.
- `agents/tests/markers_saturate.crit` — `incr trail 255` and check
  clamp.
- `agents/tests/markers_gradient.crit` — two ants, `seek highest
  trail`, confirm they cluster.
- `agents/tests/markers_unknown.crit` (negative) — `incr typo` when
  `typo` isn't declared.
- Dump comparison in `test_compiler.py`.

---

## 11. Open questions

1. ~~**`if <marker>` sugar?** Does `if trail:` mean `if trail > 0`?~~
   Resolved: yes, sugar for `> 0`. See §2.3.
2. **Replace vs add-on-top for `draw`.** See §5.
3. **`set <marker> = N`?** Explicit absolute set. Useful for
   initialization, but hurts the "agents only perturb locally" model.
   Defer to v2.
4. **Decay clamp floor other than 0?** E.g. "keep at least 1 here
   forever." Defer.
5. **Gradient cost function for `seek highest`.** Pure "highest count
   wins, nearest breaks tie" is obvious. Do we want distance-weighted
   ("closer high peaks beat far higher peaks")? Probably yes, but
   needs a tuning parameter; defer the knob, pick a sane default.
6. **Negative ramps?** `ramp (-1, 0, 0)` — tile goes *darker* with
   count. Subtractive compositing. Interesting for "shadow" effects
   but opens the clamp design. Defer.
7. **Interaction with night palette.** Does a marker have a night
   override? Probably not — markers are already "a color that scales
   with count," and at low brightness the floor-clamp will do the
   right thing. If needed, a night-ramp override is a natural v2.

---

## 12. Phasing

One PR-ish chunk:

1. Compiler: grammar, validation, IR emission, dump format.
2. Sim: tile struct grows, opcodes, conditions, seeks, decay, render
   compositing.
3. HAL: IrRuntime parse, CritterEngine counts + compositing, decay
   worker, ramp Q8 math.
4. Device header opt-out knob (`MAX_MARKERS=0`).
5. Fixtures + dump-diff tests.

Estimated: 1-2 days of focused work. About the size of the night
palette feature. Risk is mostly in the render compositing — getting
the visual to match between sim and HAL at low brightness may take
a round of eyeballing and LUT tweaks (SYRP-lerp all over again).

---

## 13. Non-goals

- Multi-tick decay with per-tile schedules. One global rate per
  marker.
- Non-linear ramps (log, quadratic). Linear only.
- Marker persistence across reboots. They live in RAM, die on reset.
- Cross-tile diffusion ("pheromone spreads"). That's a whole
  different model (cellular automaton). Stay explicit: agents move
  the counts.
- Reading a neighbor's marker in an `if`. Everything is self-cell
  or whole-board or landmark-scoped. Neighbor predicates are a big
  surface area we don't need yet.

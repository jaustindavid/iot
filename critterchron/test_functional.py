#!/usr/bin/env python3
"""Functional test harness for .crit scripts.

Where `test_compiler.py` only checks that scripts PARSE (static grammar,
name resolution, validator), this runs each fixture through the engine
for a bounded number of ticks and asserts observable runtime behavior —
agents actually moved, tiles got lit, markers accumulated where they
should, etc.

Fixtures here are CURATED, not auto-discovered: each needs a hand-written
assertion that encodes what the script is supposed to do. Keep the list
small — this is a smoke layer, not coverage. Add a case when you land a
script whose correctness is hard to eyeball from a compile pass alone
(the doozer_markers bootstrap deadlock is the motivating example: it
compiled cleanly, it just didn't do anything).

Usage:
    python3 test_functional.py          # run all, exit non-zero on any fail
    python3 test_functional.py -v       # verbose: show each case's outcome
"""

import sys
from pathlib import Path

from compiler import CritCompiler
from engine import CritterEngine
from main import MEGAFONT_5X6

ROOT = Path(__file__).parent


# --- Assertion helpers ------------------------------------------------------
# One-liners intentionally; no mini-framework. Each case reaches into the
# engine directly for whatever invariant it cares about.

def any_agent_moved(engine, start_positions):
    """True if at least one agent's position differs from its tick-0 snapshot."""
    return any(tuple(a.pos) != s for a, s in zip(engine.agents, start_positions))

def lit_count(engine):
    """Count of tiles currently in the `state` (lit) layer."""
    return sum(1 for x in range(engine.width) for y in range(engine.height)
               if engine.grid[x][y].state)

def marker_at(engine, name, x, y):
    """Marker count at a specific cell. Raises KeyError if marker undeclared."""
    idx = engine._markers[name]["index"]
    return engine.grid[x][y].count[idx]

def agent_pos(engine, agent_name, which=0):
    """(x, y) of the `which`-th agent matching `agent_name`, or None."""
    matches = [a for a in engine.agents if a.name == agent_name]
    return tuple(matches[which].pos) if which < len(matches) else None


# --- Case assertions --------------------------------------------------------
# Each takes (engine, start_positions) and returns (ok: bool, message: str).

# --- Observers (per-case state recorders) -----------------------------------
# Called once per tick during the run; mutate the `obs` dict so the final
# check can ask "did X ever happen" rather than "is X currently true." This
# matters because we tick 1000 by default to really exercise long-running
# pathways (swarms reaching steady state, decay pulses fully expiring,
# wanderers visiting many cells), but several scripts have transient peak
# behavior that's gone by tick 1000 — first-arrival, peak deposit count,
# and so on. Observers capture those moments inline.

def _obs_visit_log(eng, obs):
    """Record every (agent_name, position) seen this tick."""
    obs.setdefault("visits", set()).update(
        (a.name, tuple(a.pos)) for a in eng.agents
    )

def _obs_marker_max_at(name, x, y):
    """Closure: record max count seen at marker `name` at cell (x, y)."""
    def fn(eng, obs):
        try:
            slot = eng._markers[name]["index"]
        except KeyError:
            return
        key = f"max_{name}_{x}_{y}"
        obs[key] = max(obs.get(key, 0), eng.grid[x][y].count[slot])
    return fn

def _obs_marker_max_column(name, x, ys):
    """Closure: record max count seen at marker `name` at each cell (x, y) for y in ys."""
    def fn(eng, obs):
        try:
            slot = eng._markers[name]["index"]
        except KeyError:
            return
        col = obs.setdefault(f"col_{name}_{x}", {})
        for y in ys:
            col[y] = max(col.get(y, 0), eng.grid[x][y].count[slot])
    return fn


def _check_doozer_markers(eng, starts, obs):
    """Doozers should move AND draw at least some of the clock face."""
    if not any_agent_moved(eng, starts):
        return False, "no doozer moved from its start (bootstrap deadlock regression?)"
    lit = lit_count(eng)
    if lit < 5:
        return False, f"only {lit} cells lit after run — expected >=5 clock cells"
    return True, f"doozers moved; {lit} clock cells drawn"

def _check_markers_basic(eng, starts, obs):
    """Ant sits at (4,4) and deposits on its own cell. Observer tracks max
    trail seen because decay drives the steady-state value; we just want to
    confirm a deposit happened at all."""
    if tuple(eng.agents[0].pos) != (4, 4):
        return False, f"ant wandered to {tuple(eng.agents[0].pos)} — expected (4, 4)"
    peak = obs.get("max_trail_4_4", 0)
    if peak < 1:
        return False, f"trail@(4,4) peak = {peak}, expected >= 1 deposit"
    return True, f"ant parked at (4,4); peak trail={peak}"

def _check_hw_incr_trail(eng, starts, obs):
    """Ant climbs column 0 depositing 1..6 units per cell. Observer records
    peak count per cell — decay wipes the actual counts long before tick
    1000, but the deposits did happen and that's what we assert."""
    expected = {y: y + 1 for y in range(6)}
    got = obs.get("col_trail_0", {})
    got_filtered = {y: got.get(y, 0) for y in range(6)}
    if got_filtered != expected:
        return False, f"trail column mismatch: peak={got_filtered}, want {expected}"
    pos = agent_pos(eng, "ant")
    if pos != (0, 5):
        return False, f"ant parked at {pos}, expected (0, 5)"
    return True, f"trail column peak = {[got_filtered[y] for y in range(6)]}"

def _check_hw_decay_pulse(eng, starts, obs):
    """Trail at (4,4) should reach 10 (initial draw) and then decay. With
    1000 ticks we expect full decay to 0; the assertion is the *trajectory*:
    peak >= 10 (the deposit fired), final < peak (decay happened)."""
    peak = obs.get("max_trail_4_4", 0)
    final = marker_at(eng, "trail", 4, 4)
    if peak < 10:
        return False, f"peak trail = {peak}, expected >= 10 (deposit didn't fire)"
    if final >= peak:
        return False, f"trail final ({final}) >= peak ({peak}) — decay not running?"
    return True, f"trail rose to {peak}, decayed to {final}"

def _check_hw_gradient_follow(eng, starts, obs):
    """Follower should park on the beacon's peak at (7, 0). Beacon trail
    decays once the source script tapers, so we use the observed peak."""
    pos = agent_pos(eng, "follower")
    if pos != (7, 0):
        return False, f"follower at {pos}, expected (7, 0)"
    peak = obs.get("max_trail_7_0", 0)
    if peak < 5:
        return False, f"trail peak at (7,0) = {peak}, expected >= 5"
    return True, f"follower parked on (7, 0); peak trail = {peak}"

def _check_markers_highest_cap(eng, starts, obs):
    """Ant should land on (8,8) — the highest sub-cap pile cell — at some
    point during the run. With 1000 ticks the ant moves on after its
    `pause 30` ends; we assert the *first landing* via the visit log."""
    visits = obs.get("visits", set())
    if ("ant", (8, 8)) not in visits:
        return False, (
            f"ant never visited (8, 8). "
            f"(4,8)=forgot sort-by-count; (12,8)=forgot cap filter; else=landmark scope broken"
        )
    return True, "ant visited (8, 8) as predicted"

def _check_swarm(eng, starts, obs):
    """Swarm uses `seek nearest free current` — the regression target for
    Python sim parity with the C++ engine. Locusts spawn dynamically from
    the nest (zero initial agents in `--- agents ---`), so the
    `any_agent_moved` check doesn't apply: instead we assert the spawn
    rule fired (live agents > 0) and the grid actually converged.

    Assertion list:
      * spawn rule fired (live agents > 0)
      * grid converged (lit / intended >= 50%)
      * no pile-up (failed_seeks below a generous budget)
      * no crash (implicit — running 1000 ticks without raising)
    """
    if len(eng.agents) == 0:
        return False, "no locusts alive — spawn rule misfired?"
    lit = lit_count(eng)
    intended = sum(1 for x in range(eng.width) for y in range(eng.height)
                   if eng.grid[x][y].intended)
    if intended == 0:
        return False, "no intended cells (sync_time misfire?)"
    coverage = lit / intended
    if coverage < 0.5:
        return False, f"convergence {coverage:.0%} below 50%; lit={lit}/{intended}"
    fails = eng.health_metrics.get("failed_seeks", 0)
    # 1000 ticks × 16 agents = 16k agent-ticks; pre-`free` regression saw
    # ~57 fails/sec on hardware (~1k+ fails per minute). Headroom of 200
    # for sim drift; trip if it spikes (regression of `seek free`).
    if fails > 200:
        return False, f"failed_seeks={fails} > 200 budget (free-seek regression?)"
    return True, (
        f"swarm: {len(eng.agents)} locusts, converged {coverage:.0%}, "
        f"failed_seeks={fails}"
    )


# --- Registry ---------------------------------------------------------------
# (relative_path, ticks_to_run, sync_time_needed, observe_fn, assertion_fn)
# sync_time_needed=True draws the clock face into `intended` before ticking —
# required for any script whose behavior hinges on `missing` / `extra`.
# observe_fn is None for "no per-tick recording needed" (final-state-only
# assertions). Default tick budget is 1000 — long enough that the swarm
# reaches steady state, decay pulses fully expire, and any first-tick-only
# bug (the bug that motivated this default — `seek free` AttributeError
# only fires when an agent actually contests an occupied cell) shows up
# as a crash rather than an unobserved silent regression.

DEFAULT_TICKS = 1000

CASES = [
    ("agents/doozer_markers.crit",            DEFAULT_TICKS, True,
        None, _check_doozer_markers),
    ("agents/tests/markers_basic.crit",       DEFAULT_TICKS, False,
        _obs_marker_max_at("trail", 4, 4), _check_markers_basic),
    # Visit log captures the ant's first landing on (8,8); without it the
    # 1000-tick run would see the ant move on after `pause 30` ends and
    # the assertion would fail.
    ("agents/tests/markers_highest_cap.crit", DEFAULT_TICKS, False,
        _obs_visit_log, _check_markers_highest_cap),
    # HW visual-smoke fixtures. These double as the reference render for the
    # hardware port: the sim must produce the pattern described in each
    # fixture's header comment, so flashing becomes a pure parity check.
    # All three fit an 8×8 grid rooted at (0, 0) so they run on any device.
    # 1000-tick observation captures the deposits/peak before decay erases
    # them — the steady state at tick 1000 is "trail fully decayed to 0".
    ("agents/hw_incr_trail.crit",             DEFAULT_TICKS, False,
        _obs_marker_max_column("trail", 0, range(6)), _check_hw_incr_trail),
    ("agents/hw_decay_pulse.crit",            DEFAULT_TICKS, False,
        _obs_marker_max_at("trail", 4, 4), _check_hw_decay_pulse),
    ("agents/hw_gradient_follow.crit",        DEFAULT_TICKS, False,
        _obs_marker_max_at("trail", 7, 0), _check_hw_gradient_follow),
    # swarm.crit exercises `seek nearest free current` (occupancy-aware
    # candidate filtering). Added 2026-04-24 after a sim crash that
    # earlier fixtures missed because none ticked long enough to actually
    # contest occupancy. needs_sync because the script targets `current`
    # tiles, which requires an `intended` clock face to seek toward.
    ("agents/swarm.crit",                     DEFAULT_TICKS, True,
        None, _check_swarm),
]


def run_case(rel_path, ticks, needs_sync, observe, check):
    path = ROOT / rel_path
    try:
        ir = CritCompiler().compile(str(path))
    except Exception as e:
        return False, f"compile failed: {e}"
    eng = CritterEngine(ir)
    if needs_sync:
        eng.sync_time(MEGAFONT_5X6)
    starts = [tuple(a.pos) for a in eng.agents]
    obs = {}
    for _ in range(ticks):
        eng.tick()
        if observe is not None:
            observe(eng, obs)
    try:
        return check(eng, starts, obs)
    except Exception as e:
        return False, f"assertion crashed: {type(e).__name__}: {e}"


def main(argv):
    verbose = "-v" in argv or "--verbose" in argv
    passed = failed = 0
    for rel, ticks, needs_sync, observe, check in CASES:
        ok, msg = run_case(rel, ticks, needs_sync, observe, check)
        mark = "PASS" if ok else "FAIL"
        if verbose or not ok:
            print(f"  [{mark}] {rel}  ({ticks} ticks): {msg}")
        if ok:
            passed += 1
        else:
            failed += 1

    total = passed + failed
    print(f"\n{passed}/{total} passed" + ("" if failed == 0 else f" \u2014 {failed} failed"))
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

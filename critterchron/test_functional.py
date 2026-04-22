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

def _check_doozer_markers(eng, starts):
    """Doozers should move AND draw at least some of the clock face."""
    if not any_agent_moved(eng, starts):
        return False, "no doozer moved from its start (bootstrap deadlock regression?)"
    lit = lit_count(eng)
    if lit < 5:
        return False, f"only {lit} cells lit after run — expected >=5 clock cells"
    return True, f"doozers moved; {lit} clock cells drawn"

def _check_markers_basic(eng, starts):
    """Ant sits at (4,4) and deposits on its own cell."""
    if tuple(eng.agents[0].pos) != (4, 4):
        return False, f"ant wandered to {tuple(eng.agents[0].pos)} — expected (4, 4)"
    c = marker_at(eng, "trail", 4, 4)
    if c < 1:
        return False, f"trail@(4,4) = {c}, expected >= 1 deposit"
    return True, f"ant parked at (4,4); trail={c}"

def _check_hw_incr_trail(eng, starts):
    """Ant climbs column 0 depositing 1..6 units per cell. Exact match."""
    expected = {(0, y): y + 1 for y in range(6)}
    got = {(0, y): marker_at(eng, "trail", 0, y) for y in range(6)}
    if got != expected:
        return False, f"trail column mismatch: got {got}, want {expected}"
    pos = agent_pos(eng, "ant")
    if pos != (0, 5):
        return False, f"ant parked at {pos}, expected (0, 5)"
    return True, f"trail column = {[got[(0, y)] for y in range(6)]}"

def _check_hw_decay_pulse(eng, starts):
    """Trail at (4,4) should have decayed significantly from the initial 10."""
    c = marker_at(eng, "trail", 4, 4)
    # Decay 1/8 over 40 ticks = 5 units lost. Count should be ~5.
    # Tolerate ±1 for scheduling jitter.
    if c == 0:
        return False, "trail fully gone — decay too fast, or ticks overshot"
    if c >= 10:
        return False, f"trail = {c}, not decayed at all"
    return True, f"trail decayed from 10 to {c} (expected ~5)"

def _check_hw_gradient_follow(eng, starts):
    """Follower should park on the beacon's peak at (7, 0)."""
    pos = agent_pos(eng, "follower")
    if pos != (7, 0):
        return False, f"follower at {pos}, expected (7, 0)"
    peak = marker_at(eng, "trail", 7, 0)
    if peak < 5:
        return False, f"trail peak at (7,0) = {peak}, expected 5"
    return True, f"follower parked on (7, 0); peak = {peak}"

def _check_markers_highest_cap(eng, starts):
    """Ant should land on (8,8): the highest sub-cap pile cell."""
    pos = agent_pos(eng, "ant")
    if pos != (8, 8):
        return False, (
            f"ant at {pos}, expected (8, 8). "
            f"(4,8)=forgot sort-by-count; (12,8)=forgot cap filter; else=landmark scope broken"
        )
    return True, "ant landed on (8, 8) as predicted"


# --- Registry ---------------------------------------------------------------
# (relative_path, ticks_to_run, sync_time_needed, assertion_fn)
# sync_time_needed=True draws the clock face into `intended` before ticking —
# required for any script whose behavior hinges on `missing` / `extra`.

CASES = [
    ("agents/doozer_markers.crit",            80, True,  _check_doozer_markers),
    ("agents/tests/markers_basic.crit",       20, False, _check_markers_basic),
    # 25 ticks lands inside the ant's post-arrival `pause 30`. A longer window
    # would see the ant re-seek after pause ends — at that point (8,8) is at
    # cap and the only sub-cap cell left is (4,8), so it correctly walks
    # there. We want to assert the FIRST landing, not the follow-up.
    ("agents/tests/markers_highest_cap.crit", 25, False, _check_markers_highest_cap),
    # HW visual-smoke fixtures. These double as the reference render for the
    # hardware port: the sim must produce the pattern described in each
    # fixture's header comment, so flashing becomes a pure parity check.
    # All three fit an 8×8 grid rooted at (0, 0) so they run on any device.
    # 25 ticks: 6 states × ~3 ticks/state (set-state transitions defer by
    # a tick, step+pause account for the rest) + a few settle ticks.
    ("agents/hw_incr_trail.crit",             25, False, _check_hw_incr_trail),
    ("agents/hw_decay_pulse.crit",            40, False, _check_hw_decay_pulse),
    ("agents/hw_gradient_follow.crit",        12, False, _check_hw_gradient_follow),
]


def run_case(rel_path, ticks, needs_sync, check):
    path = ROOT / rel_path
    try:
        ir = CritCompiler().compile(str(path))
    except Exception as e:
        return False, f"compile failed: {e}"
    eng = CritterEngine(ir)
    if needs_sync:
        eng.sync_time(MEGAFONT_5X6)
    starts = [tuple(a.pos) for a in eng.agents]
    for _ in range(ticks):
        eng.tick()
    try:
        return check(eng, starts)
    except Exception as e:
        return False, f"assertion crashed: {type(e).__name__}: {e}"


def main(argv):
    verbose = "-v" in argv or "--verbose" in argv
    passed = failed = 0
    for rel, ticks, needs_sync, check in CASES:
        ok, msg = run_case(rel, ticks, needs_sync, check)
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

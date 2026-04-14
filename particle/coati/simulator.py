"""
ASCII terminal simulator for coati scripts.

Renders the grid state to the terminal using ANSI escape codes.
Supports run (live), step (one-tick-at-a-time), and fast (no render) modes.
"""
from __future__ import annotations
import sys
import os
import time
import math
import select
import termios
import tty
from .ir import CoatiIR
from .engine import CoatiEngine, Agent


# ── ANSI helpers ────────────────────────────────────────────────

def _fg(r: int, g: int, b: int) -> str:
    return f"\033[38;2;{r};{g};{b}m"

def _bg(r: int, g: int, b: int) -> str:
    return f"\033[48;2;{r};{g};{b}m"

_RESET = "\033[0m"
_CLEAR = "\033[2J\033[H"
_HIDE_CURSOR = "\033[?25l"
_SHOW_CURSOR = "\033[?25h"


def _move_to(row: int, col: int) -> str:
    return f"\033[{row};{col}H"


# ── Agent state description ─────────────────────────────────────

def _agent_status(agent: Agent) -> str:
    parts = [f"C{agent.index}"]
    carrying = agent.get("carrying")
    washed = agent.get("washed")
    bored = agent.get("bored")
    if carrying and not washed:
        wash_ticks = agent.get("wash_counter") or 0
        if wash_ticks > 0:
            parts.append(f"washing({wash_ticks})")
        else:
            parts.append("carry→wash")
    elif carrying and washed:
        parts.append("carry→place")
    elif bored:
        parts.append("bored")
    elif agent.path:
        parts.append("moving")
    else:
        parts.append("idle")
    pos = f"({agent.pos[0]},{agent.pos[1]})"
    parts.append(pos)
    if agent.claimed:
        parts.append(f"→({agent.claimed[0]},{agent.claimed[1]})")
    return " ".join(parts)


# ── Agent color ─────────────────────────────────────────────────

def _lookup_agent_color(selector: str, agent: Agent, ir: CoatiIR,
                        engine=None) -> tuple[int, int, int] | None:
    """
    Search rendering rules for (selector, agent.index).
    Index-specific rules beat generic ones.
    Returns (r, g, b) or None if no matching rule found.
    """
    generic = None
    specific = None
    for rule in ir.rendering.rules:
        if rule.selector == selector:
            if rule.agent_index is None:
                generic = rule.color
            elif rule.agent_index == agent.index:
                specific = rule.color
    color = specific or generic
    if color is None:
        return None
    return (color.r, color.g, color.b)


def _agent_color(agent: Agent, tick: int, ir: CoatiIR,
                 engine=None) -> tuple[int, int, int]:
    """Determine agent display color based on state, matching render rules."""

    # Behavior-set color hint ('agent <color>' action) — highest priority
    dc = agent.props.get("_display_color")
    if dc:
        return dc

    # Conditional 'when' rules from rendering section — second priority
    if engine is not None:
        for rule in ir.rendering.rules:
            if rule.selector == "agent_when" and rule.condition is not None:
                if engine._eval_condition(rule.condition, agent):
                    return (rule.color.r, rule.color.g, rule.color.b)

    carrying = agent.get("carrying")
    washed = agent.get("washed")
    bored = agent.get("bored")

    if agent.wait_counter > 1:
        phase = math.sin(tick * 3.0)
        if phase > 0:
            return (255, 80, 80)
        return (100, 30, 30)

    if bored:
        c = _lookup_agent_color("agent_bored", agent, ir)
        return c if c else ((128, 128, 128) if agent.index == 0 else (0, 128, 128))

    if carrying and not washed:
        pulse = 0.7 + 0.3 * math.sin(tick * 0.5)
        v = int(255 * pulse)
        return (v, v, 0)

    if carrying and washed:
        return (255, 255, 0)

    # Idle — consult IR first, fall back to white/cyan
    c = _lookup_agent_color("agent_idle", agent, ir)
    return c if c else ((255, 255, 255) if agent.index == 0 else (0, 255, 255))



# ── Render frame ────────────────────────────────────────────────

def render_frame(engine: CoatiEngine, ir: CoatiIR) -> str:
    """Render the current state to an ANSI string."""
    w, h = engine.grid_w, engine.grid_h
    cur = engine.current()
    tgt = engine.target()
    fade = engine.fade()
    tick = engine.tick_count

    # Build landmark lookup (scale colors up for terminal visibility —
    # .coati colors are NeoPixel values where 64 is "bright", but in
    # 24-bit ANSI that's nearly invisible)
    lm_map: dict[tuple[int, int], tuple[str, tuple[int, int, int]]] = {}
    for lm in ir.landmarks:
        r, g, b = lm.color or (128, 128, 128)
        peak = max(r, g, b, 1)
        scale = 255 / peak
        color = (min(int(r * scale), 255), min(int(g * scale), 255), min(int(b * scale), 255))
        for cell in lm.cells:
            lm_map[cell] = (lm.name, color)

    # Build agent position lookup
    agent_map: dict[tuple[int, int], Agent] = {}
    for a in engine.agents:
        if a.active:
            agent_map[a.pos] = a

    active_agents = [a for a in engine.agents if a.active]

    lines = []

    # Status line
    extras = engine.extras_count()
    missing = engine.missing_count()
    agents_str = "  ".join(f"[{_agent_status(a)}]" for a in active_agents)
    agents_str = f"{len(active_agents)} active  " + agents_str
    status = f"tick {tick:>5}  |  extras:{extras} missing:{missing}  |  {agents_str}"
    lines.append(status)
    lines.append("")

    # Grid
    for y in range(h):
        row_chars = []
        for x in range(w):
            pt = (x, y)

            if pt in agent_map:
                a = agent_map[pt]
                r, g, b = _agent_color(a, tick, ir, engine)
                label = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"[a.index % 36]
                row_chars.append(f"{_fg(r, g, b)}{label}{_RESET}")
            elif pt in lm_map:
                name, (r, g, b) = lm_map[pt]
                char = name[0].upper()
                row_chars.append(f"{_fg(r, g, b)}{char}{_RESET}")
            elif cur[x][y]:
                f_val = fade[x][y]
                # Green, brightness by fade
                intensity = max(int(f_val * 255), 30)
                row_chars.append(f"{_fg(0, intensity, 0)}#{_RESET}")
            elif tgt[x][y]:
                # Missing pixel — dim marker
                row_chars.append(f"{_fg(60, 60, 60)}○{_RESET}")
            else:
                row_chars.append(f"{_fg(40, 40, 40)}·{_RESET}")

        lines.append("".join(row_chars))

    return "\n".join(lines)


# ── Run modes ───────────────────────────────────────────────────

def _kbhit() -> str | None:
    """Non-blocking check for a keypress. Returns the key or None."""
    if select.select([sys.stdin], [], [], 0)[0]:
        return sys.stdin.read(1)
    return None


def run_live(engine: CoatiEngine, ir: CoatiIR, speed: float = 1.0):
    """Run the simulation live in the terminal at physics_hz * speed."""
    base_interval = 1.0 / ir.rendering.physics_hz if ir.rendering.physics_hz > 0 else 0.1
    tick_interval = base_interval / speed

    # Save terminal state and set cbreak mode (not raw — preserves output processing)
    old_settings = termios.tcgetattr(sys.stdin)
    try:
        tty.setcbreak(sys.stdin.fileno())
        sys.stdout.write(_HIDE_CURSOR)
        sys.stdout.flush()

        paused = False
        running = True
        while running:
            frame_start = time.time()

            if not paused:
                engine.tick()

            frame = render_frame(engine, ir)
            sys.stdout.write(_CLEAR + frame + "\n\n")
            controls = "[q]uit  [space]pause  [+/-]speed  [s]tep"
            if paused:
                controls += "  ** PAUSED **"
            sys.stdout.write(f"  {_fg(100,100,100)}{controls}{_RESET}\n")
            effective_hz = 1.0 / tick_interval if tick_interval > 0 else 0
            sys.stdout.write(f"  {_fg(100,100,100)}speed: {speed:.1f}x  {effective_hz:.1f}hz  ({tick_interval*1000:.0f}ms/tick){_RESET}")
            sys.stdout.flush()

            # Wait for next tick, checking for input
            while time.time() - frame_start < tick_interval:
                key = _kbhit()
                if key:
                    if key == "q" or key == "\x03":  # q or ctrl-c
                        running = False
                        break
                    elif key == " ":
                        paused = not paused
                        break
                    elif key == "+":
                        speed = min(speed * 1.5, 50.0)
                        tick_interval = base_interval / speed
                        break
                    elif key == "-":
                        speed = max(speed / 1.5, 0.1)
                        tick_interval = base_interval / speed
                        break
                    elif key == "s":
                        # Single step
                        engine.tick()
                        paused = True
                        break
                time.sleep(0.01)

    finally:
        termios.tcsetattr(sys.stdin, termios.TCSADRAIN, old_settings)
        sys.stdout.write(_SHOW_CURSOR + "\n")
        sys.stdout.flush()


def run_step(engine: CoatiEngine, ir: CoatiIR):
    """Run one tick at a time, waiting for Enter between ticks."""
    print("Step mode — press Enter to advance, q to quit.\n")
    while True:
        engine.tick()
        frame = render_frame(engine, ir)
        print(frame)
        print()

        # Detailed state dump
        for a in engine.agents:
            props = ", ".join(f"{k}={v}" for k, v in a.props.items())
            path_len = len(a.path)
            print(f"  Agent {a.index}: pos={a.pos} path_len={path_len} "
                  f"claimed={a.claimed} wait={a.wait_counter} {props}")
        print()

        try:
            inp = input("  [Enter=next, q=quit] > ").strip()
        except (EOFError, KeyboardInterrupt):
            break
        if inp.lower() == "q":
            break


def run_fast(engine: CoatiEngine, ir: CoatiIR, max_ticks: int = 5000,
             static_time: tuple[int, int] | None = None):
    """
    Run without rendering until converged or max_ticks.
    Optionally set a static time target.
    """
    if static_time:
        engine._update_goal_static(static_time[0], static_time[1])

    start = time.time()
    while engine.tick_count < max_ticks:
        engine.tick()
        if engine.is_converged():
            break

    elapsed = time.time() - start
    converged = engine.is_converged()
    print(f"{'Converged' if converged else 'Did not converge'} "
          f"in {engine.tick_count} ticks ({elapsed:.2f}s)")
    print(f"  Extras remaining: {engine.extras_count()}")
    print(f"  Missing remaining: {engine.missing_count()}")

    # Print final board
    print()
    frame = render_frame(engine, ir)
    print(frame)

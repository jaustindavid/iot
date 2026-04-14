"""
Simulation engine that interprets a CoatiIR and runs the tick loop.

This is a pure-Python reimplementation of the CoatiEngine behavior,
driven entirely by the IR (no hardcoded coaticlock logic).
"""
from __future__ import annotations
import random
import time
import math
from dataclasses import dataclass, field
from .ir import (
    CoatiIR, ConditionNode, ActionNode, IfNode, BehaviorRule,
)
from .pathfinding import find_path
from .font import render_time


# ── Agent runtime state ─────────────────────────────────────────

@dataclass
class Agent:
    index: int
    pos: tuple[int, int]
    last_pos: tuple[int, int]
    active: bool = True
    path: list[tuple[int, int]] = field(default_factory=list)
    claimed: tuple[int, int] | None = None
    wait_counter: int = 0
    props: dict = field(default_factory=dict)

    def get(self, name: str):
        if name == "wait_counter":
            return self.wait_counter
        return self.props.get(name)

    def set(self, name: str, value):
        if name == "wait_counter":
            self.wait_counter = value
        else:
            self.props[name] = value


# ── Wobbly clock ────────────────────────────────────────────────

class WobblyClock:
    def __init__(self, min_offset: int, max_offset: int,
                 fast_rate: float, slow_rate: float):
        self.min_offset = min_offset
        self.max_offset = max_offset
        self.fast_rate = fast_rate
        self.slow_rate = slow_rate
        self.target_offset = random.randint(min_offset, max_offset)
        self.virtual_time = time.time() + self.target_offset
        self.last_wall = time.time()

    def advance(self) -> float:
        now = time.time()
        elapsed = now - self.last_wall
        self.last_wall = now

        current_offset = self.virtual_time - now
        if abs(current_offset) > self.max_offset * 2:
            self.virtual_time = now + self.target_offset
            return self.virtual_time

        rate = self.fast_rate if current_offset < self.target_offset else self.slow_rate
        self.virtual_time += elapsed * rate

        if abs(current_offset - self.target_offset) < 5.0:
            self.target_offset = random.randint(self.min_offset, self.max_offset)

        return self.virtual_time


# ── Engine ──────────────────────────────────────────────────────

# Sentinel returned by behavior evaluation to signal "done" (stop processing rules)
_DONE = object()
# Sentinel to signal a pathfind was used
_PATHFIND_USED = object()


class CoatiEngine:
    def __init__(self, ir: CoatiIR):
        self.ir = ir
        self.grid_w = ir.grid.width
        self.grid_h = ir.grid.height

        # Layers
        self.layers: dict[str, list[list]] = {}
        for layer in ir.layers:
            self.layers[layer.name] = [
                [layer.default] * self.grid_h for _ in range(self.grid_w)
            ]

        # Landmarks as sets for fast lookup, and as ordered lists for index assignment
        self.landmarks: dict[str, list[tuple[int, int]]] = {}
        self.landmark_cells: set[tuple[int, int]] = set()
        for lm in ir.landmarks:
            self.landmarks[lm.name] = lm.cells
            for c in lm.cells:
                self.landmark_cells.add(c)

        # Landmark locks: name → agent_index or -1
        self.landmark_locks: dict[str, int] = {lm.name: -1 for lm in ir.landmarks}

        # Terms (precomputed each tick)
        self.term_cells: dict[str, list[tuple[int, int]]] = {}

        # Agents
        self.agents: list[Agent] = []
        self.pool_mode = ir.agents.pool_mode
        self.spawn_point = ir.agents.spawn_point
        self.max_agents = ir.agents.count
        self._next_agent_id = 0

        if self.pool_mode:
            # Pool mode: agents start inactive, spawned on demand
            for i in range(ir.agents.count):
                sp = ir.agents.spawn_point or (0, 0)
                a = Agent(index=i, pos=sp, last_pos=sp, active=False)
                for prop in ir.agents.properties:
                    a.props[prop.name] = prop.default
                self.agents.append(a)
        else:
            # Fixed mode: all agents active from the start
            for i in range(ir.agents.count):
                start = ir.agents.starts[i] if i < len(ir.agents.starts) else (0, 0)
                a = Agent(index=i, pos=start, last_pos=start, active=True)
                for prop in ir.agents.properties:
                    a.props[prop.name] = prop.default
                self.agents.append(a)

        # Spawning
        self.spawn_timer = 0

        # Goal
        self.clock: WobblyClock | None = None
        if ir.goal.type == "clock" and ir.goal.time_config:
            tc = ir.goal.time_config
            self.clock = WobblyClock(tc.min_offset, tc.max_offset,
                                     tc.fast_rate, tc.slow_rate)
        self.last_minute = -1

        # Stats
        self.tick_count = 0

    # ── Layer access helpers ────────────────────────────────────

    def get_layer(self, name: str) -> list[list]:
        return self.layers[name]

    def current(self) -> list[list[bool]]:
        return self.layers.get("current", [[]])

    def target(self) -> list[list[bool]]:
        return self.layers.get("target", [[]])

    def fade(self) -> list[list[float]]:
        return self.layers.get("fade", [[]])

    # ── Term evaluation ─────────────────────────────────────────

    def _eval_term_at(self, expr, x: int, y: int) -> bool:
        if expr.op == "eq":
            layer = self.layers.get(expr.layer)
            if layer is None:
                return False
            return layer[x][y] == expr.value
        elif expr.op == "and":
            return all(self._eval_term_at(a, x, y) for a in expr.args)
        elif expr.op == "or":
            return any(self._eval_term_at(a, x, y) for a in expr.args)
        elif expr.op == "not":
            return not self._eval_term_at(expr.args[0], x, y)
        return False

    def _compute_terms(self):
        """Recompute all term cell lists, excluding landmarks."""
        for term in self.ir.terms:
            cells = []
            for x in range(self.grid_w):
                for y in range(self.grid_h):
                    if (x, y) in self.landmark_cells:
                        continue
                    if self._eval_term_at(term.expr, x, y):
                        cells.append((x, y))
            self.term_cells[term.name] = cells

    # ── Condition evaluation ────────────────────────────────────

    def _eval_condition(self, cond: ConditionNode, agent: Agent) -> bool:
        op = cond.op

        if op == "and":
            return all(self._eval_condition(c, agent) for c in cond.args)
        if op == "or":
            return any(self._eval_condition(c, agent) for c in cond.args)
        if op == "prop_true":
            v = agent.get(cond.prop)
            return bool(v)
        if op == "prop_false":
            v = agent.get(cond.prop)
            return not v
        if op == "gt":
            v = agent.get(cond.prop)
            return v is not None and v > cond.value
        if op == "gte":
            v = agent.get(cond.prop)
            return v is not None and v >= cond.value
        if op == "eq":
            v = agent.get(cond.prop)
            return v == cond.value
        if op == "gt_expr":
            v = agent.get(cond.prop)
            # Evaluate simple expression like "3 + index * 2"
            try:
                threshold = eval(str(cond.value), {"index": agent.index})
            except Exception:
                threshold = 0
            return v is not None and v > threshold
        if op == "at_landmark":
            cells = self.landmarks.get(cond.name, [])
            return agent.pos in cells
        if op == "not_at_landmark":
            cells = self.landmarks.get(cond.name, [])
            return agent.pos not in cells
        if op == "at_claimed":
            return agent.claimed is not None and agent.pos == agent.claimed
        if op == "landmark_free":
            lock = self.landmark_locks.get(cond.name, -1)
            return lock == -1 or lock == agent.index
        if op == "standing_on_term":
            cells = self.term_cells.get(cond.name, [])
            return agent.pos in cells
        if op == "cell_is_lit" or op == "on_lit":
            cur = self.current()
            x, y = agent.pos
            return cur[x][y] if 0 <= x < self.grid_w and 0 <= y < self.grid_h else False
        if op == "at_any_landmark":
            return agent.pos in self.landmark_cells
        if op == "near_lit":
            cur = self.current()
            ax, ay = agent.pos
            for dx in (-1, 0, 1):
                for dy in (-1, 0, 1):
                    if dx == 0 and dy == 0:
                        continue
                    nx, ny = ax + dx, ay + dy
                    if 0 <= nx < self.grid_w and 0 <= ny < self.grid_h:
                        if cur[nx][ny]:
                            return True
            return False
        if op == "term_exists":
            name = cond.name
            cells = self.term_cells.get(name, [])
            if "available" in name or name.startswith("avail"):
                cells = self._available(name, agent)
            else:
                cells = self._available(name, agent)
            return len(cells) > 0
        if op == "term_empty":
            cells = self.term_cells.get(cond.name, [])
            return len(cells) == 0

        return False

    def _available(self, term_name: str, agent: Agent) -> list[tuple[int, int]]:
        """Get unclaimed cells for a term."""
        cells = self.term_cells.get(term_name, [])
        claimed_by_others = set()
        for other in self.agents:
            if other.index != agent.index and other.claimed is not None:
                claimed_by_others.add(other.claimed)
        return [c for c in cells if c not in claimed_by_others]

    # ── Action execution ────────────────────────────────────────

    def _exec_action(self, action: ActionNode, agent: Agent) -> object | None:
        """Execute a single action. Returns _DONE if 'done', _PATHFIND_USED if pathfind was computed."""
        act = action.action

        if act == "done":
            return _DONE

        if act == "set":
            val = action.value
            if val == "true" or val is True:
                val = True
            elif val == "false" or val is False:
                val = False
            agent.set(action.prop, val)
            return None

        if act == "decrement":
            v = agent.get(action.prop) or 0
            agent.set(action.prop, v - 1)
            return None

        if act == "increment":
            v = agent.get(action.prop) or 0
            agent.set(action.prop, v + 1)
            return None

        if act == "reset":
            p = action.prop
            # Find the property type to choose default
            for pdef in self.ir.agents.properties:
                if pdef.name == p:
                    agent.set(p, pdef.default)
                    return None
            agent.set(p, 0)
            return None

        if act == "clear_claimed":
            agent.claimed = None
            return None

        if act == "clear_path":
            agent.path = []
            return None

        if act == "pause":
            agent.set("pause_counter", action.value)
            return None

        if act == "release":
            lm_name = action.landmark
            if self.landmark_locks.get(lm_name) == agent.index:
                self.landmark_locks[lm_name] = -1
            return None

        if act == "claim":
            self.landmark_locks[action.landmark] = agent.index
            return None

        if act == "bob":
            # Alternate between landmark cell and one cell above
            lm_cells = self.landmarks.get(action.landmark, [])
            spot = lm_cells[agent.index % len(lm_cells)] if lm_cells else agent.pos
            if agent.pos[1] == spot[1]:
                # At landmark row → step up
                agent.path = [(spot[0], spot[1] - 1)]
            else:
                # Above → step back down
                agent.path = [spot]
            return None

        if act == "step_up":
            agent.path = [(agent.pos[0], agent.pos[1] - 1)]
            return None

        if act == "seek":
            lm_cells = self.landmarks.get(action.landmark, [])
            if lm_cells:
                dest = lm_cells[agent.index % len(lm_cells)]
                agent.path = self._pathfind(agent, dest)
                agent.claimed = dest
            return _PATHFIND_USED

        if act == "seek_nearest":
            term_name = action.term
            avail = self._available(term_name, agent)
            if avail:
                dest = self._closest(agent.pos, avail)
                agent.path = self._pathfind(agent, dest)
                if agent.path:
                    agent.claimed = dest
            return _PATHFIND_USED

        if act == "pickup":
            src = action.source
            if src == "board":
                x, y = agent.pos
                if 0 <= x < self.grid_w and 0 <= y < self.grid_h:
                    self.current()[x][y] = False
                    self.fade()[x][y] = 0.0
            # From dumpster: no board change
            agent.set("carrying", True)
            return None

        if act == "place":
            x, y = agent.pos
            if 0 <= x < self.grid_w and 0 <= y < self.grid_h:
                self.current()[x][y] = True
                self.fade()[x][y] = 0.0
            return None

        if act == "discard":
            # No board change — pixel is just dropped
            return None

        if act == "despawn":
            agent.active = False
            agent.path = []
            agent.claimed = None
            agent.wait_counter = 0
            return _DONE

        if act == "wander":
            avoid_terms = action.avoid or []
            chance = action.chance or 25
            forced_cond = action.forced_condition

            ax, ay = agent.pos
            safe_moves = []
            near_lit = False

            for dx in (-1, 0, 1):
                for dy in (-1, 0, 1):
                    if dx == 0 and dy == 0:
                        continue
                    nx, ny = ax + dx, ay + dy
                    if nx < 0 or nx >= self.grid_w or ny < 0 or ny >= self.grid_h:
                        continue
                    # Check avoidance
                    skip = False
                    if "lit" in avoid_terms:
                        if self.current()[nx][ny]:
                            near_lit = True
                            skip = True
                    if "landmarks" in avoid_terms:
                        if (nx, ny) in self.landmark_cells:
                            skip = True
                    if not skip:
                        safe_moves.append((nx, ny))

            # Check if current position triggers forced movement
            cur_on_lit = self.current()[ax][ay]
            cur_on_landmark = (ax, ay) in self.landmark_cells
            if cur_on_lit or cur_on_landmark:
                near_lit = True

            should_force = False
            if forced_cond and "near lit" in forced_cond:
                should_force = near_lit

            if safe_moves:
                if should_force:
                    choice = random.choice(safe_moves)
                    agent.path = [choice]
                elif random.randint(1, 100) <= chance:
                    choice = random.choice(safe_moves)
                    agent.path = [choice]
            return None

        if act == "fade_update":
            # Handled separately in tick()
            return None

        return None

    # ── Body evaluation (rules, if/else blocks) ─────────────────

    def _exec_body(self, body: list, agent: Agent) -> tuple[bool, bool]:
        """
        Execute a list of actions/if-nodes.
        Returns (done, used_pathfind).
        """
        done = False
        used_pathfind = False
        for node in body:
            if isinstance(node, IfNode):
                if self._eval_condition(node.condition, agent):
                    d, u = self._exec_body(node.then_body, agent)
                else:
                    d, u = self._exec_body(node.else_body, agent)
                done = done or d
                used_pathfind = used_pathfind or u
                if done:
                    return done, used_pathfind
            elif isinstance(node, ActionNode):
                result = self._exec_action(node, agent)
                if result is _DONE:
                    return True, used_pathfind
                if result is _PATHFIND_USED:
                    used_pathfind = True
            if done:
                break
        return done, used_pathfind

    # ── Pathfinding helpers ─────────────────────────────────────

    def _pathfind(self, agent: Agent, dest: tuple[int, int]) -> list[tuple[int, int]]:
        lit = set()
        cur = self.current()
        for x in range(self.grid_w):
            for y in range(self.grid_h):
                if cur[x][y]:
                    lit.add((x, y))
        positions = [a.pos for a in self.agents if a.active]
        return find_path(
            agent.pos, dest, self.grid_w, self.grid_h,
            agent.index, positions, lit, self.ir.pathfinding
        )

    def _closest(self, pos: tuple[int, int],
                 candidates: list[tuple[int, int]]) -> tuple[int, int]:
        best = candidates[0]
        best_d = math.hypot(pos[0] - best[0], pos[1] - best[1])
        for c in candidates[1:]:
            d = math.hypot(pos[0] - c[0], pos[1] - c[1])
            if d < best_d:
                best_d = d
                best = c
        return best

    # ── Goal update ─────────────────────────────────────────────

    def _update_goal(self):
        if self.ir.goal.type == "clock":
            if self.clock:
                vt = self.clock.advance()
            else:
                vt = time.time()
            lt = time.localtime(vt)
            h, m = lt.tm_hour, lt.tm_min
            if m != self.last_minute:
                self.last_minute = m
                target = self.target()
                new_pixels = render_time(h, m, self.grid_w, self.grid_h,
                                         self.ir.goal.layout)
                for x in range(self.grid_w):
                    for y in range(self.grid_h):
                        target[x][y] = (x, y) in new_pixels
                # Reset agent state on target change (matches C++ apply_pending_target)
                for lm_name in self.landmark_locks:
                    self.landmark_locks[lm_name] = -1
                for a in self.agents:
                    a.claimed = None
                    a.path = []
                    a.wait_counter = 0
                    a.set("bored_counter", 0)
                    a.set("wash_counter", 0)
                    a.set("washed", False)
                    a.set("pause_counter", 0)

    def _update_goal_static(self, hour: int, minute: int):
        """Set a static time target (for testing)."""
        target = self.target()
        new_pixels = render_time(hour, minute, self.grid_w, self.grid_h,
                                 self.ir.goal.layout)
        for x in range(self.grid_w):
            for y in range(self.grid_h):
                target[x][y] = (x, y) in new_pixels
        self.last_minute = minute

    # ── Main tick ───────────────────────────────────────────────

    def _try_spawn(self):
        """In pool mode, spawn an agent if the spawning condition is met."""
        if not self.pool_mode or not self.ir.spawning:
            return
        self.spawn_timer += 1
        if self.spawn_timer < self.ir.spawning.every_ticks:
            return
        self.spawn_timer = 0

        # Check spawn condition (use a dummy agent for eval — condition is global)
        if self.ir.spawning.condition:
            # Create a temporary context to evaluate global conditions like "missing exist"
            dummy = Agent(index=-1, pos=(0, 0), last_pos=(0, 0))
            if not self._eval_condition(self.ir.spawning.condition, dummy):
                return

        # Find an inactive agent slot
        sp = self.spawn_point or (0, 0)
        for a in self.agents:
            if not a.active:
                a.active = True
                a.pos = sp
                a.last_pos = sp
                a.path = []
                a.claimed = None
                a.wait_counter = 0
                for prop in self.ir.agents.properties:
                    a.props[prop.name] = prop.default
                return
        # All slots full — no spawn this tick

    def tick(self):
        """Run one physics tick."""
        # 1. Update goal
        self._update_goal()

        # 2. Compute terms
        self._compute_terms()

        # 3. Spawning (pool mode)
        self._try_spawn()

        # 4. Movement phase — agents with paths try to step
        for agent in self.agents:
            if not agent.active:
                continue
            agent.last_pos = agent.pos
            if agent.path:
                next_step = agent.path[0]
                # Collision check
                collision = False
                for other in self.agents:
                    if other.index != agent.index and other.active and other.pos == next_step:
                        collision = True
                        break

                if collision:
                    # Execute collision rules
                    agent.wait_counter += 1
                    self._exec_collision(agent)
                    continue

                agent.pos = next_step
                agent.path.pop(0)
                agent.wait_counter = 0

        # 5. Behavior phase
        pathfinds_used = 0
        limit = self.ir.behavior.pathfind_limit
        for agent in self.agents:
            if not agent.active:
                continue
            if agent.path or agent.wait_counter > 0:
                continue
            # Pause check
            pc = agent.get("pause_counter") or 0
            if pc > 0:
                agent.set("pause_counter", pc - 1)
                continue

            agent.set("bored", False)

            used_pathfind = False
            for rule in self.ir.behavior.rules:
                if self._eval_condition(rule.condition, agent):
                    done, up = self._exec_body(rule.body, agent)
                    used_pathfind = used_pathfind or up
                    break  # First matching rule fires

            if used_pathfind:
                pathfinds_used += 1
                if pathfinds_used >= limit:
                    break

        # 6. Fade updates
        self._update_fades()

        self.tick_count += 1

    def _exec_collision(self, agent: Agent):
        """Execute collision rules from the IR."""
        for node in self.ir.collision.body:
            if isinstance(node, IfNode):
                if self._eval_condition(node.condition, agent):
                    self._exec_body(node.then_body, agent)
                else:
                    self._exec_body(node.else_body, agent)
            elif isinstance(node, ActionNode):
                self._exec_action(node, agent)

    def _update_fades(self):
        """Increment fade for all lit cells."""
        if "current" not in self.layers or "fade" not in self.layers:
            return
        cur = self.current()
        fade = self.fade()
        for x in range(self.grid_w):
            for y in range(self.grid_h):
                if cur[x][y] and fade[x][y] < 1.0:
                    fade[x][y] = min(fade[x][y] + 0.032, 1.0)

    # ── State queries (for simulator rendering) ─────────────────

    def extras_count(self) -> int:
        return len(self.term_cells.get("extras", []))

    def missing_count(self) -> int:
        return len(self.term_cells.get("missing", []))

    def is_converged(self) -> bool:
        return self.extras_count() == 0 and self.missing_count() == 0

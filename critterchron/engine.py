import datetime
import heapq
import json
import random
import re
from dataclasses import dataclass, field

# Highest IR schema version this engine can interpret. Bump in lockstep
# with `compiler.IR_VERSION` when adding new opcodes, fields, condition
# kinds, or wire-format changes that older parsers would misinterpret.
# The HAL interpreter enforces the same bound from its own copy so old
# devices cleanly refuse newer blobs instead of silently mis-parsing.
# v5: tile markers layer (MARKERS_SPEC.md).
# v6: night-mode per-marker ramp overrides (compiler.py IR_VERSION=6 comment).
SUPPORTED_IR_VERSION = 6

# Fixed-width per-tile marker array. Matches compiler.MAX_MARKERS and
# the HAL's TileFields::count[] width. Counts are uint8 semantically —
# clamped to [0, 255] on incr/decr/decay.
MAX_MARKERS = 4

@dataclass
class Tile:
    state: bool = False  # The 'actual' physical state
    intended: bool = False  # The 'oracle' state from the clock
    color: tuple = (0, 0, 0)
    # Name of the color that produced `color`, or None. Stashed by the
    # `draw <cname>` handler so `set_night_mode()` can re-resolve the
    # tile against the other palette on a day/night edge — without this,
    # tiles painted before the flip keep their day RGB forever while
    # newly-painted tiles and the marker composite swap live. Mirrors
    # HAL Tile::color_ref (an index on device; a name here because we
    # key night_colors / colors dicts by name in Python).
    color_ref: str = None
    claimant_id: int = None  # Semaphore for logical locks
    # Per-tile marker counts, indexed by marker declaration order
    # (see compiler's ir["markers"][name]["index"]). Default zero —
    # untouched tiles show no marker. Field is always sized
    # MAX_MARKERS even if the program declares fewer, so a lookup by
    # index never IndexErrors on a Tile initialized before the IR
    # was loaded.
    count: list = field(default_factory=lambda: [0] * MAX_MARKERS)

@dataclass
class Agent:
    id: int
    name: str
    pos: list
    prev_pos: list  # Store for lerping
    color: tuple
    config: dict
    # Name of an entry in ir["colors"] (or None). If set and the referenced
    # entry is a cycle, `color` is a snapshot; the live value is resolved via
    # CritterEngine._resolve_color at read time using the tick counter.
    color_ref: str = None
    # Per-agent cycle phase offset, added to the frame counter before the
    # modulo. Randomized at spawn so multiple agents sharing the same cycle
    # color drift out of sync (natural-looking blink) instead of lockstep.
    color_phase: int = 0
    state_enum: int = 0
    remaining_ticks: int = 0  # Counter for lerp windows
    step_duration: int = 1  # Total ticks the current move animation spans
    path: list = field(default_factory=list)
    glitched: bool = False  # Triggers Rainbow state
    state_str: str = "none"
    pc: int = 0
    seek_ticks: int = 0

class CritterEngine:
    def __init__(self, ir, width=16, height=16):
        ir_version = ir.get("ir_version")
        if ir_version is None:
            raise ValueError(
                "IR is missing 'ir_version'. Recompile with a current "
                "compiler.py (expected IR_VERSION in the header)."
            )
        if ir_version > SUPPORTED_IR_VERSION:
            raise ValueError(
                f"IR schema version {ir_version} is newer than this engine "
                f"supports (max {SUPPORTED_IR_VERSION}). Update engine.py."
            )
        self.ir = ir
        self.width = width
        self.height = height
        self.grid = [[Tile() for _ in range(height)] for _ in range(width)]
        self.agents = []
        self.tick_count = 0
        # --trace flag: when true, _process_agent_behavior prints one line
        # per instruction dispatched + a YIELD/DONE marker on return. Off
        # by default (cost is a per-instruction `if`, negligible).
        self.trace = False
        # Night-mode flag. When true and the IR declares a night palette,
        # `_resolve_color_at` prefers `night_colors[ref]` → `night_default`
        # before falling through to the day palette. In the Python sim the
        # `--night` CLI flag flips this; on hardware the sink flips it via
        # Schmitt hysteresis on the ambient brightness floor. Always false
        # for blobs without a night palette — night_colors will be empty
        # and night_default None, so lookup falls straight through.
        self.night_mode = False

        # Initialize initial agents
        for init_agent in self.ir.get("initial_agents", []):
            name = init_agent["name"]
            pos = init_agent["pos"]
            phase = random.randint(0, 65535)
            color = self._resolve_color(name, default=(255, 255, 255),
                                        phase=phase)
            new_agent = Agent(
                id=len(self.agents) + 1,
                name=name,
                pos=list(pos),
                prev_pos=list(pos),
                color=color,
                config={},
                color_ref=name if name in self.ir.get("colors", {}) else None,
                color_phase=phase,
            )
            new_agent.state_str = init_agent["state"]
            self.agents.append(new_agent)
        self.health_metrics = {
            "convergences": 0,
            "glitches": 0,
            "step_contests": 0,
            "failed_seeks": 0,
            "total_intended": 0,
            "total_lit_intended": 0
        }
        
        self.painters = set()
        for agent_name, insts in self.ir.get("behaviors", {}).items():
            if any(i[1].startswith("draw") or i[1].startswith("erase") for i in insts):
                self.painters.add(agent_name)

        # Cache marker decl → (index, rgb, decay_k, decay_t). The
        # index is the authoritative mapping into Tile.count[]. This
        # cache is populated once at engine init; the IR is immutable
        # post-compile, so no need to rebuild per tick.
        self._markers = {}
        for name, spec in self.ir.get("markers", {}).items():
            self._markers[name] = {
                "index": spec["index"],
                "rgb": tuple(spec["rgb"]),
                "decay_k": spec["decay_k"],
                "decay_t": spec["decay_t"],
            }

        # Night-mode per-unit ramp override. Keyed by day-marker name;
        # values hold the night rgb only (decay K/T is a scheduler property
        # and stays on the day entry — see IR_VERSION 6 comment in
        # compiler.py). Absent entry => night renders fall back to the day
        # ramp. The renderer (renderer.py) consults this when
        # `self.night_mode` is true.
        self._night_markers = {}
        for name, spec in self.ir.get("night_markers", {}).items():
            self._night_markers[name] = {
                "rgb": tuple(spec["rgb"]),
            }

    def marker_ramp(self, name):
        """Return the (r, g, b) per-unit ramp coefficient for marker `name`,
        honoring the current night-mode state. Night override falls through
        to the day ramp when the script defines no per-marker night entry."""
        if self.night_mode:
            night = self._night_markers.get(name)
            if night is not None:
                return night["rgb"]
        spec = self._markers.get(name)
        return spec["rgb"] if spec else (0.0, 0.0, 0.0)

    def set_night_mode(self, on):
        """Toggle night mode, sweeping the grid on the edge.

        Tiles cache their paint-time RGB (engine draws snapshot, not live),
        so a naive flag flip would leave pre-flip tiles in yesterday's
        palette forever — see Tile.color_ref. On a real transition we
        re-resolve every lit tile that remembers the color name it was
        painted with. Idempotent: a set that doesn't change the flag
        short-circuits before the sweep. Caveat: cycle colors re-resolve
        at phase 0 (per-tile cycle phase isn't snapshotted); lands as a
        one-frame lockstep on the first frame after a flip. Revisit if
        it becomes visible."""
        on = bool(on)
        if on == self.night_mode:
            return
        self.night_mode = on
        for x in range(self.width):
            for y in range(self.height):
                tile = self.grid[x][y]
                if not tile.state or tile.color_ref is None:
                    continue
                tile.color = self._resolve_color_at(
                    tile.color_ref, 0, tile.color, depth=0,
                )

    def _resolve_color(self, ref, default=(255, 255, 255), phase=0):
        """Resolve a color reference (name) to an (r,g,b) tuple.

        Static colors return their literal tuple. Cycles walk their entries
        using `self.tick_count + phase` as the frame counter — the sim has
        no separate render loop, so cycle advancement is tied to physics
        ticks. `phase` is the caller's per-agent offset (0 for landmarks /
        shared lookups; agent.color_phase for per-agent resolution). Nested
        references (cycle entry referring to another named color) are
        resolved recursively; guard against cycles with a depth limit."""
        return self._resolve_color_at(ref, self.tick_count + phase, default, depth=0)

    def _resolve_color_at(self, ref, frame, default, depth, skip_night=False):
        if depth > 4:
            return default
        # Night palette takes priority when enabled, but only for the
        # outermost lookup — once we've resolved a night entry to a name
        # reference (e.g. `night_default: warmwhite`), subsequent recursion
        # uses the *day* palette to look up that name. Otherwise a night
        # name-ref to an unmapped day color would bounce back into
        # night_default and never reach a real RGB tuple.
        if self.night_mode and not skip_night:
            night = self.ir.get("night_colors", {}).get(ref)
            if night is None:
                night = self.ir.get("night_default")
            if night is not None:
                if isinstance(night, tuple):
                    return night
                if isinstance(night, list):
                    return tuple(night)
                if isinstance(night, str):
                    # Resolve through day palette from here down.
                    return self._resolve_color_at(
                        night, frame, default, depth + 1, skip_night=True,
                    )
                if isinstance(night, dict) and night.get("kind") == "cycle":
                    # Night palette supports cycles too — cheap, costs
                    # nothing when unused. Entry names resolve via the day
                    # palette for the same reason as string refs above.
                    entries = night.get("entries", [])
                    total = sum(max(1, int(f)) for _, f in entries)
                    if total > 0:
                        t = frame % total
                        for sub_name, frames in entries:
                            frames = max(1, int(frames))
                            if t < frames:
                                return self._resolve_color_at(
                                    sub_name, frame, default, depth + 1,
                                    skip_night=True,
                                )
                            t -= frames
                return default
        d = self.ir.get("colors", {}).get(ref)
        if d is None:
            return default
        if isinstance(d, tuple):
            return d
        if isinstance(d, list):
            return tuple(d)
        if isinstance(d, dict) and d.get("kind") == "cycle":
            entries = d.get("entries", [])
            total = sum(max(1, int(f)) for _, f in entries)
            if total <= 0:
                return default
            t = frame % total
            for sub_name, frames in entries:
                frames = max(1, int(frames))
                if t < frames:
                    return self._resolve_color_at(sub_name, frame, default, depth + 1)
                t -= frames
        return default

    def _agent_color(self, agent):
        if agent.color_ref is not None:
            return self._resolve_color(agent.color_ref, default=agent.color,
                                       phase=agent.color_phase)
        return agent.color

    def sync_time(self, font_data):
        """Updates the 'intended' layer based on system RTC."""
        self.sync_time_at(font_data, datetime.datetime.now())

    def sync_time_at(self, font_data, now):
        """Updates the 'intended' layer for a specific wall-clock instant.
        Used by the dump harness so reference traces are reproducible."""
        time_str = now.strftime("%H%M")

        # Clear intended layer
        for x in range(self.width):
            for y in range(self.height):
                self.grid[x][y].intended = False

        # Anchor layout: square-ish grids stack HH over MM (2x2 of glyphs);
        # wide grids lay the digits in a single row (HH MM). The 5x6 glyphs
        # need 5 cols + 1 gap each, so a 1x4 row wants width >= ~26 to look
        # balanced. Threshold on aspect ratio.
        if self.width >= 2 * self.height:
            # Wide: 1x4. 4 glyphs * 5 cols + 3 gaps of 1 + a wider HH/MM gap of 2
            # = 5+1+5+2+5+1+5 = 24 cols. Center in self.width.
            row_width = 24
            x0 = max(0, (self.width - row_width) // 2)
            y0 = max(0, (self.height - 6) // 2)
            coords = [(x0, y0), (x0 + 6, y0), (x0 + 13, y0), (x0 + 19, y0)]
        else:
            # Square: HH at top (y=1), MM at bottom (y=8 for 16-tall).
            coords = [(2, 1), (8, 1), (2, 8), (8, 8)]

        for i, char in enumerate(time_str):
            glyph = font_data.get(char, [0]*6)
            off_x, off_y = coords[i]
            for row_idx, row_val in enumerate(glyph):
                for col_idx in range(5):
                    if row_val & (0x80 >> col_idx):
                        gx, gy = off_x + col_idx, off_y + row_idx
                        if 0 <= gx < self.width and 0 <= gy < self.height:
                            self.grid[gx][gy].intended = True

    _PF_DEFAULTS = {
        "max_nodes": 512,
        "penalty_lit": 10,
        "penalty_occupied": 20,
        "diagonal": False,
        "diagonal_cost": None,
        "step_rate": 2,
    }

    def _pf(self, agent_name, key):
        """Resolve a pathfinding config value for `agent_name`.

        Lookup chain: per_agent[name] -> per_agent['all'] -> top-level (for
        diagonal / diagonal_cost only) -> hardcoded default."""
        pf = self.ir.get("pathfinding", {})
        per = pf.get("per_agent", {})
        if agent_name in per and key in per[agent_name]:
            return per[agent_name][key]
        if "all" in per and key in per["all"]:
            return per["all"][key]
        if key in ("diagonal", "diagonal_cost") and key in pf:
            return pf[key]
        return self._PF_DEFAULTS.get(key)

    def _get_a_star_path(self, start, target, agent_name):
        """Bounded A* with best-so-far fallback.

        Returns a path (excluding start) to `target`. If the node budget is
        exhausted, returns the path to the closest-by-heuristic explored node
        and bumps `failed_seeks` so telemetry sees budget pressure. If the
        target happens to be the start, or no progress was possible, returns
        the empty list \u2014 callers treat that as 'no move this tick'."""
        start = tuple(start)
        target = tuple(target)
        if start == target:
            return []

        diagonal = self._pf(agent_name, "diagonal")
        diag_cost = self._pf(agent_name, "diagonal_cost") if diagonal else None
        max_nodes = self._pf(agent_name, "max_nodes")
        penalty_lit = self._pf(agent_name, "penalty_lit")
        penalty_occupied = self._pf(agent_name, "penalty_occupied")
        if diagonal:
            moves = [(0, 1, 1.0), (0, -1, 1.0), (1, 0, 1.0), (-1, 0, 1.0),
                     (1, 1, diag_cost), (1, -1, diag_cost),
                     (-1, 1, diag_cost), (-1, -1, diag_cost)]
        else:
            moves = [(0, 1, 1.0), (0, -1, 1.0), (1, 0, 1.0), (-1, 0, 1.0)]

        def heuristic(node):
            dx = abs(target[0] - node[0])
            dy = abs(target[1] - node[1])
            if diagonal:
                # Octile: min(dx,dy) diagonal steps + |dx-dy| orthogonal steps
                return diag_cost * min(dx, dy) + abs(dx - dy)
            return dx + dy

        queue = [(0, start)]
        came_from = {start: None}
        cost_so_far = {start: 0}
        nodes_searched = 0

        # Track the closest-to-target explored node so we can fall back on it
        # when the budget is exhausted. Initialized to start so a zero-progress
        # exhaustion returns an empty path (caller treats as 'don't move').
        best_node = start
        best_h = heuristic(start)

        def reconstruct(node):
            path = []
            while node is not None:
                path.append(node)
                node = came_from[node]
            return path[::-1][1:]  # drop start

        while queue:
            _, current = heapq.heappop(queue)
            nodes_searched += 1

            if current == target:
                return reconstruct(current)

            h = heuristic(current)
            if h < best_h:
                best_h = h
                best_node = current

            if nodes_searched >= max_nodes:
                self.health_metrics["failed_seeks"] += 1
                return reconstruct(best_node)

            for dx, dy, step_cost in moves:
                next_node = (current[0] + dx, current[1] + dy)
                if 0 <= next_node[0] < self.width and 0 <= next_node[1] < self.height:
                    new_cost = cost_so_far[current] + step_cost
                    if self.grid[next_node[0]][next_node[1]].state:
                        new_cost += penalty_lit
                    if any(a.pos == list(next_node) for a in self.agents):
                        new_cost += penalty_occupied

                    if next_node not in cost_so_far or new_cost < cost_so_far[next_node]:
                        cost_so_far[next_node] = new_cost
                        priority = new_cost + heuristic(next_node)
                        heapq.heappush(queue, (priority, next_node))
                        came_from[next_node] = current

        # Queue drained without reaching target \u2014 target is unreachable from start
        # given current costs. Return best-so-far (may be empty path).
        return reconstruct(best_node)

    def _effective_state(self, x, y):
        if self.grid[x][y].state: return True
        return (x, y) in self._occupied_positions

    def tick(self):
        """Executes one simulation heartbeat."""
        self.tick_count += 1
        
        # Precompute occupied positions for fast lookup this tick (only non-painters act as physical pixels)
        self._occupied_positions = {tuple(a.pos) for a in self.agents if a.name not in self.painters}
        
        # 1. Clear claims from previous tick
        for x in range(self.width):
            for y in range(self.height):
                self.grid[x][y].claimant_id = None

        # 1.5. Spawning
        for spawn_rule in self.ir.get("spawning", []):
            if self.tick_count % spawn_rule["interval"] == 0:
                cond = spawn_rule["condition"]
                if cond == "always" or self._check_condition(cond):
                    atype = spawn_rule["agent_type"]
                    count = sum(1 for a in self.agents if a.name == atype)
                    limit = self.ir.get("agents", {}).get(atype, {}).get("limit", 0)
                    if count < limit:
                        lmark = spawn_rule["landmark"]
                        pts = self.ir.get("landmarks", {}).get(lmark, {}).get("points", [])
                        if pts:
                            spawn_pos = list(random.choice(pts))
                            cname = spawn_rule.get("color")
                            if not cname: cname = atype
                            phase = random.randint(0, 65535)
                            color = self._resolve_color(cname, default=(255, 255, 255),
                                                        phase=phase)
                            new_agent = Agent(
                                id=len(self.agents) + 1,
                                name=atype,
                                pos=spawn_pos,
                                prev_pos=list(spawn_pos),
                                color=color,
                                config={},
                                color_ref=cname if cname in self.ir.get("colors", {}) else None,
                                color_phase=phase,
                            )
                            self.agents.append(new_agent)

        # 2. Process Agent Logic
        for agent in self.agents:
            if agent.glitched: continue
            
            if agent.remaining_ticks > 0:
                agent.remaining_ticks -= 1
                if agent.remaining_ticks == 0:
                    agent.prev_pos = list(agent.pos)
                else:
                    continue

            self._process_agent_behavior(agent)

        # 3. Conflict Resolution (Lower ID Wins)
        self.agents.sort(key=lambda a: a.id)

        # 3.5. Eager marker decay. For each declared marker with
        # decay_k > 0, decrement every cell's count by K every T ticks.
        # K/0 is invalid (rejected at compile time). 0/T is "never
        # decay" — matching any K=0 — so we short-circuit. Decay runs
        # AFTER agent action so deposits land before they bleed off.
        for name, spec in self._markers.items():
            k = spec["decay_k"]
            t = spec["decay_t"]
            if k <= 0 or t <= 0:
                continue
            if self.tick_count % t != 0:
                continue
            idx = spec["index"]
            for x in range(self.width):
                col = self.grid[x]
                for y in range(self.height):
                    cnt = col[y].count[idx]
                    if cnt:
                        col[y].count[idx] = max(0, cnt - k)

        # 4. Convergence Check
        matches = 0
        total = self.width * self.height
        intended_count = 0
        lit_intended_count = 0
        
        for x in range(self.width):
            for y in range(self.height):
                eff = self._effective_state(x, y)
                t_intended = self.grid[x][y].intended
                
                if eff == t_intended:
                    matches += 1
                    
                if t_intended:
                    intended_count += 1
                    if eff:
                        lit_intended_count += 1
                        
        if matches == total:
            self.health_metrics["convergences"] += 1
            
        self.health_metrics["total_intended"] += intended_count
        self.health_metrics["total_lit_intended"] += lit_intended_count

    def dump_state_jsonl(self):
        """Serialize post-tick state as a single JSON Lines record.

        The schema is the contract the HAL smoke-test diffs against. If you
        change it, bump `SUPPORTED_IR_VERSION` and update `ir_encoder.py`
        and the C++ interpreter's dump path in lockstep.

        Tiles are packed as two bitmaps over the grid (row-major: x outer,
        y inner) plus a sparse list of per-tile colors for any non-black
        painted tile. Agents are listed in id order with position and
        color only \u2014 continuation state (pc, remaining_ticks) is
        intentionally omitted because it is not observable to the HAL's
        LedSink and would couple the dump to interpreter internals."""
        state_bits = []
        intended_bits = []
        colors = []
        # Sparse marker-count records. One entry per (cell, marker)
        # with a non-zero count — most tiles are at zero, so a dense
        # W*H*M array would blow up the record size on big boards.
        # Each entry: [x, y, marker_index, count]. Marker names are
        # carried in the `markers` header so the reader can resolve
        # indices back to declared names.
        markers_sparse = []
        active_marker_indices = [spec["index"]
                                 for spec in self._markers.values()]
        for x in range(self.width):
            for y in range(self.height):
                t = self.grid[x][y]
                state_bits.append(1 if t.state else 0)
                intended_bits.append(1 if t.intended else 0)
                if t.state and t.color != (0, 0, 0):
                    colors.append([x, y, list(t.color)])
                for idx in active_marker_indices:
                    if t.count[idx]:
                        markers_sparse.append([x, y, idx, t.count[idx]])
        agents = [
            {"id": a.id, "name": a.name, "pos": list(a.pos),
             "color": list(self._agent_color(a)), "glitched": a.glitched}
            for a in sorted(self.agents, key=lambda a: a.id)
        ]
        # Marker declarations (name -> index) so consumers can interpret
        # the sparse list. Kept compact; omitted if no markers declared
        # so pre-v5 consumers diffing against pre-v5 dumps don't see an
        # unexpected key.
        marker_header = (
            {name: spec["index"] for name, spec in self._markers.items()}
            if self._markers else None
        )
        record = {
            "tick": self.tick_count,
            "w": self.width,
            "h": self.height,
            "state": state_bits,
            "intended": intended_bits,
            "tile_colors": colors,
            "agents": agents,
            "metrics": dict(self.health_metrics),
        }
        if marker_header is not None:
            record["markers"] = marker_header
            record["tile_markers"] = markers_sparse
        return json.dumps(record, separators=(",", ":")) + "\n"

    def _trace_dispatch(self, agent, pc, inst, pre_pos):
        """Deferred trace emit for movement-capable opcodes.

        Same format as the eager trace print, plus a `→(nx,ny)` suffix when
        the opcode relocated the agent. Callers pass the pre-execution
        position so the suffix reflects the actual delta caused by *this*
        instruction, not cumulative motion from later ticks."""
        line = (f"[t={self.tick_count}] {agent.name}#{agent.id} "
                f"@({pre_pos[0]},{pre_pos[1]}) "
                f"state={getattr(agent, 'state_str', 'none')} "
                f"pc={pc}  {inst}")
        if tuple(agent.pos) != pre_pos:
            line += f"  \u2192({agent.pos[0]},{agent.pos[1]})"
        print(line)

    def _process_agent_behavior(self, agent):
        """Interprets the IR tokens for the given agent."""
        instructions = self.ir.get("behaviors", {}).get(agent.name, [])
        if not instructions:
            return
            
        pc = agent.pc

        executed = 0
        while pc < len(instructions):
            if executed > 100:
                agent.glitched = True
                self.health_metrics["glitches"] += 1
                if self.trace:
                    print(f"[t={self.tick_count}] {agent.name}#{agent.id} GLITCH")
                return
            executed += 1

            indent, inst = instructions[pc]
            dispatch_pc = pc
            pre_pos = tuple(agent.pos)
            # Movement-capable opcodes defer their trace print to after
            # dispatch so we can tack on a `→(nx,ny)` suffix when the
            # instruction actually relocated the agent. Everything else
            # prints eagerly — pos/state can't change, so there's nothing
            # to wait for.
            is_mover = inst.startswith(("step", "wander", "seek"))
            if self.trace and not is_mover:
                # One line per dispatched instruction, tagged with tick, agent
                # identity, current position, state, and PC. The state and pos
                # shown are pre-execution of this opcode so you can reason
                # about what's being evaluated against what the world looked
                # like when it ran.
                print(f"[t={self.tick_count}] {agent.name}#{agent.id} "
                      f"@({pre_pos[0]},{pre_pos[1]}) "
                      f"state={getattr(agent, 'state_str', 'none')} "
                      f"pc={dispatch_pc}  {inst}")
            
            if inst.startswith("if "):
                condition_met = self._evaluate_if(agent, inst)
                if condition_met:
                    pc += 1
                else:
                    # Skip to next instruction with indent <= current indent
                    pc += 1
                    while pc < len(instructions) and instructions[pc][0] > indent:
                        pc += 1
                continue
                
            elif inst == "else:":
                # If we naturally reach an else, we just finished the True branch of an if
                # Skip to the end of this else block
                pc += 1
                while pc < len(instructions) and instructions[pc][0] > indent:
                    pc += 1
                continue
                
            elif inst == "done":
                agent.pc = 0
                return
                
            elif inst.startswith("set state ="):
                agent.state_str = inst.split("=")[1].strip()
                pc += 1
                continue
                
            elif inst == "erase":
                self.grid[agent.pos[0]][agent.pos[1]].state = False
                pc += 1
                continue
                
            elif inst.startswith("draw"):
                tile = self.grid[agent.pos[0]][agent.pos[1]]
                tile.state = True
                parts = inst.split()
                if len(parts) > 1:
                    cname = parts[1]
                    # Tiles snapshot the current cycle value at paint-time —
                    # no live animation on tiles by design. Use the painting
                    # agent's phase so the snapshot matches what the agent
                    # looked like when it painted.
                    tile.color = self._resolve_color(
                        cname, default=(255, 255, 255), phase=agent.color_phase)
                    # Remember the *name* so set_night_mode() can re-resolve
                    # this tile against the other palette on the next flip.
                    # Bare `draw` (no name) leaves color_ref alone — keeps
                    # whatever the previous paint stashed, so re-drawing a
                    # tile that was first painted with a name still swaps.
                    tile.color_ref = cname
                pc += 1
                continue

            elif inst.startswith("incr marker ") or inst.startswith("decr marker "):
                # `incr marker <name> [N]` / `decr marker <name> [N]`
                # Target is the agent's current cell. N defaults to 1.
                # Count is clamped uint8 (0..255). Matches `draw`/`erase`
                # in that this does not yield and does not terminate.
                parts = inst.split()
                opcode = parts[0]  # incr | decr
                mname = parts[2]
                n = int(parts[3]) if len(parts) > 3 else 1
                marker = self._markers.get(mname)
                if marker is not None:
                    idx = marker["index"]
                    tile = self.grid[agent.pos[0]][agent.pos[1]]
                    # Tile.count is a shared default factory? No — @dataclass
                    # with default_factory gives each instance its own list.
                    # Safe to mutate in place.
                    if opcode == "incr":
                        tile.count[idx] = min(255, tile.count[idx] + n)
                    else:
                        tile.count[idx] = max(0, tile.count[idx] - n)
                pc += 1
                continue

            elif inst.startswith("set color ="):
                cname = inst.split("=", 1)[1].strip()
                if cname in self.ir.get("colors", {}):
                    agent.color_ref = cname
                    agent.color = self._resolve_color(cname, default=agent.color,
                                                      phase=agent.color_phase)
                pc += 1
                continue
                
            elif inst.startswith("pause"):
                ticks = 1
                parts = inst.split()
                if len(parts) > 1:
                    if "-" in parts[1]:
                        low, high = map(int, parts[1].split("-"))
                        ticks = random.randint(low, high)
                    else:
                        ticks = int(parts[1])
                agent.remaining_ticks = ticks
                agent.step_duration = 1
                agent.prev_pos = list(agent.pos)
                agent.pc = pc + 1
                return

            elif inst.startswith("despawn"):
                parts = inst.split()
                if len(parts) == 1:
                    self.agents.remove(agent)
                    return
                target_name = parts[-1]
                for other in self.agents:
                    if other.id != agent.id and other.name == target_name and tuple(other.pos) == tuple(agent.pos):
                        self.agents.remove(other)
                        break
                pc += 1
                continue
                
            elif inst.startswith("step"):
                # Literal relative move: `step (dx, dy)`. No collision check —
                # caller (coati bobbing at pool) accepts the fragility. Target
                # clamped to grid bounds so we can't fall off the edge.
                m = re.search(r'step\s*\(\s*(-?\d+)\s*,\s*(-?\d+)\s*\)', inst)
                dx, dy = (int(m.group(1)), int(m.group(2))) if m else (0, 0)
                nx = max(0, min(self.width  - 1, agent.pos[0] + dx))
                ny = max(0, min(self.height - 1, agent.pos[1] + dy))
                agent.prev_pos = list(agent.pos)
                agent.pos = [nx, ny]
                if agent.name not in self.painters:
                    self._occupied_positions.discard(tuple(agent.prev_pos))
                    self._occupied_positions.add(tuple(agent.pos))
                step_rate = self._pf(agent.name, "step_rate")
                agent.step_duration = step_rate
                agent.remaining_ticks = step_rate
                agent.pc = pc + 1
                if self.trace:
                    self._trace_dispatch(agent, dispatch_pc, inst, pre_pos)
                return

            elif inst.startswith("wander"):
                neighbors = []
                if self._pf(agent.name, "diagonal"):
                    deltas = [(0,1),(0,-1),(1,0),(-1,0),(1,1),(1,-1),(-1,1),(-1,-1)]
                else:
                    deltas = [(0,1),(0,-1),(1,0),(-1,0)]
                for dx, dy in deltas:
                    nx, ny = agent.pos[0]+dx, agent.pos[1]+dy
                    if 0 <= nx < self.width and 0 <= ny < self.height:
                        if "avoiding current" in inst:
                            if self.grid[nx][ny].intended and self._effective_state(nx, ny):
                                continue
                        if "avoiding any" in inst:
                            if self._effective_state(nx, ny):
                                continue
                        neighbors.append((nx, ny))
                if neighbors:
                    agent.prev_pos = list(agent.pos)
                    agent.pos = list(random.choice(neighbors))
                    # Update occupied positions cache if non-painter
                    if agent.name not in self.painters:
                        self._occupied_positions.discard(tuple(agent.prev_pos))
                        self._occupied_positions.add(tuple(agent.pos))
                    step_rate = self._pf(agent.name, "step_rate")
                    agent.step_duration = step_rate
                    agent.remaining_ticks = step_rate
                else:
                    agent.step_duration = 1
                    agent.remaining_ticks = 1
                agent.pc = pc + 1
                if self.trace:
                    self._trace_dispatch(agent, dispatch_pc, inst, pre_pos)
                return

            elif inst.startswith("seek"):
                parsed = self._parse_seek(inst)
                # Timeout bookkeeping: the per-agent seek_ticks counter
                # resets when we land on a different seek PC (started
                # fresh), accumulates on every re-entry for the SAME pc
                # (blocked on the same seek). When the timeout is
                # exceeded, advance past the seek — a failed seek does
                # NOT yield, the caller must post-check.
                if pc != agent.pc:
                    agent.seek_ticks = 0
                agent.seek_ticks += 1

                timeout = parsed["timeout"]
                if timeout is not None and agent.seek_ticks > timeout:
                    self.health_metrics["failed_seeks"] += 1
                    agent.seek_ticks = 0
                    pc += 1
                    if self.trace:
                        self._trace_dispatch(agent, dispatch_pc, inst, pre_pos)
                    continue

                candidates = self._build_seek_candidates(agent, parsed)
                target = self._pick_seek_target(agent, candidates, parsed)

                if target and tuple(target) != tuple(agent.pos):
                    # `missing` / `extra` tile-predicate seeks claim the
                    # target cell so multiple agents don't converge on
                    # the same doomed tile. Gradient seeks don't claim —
                    # pheromone following has no exclusive-access
                    # semantics, and brief overshoot on the cap is
                    # handled by the script (see MARKERS_SPEC §2.4).
                    if parsed["kind"] == "classic" and parsed["target_type"] in (
                        "missing", "extra",
                    ):
                        self.grid[target[0]][target[1]].claimant_id = agent.id
                    path = self._get_a_star_path(agent.pos, target, agent.name)
                    if path:
                        agent.prev_pos = list(agent.pos)
                        agent.pos = list(path[0])
                        if agent.name not in self.painters:
                            self._occupied_positions.discard(tuple(agent.prev_pos))
                            self._occupied_positions.add(tuple(agent.pos))
                        step_rate = self._pf(agent.name, "step_rate")
                        agent.step_duration = step_rate
                        agent.remaining_ticks = step_rate
                    else:
                        agent.step_duration = 1
                        agent.remaining_ticks = 1
                    agent.pc = pc
                    if self.trace:
                        self._trace_dispatch(agent, dispatch_pc, inst, pre_pos)
                    return

                agent.seek_ticks = 0
                pc += 1
                if self.trace:
                    self._trace_dispatch(agent, dispatch_pc, inst, pre_pos)
                continue
                
            pc += 1
            
        # If we fall off the bottom, just pause to avoid infinite loops
        agent.remaining_ticks = 1
        agent.step_duration = 1
        agent.prev_pos = list(agent.pos)
        agent.pc = 0

    # ---- Seek parsing / candidate building (markers + classic) ----

    # Match a canonical gradient seek:
    #   seek highest|lowest marker <m> [op N] [on landmark <l>] [timeout N]
    _SEEK_GRADIENT_RE = re.compile(
        r'^seek\s+(?P<dir>highest|lowest)\s+marker\s+(?P<m>\w+)'
        r'(?:\s+(?P<op>[<>])\s*(?P<n>\d+))?'
        r'(?:\s+on\s+landmark\s+(?P<l>\w+))?'
        r'(?:\s+timeout\s+(?P<t>\d+))?$'
    )

    def _parse_seek(self, inst):
        """Parse a seek instruction into a dict of fields.

        Two top-level shapes:
          - kind='gradient' (seek highest/lowest marker): dir, marker_name,
            marker_op, marker_n, landmark, timeout.
          - kind='classic' (target-first seek): target_kind (agent/landmark/
            None), target_type (name or tile-predicate), state_filter,
            filter_mode (on/not on/None), filter_kind (a predicate OR
            'marker'), filter_marker_name, filter_marker_op,
            filter_marker_n, timeout.

        The caller does NOT need to handle malformed instructions —
        validation happened at compile time. Fields that don't apply for
        a given shape are present and set to None, so dispatch code can
        always read them.
        """
        m = self._SEEK_GRADIENT_RE.match(inst)
        if m:
            return {
                "kind": "gradient",
                "dir": m.group("dir"),
                "marker_name": m.group("m"),
                "marker_op": m.group("op"),
                "marker_n": int(m.group("n")) if m.group("n") is not None else None,
                "landmark": m.group("l"),
                "timeout": int(m.group("t")) if m.group("t") is not None else None,
                # unused in gradient shape but present for uniform dispatch
                "target_kind": None, "target_type": None,
                "state_filter": None, "filter_mode": None, "filter_kind": None,
                "filter_marker_name": None, "filter_marker_op": None,
                "filter_marker_n": None,
            }

        # Classic path. Tokenize and walk the grammar; tolerant to the
        # marker-as-filter suffix inserted by the compiler's canonical-
        # ization pass.
        parts = inst.split()
        i = 1
        if i < len(parts) and parts[i] == "nearest":
            i += 1
        target_kind = None
        if i < len(parts) and parts[i] in ("agent", "landmark"):
            target_kind = parts[i]
            i += 1
        target_type = parts[i] if i < len(parts) else None
        i += 1

        state_filter = None
        if (i + 3 < len(parts) and parts[i] == "with"
                and parts[i + 1] == "state" and parts[i + 2] == "=="):
            state_filter = parts[i + 3]
            i += 4

        filter_mode = None
        filter_kind = None
        filter_marker_name = None
        filter_marker_op = None
        filter_marker_n = None
        if i < len(parts) and parts[i] == "on":
            filter_mode = "on"
            i += 1
        elif i + 1 < len(parts) and parts[i] == "not" and parts[i + 1] == "on":
            filter_mode = "not on"
            i += 2

        if filter_mode is not None and i < len(parts):
            if parts[i] == "marker" and i + 1 < len(parts):
                filter_kind = "marker"
                filter_marker_name = parts[i + 1]
                i += 2
                # Optional comparison tail: `> N` or `< N`.
                if i + 1 < len(parts) and parts[i] in (">", "<"):
                    filter_marker_op = parts[i]
                    try:
                        filter_marker_n = int(parts[i + 1])
                        i += 2
                    except ValueError:
                        pass
            else:
                filter_kind = parts[i]
                i += 1

        timeout = None
        if i + 1 < len(parts) and parts[i] == "timeout":
            try:
                timeout = int(parts[i + 1])
            except ValueError:
                pass

        return {
            "kind": "classic",
            "target_kind": target_kind,
            "target_type": target_type,
            "state_filter": state_filter,
            "filter_mode": filter_mode,
            "filter_kind": filter_kind,
            "filter_marker_name": filter_marker_name,
            "filter_marker_op": filter_marker_op,
            "filter_marker_n": filter_marker_n,
            "timeout": timeout,
            # gradient-only fields, unused:
            "dir": None, "marker_name": None, "marker_op": None,
            "marker_n": None, "landmark": None,
        }

    def _marker_filter_pass(self, tile, op, n):
        """Apply a marker count filter to a count value. `tile` is the
        pre-read count for a single cell (already indexed into). Returns
        True if the filter admits this cell."""
        if op is None:
            # Bare `on marker X` sugar — resolver rewrote it to `> 0`, so
            # this branch should not fire; included for defensiveness.
            return tile > 0
        if op == ">":
            return tile > n
        if op == "<":
            return tile < n
        return False

    def _build_seek_candidates(self, agent, parsed):
        """Build the candidate cell list for a parsed seek."""
        if parsed["kind"] == "gradient":
            marker = self._markers.get(parsed["marker_name"])
            if marker is None:
                return []
            idx = marker["index"]
            # Scope cells to the landmark if given, else whole board.
            if parsed["landmark"] is not None:
                pts = self.ir.get("landmarks", {}).get(
                    parsed["landmark"], {}
                ).get("points", [])
                cells = [(p[0], p[1]) for p in pts]
            else:
                cells = [(x, y) for x in range(self.width)
                         for y in range(self.height)]
            op = parsed["marker_op"]
            n = parsed["marker_n"]
            if op is None:
                # Bare `seek highest marker X` — EXCLUDES zero-count
                # cells (no gradient to follow on empty board). This
                # is the pheromone-follower semantic per MARKERS_SPEC
                # §2.4.
                return [(x, y) for (x, y) in cells
                        if self.grid[x][y].count[idx] > 0]
            # With filter: candidate set is cells satisfying the
            # comparison. `< N` INCLUDES zero-count cells — that's the
            # soft-cap deposit primitive (MARKERS_SPEC §2.4).
            if op == ">":
                return [(x, y) for (x, y) in cells
                        if self.grid[x][y].count[idx] > n]
            if op == "<":
                return [(x, y) for (x, y) in cells
                        if self.grid[x][y].count[idx] < n]
            return []

        # Classic path — target-first, optional on/not-on filter.
        target_kind = parsed["target_kind"]
        target_type = parsed["target_type"]
        state_filter = parsed["state_filter"]

        candidates = []
        if target_kind == "agent":
            candidates = [
                tuple(a.pos) for a in self.agents
                if a.name == target_type and a.id != agent.id
                and (state_filter is None
                     or getattr(a, "state_str", "none") == state_filter)
            ]
        elif target_kind == "landmark":
            pts = self.ir.get("landmarks", {}).get(target_type, {}).get("points", [])
            candidates = [tuple(p) for p in pts]
        elif target_type in ("missing", "extra", "current"):
            for x in range(self.width):
                for y in range(self.height):
                    tile = self.grid[x][y]
                    if tile.claimant_id is not None:
                        continue
                    if target_type == "missing" and tile.intended and not tile.state:
                        candidates.append((x, y))
                    elif target_type == "extra" and not tile.intended and tile.state:
                        candidates.append((x, y))
                    elif target_type == "current" and tile.intended and tile.state:
                        candidates.append((x, y))

        filter_mode = parsed["filter_mode"]
        filter_kind = parsed["filter_kind"]
        if filter_mode and filter_kind:
            if filter_kind == "marker":
                marker = self._markers.get(parsed["filter_marker_name"])
                if marker is None:
                    return []
                idx = marker["index"]
                op = parsed["filter_marker_op"]
                n = parsed["filter_marker_n"]
                want_on = filter_mode == "on"
                candidates = [
                    c for c in candidates
                    if self._marker_filter_pass(
                        self.grid[c[0]][c[1]].count[idx], op, n,
                    ) == want_on
                ]
            else:
                want_on = filter_mode == "on"
                candidates = [
                    c for c in candidates
                    if self._tile_matches(c, filter_kind) == want_on
                ]
        return candidates

    def _pick_seek_target(self, agent, candidates, parsed):
        """Choose the winning target from a candidate list.

        Classic seeks use Manhattan-nearest, matching historical
        behavior. Gradient seeks sort primarily by count (descending for
        `highest`, ascending for `lowest`) and break ties by Manhattan
        distance — so a tied pile of count-0 cells reliably resolves to
        the nearest one.
        """
        if not candidates:
            return None
        ax, ay = agent.pos[0], agent.pos[1]
        if parsed["kind"] != "gradient":
            return min(candidates, key=lambda t: abs(t[0] - ax) + abs(t[1] - ay))
        marker = self._markers.get(parsed["marker_name"])
        if marker is None:
            return None
        idx = marker["index"]
        if parsed["dir"] == "highest":
            # key: (-count, distance) → highest count, tie-break nearest
            key = lambda t: (
                -self.grid[t[0]][t[1]].count[idx],
                abs(t[0] - ax) + abs(t[1] - ay),
            )
        else:
            key = lambda t: (
                self.grid[t[0]][t[1]].count[idx],
                abs(t[0] - ax) + abs(t[1] - ay),
            )
        return min(candidates, key=key)

    def _check_condition(self, condition):
        """Checks the global grid state for triggers."""
        for x in range(self.width):
            for y in range(self.height):
                tile = self.grid[x][y]
                if condition == "missing" and (tile.intended and not tile.state):
                    return True
                if condition == "extra" and (not tile.intended and tile.state):
                    return True
                if condition == "current" and (tile.intended and tile.state):
                    return True
        return False

    def _evaluate_if(self, agent, inst):
        """Evaluate a resolved `if ...:` instruction. Returns True/False.
        Unrecognized forms evaluate to False so the script author gets a
        silent no-op they can spot in simulation rather than a crash."""
        body = inst[:-1].rstrip() if inst.endswith(':') else inst
        ax, ay = agent.pos[0], agent.pos[1]

        m = re.match(r'^if state (==|!=) (\w+)$', body)
        if m:
            eq = getattr(agent, "state_str", "none") == m.group(2)
            return eq if m.group(1) == "==" else not eq

        m = re.match(r'^if random (\d+)%?$', body)
        if m:
            return random.randint(1, 100) <= int(m.group(1))

        # Positional tile predicate: "if on <pred>" or "if standing on <pred>"
        m = re.match(r'^if (?:standing )?on (current|extra|missing)$', body)
        if m:
            return self._tile_matches((ax, ay), m.group(1))

        # Positional landmark proximity: "if (standing )?on landmark <name>"
        m = re.match(r'^if (?:standing )?on landmark (\w+)$', body)
        if m:
            pts = self.ir.get("landmarks", {}).get(m.group(1), {}).get("points", [])
            return tuple(agent.pos) in pts

        # Positional color match: "if standing on color <name>" — true iff
        # this agent's current tile is lit with exactly the referenced RGB.
        m = re.match(r'^if standing on color (\w+)$', body)
        if m:
            color_def = self.ir.get("colors", {}).get(m.group(1))
            if color_def is None:
                return False
            tile = self.grid[ax][ay]
            if not tile.state:
                return False
            if isinstance(color_def, dict):
                # cycle colors: match if tile RGB equals any phase's RGB
                for entry_name, _frames in color_def.get("entries", []):
                    entry_rgb = self.ir.get("colors", {}).get(entry_name)
                    if (isinstance(entry_rgb, (tuple, list))
                            and tuple(tile.color) == tuple(entry_rgb)):
                        return True
                return False
            return tuple(tile.color) == tuple(color_def)

        # Positional agent colocation: "if on agent <name> [with state == <value>]"
        m = re.match(r'^if on agent (\w+)(?: with state == (\w+))?$', body)
        if m:
            name = m.group(1)
            want_state = m.group(2)
            return any(
                a.id != agent.id and a.name == name and tuple(a.pos) == (ax, ay)
                and (want_state is None
                     or getattr(a, "state_str", "none") == want_state)
                for a in self.agents
            )

        # Board-level tile predicate: "if missing" / "if extra" / "if current"
        m = re.match(r'^if (current|extra|missing)$', body)
        if m:
            return self._check_condition(m.group(1))

        # Agent existence: "if agent <name>"
        m = re.match(r'^if agent (\w+)$', body)
        if m:
            name = m.group(1)
            return any(a.name == name and a.id != agent.id for a in self.agents)

        # Landmark existence: "if landmark <name>" (true if landmark is defined)
        m = re.match(r'^if landmark (\w+)$', body)
        if m:
            return m.group(1) in self.ir.get("landmarks", {})

        # --- Marker predicates. Canonical forms only; the compiler
        # pre-pass has already desugared `if trail > 0` etc. ---

        # Board or landmark-scoped boolean negation:
        #   if no marker <m>                 — all cells == 0
        #   if no marker <m> on landmark <l> — all landmark cells == 0
        m = re.match(r'^if no marker (\w+)(?: on landmark (\w+))?$', body)
        if m:
            return self._eval_marker_scope(
                m.group(1), op="no", n=0, landmark=m.group(2),
            )

        # Board or landmark-scoped comparison:
        #   if marker <m> (>|<) N [on landmark <l>]
        m = re.match(
            r'^if marker (\w+)\s*([<>])\s*(\d+)'
            r'(?:\s+on\s+landmark\s+(\w+))?$',
            body,
        )
        if m:
            return self._eval_marker_scope(
                m.group(1), op=m.group(2), n=int(m.group(3)),
                landmark=m.group(4),
            )

        # Self-cell scope:
        #   if on marker <m> (>|<) N
        #   if on no marker <m>
        m = re.match(r'^if on no marker (\w+)$', body)
        if m:
            marker = self._markers.get(m.group(1))
            if marker is None:
                return False
            return self.grid[ax][ay].count[marker["index"]] == 0
        m = re.match(r'^if on marker (\w+)\s*([<>])\s*(\d+)$', body)
        if m:
            marker = self._markers.get(m.group(1))
            if marker is None:
                return False
            cnt = self.grid[ax][ay].count[marker["index"]]
            n = int(m.group(3))
            return (cnt > n) if m.group(2) == ">" else (cnt < n)

        return False

    def _eval_marker_scope(self, mname, op, n, landmark):
        """Evaluate a scoped marker predicate.

        `op` is one of '>', '<', 'no'. 'no' means "all cells in scope
        have count == 0" (a universal predicate); '>' and '<' mean
        "ANY cell in scope satisfies the strict comparison" (existential).
        `landmark` is a landmark name to restrict scope to, or None for
        board-wide.

        Unknown markers evaluate to False — matches the existing
        "unrecognized → False" posture of _evaluate_if, and the
        compiler's canonicalization already rejects unknown marker
        references, so this branch is defensive only.
        """
        marker = self._markers.get(mname)
        if marker is None:
            return False
        idx = marker["index"]
        if landmark is not None:
            pts = self.ir.get("landmarks", {}).get(landmark, {}).get("points", [])
            cells = [(p[0], p[1]) for p in pts]
        else:
            cells = [(x, y) for x in range(self.width) for y in range(self.height)]
        if op == "no":
            return all(self.grid[x][y].count[idx] == 0 for x, y in cells)
        if op == ">":
            return any(self.grid[x][y].count[idx] > n for x, y in cells)
        if op == "<":
            return any(self.grid[x][y].count[idx] < n for x, y in cells)
        return False

    def _tile_matches(self, pos, kind):
        """Does the tile at pos match 'current'/'extra'/'missing' or a named landmark?"""
        x, y = pos
        tile = self.grid[x][y]
        if kind == "current":
            return tile.intended and tile.state
        if kind == "extra":
            return (not tile.intended) and tile.state
        if kind == "missing":
            return tile.intended and not tile.state
        if kind in self.ir.get("landmarks", {}):
            return (x, y) in self.ir["landmarks"][kind]["points"]
        return False

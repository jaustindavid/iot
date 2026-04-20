import re
import sys
import json

IR_VERSION = 1

# Tile-predicate tokens that can appear after `if on` / `seek [nearest]`.
# These are NOT landmark or agent names — they describe cell state.
_TILE_PREDICATES = {"current", "extra", "missing"}


class CritCompiler:
    def __init__(self, width=16, height=16):
        self.width = width
        self.height = height
        self.ir = {
            "ir_version": IR_VERSION,
            "simulation": {"tick_rate": 500},
            "colors": {},
            "landmarks": {},
            "agents": {},
            "behaviors": {},
            "spawning": [],
            "initial_agents": [],
            "pathfinding": {
                "diagonal": False,
                "diagonal_cost": None,
                "per_agent": {},
            },
        }

    def compile(self, file_path):
        with open(file_path, 'r') as f:
            content = f.read()

        # Split by sections, ignoring the header/comment block
        sections = re.split(r'--- (.*?) ---', content)

        for i in range(1, len(sections), 2):
            header = sections[i].strip()
            body = sections[i+1].strip()
            self._parse_section(header, body)

        # Post-passes run after all sections are parsed, so that name
        # resolution has the full landmark + agent declaration set in
        # scope.
        self._resolve_names_in_behaviors()
        self._validate_instructions()
        self._validate_pathfinding()
        self._validate_loops()

        # Validate agent colors
        for agent_name in self.ir["agents"]:
            if agent_name not in self.ir.get("colors", {}):
                raise ValueError(f"Compilation Failed: Agent '{agent_name}' has no color defined in the 'colors' block.")

        for init_agent in self.ir.get("initial_agents", []):
            if init_agent["name"] not in self.ir.get("colors", {}):
                raise ValueError(f"Compilation Failed: Agent '{init_agent['name']}' has no color defined in the 'colors' block.")

        return self.ir

    def _parse_section(self, header, body):
        # Preserve leading whitespace for indentation checks
        lines = [l for l in body.split('\n') if l.strip() and not l.strip().startswith('#')]
        
        if header == "simulation":
            for line in lines:
                line = line.strip()
                if "tick rate" in line:
                    val = re.search(r'(\d+)', line).group(1)
                    self.ir["simulation"]["tick_rate"] = int(val)

        elif header == "colors":
            for line in lines:
                line = line.strip()
                name, val = line.split(':', 1)
                val = val.strip()
                name = name.strip()
                if val.startswith('cycle '):
                    # cycle red 2, green 2, blue 2 — entries are (color_name, render_frames)
                    entries = []
                    for part in val[len('cycle '):].split(','):
                        toks = part.strip().split()
                        if len(toks) < 2:
                            raise ValueError(
                                f"Compilation Failed: cycle color {name!r} "
                                f"entry {part.strip()!r} needs a color name "
                                f"and a frame count."
                            )
                        if len(toks) > 2:
                            # Almost always a missing comma between entries —
                            # silently taking toks[0:2] here would paint the
                            # wrong color on hardware with no explanation.
                            raise ValueError(
                                f"Compilation Failed: cycle color {name!r} "
                                f"entry {part.strip()!r} has extra tokens — "
                                f"did you forget commas between entries? "
                                f"Expected: 'cycle red 2, green 2, blue 2'."
                            )
                        try:
                            frames = int(toks[1])
                        except ValueError:
                            raise ValueError(
                                f"Compilation Failed: cycle color {name!r} "
                                f"entry {part.strip()!r}: frame count "
                                f"{toks[1]!r} is not an integer."
                            )
                        entries.append((toks[0], frames))
                    self.ir["colors"][name] = {"kind": "cycle", "entries": entries}
                else:
                    self.ir["colors"][name] = eval(val)

        elif header == "landmarks":
            current_landmark = None
            for line in lines:
                # Landmark header (no indent)
                if ':' in line and not (line.startswith(' ') or line.startswith('\t')):
                    name, color = line.split(':')
                    current_landmark = name.strip()
                    self.ir["landmarks"][current_landmark] = {"color": color.strip(), "points": []}
                elif current_landmark:
                    # Resolve max_x/max_y into actual grid coordinates
                    raw_point = line.strip().replace('max_x', str(self.width - 1))
                    raw_point = raw_point.replace('max_y', str(self.height - 1))
                    try:
                        self.ir["landmarks"][current_landmark]["points"].append(eval(raw_point))
                    except:
                        pass

        elif header == "agents":
            for line in lines:
                line = line.strip()
                match = re.match(r'up to (\d+) (\w+)', line)
                if match:
                    count, name = match.groups()
                    self.ir["agents"][name] = {"limit": int(count), "states": []}
                
                state_match = re.search(r'state \{(.*?)\}', line)
                if state_match:
                    agent_name = line.split(':')[0].strip()
                    if agent_name not in self.ir["agents"]:
                        self.ir["agents"][agent_name] = {"limit": 0, "states": []}
                    states = [s.strip() for s in state_match.group(1).split(',')]
                    self.ir["agents"][agent_name]["states"] = states
                    
                starts_match = re.search(r'(\w+)\s+starts at\s+\((.*?)\)(?:,\s*state\s*=\s*(\w+))?', line)
                if starts_match:
                    agent_name, coords_str, state_val = starts_match.groups()
                    raw_point = coords_str.replace('max_x', str(self.width - 1))
                    raw_point = raw_point.replace('max_y', str(self.height - 1))
                    try:
                        pos = list(eval(f"({raw_point})"))
                        # Auto-register the type if `starts at` is the only
                        # mention. tortoise.crit has no `up to N` or `state {}`
                        # line — without this, AGENT_TYPES would be empty and
                        # the C++ engine would index into a 0-element array.
                        if agent_name not in self.ir["agents"]:
                            self.ir["agents"][agent_name] = {"limit": 0, "states": []}
                        self.ir["initial_agents"].append({
                            "name": agent_name,
                            "pos": pos,
                            "state": state_val or "none"
                        })
                    except:
                        pass

        elif header == "behavior":
            current_agent = None
            for line in lines:
                # Agent headers MUST have 0 indentation
                if line.endswith(':') and not (line.startswith(' ') or line.startswith('\t')):
                    current_agent = line[:-1].strip()
                    self.ir["behaviors"][current_agent] = []
                elif current_agent:
                    # Keep indentation level to build the CFG
                    if line.strip() and not line.strip().startswith('#'):
                        indent = len(line) - len(line.lstrip())
                        text = line.strip()
                        # Strip inline trailing comments so they don't end up
                        # in the IR token stream (the interpreter doesn't know
                        # they are comments).
                        if '#' in text:
                            text = text.split('#', 1)[0].rstrip()
                        if text:
                            self.ir["behaviors"][current_agent].append((indent, text))

        elif header == "spawning":
            for line in lines:
                # Example: "spawn ant at nest every 10 ticks when missing" [cite: 1]
                match = re.search(r'spawn (\w+) at (\w+) every (\d+) ticks when (\w+)', line)
                if match:
                    agent_type, landmark, interval, condition = match.groups()
                    self.ir["spawning"].append({
                        "agent_type": agent_type,
                        "landmark": landmark,
                        "interval": int(interval),
                        "condition": condition
                    })

        elif header == "pathfinding":
            self._parse_pathfinding_section(lines)

    # Map authored pathfinding keys (as they appear in .crit source) to the
    # canonical IR key names used by the engine. Keep spelling stable — the
    # engine looks up these exact names.
    _PF_KEY_ALIASES = {
        "max nodes":              "max_nodes",
        "max_nodes":              "max_nodes",
        "penalty lit cell":       "penalty_lit",
        "penalty_lit":            "penalty_lit",
        "penalty occupied cell":  "penalty_occupied",
        "penalty_occupied":       "penalty_occupied",
        "step rate":              "step_rate",
        "step_rate":              "step_rate",
        "diagonal":               "diagonal",
        "diagonal cost":          "diagonal_cost",
        "diagonal_cost":          "diagonal_cost",
    }

    def _parse_pathfinding_section(self, lines):
        current_type = None
        for line in lines:
            indent = len(line) - len(line.lstrip())
            stripped = line.strip()
            if '#' in stripped:
                stripped = stripped.split('#', 1)[0].rstrip()
            if not stripped:
                continue

            key, sep, val_str = stripped.partition(':')
            if not sep:
                raise ValueError(f"Pathfinding line missing ':': {line!r}")
            key = key.strip()
            val_str = val_str.strip()

            if indent == 0 and not val_str:
                current_type = key
                self.ir["pathfinding"]["per_agent"].setdefault(current_type, {})
                continue

            canonical = self._PF_KEY_ALIASES.get(key)
            if canonical is None:
                raise ValueError(f"Unknown pathfinding key: {key!r}")
            parsed = self._parse_pf_value(canonical, val_str)

            if indent == 0:
                # Top-level option
                if canonical not in ("diagonal", "diagonal_cost"):
                    raise ValueError(
                        f"Pathfinding key {canonical!r} is per-agent only; "
                        f"place it under an agent-type header (e.g. 'all:')."
                    )
                self.ir["pathfinding"][canonical] = parsed
            else:
                if current_type is None:
                    raise ValueError(
                        f"Pathfinding entry {key!r} appears before any agent-type header"
                    )
                self.ir["pathfinding"]["per_agent"][current_type][canonical] = parsed

    def _parse_pf_value(self, canonical, val_str):
        if canonical == "diagonal":
            if val_str == "allowed":
                return True
            if val_str == "disallowed":
                return False
            raise ValueError(
                f"Pathfinding 'diagonal' must be 'allowed' or 'disallowed', got {val_str!r}"
            )
        if canonical == "diagonal_cost":
            try:
                return float(val_str)
            except ValueError:
                raise ValueError(
                    f"Pathfinding 'diagonal_cost' must be numeric, got {val_str!r}"
                )
        # Numeric keys (max_nodes, penalty_*, step_rate): int if possible, else float.
        try:
            return int(val_str)
        except ValueError:
            try:
                return float(val_str)
            except ValueError:
                raise ValueError(
                    f"Pathfinding {canonical!r} must be numeric, got {val_str!r}"
                )

    def _validate_pathfinding(self):
        pf = self.ir["pathfinding"]
        # Top-level both-or-neither.
        self._check_diagonal_pair(
            "top-level", pf.get("diagonal", False), pf.get("diagonal_cost")
        )
        # Per-agent both-or-neither. Only enforce when the agent block
        # explicitly sets at least one of the two keys.
        for agent, block in pf.get("per_agent", {}).items():
            if "diagonal" in block or "diagonal_cost" in block:
                self._check_diagonal_pair(
                    f"agent {agent!r}",
                    block.get("diagonal", False),
                    block.get("diagonal_cost"),
                )

    def _check_diagonal_pair(self, scope, diagonal, diagonal_cost):
        if diagonal and diagonal_cost is None:
            raise ValueError(
                f"Compilation Failed: pathfinding ({scope}) 'diagonal: allowed' "
                f"requires an explicit 'diagonal_cost:' value."
            )
        if (not diagonal) and diagonal_cost is not None:
            raise ValueError(
                f"Compilation Failed: pathfinding ({scope}) 'diagonal_cost' is set "
                f"but 'diagonal' is not 'allowed' \u2014 author intent is ambiguous."
            )

    def _resolve_names_in_behaviors(self):
        """Rewrite bare names in `if on` / `seek` instructions into disambiguated
        `landmark <name>` or `agent <name>` forms. Tile predicates
        (current/extra/missing) pass through unchanged. Already-disambiguated
        forms (`if on landmark X`, `seek agent X`) are left alone."""

        landmarks = set(self.ir["landmarks"].keys())
        agents = set(self.ir["agents"].keys())
        for ia in self.ir["initial_agents"]:
            agents.add(ia["name"])
        agents |= set(self.ir["behaviors"].keys())

        def classify(name, context):
            in_l = name in landmarks
            in_a = name in agents
            if in_l and in_a:
                raise ValueError(
                    f"Compilation Failed: name {name!r} is both a landmark and "
                    f"an agent. Disambiguate with 'landmark {name}' or "
                    f"'agent {name}' in instruction: {context!r}"
                )
            if in_l:
                return "landmark"
            if in_a:
                return "agent"
            raise ValueError(
                f"Compilation Failed: unknown name {name!r} in instruction "
                f"{context!r} — not a landmark, agent, or tile predicate"
            )

        if_on_re = re.compile(r'^(if on )(\w+)(\s.*)?$')
        if_standing_re = re.compile(r'^(if standing on )(\w+)(\s.*)?$')
        if_exist_re = re.compile(r'^(if )(\w+)$')
        seek_re = re.compile(r'^(seek\s+)(nearest\s+)?(\w+)(\s.*)?$')
        despawn_re = re.compile(r'^despawn\s+(\w+)(\s.*)?$')

        for agent, insts in self.ir["behaviors"].items():
            for idx, (indent, inst) in enumerate(insts):
                colon = ''
                body = inst
                if body.endswith(':'):
                    colon = ':'
                    body = body[:-1]

                m = if_on_re.match(body)
                if m:
                    prefix, name, rest = m.group(1), m.group(2), m.group(3) or ''
                    if name in _TILE_PREDICATES or name in ("landmark", "agent"):
                        continue
                    kind = classify(name, inst)
                    insts[idx] = (indent, f"{prefix}{kind} {name}{rest}{colon}")
                    continue

                m = if_standing_re.match(body)
                if m:
                    prefix, name, rest = m.group(1), m.group(2), m.group(3) or ''
                    if name in _TILE_PREDICATES:
                        continue
                    raise ValueError(
                        f"Compilation Failed: 'if standing on {name}' \u2014 "
                        f"'standing on' only accepts tile predicates "
                        f"({', '.join(sorted(_TILE_PREDICATES))}). "
                        f"For agent/landmark proximity use 'if on {name}'. "
                        f"Instruction: {inst!r}"
                    )

                m = if_exist_re.match(body)
                if m:
                    prefix, name = m.group(1), m.group(2)
                    if name in _TILE_PREDICATES or name in ("landmark", "agent"):
                        continue
                    kind = classify(name, inst)
                    insts[idx] = (indent, f"{prefix}{kind} {name}{colon}")
                    continue

                m = seek_re.match(body)
                if m:
                    seek_prefix = m.group(1)
                    nearest = m.group(2) or ''
                    name = m.group(3)
                    rest = m.group(4) or ''
                    if name in _TILE_PREDICATES or name in ("landmark", "agent"):
                        continue
                    kind = classify(name, inst)
                    insts[idx] = (
                        indent,
                        f"{seek_prefix}{nearest}{kind} {name}{rest}{colon}",
                    )
                    continue

                m = despawn_re.match(body)
                if m:
                    name, rest = m.group(1), m.group(2) or ''
                    if name == "agent":
                        continue  # already disambiguated
                    if name in landmarks:
                        raise ValueError(
                            f"Compilation Failed: cannot 'despawn {name}' \u2014 "
                            f"{name!r} is a landmark, not an agent. Instruction: {inst!r}"
                        )
                    if name not in agents:
                        raise ValueError(
                            f"Compilation Failed: cannot 'despawn {name}' \u2014 "
                            f"{name!r} is not a known agent. Instruction: {inst!r}"
                        )
                    insts[idx] = (indent, f"despawn agent {name}{rest}{colon}")
                    continue

    # Canonical opcode shapes. An instruction must fullmatch one of these
    # (post name-resolution) or the compiler rejects it — silent fall-through
    # on an unknown instruction has bitten us (e.g. `state = running` instead
    # of `set state = running` firing a guarded startup pause every loop).
    _OPCODE_PATTERNS = [
        re.compile(r'if\s+.+:'),
        re.compile(r'else:'),
        re.compile(r'done'),
        re.compile(r'set\s+state\s*=\s*\w+'),
        re.compile(r'set\s+color\s*=\s*\w+'),
        re.compile(r'erase'),
        re.compile(r'draw(\s+\w+)?'),
        re.compile(r'pause(\s+\d+(-\d+)?)?'),
        re.compile(r'despawn(\s+agent\s+\w+)?'),
        re.compile(r'wander(\s+avoiding\s+(current|any))?'),
        re.compile(r'step\s*\(\s*-?\d+\s*,\s*-?\d+\s*\)'),
        re.compile(
            r'seek(\s+nearest)?(\s+(agent|landmark))?\s+\w+'
            r'(\s+(not\s+)?on\s+\w+)?(\s+timeout\s+\d+)?'
        ),
    ]

    def _validate_instructions(self):
        for agent, insts in self.ir.get("behaviors", {}).items():
            for indent, inst in insts:
                if any(p.fullmatch(inst) for p in self._OPCODE_PATTERNS):
                    continue
                raise ValueError(
                    f"Compilation Failed: unrecognized instruction {inst!r} "
                    f"in agent {agent!r}. Check spelling and required keywords "
                    f"(e.g. `state = X` must be `set state = X`)."
                )

    @staticmethod
    def _is_yield(inst):
        """Instructions that pass control back to the scheduler / consume a tick.
        A *failed* `seek` does NOT yield — only `seek ... timeout N` does, because
        the timeout form blocks across ticks."""
        if inst == "despawn":
            return True  # bare despawn: self-removal, tick ends
        first = inst.split(' ', 1)[0]
        if first in ("pause", "step", "wander"):
            return True
        if first == "seek" and " timeout " in f" {inst} ":
            return True
        return False

    def _validate_loops(self):
        # Bare `despawn` (no argument) removes the agent itself and therefore
        # terminates this tick's execution path. `despawn <name>` kills another
        # agent and falls through — it is NOT a terminator.
        terminators = ("done", "despawn")
        for agent, instructions in self.ir.get("behaviors", {}).items():
            # check_path returns (reaches_terminator, path_yielded_before_terminator).
            # A valid agent requires every reachable terminator to be preceded by
            # at least one yield instruction on that path. `path` accumulates the
            # instruction trace so error messages can show which branch failed —
            # essential when an agent has several `done`s.
            def check_path(pc, visited, yielded, path):
                if pc in visited:
                    return False, False  # infinite loop without done

                if pc >= len(instructions):
                    return False, False  # fell off the bottom

                indent, inst = instructions[pc]
                trace = path + [(pc, inst)]

                if inst in terminators:
                    # Bare `despawn` is itself a yield; `done` requires a prior yield.
                    path_yield = yielded or inst == "despawn"
                    if not path_yield:
                        trace_str = "\n".join(
                            f"    [{p}] {t}" for p, t in trace
                        )
                        raise ValueError(
                            f"Compilation Failed: Agent '{agent}' has a path to "
                            f"'{inst}' (instruction {pc}) that never yields "
                            f"(no pause/step/wander/seek-with-timeout/bare-despawn "
                            f"on the way). Add a yield so the agent can make "
                            f"progress each tick.\n"
                            f"  Failing path:\n{trace_str}"
                        )
                    return True, True

                inst_yields = yielded or CritCompiler._is_yield(inst)

                if inst.startswith("if "):
                    true_ok, _ = check_path(pc + 1, visited | {pc}, inst_yields, trace)
                    if not true_ok:
                        return False, False
                    false_pc = pc + 1
                    while false_pc < len(instructions) and instructions[false_pc][0] > indent:
                        false_pc += 1
                    false_ok, _ = check_path(false_pc, visited | {pc}, inst_yields, trace)
                    if not false_ok:
                        return False, False
                    return True, True

                if inst == "else:":
                    skip_pc = pc + 1
                    while skip_pc < len(instructions) and instructions[skip_pc][0] > indent:
                        skip_pc += 1
                    return check_path(skip_pc, visited | {pc}, inst_yields, trace)

                return check_path(pc + 1, visited | {pc}, inst_yields, trace)

            reaches, _ = check_path(0, set(), False, [])
            if not reaches:
                raise ValueError(
                    f"Compilation Failed: Agent '{agent}' has an execution path "
                    f"that can fall off the bottom of its behavior block without "
                    f"hitting a 'done' or bare 'despawn'."
                )


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python compiler.py <file.crit>")
        sys.exit(1)
        
    compiler = CritCompiler()
    ir = compiler.compile(sys.argv[1])
    print(f"Successfully compiled {sys.argv[1]}")
    # print(json.dumps(ir, indent=2))

import re
import sys
import json

# IR_VERSION 5 adds tile markers: `--- colors ---` accepts
# `<name>: ramp (r,g,b) decay K/T`; new opcodes `incr`/`decr`; new
# condition forms `if <marker> [op N] [on ...]` and sugar; new seek
# primitives `seek highest|lowest <marker> [op N] [on landmark X]`.
# See MARKERS_SPEC.md for full grammar.
#
# IR_VERSION 6 adds night-mode marker ramps: inside a `night:` block
# under `--- colors ---`, a line `<name>: ramp (r,g,b)` overrides the
# per-unit ramp coefficient while night_mode_ is engaged. Decay K/T is
# shared with the day marker (that's a scheduler property, not a visual
# one); names must match an existing day marker. Wire format adds a
# NIGHT_MARKERS section (4 tokens per entry: name r g b).
IR_VERSION = 6

# Tile-predicate tokens that can appear after `if on` / `seek [nearest]`.
# These are NOT landmark or agent names — they describe cell state.
_TILE_PREDICATES = {"current", "extra", "missing"}

# Hard cap on distinct marker names per program. Keeps the per-tile
# footprint at MAX_MARKERS bytes and matches the HAL's fixed-size array.
# Bump in lockstep with `CritterEngine.MAX_MARKERS` and the HAL's
# TileFields::count[] width when raising it.
MAX_MARKERS = 4


class CritCompiler:
    def __init__(self, width=16, height=16, preserve_symbolic_coords=False):
        self.width = width
        self.height = height
        # When True, landmark points and initial-agent positions that
        # reference max_x/max_y are kept as symbolic strings (e.g.
        # "max_x-1") in the IR dict instead of being resolved to integers.
        # Used by publish_ir so the on-device parser resolves against the
        # target's real geometry at load time — one blob, any device.
        # Compile-time paths (ir_encoder building a device-specific header)
        # stay with the default and get integers.
        self.preserve_symbolic_coords = preserve_symbolic_coords
        self.ir = {
            "ir_version": IR_VERSION,
            "simulation": {"tick_rate": 500},
            "colors": {},
            # Sparse night-mode palette. Populated only when a `--- colors ---`
            # section contains a `night:` child block. Each entry is a tuple
            # (literal RGB) or a string (name reference resolved via the day
            # colors table). When empty, night mode is a no-op — old blobs
            # behave exactly as they did before the feature existed.
            "night_colors": {},
            # Optional fallback color for agents/landmarks not listed in
            # night_colors. Tuple or name reference. None = unlisted colors
            # stay at their day value at night (the user's chosen default).
            "night_default": None,
            # Marker declarations (tile-scalar-field layer). Populated from
            # `--- colors ---` lines of the form
            #   name: ramp (r, g, b) decay K/T
            # Each entry: {"rgb": (r, g, b),          # floats
            #              "decay_k": int, "decay_t": int,
            #              "index": int}              # 0-based declaration order
            # Index is the wire/runtime ID — stable across recompiles as long
            # as declaration order doesn't change. See MARKERS_SPEC.md §2.1.
            "markers": {},
            # Night-mode marker coefficient overrides. Sparse — only markers
            # that need a different coefficient at night appear here. Each
            # entry: {"rgb": (r, g, b)}. Decay (K/T) is NOT overrideable per
            # night — decay is a dynamics property of the marker, not a
            # visual one, so it's inherited from the day declaration. At
            # render time the engine picks NIGHT_MARKERS for a marker that
            # has an entry here and night_mode_ is on, falling through to
            # the day MARKERS coefficient otherwise. Empty = night mode is
            # a no-op for markers (pre-v6 behavior).
            "night_markers": {},
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

    def _parse_coord(self, raw, axis_max):
        """Parse one axis token: int, "max_x[-N]", or "max_y[-N]".

        Returns a cleaned symbolic string when preserve_symbolic_coords
        is on and the token references max_x/max_y; otherwise returns
        an int resolved against `axis_max` (the grid's max index on this
        axis, i.e. width-1 or height-1).
        """
        s = raw.strip()
        has_symbol = ('max_x' in s) or ('max_y' in s)
        if self.preserve_symbolic_coords and has_symbol:
            return ''.join(s.split())
        t = s.replace('max_x', str(axis_max)).replace('max_y', str(axis_max))
        return int(eval(t))

    def _parse_point(self, raw):
        """Parse a `(x, y)` or `x, y` coordinate pair. Returns (x, y)
        where each entry is either int or symbolic str per `_parse_coord`.
        """
        s = raw.strip()
        if s.startswith('('):
            s = s[1:]
        if s.endswith(')'):
            s = s[:-1]
        xs, ys = s.split(',', 1)
        return (self._parse_coord(xs, self.width - 1),
                self._parse_coord(ys, self.height - 1))

    def compile(self, file_path):
        with open(file_path, 'r') as f:
            content = f.read()

        # Tabs are banned: whitespace-sensitive sections (behavior indent,
        # landmark indent) need deterministic column math, and mixing tabs
        # with spaces silently shifts the parsed indent.
        for lineno, line in enumerate(content.splitlines(), start=1):
            if '\t' in line:
                raise ValueError(
                    f"Compilation Failed: tab character on line {lineno}. "
                    f"Indent .crit files with spaces only."
                )

        # Split by sections. Anchor the regex to start-of-line so a
        # comment like `# --- returning ---` (a prose divider) inside
        # a behavior body doesn't get eaten as a section header and
        # silently truncate the behavior. Section headers occupy the
        # whole line; nothing else is allowed on the row.
        sections = re.split(
            r'^--- (.*?) ---\s*$',
            content,
            flags=re.MULTILINE,
        )

        for i in range(1, len(sections), 2):
            header = sections[i].strip()
            body = sections[i+1].strip()
            self._parse_section(header, body)

        # Post-passes run after all sections are parsed, so that name
        # resolution has the full landmark + agent declaration set in
        # scope.
        self._validate_name_collisions()
        # Marker canonicalization MUST run before the general name
        # resolver. It rewrites sugared marker conditions and seeks
        # (e.g. `if trail`, `seek highest trail < 5`) into their fully
        # disambiguated form (`if marker trail > 0`, `seek highest marker
        # trail < 5`). After this pass, all marker references carry the
        # `marker` keyword, so the general resolver can safely skip them
        # the same way it skips `landmark`-prefixed references.
        self._canonicalize_markers_in_behaviors()
        self._resolve_names_in_behaviors()
        self._validate_instructions()
        self._validate_markers()
        self._validate_pathfinding()
        self._validate_loops()
        self._validate_night_palette()
        self._validate_night_markers()

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
            self._parse_colors_section(lines)

        elif header == "landmarks":
            current_landmark = None
            for line in lines:
                # Landmark header (no indent)
                if ':' in line and not (line.startswith(' ') or line.startswith('\t')):
                    name, color = line.split(':')
                    current_landmark = name.strip()
                    self.ir["landmarks"][current_landmark] = {"color": color.strip(), "points": []}
                elif current_landmark:
                    try:
                        pt = self._parse_point(line.strip())
                    except Exception:
                        continue
                    self.ir["landmarks"][current_landmark]["points"].append(pt)

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
                    
                starts_match = re.search(
                    r'(\w+)\s+starts at\s+(\(([^)]*)\)|(\w+))'
                    r'(?:\s*,\s*state\s*=\s*(\w+))?',
                    line
                )
                if starts_match:
                    agent_name = starts_match.group(1)
                    coords_str = starts_match.group(3)
                    landmark_name = starts_match.group(4)
                    state_val = starts_match.group(5)
                    if coords_str is not None:
                        try:
                            pos = list(self._parse_point(coords_str))
                        except Exception:
                            continue
                    else:
                        lm = self.ir["landmarks"].get(landmark_name)
                        if lm is None or not lm.get("points"):
                            raise ValueError(
                                f"Compilation Failed: agent {agent_name!r} "
                                f"has 'starts at {landmark_name}', but "
                                f"{landmark_name!r} is not a defined landmark "
                                f"with at least one point. Declare the "
                                f"landmarks section before agents."
                            )
                        pos = list(lm["points"][0])
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

    def _parse_colors_section(self, lines):
        """Parse the `--- colors ---` section, including the optional nested
        `night:` child block.

        Top level: `name: <value>` where value is an RGB tuple literal or a
        `cycle ...` directive (same as before).

        Nested `night:` block (no value after the colon, followed by indented
        entries):
            night:
              <name>:   <value>        # overrides day color for this agent
              default:  <value>        # fallback for unlisted colors
        Night entries additionally accept a bare name reference (e.g.
        `doozer: warmwhite`), which resolves through the day palette at
        color-lookup time. `night` and `default` are reserved identifiers
        inside the colors section — `night` can't be a top-level color
        (it's the block marker), `default` is reserved only inside the
        night block."""
        in_night = False
        for raw in lines:
            stripped = raw.strip()
            # filter already dropped blanks/comment-onlys, but we still need
            # to strip trailing inline comments — the interpreter never sees
            # color lines, but eval() would choke on `(255,0,0) # red`.
            if '#' in stripped:
                stripped = stripped.split('#', 1)[0].rstrip()
            if not stripped:
                continue
            indent = len(raw) - len(raw.lstrip())

            name, sep, val = stripped.partition(':')
            if not sep:
                raise ValueError(
                    f"Compilation Failed: color line {stripped!r} is missing ':'."
                )
            name = name.strip()
            val = val.strip()

            if indent == 0:
                in_night = False  # leaving any previous nested block
                if name == "night":
                    if val:
                        raise ValueError(
                            "Compilation Failed: 'night' is a reserved block "
                            "marker in the colors section and cannot also be "
                            "a color name. Remove the value, or rename the "
                            "color."
                        )
                    in_night = True
                    continue
                # Markers share the colors section syntactically but live
                # in ir["markers"] — they are a scalar field per tile, not a
                # paintable color. Divert before the general color parser so
                # `ramp (r,g,b) decay K/T` doesn't try to eval() as a tuple.
                if val.startswith('ramp '):
                    self._parse_ramp_declaration(name, val)
                    continue
                self.ir["colors"][name] = self._parse_color_value(
                    name, val, allow_name_ref=False,
                )
            else:
                if not in_night:
                    raise ValueError(
                        f"Compilation Failed: indented color entry "
                        f"{stripped!r} is not inside a recognized nested "
                        f"block. Only 'night:' introduces a nested block "
                        f"in the colors section."
                    )
                # Ramp entry inside night: — overrides a day marker's RGB
                # coefficient when night mode is active. Decay (K/T) is
                # NOT overrideable; rejected here so authors don't get
                # surprise "my night decay didn't do anything" bugs.
                # `_validate_night_markers` cross-checks that the name
                # matches a declared day marker.
                if val.startswith('ramp '):
                    self._parse_night_ramp_declaration(name, val)
                    continue
                parsed = self._parse_color_value(
                    name, val, allow_name_ref=True,
                )
                if name == "default":
                    self.ir["night_default"] = parsed
                else:
                    self.ir["night_colors"][name] = parsed

    def _parse_color_value(self, name, val, allow_name_ref):
        """Parse the right-hand side of a color entry.

        Accepts:
          - `(r, g, b)` tuple literal
          - `cycle <name> <frames>, <name> <frames>, ...`
          - a bare identifier (name reference) iff allow_name_ref

        `name` is passed only for error messages."""
        if val.startswith('cycle '):
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
            return {"kind": "cycle", "entries": entries}
        if val.startswith('('):
            return eval(val)
        if allow_name_ref and re.fullmatch(r'\w+', val):
            return val
        raise ValueError(
            f"Compilation Failed: color {name!r} value {val!r} is not a "
            f"tuple literal, a cycle, or "
            f"{'a color name reference' if allow_name_ref else 'a recognized form'}."
        )

    # Matches `ramp (r, g, b) decay K/T`. RGB entries are floats (ints
    # are accepted by Python's float()); decay integers must be
    # non-negative. Extra whitespace is tolerated; the overall shape is
    # strict to avoid silently accepting malformed forms.
    # Night-marker variant: ramp-only, no decay suffix. Decay is a dynamics
    # property of the marker and doesn't vary day vs night, so night entries
    # override only the RGB coefficient. Matches `ramp (r, g, b)` with no
    # trailing `decay ...`. An authored night ramp that tries to include a
    # decay clause is rejected with a pointed message so authors don't think
    # night mode can change their marker's decay rate.
    _NIGHT_RAMP_RE = re.compile(
        r'^ramp\s*\(\s*([-\d.]+)\s*,\s*([-\d.]+)\s*,\s*([-\d.]+)\s*\)\s*$'
    )
    _RAMP_RE = re.compile(
        r'^ramp\s*\(\s*([-\d.]+)\s*,\s*([-\d.]+)\s*,\s*([-\d.]+)\s*\)'
        r'\s+decay\s+(\d+)\s*/\s*(\d+)\s*$'
    )

    def _parse_ramp_declaration(self, name, val):
        """Parse `ramp (r,g,b) decay K/T` into ir["markers"][name].

        Enforces at parse time:
          - shape matches the grammar,
          - declaration count <= MAX_MARKERS (HAL fixed-size array),
          - marker name does not duplicate an existing marker.
        Zero-ramp (all of r/g/b == 0) is NOT rejected here — it's a
        semantic footgun, not a parse error, and the validator in
        `_validate_markers` issues a soft warning. `decay K/0` likewise
        passes here and is rejected by the validator so error messages
        can refer to the whole program context, not the raw line."""
        m = self._RAMP_RE.match(val)
        if not m:
            raise ValueError(
                f"Compilation Failed: marker {name!r} declaration "
                f"{val!r} is malformed. Expected "
                f"'ramp (r, g, b) decay K/T' (three floats, two ints)."
            )
        try:
            r, g, b = float(m.group(1)), float(m.group(2)), float(m.group(3))
        except ValueError:
            raise ValueError(
                f"Compilation Failed: marker {name!r} has non-numeric "
                f"ramp components in {val!r}."
            )
        decay_k = int(m.group(4))
        decay_t = int(m.group(5))
        if name in self.ir["markers"]:
            raise ValueError(
                f"Compilation Failed: marker {name!r} is declared twice "
                f"in the colors section."
            )
        if len(self.ir["markers"]) >= MAX_MARKERS:
            raise ValueError(
                f"Compilation Failed: too many markers declared "
                f"({len(self.ir['markers']) + 1} > max {MAX_MARKERS}). "
                f"The HAL backs each tile with a fixed-size count[] "
                f"array — adding more markers requires bumping the "
                f"array width on the device side."
            )
        self.ir["markers"][name] = {
            "rgb": (r, g, b),
            "decay_k": decay_k,
            "decay_t": decay_t,
            "index": len(self.ir["markers"]),
        }

    def _parse_night_ramp_declaration(self, name, val):
        """Parse a `ramp (r, g, b)` entry inside the `night:` block.

        Stores `{"rgb": (r, g, b)}` in `ir["night_markers"][name]`. The
        name-matches-day-marker check is deferred to `_validate_night_markers`
        so the error can name the whole set of declared markers for context.
        Decay is NOT overrideable per-night — detection happens via regex
        split (the day `_RAMP_RE` requires a trailing `decay K/T`, the night
        `_NIGHT_RAMP_RE` rejects it). An authored `decay` clause gets a
        pointed error rather than a generic parse failure."""
        if self._RAMP_RE.match(val):
            raise ValueError(
                f"Compilation Failed: night ramp for marker {name!r} "
                f"includes a 'decay K/T' clause: {val!r}. Decay is a "
                f"dynamics property and is inherited from the day "
                f"declaration — night entries override only the RGB "
                f"coefficient. Remove the decay clause."
            )
        m = self._NIGHT_RAMP_RE.match(val)
        if not m:
            raise ValueError(
                f"Compilation Failed: night marker {name!r} declaration "
                f"{val!r} is malformed. Expected 'ramp (r, g, b)' with "
                f"three numeric components and no trailing clauses."
            )
        try:
            r, g, b = (float(m.group(1)),
                       float(m.group(2)),
                       float(m.group(3)))
        except ValueError:
            raise ValueError(
                f"Compilation Failed: night marker {name!r} has "
                f"non-numeric ramp components in {val!r}."
            )
        if name in self.ir["night_markers"]:
            raise ValueError(
                f"Compilation Failed: night marker {name!r} is declared "
                f"twice in the night: block."
            )
        self.ir["night_markers"][name] = {"rgb": (r, g, b)}

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

    def _validate_name_collisions(self):
        """Reject name reuse across landmarks vs agents and landmarks vs
        colors. Color/agent same-name is *required* (every agent must have
        a color of its own name for default rendering), so we don't check
        that pair. A shared *color* name across a non-self agent is still
        allowed — if the author wants `bricks` color used by `doozer`
        agent, that's fine — but they must avoid picking a color name that
        matches a *different* agent's name (unusual; left as future work)."""
        colors = set(self.ir["colors"].keys())
        landmarks = set(self.ir["landmarks"].keys())
        agents = set(self.ir["agents"].keys())
        markers = set(self.ir.get("markers", {}).keys())
        for ia in self.ir["initial_agents"]:
            agents.add(ia["name"])
        # Markers occupy the same namespace as colors (they're declared
        # in the `--- colors ---` section) and as landmarks/agents at
        # reference sites. Check all three pair collisions — reusing a
        # marker name as a landmark would make `if pile > 0` ambiguous.
        pairs = (
            ("color", colors, "landmark", landmarks),
            ("landmark", landmarks, "agent", agents),
            ("marker", markers, "landmark", landmarks),
            ("marker", markers, "agent", agents),
            ("marker", markers, "color", colors),
        )
        for kind_a, set_a, kind_b, set_b in pairs:
            overlap = sorted(set_a & set_b)
            if overlap:
                raise ValueError(
                    f"Compilation Failed: name {overlap[0]!r} is declared "
                    f"as both a {kind_a} and a {kind_b}. Pick distinct "
                    f"names."
                )

    # Pattern catalog for _canonicalize_markers_in_behaviors. Each entry
    # is (pattern, rewrite-template). Templates use str.format with named
    # groups from the match. Order matters: more specific forms (with
    # `on landmark X` suffixes) come first so the shorter forms don't
    # match greedily and swallow the tail.
    _MARKER_IF_PATTERNS = [
        # --- board / landmark scope ---
        (re.compile(r'^if no (?P<m>\w+) on landmark (?P<l>\w+)$'),
         'if no marker {m} on landmark {l}'),
        (re.compile(r'^if no (?P<m>\w+)$'),
         'if no marker {m}'),
        # `== 0` is a readability alias for the `no` predicate. Accepting
        # it keeps the `> N` / `== 0` pair symmetric for authors — `no`
        # is the canonical form, but `== 0` reads more naturally next to
        # an adjacent `> N` test. Only `0` is supported on the RHS;
        # general `== N` would need engine-level equality, which the
        # runtime doesn't carry (only `<` / `>`).
        (re.compile(r'^if (?P<m>\w+)\s*==\s*0 on landmark (?P<l>\w+)$'),
         'if no marker {m} on landmark {l}'),
        (re.compile(r'^if (?P<m>\w+)\s*==\s*0$'),
         'if no marker {m}'),
        (re.compile(r'^if (?P<m>\w+)\s*(?P<op>[<>])\s*(?P<n>\d+) on landmark (?P<l>\w+)$'),
         'if marker {m} {op} {n} on landmark {l}'),
        (re.compile(r'^if (?P<m>\w+)\s*(?P<op>[<>])\s*(?P<n>\d+)$'),
         'if marker {m} {op} {n}'),
        (re.compile(r'^if (?P<m>\w+) on landmark (?P<l>\w+)$'),
         'if marker {m} > 0 on landmark {l}'),
        # --- self-cell scope (`if on ...`) ---
        (re.compile(r'^if on no (?P<m>\w+)$'),
         'if on no marker {m}'),
        # Self-cell inverses: `not on <m>` and `not standing on <m>`
        # both mean "this cell's count is zero". `standing on` is a
        # legal alias for `on` elsewhere in the grammar, and authors
        # reach for both spellings; treat them identically. Same
        # `== 0` alias reasoning as the board-scope case above.
        (re.compile(r'^if not on (?P<m>\w+)$'),
         'if on no marker {m}'),
        (re.compile(r'^if not standing on (?P<m>\w+)$'),
         'if on no marker {m}'),
        (re.compile(r'^if on (?P<m>\w+)\s*==\s*0$'),
         'if on no marker {m}'),
        (re.compile(r'^if on (?P<m>\w+)\s*(?P<op>[<>])\s*(?P<n>\d+)$'),
         'if on marker {m} {op} {n}'),
        # The bare `if on <m>` and `if <m>` sugar forms are handled
        # specially below (they overlap with landmark/agent references,
        # so the marker guard has to dispatch per-name).
    ]

    _MARKER_SEEK_PATTERNS = [
        # seek highest|lowest <m> [op N] [on landmark X] [timeout N]
        (re.compile(
            r'^seek\s+(?P<dir>highest|lowest)\s+(?P<m>\w+)'
            r'(?:\s+(?P<op>[<>])\s*(?P<n>\d+))?'
            r'(?:\s+on\s+landmark\s+(?P<l>\w+))?'
            r'(?:\s+timeout\s+(?P<t>\d+))?$'),
         None),  # Special-cased: assembled by helper below.
        # Existing: `seek landmark X on <m> [op N]` — marker-as-filter,
        # target stays a landmark. Need to disambiguate the filter so the
        # engine knows it's a marker, not a tile-predicate.
        (re.compile(
            r'^seek\s+(landmark|agent)\s+(?P<tname>\w+)'
            r'(?:\s+with\s+state\s*==\s*(?P<st>\w+))?'
            r'\s+on\s+(?P<m>\w+)'
            r'(?:\s*(?P<op>[<>])\s*(?P<n>\d+))?'
            r'(?:\s+timeout\s+(?P<t>\d+))?$'),
         None),  # Special-cased: see helper below.
    ]

    def _canonicalize_markers_in_behaviors(self):
        """First-pass marker canonicalization.

        Rewrites sugared marker references in `if` / `seek` / `incr` /
        `decr` instructions to their fully-disambiguated `marker <name>`
        form. After this pass, any instruction containing a marker name
        wears the `marker` keyword, and the general name resolver
        (`_resolve_names_in_behaviors`) can skip it the same way it
        skips `landmark <name>` / `agent <name>`.

        A rewrite only fires when the captured identifier is a declared
        marker. Bare forms like `if pile` (pile being a landmark) are
        left for the general resolver to classify. This keeps marker
        sugar from accidentally claiming landmark/agent references.
        """
        markers = set(self.ir.get("markers", {}).keys())
        if not markers:
            # Nothing to canonicalize — skip the whole pass. Cheap guard
            # for programs that don't use markers at all.
            return

        for agent, insts in self.ir.get("behaviors", {}).items():
            for idx, (indent, inst) in enumerate(insts):
                # Separate the optional trailing colon — the `if` forms
                # have one, the seek/incr/decr forms don't.
                colon = ':' if inst.endswith(':') else ''
                body = inst[:-1].rstrip() if colon else inst

                new_body = self._canonicalize_marker_body(body, markers, inst)
                if new_body is not None and new_body != body:
                    insts[idx] = (indent, new_body + colon)

    def _canonicalize_marker_body(self, body, markers, full_inst):
        """Return the canonical rewrite of `body` for marker forms, or
        None / body unchanged if no rewrite applies.

        `full_inst` is only used for error messages so the author sees
        the instruction they actually wrote (colon and all)."""
        # --- incr / decr: opcode unambiguously takes a marker name ---
        m = re.match(r'^(incr|decr)\s+(\w+)(?:\s+(\d+))?$', body)
        if m:
            opcode, name, n = m.group(1), m.group(2), m.group(3)
            if name not in markers:
                raise ValueError(
                    f"Compilation Failed: {opcode} references unknown "
                    f"marker {name!r}. Declare it in the colors section "
                    f"as 'ramp (r,g,b) decay K/T'. Instruction: "
                    f"{full_inst!r}"
                )
            tail = f" {n}" if n else ""
            return f"{opcode} marker {name}{tail}"

        # --- if forms from the pattern catalog ---
        for pat, template in self._MARKER_IF_PATTERNS:
            pm = pat.match(body)
            if pm:
                if pm.group('m') not in markers:
                    # Template assumes the captured name is a marker.
                    # `if no pile` with pile=landmark is a semantic error
                    # today — landmarks don't have a "no" predicate — but
                    # catching it cleanly here would require threading the
                    # landmarks set in. Leave it to the opcode validator.
                    return None
                return template.format(**pm.groupdict())

        # --- bare sugar: `if on <m>` (self-cell, count > 0) and
        # `if <m>` (board, count > 0). Overlap with landmark/agent
        # existence checks, so we only rewrite when <m> is a marker. ---
        m = re.match(r'^if on (\w+)$', body)
        if m and m.group(1) in markers:
            return f"if on marker {m.group(1)} > 0"
        m = re.match(r'^if (\w+)$', body)
        if m and m.group(1) in markers:
            return f"if marker {m.group(1)} > 0"

        # --- seek forms ---
        # seek highest|lowest <m> [op N] [on landmark X] [timeout N]
        m = re.match(
            r'^seek\s+(?P<dir>highest|lowest)\s+(?P<m>\w+)'
            r'(?:\s+(?P<op>[<>])\s*(?P<n>\d+))?'
            r'(?:\s+on\s+landmark\s+(?P<l>\w+))?'
            r'(?:\s+timeout\s+(?P<t>\d+))?$',
            body,
        )
        if m:
            name = m.group('m')
            if name not in markers:
                raise ValueError(
                    f"Compilation Failed: 'seek {m.group('dir')} {name}' "
                    f"references unknown marker {name!r}. Declare it in "
                    f"the colors section as 'ramp (r,g,b) decay K/T'. "
                    f"Instruction: {full_inst!r}"
                )
            out = f"seek {m.group('dir')} marker {name}"
            if m.group('op'):
                out += f" {m.group('op')} {m.group('n')}"
            if m.group('l'):
                out += f" on landmark {m.group('l')}"
            if m.group('t'):
                out += f" timeout {m.group('t')}"
            return out

        # seek [landmark|agent] <tname> [with state==X] on <m> [op N] [timeout N]
        # — marker-as-filter on an otherwise normal seek. Only the filter
        # part needs canonicalization; leave the rest alone.
        m = re.match(
            r'^(?P<head>seek(?:\s+nearest)?(?:\s+free)?(?:\s+(?:landmark|agent))?\s+\w+'
            r'(?:\s+with\s+state\s*==\s*\w+)?)'
            r'\s+on\s+(?P<m>\w+)'
            r'(?:\s*(?P<op>[<>])\s*(?P<n>\d+))?'
            r'(?P<tail>(?:\s+timeout\s+\d+)?)$',
            body,
        )
        if m and m.group('m') in markers:
            head = m.group('head')
            op = m.group('op')
            n = m.group('n')
            tail = m.group('tail') or ''
            threshold = f" {op} {n}" if op else " > 0"
            return f"{head} on marker {m.group('m')}{threshold}{tail}"

        return None

    def _resolve_names_in_behaviors(self):
        """Rewrite bare names in `if on` / `seek` instructions into disambiguated
        `landmark <name>` or `agent <name>` forms. Tile predicates
        (current/extra/missing) pass through unchanged. Already-disambiguated
        forms (`if on landmark X`, `seek agent X`) are left alone."""

        landmarks = set(self.ir["landmarks"].keys())
        agents = set(self.ir["agents"].keys())
        colors = set(self.ir["colors"].keys())
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
        seek_re = re.compile(r'^(seek\s+)(nearest\s+)?(free\s+)?(\w+)(\s.*)?$')
        despawn_re = re.compile(r'^despawn\s+(\w+)(\s.*)?$')

        for agent, insts in self.ir["behaviors"].items():
            for idx, (indent, inst) in enumerate(insts):
                colon = ''
                body = inst
                if body.endswith(':'):
                    colon = ':'
                    body = body[:-1]

                # Marker forms are already canonicalized with the
                # `marker` keyword — the classifier doesn't know about
                # markers, so skipping them here is both necessary and
                # safe. `no` likewise: `if no marker X` / `if on no
                # marker X` are produced by the marker pre-pass.
                if body.startswith(("if marker ", "if on marker ",
                                     "if no marker ", "if on no marker ")):
                    continue

                m = if_on_re.match(body)
                if m:
                    prefix, name, rest = m.group(1), m.group(2), m.group(3) or ''
                    if name in _TILE_PREDICATES or name in ("landmark", "agent", "marker", "no"):
                        continue
                    kind = classify(name, inst)
                    insts[idx] = (indent, f"{prefix}{kind} {name}{rest}{colon}")
                    continue

                m = if_standing_re.match(body)
                if m:
                    prefix, name, rest = m.group(1), m.group(2), m.group(3) or ''
                    if (name in _TILE_PREDICATES
                            or name in ("landmark", "color")):
                        continue
                    if name in colors:
                        kind = "color"
                    elif name in landmarks:
                        kind = "landmark"
                    else:
                        raise ValueError(
                            f"Compilation Failed: 'if standing on {name}' \u2014 "
                            f"{name!r} is not a tile predicate "
                            f"({', '.join(sorted(_TILE_PREDICATES))}), "
                            f"color, or landmark. Instruction: {inst!r}"
                        )
                    insts[idx] = (
                        indent, f"{prefix}{kind} {name}{rest}{colon}"
                    )
                    continue

                m = if_exist_re.match(body)
                if m:
                    prefix, name = m.group(1), m.group(2)
                    if name in _TILE_PREDICATES or name in ("landmark", "agent", "marker"):
                        continue
                    kind = classify(name, inst)
                    insts[idx] = (indent, f"{prefix}{kind} {name}{colon}")
                    continue

                m = seek_re.match(body)
                if m:
                    seek_prefix = m.group(1)
                    nearest = m.group(2) or ''
                    free = m.group(3) or ''
                    name = m.group(4)
                    rest = m.group(5) or ''
                    # `seek highest|lowest marker X` and `seek landmark
                    # X on marker Y` are already canonicalized by the
                    # marker pre-pass; classify() has no concept of
                    # markers, so skip.
                    if name in _TILE_PREDICATES or name in (
                        "landmark", "agent", "marker", "highest", "lowest",
                    ):
                        continue
                    kind = classify(name, inst)
                    insts[idx] = (
                        indent,
                        f"{seek_prefix}{nearest}{free}{kind} {name}{rest}{colon}",
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
        re.compile(r'done'),
        re.compile(r'set\s+state\s*=\s*\w+'),
        re.compile(r'set\s+color\s*=\s*\w+'),
        re.compile(r'erase'),
        re.compile(r'draw(\s+\w+)?'),
        re.compile(r'pause(\s+\d+(-\d+)?)?'),
        re.compile(r'despawn(\s+agent\s+\w+)?'),
        re.compile(r'wander(\s+avoiding\s+(current|any))?'),
        re.compile(r'step\s*\(\s*-?\d+\s*,\s*-?\d+\s*\)'),
        # Marker deposit/withdraw. Always carry the `marker` keyword at
        # this stage — the pre-pass canonicalized any bare form.
        re.compile(r'(incr|decr)\s+marker\s+\w+(\s+\d+)?'),
        re.compile(
            # Existing target-first seek, with optional marker-as-filter
            # tail: `seek [nearest] [free] [agent|landmark] <name> [with state]`
            # followed optionally by `[not] on <pred> [op N]` and
            # `timeout N`. Marker filters look like `on marker X [op N]`.
            # `free` is an engine-side occupancy filter; see
            # CritterEngine.cpp classic-seek block.
            r'seek(\s+nearest)?(\s+free)?(\s+(agent|landmark))?\s+\w+'
            r'(\s+with\s+state\s*==\s*\w+)?'
            r'(\s+(not\s+)?on\s+(marker\s+\w+(\s*[<>]\s*\d+)?|\w+))?'
            r'(\s+timeout\s+\d+)?'
        ),
        # Gradient seek primitives. `on landmark X` is the ONLY filter
        # allowed on these — the comparison op is fused into the target
        # description, not expressed as a separate `on marker ...`.
        re.compile(
            r'seek\s+(highest|lowest)\s+marker\s+\w+'
            r'(\s*[<>]\s*\d+)?'
            r'(\s+on\s+landmark\s+\w+)?'
            r'(\s+timeout\s+\d+)?'
        ),
    ]

    def _validate_instructions(self):
        for agent, insts in self.ir.get("behaviors", {}).items():
            for indent, inst in insts:
                if inst == "else:":
                    # `else:` looks like Python but the runtime only treats it
                    # as a "finished the true branch, skip my body" marker —
                    # the false branch of the preceding `if` skips everything
                    # at higher indent, including the else body. So code under
                    # `else:` is effectively dead. Reject outright and point
                    # the author at the idiomatic rewrite.
                    raise ValueError(
                        f"Compilation Failed: `else:` is not supported in "
                        f"agent {agent!r}. The false branch of an `if` already "
                        f"skips to the next instruction at the if's indent — "
                        f"put fallback code there (or use a separate `if` "
                        f"block), not under `else:`."
                    )
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

    def _validate_markers(self):
        """Semantic checks over ir["markers"] and behavior usage.

        Runs AFTER `_canonicalize_markers_in_behaviors`, so every marker
        reference in the behaviors table carries the `marker` keyword
        and has already been checked against the declared set (unknown
        references raise during canonicalization). This pass covers the
        remaining classes of programmer error that can't be caught at
        rewrite time:

          1. `decay K/T` with T == 0 — would divide by zero at
             render/sim time.
          2. All-zero ramp RGB — marker renders nothing, so the script
             can't visually show what it's doing. Warn, don't fail
             (might be useful for headless accumulation).
          3. `draw <marker>` / `set color = <marker>` — ramp colors
             can't paint a tile, the operation is a semantic no-op and
             almost certainly a typo.
        """
        markers = self.ir.get("markers", {})
        for name, spec in markers.items():
            if spec["decay_t"] <= 0:
                raise ValueError(
                    f"Compilation Failed: marker {name!r} has decay "
                    f"period T={spec['decay_t']}; T must be a positive "
                    f"integer. Use '0/1' to mean 'never decays'."
                )
            if spec["decay_k"] < 0:
                raise ValueError(
                    f"Compilation Failed: marker {name!r} has decay "
                    f"amount K={spec['decay_k']}; K must be non-negative."
                )
            r, g, b = spec["rgb"]
            if r == 0 and g == 0 and b == 0:
                # Not a hard error — a headless accumulator marker is
                # conceivable. Emit to stderr so the author notices.
                print(
                    f"warning: marker {name!r} has all-zero ramp RGB; "
                    f"it will never render visibly.",
                    file=sys.stderr,
                )
            # Sub-unit coefficient footgun. On device, ramp coefficients
            # are stored as uint8_t (IrRuntime.h Marker struct), decoded
            # via (uint8_t)atof(...), which truncates 0.25 to 0. That makes
            # "dim version of a bright marker" (the natural first
            # intuition, especially for night overrides) render invisible.
            # Warn on any non-zero component below 1 so the author isn't
            # surprised when the pixels don't show up. Skip fully-zero
            # ramps — they already got the all-zero warning above.
            if not (r == 0 and g == 0 and b == 0):
                for ch_name, v in (("r", r), ("g", g), ("b", b)):
                    if 0 < v < 1:
                        print(
                            f"warning: marker {name!r} ramp component "
                            f"{ch_name}={v} is between 0 and 1; it will "
                            f"truncate to 0 on device (uint8_t storage). "
                            f"Use 1 for the dimmest visible step.",
                            file=sys.stderr,
                        )
                        break  # one warning per marker is enough

        # `draw <marker>` / `set color = <marker>` — ramp colors don't
        # paint, so the engine would silently fall through. Better to
        # reject at compile time than hunt a no-op on hardware.
        for agent, insts in self.ir.get("behaviors", {}).items():
            for indent, inst in insts:
                m = re.match(r'^draw\s+(\w+)$', inst)
                if m and m.group(1) in markers:
                    raise ValueError(
                        f"Compilation Failed: 'draw {m.group(1)}' in "
                        f"agent {agent!r} — {m.group(1)!r} is a ramp "
                        f"marker, not a paintable color. Use 'incr "
                        f"{m.group(1)}' to deposit a marker unit."
                    )
                m = re.match(r'^set\s+color\s*=\s*(\w+)$', inst)
                if m and m.group(1) in markers:
                    raise ValueError(
                        f"Compilation Failed: 'set color = {m.group(1)}'"
                        f" in agent {agent!r} — {m.group(1)!r} is a "
                        f"ramp marker, not an agent-paintable color."
                    )

    def _validate_night_palette(self):
        """Cross-check the night palette against the day palette.

        Every night override must target a day color that actually exists,
        and every name-ref or cycle sub-entry inside a night value must
        resolve to a real day color. The HAL IR loader already enforces
        this at device boot — if we don't catch it here, a stray name
        (e.g. `landed:` with no corresponding day color) silently passes
        the Python sim, then bricks a freshly-OTA'd device when the blob
        fails to load and `begin()` returns false. Fail at compile time
        instead so the error surfaces on the laptop, not in the field.
        """
        day = self.ir.get("colors", {})

        def _check_ref_target(target, where):
            if target not in day:
                raise ValueError(
                    f"Compilation Failed: {where} references day color "
                    f"{target!r}, which is not defined in the day "
                    f"palette. Known day colors: "
                    f"{', '.join(sorted(day)) or '(none)'}."
                )

        def _check_value(val, where):
            # Tuple RGB — always OK.
            if isinstance(val, tuple):
                return
            # Bare name ref.
            if isinstance(val, str):
                _check_ref_target(val, where)
                return
            # Cycle dict — every sub-entry color name must be a day color.
            if isinstance(val, dict) and val.get("kind") == "cycle":
                for sub_name, _frames in val.get("entries", []):
                    _check_ref_target(sub_name, f"{where} cycle entry")
                return
            # Anything else is an internal parse bug, not a user error —
            # the colors-section parser only emits the shapes above.
            raise ValueError(
                f"Compilation Failed: {where} has unexpected value "
                f"shape {val!r} (internal parser error)."
            )

        for override_name, val in self.ir.get("night_colors", {}).items():
            # The override must name a day color — this is the check
            # that would have caught swarm.crit's `landed:` stray.
            if override_name not in day:
                raise ValueError(
                    f"Compilation Failed: night override "
                    f"{override_name!r} has no matching day color. "
                    f"Each entry in the 'night:' block must override a "
                    f"color declared at the top level of the colors "
                    f"section. Known day colors: "
                    f"{', '.join(sorted(day)) or '(none)'}. "
                    f"Did you forget to add {override_name!r} to the "
                    f"day palette, or mean a different name?"
                )
            _check_value(val, f"night override {override_name!r}")

        night_default = self.ir.get("night_default")
        if night_default is not None:
            _check_value(night_default, "night default")

    def _validate_night_markers(self):
        """Cross-check ir["night_markers"] against the day markers table.

        Each night marker entry must name a day marker that actually
        exists. Same rationale as `_validate_night_palette`: catch name
        mismatches on the laptop instead of letting the device silently
        drop the override (or worse, fail to load the blob).

        Also re-applies the sub-unit coefficient warning — a night
        author is likely to try `heap: ramp (0.25, 0, 0)` as "dim red
        for night," and the uint8_t truncation footgun bites the same
        way as it does for day ramps.
        """
        day_markers = self.ir.get("markers", {})
        for name, spec in self.ir.get("night_markers", {}).items():
            if name not in day_markers:
                raise ValueError(
                    f"Compilation Failed: night marker override "
                    f"{name!r} has no matching day marker. Each entry "
                    f"in the 'night:' block that uses 'ramp (...)' must "
                    f"override a marker declared at the top level of "
                    f"the colors section. Known day markers: "
                    f"{', '.join(sorted(day_markers)) or '(none)'}."
                )
            r, g, b = spec["rgb"]
            if r == 0 and g == 0 and b == 0:
                # Same semantic as day: a zero-ramp night override
                # makes the marker invisible at night. Warn, don't fail
                # — could be intentional ("this marker is day-only").
                print(
                    f"warning: night marker {name!r} has all-zero ramp "
                    f"RGB; it will render invisible at night.",
                    file=sys.stderr,
                )
            else:
                for ch_name, v in (("r", r), ("g", g), ("b", b)):
                    if 0 < v < 1:
                        print(
                            f"warning: night marker {name!r} ramp "
                            f"component {ch_name}={v} is between 0 and "
                            f"1; it will truncate to 0 on device. Use "
                            f"1 for the dimmest visible step.",
                            file=sys.stderr,
                        )
                        break


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python compiler.py <file.crit>")
        sys.exit(1)
        
    compiler = CritCompiler()
    ir = compiler.compile(sys.argv[1])
    print(f"Successfully compiled {sys.argv[1]}")
    # print(json.dumps(ir, indent=2))

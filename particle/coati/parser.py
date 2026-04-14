"""
Parser for .coati script files → CoatiIR.

Two-pass:
  1. Split file into named sections at '--- name ---' headers.
  2. Parse each section with a specialized function.
"""
from __future__ import annotations
import re
from .ir import (
    CoatiIR, GridIR, LayerIR, LandmarkIR, TermExpr, TermIR,
    AgentsIR, PropertyIR, PathfindingIR, PenaltyIR,
    BehaviorIR, BehaviorRule, ConditionNode, ActionNode, IfNode,
    RenderingIR, RenderRule, ColorExpr,
    GoalIR, WobblyTimeIR, CollisionIR, SpawningIR,
)


class ParseError(Exception):
    def __init__(self, message: str, section: str = "", line: int = 0):
        self.section = section
        self.line = line
        loc = f"{section}:{line}: " if section else ""
        super().__init__(f"{loc}{message}")


# ── Helpers ─────────────────────────────────────────────────────

def _strip_comments(text: str) -> list[str]:
    """Remove ## comments and blank lines, return list of lines."""
    lines = []
    for line in text.splitlines():
        # Strip inline comments
        idx = line.find("##")
        if idx >= 0:
            line = line[:idx]
        line = line.rstrip()
        if line.strip():
            lines.append(line)
    return lines


def _parse_int(s: str) -> int:
    return int(s.strip().rstrip("s"))  # handle "120s" → 120


def _parse_float(s: str) -> float:
    s = s.strip().rstrip("x")  # handle "1.3x" → 1.3
    return float(s)


def _parse_coord(s: str) -> tuple[int, int]:
    """Parse '(x, y)' → (x, y)."""
    m = re.match(r"\(\s*(\d+)\s*,\s*(\d+)\s*\)", s.strip())
    if not m:
        raise ParseError(f"invalid coordinate: {s!r}")
    return (int(m.group(1)), int(m.group(2)))


def _parse_color(s: str) -> tuple[int, int, int]:
    """Parse '(r, g, b)' → (r, g, b)."""
    m = re.search(r"\(\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\)", s)
    if not m:
        raise ParseError(f"invalid color: {s!r}")
    return (int(m.group(1)), int(m.group(2)), int(m.group(3)))


# ── Section Splitter ────────────────────────────────────────────

_SECTION_RE = re.compile(r"^---\s+(\w+)\s+---\s*$")

def _split_sections(text: str) -> dict[str, str]:
    """Split file into {section_name: section_text}."""
    sections: dict[str, str] = {}
    current_name: str | None = None
    current_lines: list[str] = []

    for line in text.splitlines():
        m = _SECTION_RE.match(line.strip())
        if m:
            if current_name is not None:
                sections[current_name] = "\n".join(current_lines)
            current_name = m.group(1).lower()
            current_lines = []
        elif current_name is not None:
            current_lines.append(line)

    if current_name is not None:
        sections[current_name] = "\n".join(current_lines)

    return sections


# ── Grid ────────────────────────────────────────────────────────

def _parse_grid(text: str) -> GridIR:
    lines = _strip_comments(text)
    if not lines:
        return GridIR(32, 8)
    line = " ".join(lines).lower()
    # "32 wide, 8 tall, zigzag wiring"
    w_m = re.search(r"(\d+)\s*wide", line)
    h_m = re.search(r"(\d+)\s*tall", line)
    width = int(w_m.group(1)) if w_m else 32
    height = int(h_m.group(1)) if h_m else 8
    wiring = "zigzag"
    if "linear" in line:
        wiring = "linear"
    elif "custom" in line:
        wiring = "custom"
    return GridIR(width, height, wiring)


# ── Layers ──────────────────────────────────────────────────────

def _parse_layers(text: str) -> list[LayerIR]:
    layers = []
    for line in _strip_comments(text):
        # "current: bool, default off"
        m = re.match(r"(\w+)\s*:\s*(\w+)\s*,\s*default\s+(.+)", line.strip())
        if not m:
            continue
        name = m.group(1)
        typ = m.group(2).lower()
        raw_default = m.group(3).strip().lower()
        if typ == "bool":
            default = raw_default not in ("off", "false", "0")
        elif typ == "int":
            default = int(raw_default)
        elif typ == "float":
            default = float(raw_default)
        else:
            default = raw_default
        layers.append(LayerIR(name, typ, default))
    return layers


# ── Landmarks ───────────────────────────────────────────────────

def _parse_landmarks(text: str) -> list[LandmarkIR]:
    landmarks = []
    for line in _strip_comments(text):
        # "dumpster at (0, 7) and (1, 7), color red (64, 0, 0)"
        name_m = re.match(r"(\w+)\s+at\s+", line.strip())
        if not name_m:
            continue
        name = name_m.group(1)
        # Find all coordinates
        coords = re.findall(r"\(\s*\d+\s*,\s*\d+\s*\)", line)
        cells = [_parse_coord(c) for c in coords]
        # Find color (last parenthesized triple, after "color")
        color = None
        color_m = re.search(r"color\s+\w+\s+\((\d+)\s*,\s*(\d+)\s*,\s*(\d+)\)", line)
        if color_m:
            color = (int(color_m.group(1)), int(color_m.group(2)), int(color_m.group(3)))
            # Remove color coords from cells
            color_str = f"({color_m.group(1)}, {color_m.group(2)}, {color_m.group(3)})"
            # cells may have picked up the color tuple — filter by checking if it matches
            # a grid coordinate (small numbers) vs color (larger triples)
            # Simpler: cells are before "color" keyword
            before_color = line[:line.index("color")] if "color" in line else line
            coords = re.findall(r"\(\s*\d+\s*,\s*\d+\s*\)", before_color)
            cells = [_parse_coord(c) for c in coords]
        landmarks.append(LandmarkIR(name, cells, color))
    return landmarks


# ── Terms ───────────────────────────────────────────────────────

def _parse_term_expr(text: str) -> TermExpr:
    """Parse 'current is on and target is off' → TermExpr tree."""
    text = text.strip()
    # Split on ' and '
    if " and " in text:
        parts = text.split(" and ")
        return TermExpr(op="and", args=[_parse_term_expr(p) for p in parts])
    if " or " in text:
        parts = text.split(" or ")
        return TermExpr(op="or", args=[_parse_term_expr(p) for p in parts])
    # "current is on" / "current is off"
    m = re.match(r"(\w+)\s+is\s+(on|off|true|false|\d+)", text)
    if m:
        layer = m.group(1)
        val_str = m.group(2).lower()
        if val_str in ("on", "true"):
            value = True
        elif val_str in ("off", "false"):
            value = False
        else:
            value = int(val_str)
        return TermExpr(op="eq", layer=layer, value=value)
    raise ParseError(f"cannot parse term expression: {text!r}")

def _parse_terms(text: str) -> list[TermIR]:
    terms = []
    for line in _strip_comments(text):
        m = re.match(r"(\w+)\s*:\s*(.+)", line.strip())
        if m:
            terms.append(TermIR(m.group(1), _parse_term_expr(m.group(2))))
    return terms


# ── Agents ──────────────────────────────────────────────────────

def _parse_agents(text: str) -> AgentsIR:
    lines = _strip_comments(text)
    type_name = "agent"
    count = 1
    starts: list[tuple[int, int]] = []
    properties: list[PropertyIR] = []
    pool_mode = False
    spawn_point = None

    in_properties = False
    for line in lines:
        stripped = line.strip().lower()

        # "up to 12 ant agents"
        m = re.match(r"up\s+to\s+(\d+)\s+(\w+)\s+agents?", stripped)
        if m:
            count = int(m.group(1))
            type_name = m.group(2)
            pool_mode = True
            continue

        # "2 coati agents"
        m = re.match(r"(\d+)\s+(\w+)\s+agents?", stripped)
        if m:
            count = int(m.group(1))
            type_name = m.group(2)
            continue

        # "spawn at (0, 7)"
        m = re.match(r"spawn\s+at\s+(.+)", stripped)
        if m:
            spawn_point = _parse_coord(m.group(1))
            continue

        # "coati 0 starts at (8, 4)"
        m = re.match(r"\w+\s+(\d+)\s+starts?\s+at\s+(.+)", stripped)
        if m:
            starts.append(_parse_coord(m.group(2)))
            continue

        if stripped == "properties:":
            in_properties = True
            continue

        if in_properties:
            # "carrying: bool = false"
            m = re.match(r"(\w+)\s*:\s*(\w+)\s*=\s*(.+)", stripped)
            if m:
                pname = m.group(1)
                ptype = m.group(2)
                raw = m.group(3).strip()
                if ptype == "bool":
                    default = raw not in ("false", "0", "off")
                elif ptype == "int":
                    default = int(raw)
                elif ptype == "float":
                    default = float(raw)
                else:
                    default = raw
                properties.append(PropertyIR(pname, ptype, default))

    return AgentsIR(type_name, count, starts, properties, pool_mode, spawn_point)


# ── Pathfinding ─────────────────────────────────────────────────

def _parse_pathfinding(text: str) -> PathfindingIR:
    pf = PathfindingIR()
    for line in _strip_comments(text):
        stripped = line.strip().lower()
        if stripped.startswith("algorithm:"):
            val = stripped.split(":", 1)[1].strip()
            pf.algorithm = "astar" if "a*" in val or "astar" in val else val
        elif stripped.startswith("heuristic:"):
            pf.heuristic = stripped.split(":", 1)[1].strip()
        elif stripped.startswith("max nodes:"):
            pf.max_nodes = int(stripped.split(":", 1)[1].strip())
        elif "orthogonal" in stripped and "cost" in stripped:
            pf.orthogonal_cost = float(re.search(r"[\d.]+", stripped.split(":", 1)[1]).group())
        elif "diagonal" in stripped and "cost" in stripped:
            pf.diagonal_cost = float(re.search(r"[\d.]+", stripped.split(":", 1)[1]).group())
        elif stripped.startswith("penalty"):
            m = re.match(r"penalty\s+(.+?):\s*([\d.]+)", stripped)
            if m:
                cond_raw = m.group(1).strip()
                cost = float(m.group(2))
                if "lit" in cond_raw:
                    cond = "lit"
                elif "occupied" in cond_raw or "agent" in cond_raw:
                    cond = "occupied"
                else:
                    cond = cond_raw
                pf.penalties.append(PenaltyIR(cond, cost))
    return pf


# ── Behavior ────────────────────────────────────────────────────

def _indent_level(line: str) -> int:
    """Count leading spaces, divide by 2."""
    stripped = line.lstrip()
    if not stripped:
        return 0
    return (len(line) - len(stripped)) // 2


def _parse_condition(text: str) -> ConditionNode:
    """Parse a condition string into a ConditionNode tree."""
    text = text.strip().rstrip(":")

    # Handle 'and' at the top level (careful not to split inside sub-expressions)
    if " and " in text:
        parts = text.split(" and ")
        return ConditionNode(op="and", args=[_parse_condition(p) for p in parts])
    if " or " in text:
        parts = text.split(" or ")
        return ConditionNode(op="or", args=[_parse_condition(p) for p in parts])

    # "not at <landmark>"
    m = re.match(r"not\s+at\s+(\w+)$", text)
    if m:
        return ConditionNode(op="not_at_landmark", name=m.group(1))

    # "not <prop>"
    m = re.match(r"not\s+(\w+)$", text)
    if m:
        return ConditionNode(op="prop_false", prop=m.group(1))

    # "<prop> > <value>" / "<prop> >= <value>"
    m = re.match(r"(\w+)\s*>\s*([\d.]+)$", text)
    if m:
        return ConditionNode(op="gt", prop=m.group(1), value=_try_numeric(m.group(2)))
    m = re.match(r"(\w+)\s*>=\s*([\d.]+)$", text)
    if m:
        return ConditionNode(op="gte", prop=m.group(1), value=_try_numeric(m.group(2)))

    # "<prop> is <value>"
    m = re.match(r"(\w+)\s+is\s+(\d+|true|false)$", text)
    if m:
        return ConditionNode(op="eq", prop=m.group(1), value=_try_numeric(m.group(2)))

    # "at claimed"
    if text == "at claimed":
        return ConditionNode(op="at_claimed")

    # "at <landmark>"
    m = re.match(r"at\s+(\w+)$", text)
    if m:
        return ConditionNode(op="at_landmark", name=m.group(1))

    # "<landmark> is free"
    m = re.match(r"(\w+)\s+is\s+free$", text)
    if m:
        return ConditionNode(op="landmark_free", name=m.group(1))

    # "standing on lit" — alias for cell_is_lit (lit is a layer, not a term)
    if text == "standing on lit":
        return ConditionNode(op="cell_is_lit")

    # "standing on <term>"
    m = re.match(r"standing\s+on\s+(\w+)$", text)
    if m:
        return ConditionNode(op="standing_on_term", name=m.group(1))

    # "cell is lit"
    if text == "cell is lit":
        return ConditionNode(op="cell_is_lit")

    # "on lit"
    if text == "on lit":
        return ConditionNode(op="on_lit")

    # "at any landmark"
    if text == "at any landmark":
        return ConditionNode(op="at_any_landmark")

    # "near lit"
    if text == "near lit":
        return ConditionNode(op="near_lit")

    # "available <term> exist"
    m = re.match(r"available\s+(\w+)\s+exist$", text)
    if m:
        return ConditionNode(op="term_exists", name=m.group(1))

    # "<term> exist"
    m = re.match(r"(\w+)\s+exist$", text)
    if m:
        return ConditionNode(op="term_exists", name=m.group(1))

    # "no <term>"
    m = re.match(r"no\s+(\w+)$", text)
    if m:
        return ConditionNode(op="term_empty", name=m.group(1))

    # "<prop> > <expr>" where expr might be "3 + index * 2"
    m = re.match(r"(\w+)\s*>\s*(.+)$", text)
    if m:
        return ConditionNode(op="gt_expr", prop=m.group(1), value=m.group(2).strip())

    # Bare property name = truthy test
    m = re.match(r"(\w+)$", text)
    if m:
        return ConditionNode(op="prop_true", prop=m.group(1))

    raise ParseError(f"cannot parse condition: {text!r}")


def _try_numeric(s: str) -> int | float | bool:
    s = s.strip()
    if s == "true":
        return True
    if s == "false":
        return False
    try:
        return int(s)
    except ValueError:
        try:
            return float(s)
        except ValueError:
            return s


def _parse_action(text: str) -> ActionNode | None:
    """Parse a single action line into an ActionNode."""
    text = text.strip()

    if text == "done":
        return ActionNode(action="done")

    # "decrement <prop>"
    m = re.match(r"decrement\s+(\w+)$", text)
    if m:
        return ActionNode(action="decrement", prop=m.group(1))

    # "increment <prop>"
    m = re.match(r"increment\s+(\w+)$", text)
    if m:
        return ActionNode(action="increment", prop=m.group(1))

    # "reset <prop>"
    m = re.match(r"reset\s+(\w+)$", text)
    if m:
        return ActionNode(action="reset", prop=m.group(1))

    # "set <prop> to <value>"
    m = re.match(r"set\s+(\w+)\s+to\s+(.+)$", text)
    if m:
        return ActionNode(action="set", prop=m.group(1), value=_try_numeric(m.group(2)))

    # "force <prop> to <value>"
    m = re.match(r"force\s+(\w+)\s+to\s+(.+)$", text)
    if m:
        return ActionNode(action="set", prop=m.group(1), value=_try_numeric(m.group(2)))

    # "release <landmark>"
    m = re.match(r"release\s+(\w+)$", text)
    if m:
        return ActionNode(action="release", landmark=m.group(1))

    # "claim <landmark>"
    m = re.match(r"claim\s+(\w+)$", text)
    if m:
        return ActionNode(action="claim", landmark=m.group(1))

    # "clear claimed"
    if text == "clear claimed":
        return ActionNode(action="clear_claimed")

    # "clear path"
    if text == "clear path":
        return ActionNode(action="clear_path")

    # "pause <n>"
    m = re.match(r"pause\s+(\d+)$", text)
    if m:
        return ActionNode(action="pause", value=int(m.group(1)))

    # "bob at <landmark>"
    m = re.match(r"bob\s+at\s+(\w+)$", text)
    if m:
        return ActionNode(action="bob", landmark=m.group(1))

    # "step up from <landmark>"
    m = re.match(r"step\s+up\s+from\s+(\w+)$", text)
    if m:
        return ActionNode(action="step_up", landmark=m.group(1))

    # "seek nearest available <term>"
    m = re.match(r"seek\s+nearest\s+available\s+(\w+)$", text)
    if m:
        return ActionNode(action="seek_nearest", term=m.group(1))

    # "seek <landmark>"
    m = re.match(r"seek\s+(\w+)$", text)
    if m:
        return ActionNode(action="seek", landmark=m.group(1))

    # "pickup from board"
    m = re.match(r"pickup\s+from\s+(\w+)$", text)
    if m:
        return ActionNode(action="pickup", source=m.group(1))

    # "place"
    if text == "place":
        return ActionNode(action="place")

    # "discard"
    if text == "discard":
        return ActionNode(action="discard")

    # "despawn"
    if text == "despawn":
        return ActionNode(action="despawn")

    # "become bored"
    if text == "become bored":
        return ActionNode(action="set", prop="bored", value=True)

    # "wander avoiding lit and landmarks, chance 25%, forced if near lit"
    m = re.match(r"wander\s+avoiding\s+(.+?)(?:,\s*chance\s+(\d+)%)?(?:,\s*forced\s+if\s+(.+))?$", text)
    if m:
        avoid = [a.strip() for a in m.group(1).split(" and ")]
        chance = int(m.group(2)) if m.group(2) else 25
        forced = m.group(3).strip() if m.group(3) else None
        return ActionNode(action="wander", avoid=avoid, chance=chance, forced_condition=forced)

    # "for each lit cell in current: increase fade by 0.032, cap 1.0"
    m = re.match(r"for\s+each\s+lit\s+cell\s+in\s+(\w+)", text)
    if m:
        return ActionNode(action="fade_update", prop="fade", value=0.032)

    # "increase <prop> by <val>, cap <max>"
    m = re.match(r"increase\s+(\w+)\s+by\s+([\d.]+)\s*,\s*cap\s+([\d.]+)$", text)
    if m:
        return ActionNode(action="fade_update", prop=m.group(1),
                          value=float(m.group(2)),
                          source=m.group(3))  # abuse source for cap

    # "agent <colorname> (r, g, b)" — inline display color hint
    m = re.match(r"agent\s+\w+\s+\((\d+)\s*,\s*(\d+)\s*,\s*(\d+)\)", text)
    if m:
        return ActionNode(action="set_display_color",
                          value=(int(m.group(1)), int(m.group(2)), int(m.group(3))))

    return None


def _parse_behavior_block(lines: list[str], start_indent: int) -> list[ActionNode | IfNode]:
    """Parse an indented block of actions and if/else structures."""
    body: list[ActionNode | IfNode] = []
    i = 0
    while i < len(lines):
        line = lines[i]
        indent = _indent_level(line)
        stripped = line.strip()

        if indent < start_indent:
            break

        if indent > start_indent:
            i += 1
            continue

        # if/else
        if stripped.startswith("if ") and stripped.endswith(":"):
            cond_text = stripped[3:].rstrip(":")
            cond = _parse_condition(cond_text)
            # Collect then-body
            then_lines = []
            i += 1
            while i < len(lines) and _indent_level(lines[i]) > start_indent:
                then_lines.append(lines[i])
                i += 1
            then_body = _parse_behavior_block(then_lines, start_indent + 1)
            # Check for else
            else_body: list[ActionNode | IfNode] = []
            if i < len(lines) and lines[i].strip().startswith("else"):
                i += 1
                else_lines = []
                while i < len(lines) and _indent_level(lines[i]) > start_indent:
                    else_lines.append(lines[i])
                    i += 1
                else_body = _parse_behavior_block(else_lines, start_indent + 1)
            body.append(IfNode(condition=cond, then_body=then_body, else_body=else_body))
            continue

        # Regular action
        action = _parse_action(stripped)
        if action:
            body.append(action)
        i += 1

    return body


def _parse_behavior(text: str) -> BehaviorIR:
    lines = _strip_comments(text)
    ir = BehaviorIR()

    # Check for pathfind limit directive
    remaining = []
    tick_section = False
    tick_lines: list[str] = []
    for line in lines:
        stripped = line.strip().lower()
        if "one pathfind per tick" in stripped:
            ir.pathfind_limit = 1
        elif "unlimited pathfinds per tick" in stripped:
            ir.pathfind_limit = 999
        elif stripped == "each tick:":
            tick_section = True
        elif tick_section and _indent_level(line) >= 1:
            tick_lines.append(line)
        else:
            tick_section = False
            remaining.append(line)

    # Parse tick actions
    if tick_lines:
        ir.tick_actions = _parse_behavior_block(tick_lines, 1)

    # Split into when-blocks
    when_blocks: list[tuple[str, list[str]]] = []
    current_cond: str | None = None
    current_lines: list[str] = []

    for line in remaining:
        stripped = line.strip()
        if stripped.startswith("when ") and stripped.endswith(":") and _indent_level(line) == 0:
            if current_cond is not None:
                when_blocks.append((current_cond, current_lines))
            current_cond = stripped[5:].rstrip(":")
            current_lines = []
        elif current_cond is not None:
            current_lines.append(line)

    if current_cond is not None:
        when_blocks.append((current_cond, current_lines))

    # Parse each when-block
    for cond_text, block_lines in when_blocks:
        condition = _parse_condition(cond_text)
        body = _parse_behavior_block(block_lines, 1)
        ir.rules.append(BehaviorRule(condition=condition, body=body))

    return ir


# ── Rendering ───────────────────────────────────────────────────

def _parse_rendering(text: str) -> RenderingIR:
    ir = RenderingIR()
    for line in _strip_comments(text):
        stripped = line.strip().lower()

        # "when <condition>: agent <colorname> (r, g, b)"
        m = re.match(r"when\s+(.+?):\s*agent\s+\w+\s+\((\d+)\s*,\s*(\d+)\s*,\s*(\d+)\)(.*)", stripped)
        if m:
            cond = _parse_condition(m.group(1).strip())
            r, g, b = int(m.group(2)), int(m.group(3)), int(m.group(4))
            rest = m.group(5).strip()
            modulation = "solid"
            mod_period = 0
            multiply_layer = None
            if "pulse" in rest:
                modulation = "pulse"
                pm = re.search(r"pulse\s+(\d+)\s*ms", rest)
                if pm:
                    mod_period = int(pm.group(1))
            elif "shimmer" in rest:
                modulation = "shimmer"
                sm = re.search(r"shimmer\s+(\d+)\s*ms", rest)
                if sm:
                    mod_period = int(sm.group(1))
            color = ColorExpr(r, g, b, modulation, mod_period, multiply_layer)
            ir.rules.append(RenderRule(selector="agent_when", condition=cond, color=color))
            continue

        # "physics: 10 hz"
        m = re.match(r"physics:\s*(\d+)\s*hz", stripped)
        if m:
            ir.physics_hz = int(m.group(1))
            continue

        # "display: 60 hz, interpolated"
        m = re.match(r"display:\s*(\d+)\s*hz", stripped)
        if m:
            ir.display_hz = int(m.group(1))
            ir.interpolated = "interpolated" in stripped
            continue

        # Render rules: "board pixel: green (0, 64, 0) * fade"
        # "agent carrying not washed: yellow (64, 64, 0) pulse 150ms"
        m = re.match(r"(.+?):\s*\w+\s+\((\d+)\s*,\s*(\d+)\s*,\s*(\d+)\)(.*)", stripped)
        if m:
            selector_raw = m.group(1).strip()
            r, g, b = int(m.group(2)), int(m.group(3)), int(m.group(4))
            rest = m.group(5).strip()

            modulation = "solid"
            mod_period = 0
            multiply_layer = None

            if "pulse" in rest:
                modulation = "pulse"
                pm = re.search(r"pulse\s+(\d+)\s*ms", rest)
                if pm:
                    mod_period = int(pm.group(1))
            elif "shimmer" in rest:
                modulation = "shimmer"
                sm = re.search(r"shimmer\s+(\d+)\s*ms", rest)
                if sm:
                    mod_period = int(sm.group(1))
            if "* " in rest:
                fm = re.search(r"\*\s*(\w+)", rest)
                if fm:
                    multiply_layer = fm.group(1)

            color = ColorExpr(r, g, b, modulation, mod_period, multiply_layer)

            # Parse selector
            selector, agent_index = _parse_render_selector(selector_raw)
            ir.rules.append(RenderRule(selector=selector, agent_index=agent_index, color=color))

    return ir


def _parse_render_selector(raw: str) -> tuple[str, int | None]:
    """Parse render rule selector, extract agent index if present."""
    agent_index = None
    m = re.search(r"index\s+(\d+)", raw)
    if m:
        agent_index = int(m.group(1))
        raw = raw[:m.start()].strip() + raw[m.end():].strip()

    # Normalize to underscore form
    selector = raw.strip().replace(" ", "_")
    return selector, agent_index


# ── Goal ────────────────────────────────────────────────────────

def _parse_goal(text: str) -> GoalIR:
    ir = GoalIR()
    for line in _strip_comments(text):
        stripped = line.strip().lower()
        if stripped.startswith("type:"):
            ir.type = stripped.split(":", 1)[1].strip()
        elif stripped.startswith("font:"):
            val = stripped.split(":", 1)[1].strip()
            m = re.match(r"(\w+)\s+(\d+)x(\d+)", val)
            if m:
                ir.font = m.group(1)
                ir.font_size = (int(m.group(2)), int(m.group(3)))
            else:
                ir.font = val
        elif stripped.startswith("layout:"):
            ir.layout = stripped.split(":", 1)[1].strip()
        elif stripped.startswith("time:"):
            val = stripped.split(":", 1)[1].strip()
            wt = WobblyTimeIR()
            m = re.search(r"wobbly\s+(\d+)s?\s+to\s+(\d+)s?", val)
            if m:
                wt.min_offset = int(m.group(1))
                wt.max_offset = int(m.group(2))
            m = re.search(r"fast\s+([\d.]+)x?", val)
            if m:
                wt.fast_rate = float(m.group(1))
            m = re.search(r"slow\s+([\d.]+)x?", val)
            if m:
                wt.slow_rate = float(m.group(1))
            ir.time_config = wt
    return ir


# ── Collision ───────────────────────────────────────────────────

def _parse_collision(text: str) -> CollisionIR:
    lines = _strip_comments(text)
    # Skip the "on collision:" header line
    body_lines = []
    started = False
    for line in lines:
        if line.strip().lower().startswith("on collision"):
            started = True
            continue
        if started:
            body_lines.append(line)

    body = _parse_behavior_block(body_lines, 1) if body_lines else []
    return CollisionIR(body=body)


# ── Spawning ────────────────────────────────────────────────────

def _parse_spawning(text: str) -> SpawningIR:
    ir = SpawningIR()
    for line in _strip_comments(text):
        stripped = line.strip().lower()
        # "spawn every 10 ticks when missing exist"
        m = re.match(r"spawn\s+every\s+(\d+)\s+ticks?\s+when\s+(.+)", stripped)
        if m:
            ir.every_ticks = int(m.group(1))
            ir.condition = _parse_condition(m.group(2))
            continue
        # "spawn every 10 ticks" (unconditional)
        m = re.match(r"spawn\s+every\s+(\d+)\s+ticks?", stripped)
        if m:
            ir.every_ticks = int(m.group(1))
            continue
    return ir


# ── Main Entry Point ────────────────────────────────────────────

def parse(text: str) -> CoatiIR:
    """Parse a .coati script string into a CoatiIR."""
    sections = _split_sections(text)
    ir = CoatiIR()

    if "grid" in sections:
        ir.grid = _parse_grid(sections["grid"])
    if "layers" in sections:
        ir.layers = _parse_layers(sections["layers"])
    if "landmarks" in sections:
        ir.landmarks = _parse_landmarks(sections["landmarks"])
    if "terms" in sections:
        ir.terms = _parse_terms(sections["terms"])
    if "agents" in sections:
        ir.agents = _parse_agents(sections["agents"])
    if "pathfinding" in sections:
        ir.pathfinding = _parse_pathfinding(sections["pathfinding"])
    if "behavior" in sections:
        ir.behavior = _parse_behavior(sections["behavior"])
    if "rendering" in sections:
        ir.rendering = _parse_rendering(sections["rendering"])
    if "goal" in sections:
        ir.goal = _parse_goal(sections["goal"])
    if "spawning" in sections:
        ir.spawning = _parse_spawning(sections["spawning"])
    if "collision" in sections:
        ir.collision = _parse_collision(sections["collision"])

    return ir


def parse_file(path: str) -> CoatiIR:
    """Parse a .coati file by path."""
    with open(path) as f:
        return parse(f.read())

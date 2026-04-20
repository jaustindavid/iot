"""Text wire-format encoder/decoder for OTA IR.

See OTA_IR.md for the format grammar and rationale. Summary: line-oriented
plain text, `CRIT <v>` magic header, `---`-separated metadata block, then
typed sections (`COLORS <n>`, `LANDMARKS <n>`, …), closed with
`END <hex16>` (Fletcher-16 checksum over everything before the END line).

The encoder is the source of truth — the decoder is for round-trip tests
and for the upload tool's "same-source, skip re-upload" check against
whatever's currently in KV. The device-side parser is a separate C++
implementation in hal/particle/src/ (pending).
"""

from __future__ import annotations
from collections import OrderedDict
from typing import Any

FORMAT_VERSION = 1

REQUIRED_META_KEYS = (
    "name", "src_sha256", "encoded_at", "encoder_version", "ir_version",
)


# ---------------------------------------------------------------------------
# Checksum
# ---------------------------------------------------------------------------

def fletcher16(data: bytes) -> int:
    """Fletcher-16 over the bytes. Cheaper than CRC to implement in C,
    catches truncation and flipped bytes. Not cryptographic — the transport
    layer (Stra2us response signing) provides integrity against tampering."""
    sum1 = 0
    sum2 = 0
    for b in data:
        sum1 = (sum1 + b) % 255
        sum2 = (sum2 + sum1) % 255
    return (sum2 << 8) | sum1


# ---------------------------------------------------------------------------
# Encoder
# ---------------------------------------------------------------------------

def encode(ir: dict, meta: dict) -> str:
    """Serialize a compiled IR dict + metadata to the text wire format.

    `meta` must contain every key in REQUIRED_META_KEYS. `ir` is the dict
    shape produced by `CritCompiler.compile()`.
    """
    for k in REQUIRED_META_KEYS:
        if k not in meta:
            raise ValueError(f"Missing required meta key: {k!r}")

    lines: list[str] = []
    emit = lines.append

    # ---- header ----
    emit(f"CRIT {FORMAT_VERSION}")
    for k in REQUIRED_META_KEYS:
        v = meta[k]
        # Reject embedded newlines — they'd break line-oriented parsing.
        sv = str(v)
        if "\n" in sv:
            raise ValueError(f"meta[{k!r}] contains a newline")
        emit(f"{k} {sv}")
    emit("---")
    emit("")

    # ---- TICK ----
    tick = int(ir.get("simulation", {}).get("tick_rate", 50))
    emit(f"TICK {tick}")
    emit("")

    # ---- COLORS ----
    # Cycle entries reference colors by name (not index); the loader
    # resolves names → indices when materializing the runtime color table.
    colors = ir.get("colors", OrderedDict())
    emit(f"COLORS {len(colors)}")
    for name, val in colors.items():
        _check_token(name, "color name")
        if isinstance(val, dict) and val.get("kind") == "cycle":
            entries = val.get("entries", [])
            parts = [f"cycle {name} {len(entries)}"]
            for sub_name, frames in entries:
                _check_token(sub_name, "cycle color ref")
                parts.append(f"{sub_name} {int(frames)}")
            emit(" ".join(parts))
        else:
            r, g, b = val
            emit(f"static {name} {int(r)} {int(g)} {int(b)}")
    emit("")

    # ---- LANDMARKS ----
    landmarks = ir.get("landmarks", OrderedDict())
    emit(f"LANDMARKS {len(landmarks)}")
    for name, lm in landmarks.items():
        _check_token(name, "landmark name")
        _check_token(lm["color"], "landmark color ref")
        pts = lm.get("points", [])
        emit(f"{name} {lm['color']} {len(pts)}")
        for x, y in pts:
            emit(f"  {int(x)} {int(y)}")
    emit("")

    # ---- AGENTS ----
    agents = ir.get("agents", OrderedDict())
    emit(f"AGENTS {len(agents)}")
    for name, a in agents.items():
        _check_token(name, "agent name")
        states = a.get("states", [])
        emit(f"{name} {int(a.get('limit', 0))} {len(states)}")
        for s in states:
            _check_token(s, "agent state name")
            emit(f"  {s}")
    emit("")

    # ---- INITIAL ----
    initial = ir.get("initial_agents", [])
    emit(f"INITIAL {len(initial)}")
    for ia in initial:
        _check_token(ia["name"], "initial agent name")
        state = ia.get("state") or "none"
        _check_token(state, "initial agent state")
        x, y = ia["pos"]
        emit(f"{ia['name']} {int(x)} {int(y)} {state}")
    emit("")

    # ---- SPAWNS ----
    # Condition is a free-form string that runs to end of line. Rejected
    # earlier by the compiler if it contained a newline.
    spawns = ir.get("spawning", [])
    emit(f"SPAWNS {len(spawns)}")
    for s in spawns:
        _check_token(s["agent_type"], "spawn agent_type")
        _check_token(s["landmark"],   "spawn landmark")
        cond = s.get("condition", "")
        if "\n" in cond:
            raise ValueError(f"spawn condition contains a newline: {cond!r}")
        emit(f"{s['agent_type']} {s['landmark']} {int(s['interval'])} {cond}")
    emit("")

    # ---- PF_TOP ----
    pf = ir.get("pathfinding", {})
    top_diag = 1 if pf.get("diagonal") else 0
    top_cost = pf.get("diagonal_cost")
    has_cost = 1 if top_cost is not None else 0
    cost_val = float(top_cost) if top_cost is not None else 0.0
    emit(f"PF_TOP {top_diag} {has_cost} {_fmt_float(cost_val)}")
    emit("")

    # ---- PF ----
    # key=value style: the per-agent block has six optional fields with a
    # mix of int/float/bool. Emitting only present keys keeps the wire
    # compact and preserves the "has this been overridden" distinction the
    # engine cares about (has_max_nodes etc. in the old C header).
    per = pf.get("per_agent", OrderedDict())
    emit(f"PF {len(per)}")
    pf_keys = ("max_nodes", "penalty_lit", "penalty_occupied", "diagonal",
               "diagonal_cost", "step_rate")
    for agent, block in per.items():
        _check_token(agent, "pf agent name")
        parts = [agent]
        for k in pf_keys:
            if k not in block:
                continue
            v = block[k]
            if isinstance(v, bool):
                parts.append(f"{k}={1 if v else 0}")
            elif isinstance(v, float):
                parts.append(f"{k}={_fmt_float(v)}")
            else:
                parts.append(f"{k}={v}")
        emit(" ".join(parts))
    emit("")

    # ---- BEHAVIORS ----
    # Instruction text is free-form, runs to end of line. Compiler already
    # rejects embedded newlines.
    behaviors = ir.get("behaviors", OrderedDict())
    emit(f"BEHAVIORS {len(behaviors)}")
    for agent, insns in behaviors.items():
        _check_token(agent, "behavior agent name")
        emit(f"{agent} {len(insns)}")
        for indent, text in insns:
            if "\n" in text:
                raise ValueError(f"behavior text contains a newline: {text!r}")
            emit(f"  {int(indent)} {text}")

    # ---- SOURCE (optional) ----
    # Original .crit text, for debugging / audit. Carried as a line-count-
    # prefixed block; every source line gets a `| ` prefix (`|` alone for
    # empty lines) so the blob stays greppable and the line-oriented parser
    # doesn't need to reason about byte counts.
    source = meta.get("source")
    if source is not None:
        src_lines = source.splitlines()
        emit("")
        emit(f"SOURCE {len(src_lines)}")
        for ln in src_lines:
            emit(f"|" if ln == "" else f"| {ln}")

    # ---- checksum + END ----
    body = "\n".join(lines) + "\n"
    csum = fletcher16(body.encode("utf-8"))
    return body + f"END {csum:04x}\n"


# ---------------------------------------------------------------------------
# Decoder
# ---------------------------------------------------------------------------

class DecodeError(ValueError):
    pass


def decode(text: str) -> tuple[dict, dict]:
    """Parse the text wire format back into (ir_dict, meta_dict).

    Validates magic, format version, and checksum. Rejects unknown section
    kinds (device-side loader silently skips them; this tool is strict so
    round-trip bugs surface loudly)."""
    raw = text if text.endswith("\n") else text + "\n"

    # Find and verify END line.
    end_pos = raw.rfind("\nEND ")
    if end_pos < 0:
        raise DecodeError("Missing END line")
    body = raw[: end_pos + 1]                      # up to and including its trailing \n
    end_line = raw[end_pos + 1:].rstrip("\n")
    try:
        expected_csum = int(end_line.split()[1], 16)
    except (IndexError, ValueError):
        raise DecodeError(f"Malformed END line: {end_line!r}")
    actual_csum = fletcher16(body.encode("utf-8"))
    if actual_csum != expected_csum:
        raise DecodeError(
            f"Checksum mismatch: got {actual_csum:04x}, expected {expected_csum:04x}"
        )

    lines = body.splitlines()
    cur = _Cursor(lines)

    # ---- magic ----
    first = cur.next_nonempty()
    if not first.startswith("CRIT "):
        raise DecodeError(f"Missing CRIT magic: {first!r}")
    try:
        fmt_ver = int(first.split()[1])
    except ValueError:
        raise DecodeError(f"Malformed CRIT line: {first!r}")
    if fmt_ver != FORMAT_VERSION:
        raise DecodeError(f"Unsupported format version: {fmt_ver}")

    # ---- metadata ----
    meta: dict[str, Any] = {}
    while True:
        ln = cur.next()
        if ln == "---":
            break
        if not ln.strip():
            continue
        key, _, val = ln.partition(" ")
        meta[key] = val.strip()
    for k in REQUIRED_META_KEYS:
        if k not in meta:
            raise DecodeError(f"Missing required meta key: {k!r}")

    # ---- sections ----
    ir: dict[str, Any] = {
        "ir_version": int(meta.get("ir_version", 1)),
        "simulation": {},
        "colors": OrderedDict(),
        "landmarks": OrderedDict(),
        "agents": OrderedDict(),
        "behaviors": OrderedDict(),
        "spawning": [],
        "initial_agents": [],
        "pathfinding": {"diagonal": False, "diagonal_cost": None,
                        "per_agent": OrderedDict()},
    }

    while not cur.eof():
        ln = cur.next_nonempty_or_none()
        if ln is None:
            break

        toks = ln.split()
        kind = toks[0]

        if kind == "TICK":
            ir["simulation"]["tick_rate"] = int(toks[1])

        elif kind == "COLORS":
            n = int(toks[1])
            for _ in range(n):
                parts = cur.next().split()
                if parts[0] == "static":
                    name = parts[1]
                    r, g, b = int(parts[2]), int(parts[3]), int(parts[4])
                    ir["colors"][name] = [r, g, b]
                elif parts[0] == "cycle":
                    name = parts[1]
                    m = int(parts[2])
                    entries = []
                    for i in range(m):
                        sub_name = parts[3 + 2 * i]
                        frames = int(parts[4 + 2 * i])
                        entries.append([sub_name, frames])
                    ir["colors"][name] = {"kind": "cycle", "entries": entries}
                else:
                    raise DecodeError(f"Unknown color kind: {parts[0]!r}")

        elif kind == "LANDMARKS":
            n = int(toks[1])
            for _ in range(n):
                parts = cur.next().split()
                name, color, pt_count = parts[0], parts[1], int(parts[2])
                pts = []
                for _ in range(pt_count):
                    xy = cur.next().split()
                    pts.append([int(xy[0]), int(xy[1])])
                ir["landmarks"][name] = {"color": color, "points": pts}

        elif kind == "AGENTS":
            n = int(toks[1])
            for _ in range(n):
                parts = cur.next().split()
                name, limit, state_count = parts[0], int(parts[1]), int(parts[2])
                states = []
                for _ in range(state_count):
                    states.append(cur.next().strip())
                ir["agents"][name] = {"limit": limit, "states": states}

        elif kind == "INITIAL":
            n = int(toks[1])
            for _ in range(n):
                parts = cur.next().split()
                ir["initial_agents"].append({
                    "name":  parts[0],
                    "pos":   [int(parts[1]), int(parts[2])],
                    "state": parts[3],
                })

        elif kind == "SPAWNS":
            n = int(toks[1])
            for _ in range(n):
                # condition is the free-form rest of the line
                ln2 = cur.next()
                head, _, cond = ln2.partition(" ")
                agent_type = head
                parts = cond.split(" ", 2)
                landmark, interval, condition = parts[0], int(parts[1]), parts[2] if len(parts) > 2 else ""
                ir["spawning"].append({
                    "agent_type": agent_type,
                    "landmark":   landmark,
                    "interval":   interval,
                    "condition":  condition,
                })

        elif kind == "PF_TOP":
            ir["pathfinding"]["diagonal"] = bool(int(toks[1]))
            has_cost = bool(int(toks[2]))
            cost = float(toks[3])
            ir["pathfinding"]["diagonal_cost"] = cost if has_cost else None

        elif kind == "PF":
            n = int(toks[1])
            for _ in range(n):
                parts = cur.next().split()
                agent = parts[0]
                block: dict[str, Any] = {}
                for kv in parts[1:]:
                    k, _, v = kv.partition("=")
                    block[k] = _parse_pf_value(k, v)
                ir["pathfinding"]["per_agent"][agent] = block

        elif kind == "SOURCE":
            n = int(toks[1])
            src_lines: list[str] = []
            for _ in range(n):
                ln2 = cur.next()
                if ln2 == "|":
                    src_lines.append("")
                elif ln2.startswith("| "):
                    src_lines.append(ln2[2:])
                else:
                    raise DecodeError(f"Malformed SOURCE line: {ln2!r}")
            meta["source"] = "\n".join(src_lines)

        elif kind == "BEHAVIORS":
            n = int(toks[1])
            for _ in range(n):
                parts = cur.next().split(maxsplit=1)
                agent = parts[0]
                insn_count = int(parts[1])
                insns = []
                for _ in range(insn_count):
                    # Instruction text: leading indentation tokens, then the
                    # indent level (int), then free-form text to EOL.
                    ln2 = cur.next()
                    s = ln2.lstrip()
                    indent_s, _, rest = s.partition(" ")
                    indent = int(indent_s)
                    insns.append([indent, rest])
                ir["behaviors"][agent] = insns

        else:
            raise DecodeError(f"Unknown section kind: {kind!r}")

    return ir, meta


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

class _Cursor:
    """Line cursor over a list of strings. Tracks position for error
    messages (wishlist — not plumbed through yet)."""
    def __init__(self, lines: list[str]):
        self.lines = lines
        self.i = 0

    def eof(self) -> bool:
        return self.i >= len(self.lines)

    def next(self) -> str:
        if self.eof():
            raise DecodeError("Unexpected end of blob")
        ln = self.lines[self.i]
        self.i += 1
        return ln

    def next_nonempty(self) -> str:
        while True:
            ln = self.next()
            if ln.strip():
                return ln

    def next_nonempty_or_none(self):
        while not self.eof():
            ln = self.lines[self.i]
            self.i += 1
            if ln.strip():
                return ln
        return None


def _check_token(s: str, what: str) -> None:
    """Validate that `s` is a single whitespace-free token — the text format
    uses whitespace as the record separator, so a name with an embedded
    space would corrupt the next field."""
    if not isinstance(s, str) or not s:
        raise ValueError(f"Empty {what}")
    if any(c.isspace() for c in s):
        raise ValueError(f"{what} contains whitespace: {s!r}")


def _fmt_float(x: float) -> str:
    """Format a float compactly but round-trippable. `repr()` keeps enough
    precision that float(repr(x)) == x, which keeps the round-trip test
    byte-exact for any value we'd actually encode (step_rate, diag_cost)."""
    s = repr(float(x))
    return s


def _parse_pf_value(key: str, raw: str):
    # `diagonal` is the only boolean; everything else is numeric.
    if key == "diagonal":
        return bool(int(raw))
    if key in ("diagonal_cost", "penalty_lit", "penalty_occupied"):
        # penalty_* came out of the compiler as int or float depending on
        # the script — preserve int when we can so the round-trip is exact.
        if "." in raw or "e" in raw.lower():
            return float(raw)
        return int(raw)
    # max_nodes, step_rate: always int.
    return int(raw)

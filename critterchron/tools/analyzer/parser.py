"""Parse the critterchron heartbeat string into a flat metrics dict.

The firmware publishes telemetry as a free-form whitespace-delimited
string (see `hal/particle/src/critterchron_particle.cpp:594` and the
ESP32 mirror) — historical reasons, not a wire-format we control here.

Format (single line, all fields optional except a few):

    up=N wall=N rssi=N mem=N rst=N fw=X script=Y net=Z
    bri=(min<cur<max[ sched|sched-err]) phys=(avg<max<budget)us
    rend=(avg<max<budget)us interp=(avg<max)us astar=(avg<max)us
    agents=N seeks_fail=N [light=(cb<raw<cd)] [err=cat:msg ...rest...]

`err=` (when present) is always last and consumes the rest of the
line (error messages can contain spaces). `script=` looks like
`name@8hexsha` or the literal `default`. Triples `(a<b<c)` and pairs
`(a<b)` may carry a `us` suffix.

Output: a flat dict suitable for the detector (numeric metrics first,
context strings second). Unknown tokens are kept under `_extra` so
adding a new heartbeat field doesn't silently break the parser.
"""

from __future__ import annotations

import re
from typing import Any


# Triple/pair extraction. Trailing unit suffix optional — accept any
# lowercase letters: `us` (timing fields), `s` (wobble), `ms`, etc.
_TRIPLE_RE = re.compile(
    r"^\((-?\d+)<(-?\d+)<(-?\d+)(?:\s+(\S+))?\)[a-z]*$"
)
_PAIR_RE = re.compile(r"^\((-?\d+)<(-?\d+)\)[a-z]*$")


def _try_int(s: str) -> Any:
    try:
        return int(s)
    except ValueError:
        return s


def parse_heartbeat(line: str) -> dict:
    """Parse one heartbeat line → flat dict of metrics + context.

    Numeric metrics use their natural names (e.g. `seeks_fail`,
    `agents`, `bri_min`, `phys_avg_us`). String context fields land
    under their key (e.g. `script`, `net`, `fw`).

    Returns an empty dict for an empty/whitespace string.
    """
    if not isinstance(line, str):
        raise TypeError(f"expected str, got {type(line).__name__}")
    line = line.strip()
    if not line:
        return {}

    out: dict = {}
    extra: list[str] = []

    tokens = line.split(" ")
    i = 0
    while i < len(tokens):
        tok = tokens[i]
        if not tok:
            i += 1
            continue

        # `err=cat:msg ...` consumes the rest of the line.
        if tok.startswith("err="):
            err = tok[len("err="):]
            if i + 1 < len(tokens):
                err = err + " " + " ".join(tokens[i + 1:])
            cat, _, msg = err.partition(":")
            out["err_cat"] = cat
            out["err_msg"] = msg if _ else ""
            out["err_raw"] = err
            break

        if "=" not in tok:
            extra.append(tok)
            i += 1
            continue

        key, _, value = tok.partition("=")

        # Triples / pairs may have a space inside the parens
        # (`bri=(0<32<64 sched)`). Re-glue with the next token until
        # the closing paren is seen.
        if value.startswith("(") and ")" not in value:
            j = i + 1
            while j < len(tokens) and ")" not in tokens[j]:
                value = value + " " + tokens[j]
                j += 1
            if j < len(tokens):
                value = value + " " + tokens[j]
                i = j
        i += 1

        _assign(out, key, value)

    if extra:
        out["_extra"] = extra
    return out


def _assign(out: dict, key: str, value: str) -> None:
    """Map a single (key, value) into the flat output dict."""
    # Triples
    m = _TRIPLE_RE.match(value)
    if m:
        a, b, c, suffix = m.groups()
        a, b, c = int(a), int(b), int(c)
        # Timing fields (phys/rend/interp/astar) use suffix=None and
        # the value is already in us thanks to the regex stripping
        # `)us`. The doc-format slot names are avg/max/budget.
        if key in ("phys", "rend"):
            out[f"{key}_avg_us"] = a
            out[f"{key}_max_us"] = b
            out[f"{key}_budget_us"] = c
        elif key == "bri":
            out["bri_min"] = a
            out["bri"] = b
            out["bri_max"] = c
            if suffix:
                out["bri_sched"] = suffix
        elif key == "light":
            out["light_cal_bright"] = a
            out["light_raw"] = b
            out["light_cal_dark"] = c
        elif key == "wobble":
            # `wobble=(min<cur<max)s`. Center value is the diagnostic;
            # bounds are the configured knobs.
            out["wobble_min"] = a
            out["wobble"] = b
            out["wobble_max"] = c
        elif key == "rtt":
            # `rtt=(min<mean<max)ms`. All three are diagnostic — fleet-MAD
            # works on any of them. Field is omitted from the heartbeat
            # when no in-threshold samples this window; analyzer can
            # detect that as `rtt` key absent from the parsed dict.
            out["rtt_min"] = a
            out["rtt"] = b
            out["rtt_max"] = c
        else:
            # Unknown triple — preserve as a tuple under the key so
            # operators can still see it without the parser growing
            # a special-case ahead of the firmware.
            out[key] = (a, b, c)
        return

    # Engine-wedge diagnostic fields, added 2026-05-17 (see firmware
    # comment in critterchron_esp32.ino:1008). Shapes don't fit the
    # generic triple/pair grammar, so each gets a small special-case.
    #
    # cells=(m=N,x=N) → cells_missing + cells_extra (numeric, drivable
    # by staleness rules — e.g. "missing > 0 for an hour but seeks_fail
    # flat" is the new bug class).
    if key == "cells" and value.startswith("(") and value.endswith(")"):
        inner = value[1:-1]
        for piece in inner.split(","):
            sub_k, _, sub_v = piece.strip().partition("=")
            if sub_k == "m":
                try: out["cells_missing"] = int(sub_v)
                except ValueError: pass
            elif sub_k == "x":
                try: out["cells_extra"] = int(sub_v)
                except ValueError: pass
        return

    # ast= → list[str] under `ast` plus an `ast_uniform` flag (1 = all
    # agents in the same state, the "everyone stuck in idle" signature).
    # String list survives into the alert context but is filtered out of
    # numeric_metrics(). Two wire formats:
    #   verbose: ast=(s1,s2,...)   one entry per agent slot
    #   compact: ast=Nxstate        all N slots in the same state
    # Compact form is emitted by stateless scripts (swarm-fade etc.,
    # 16 agents of `none`) to save ~70 bytes on the wire — critical
    # given Particle's 384-byte heartbeat buffer. Parser distinguishes
    # by the leading `(` vs digit.
    if key == "ast" and value.startswith("(") and value.endswith(")"):
        inner = value[1:-1]
        states = [s.strip() for s in inner.split(",") if s.strip()]
        out["ast"] = states
        out["ast_uniform"] = 1 if states and len(set(states)) == 1 else 0
        return
    if key == "ast" and value and value[0].isdigit():
        # `Nxstate` compact form. Split on the first 'x' — N is the
        # integer count, the rest is the state name (which can itself
        # contain letters but not digits before the 'x').
        x_idx = value.find("x")
        if x_idx > 0:
            try:
                n = int(value[:x_idx])
                state = value[x_idx + 1:]
                if n > 0 and state:
                    out["ast"] = [state] * n
                    out["ast_uniform"] = 1
                    return
            except ValueError:
                pass
        # Fall through to default scalar handling if shape didn't match.

    # esync_lag=<int>s — seconds since the engine was last time-pulsed.
    # `-1s` is the "never synced" sentinel from the firmware; pass it
    # through as -1 so the analyzer can rule-out "boot transient" with
    # `> N` thresholds (any positive value crosses a -1 threshold).
    if key == "esync_lag" and value.endswith("s"):
        try: out["esync_lag"] = int(value[:-1])
        except ValueError: pass
        return

    # WEDGE_DIAG opt-in field. `agid=(t<N>/s<M>,...)` — per-agent raw
    # type_idx and state_str_id. Exposes the bytes that drive `ast=`;
    # added 2026-05-20 to localize boober-on-C3 wedge corruption.
    # Parsed into a list of (type, state) tuples under `agid`. Filtered
    # out of numeric_metrics().
    if key == "agid" and value.startswith("(") and value.endswith(")"):
        inner = value[1:-1]
        pairs = []
        for piece in inner.split(","):
            piece = piece.strip()
            if not piece: continue
            m = re.match(r"t(\d+)/s(\d+)", piece)
            if m:
                pairs.append((int(m.group(1)), int(m.group(2))))
        out["agid"] = pairs
        return

    # WEDGE_DIAG follow-ons (2026-05-23):
    #   irst=<16hex>  — first 2 bytes of each of 4 state strings of
    #                   AGENT_TYPES[0]. Hex; healthy boober reads as
    #                   "id"/"ca"/"re"/"st" → "6964636172657374".
    #   canary=ok | canary=BAD/<hex> — Agent struct trailing canary
    #                   check. "ok" = all alive agents have intact
    #                   0xDEADBEEF; else value of first corrupted one.
    # Both useful at-a-glance: ast=() empty + irst contains 0000 = IR
    # string memory corrupted. ast=() empty + irst healthy = corruption
    # elsewhere. Either + canary=BAD = adjacent struct memory hit too.
    if key == "irst":
        out["irst"] = value  # keep raw hex; analyzer can compare to baseline
        # Convenience flag: ir_strings_zero=1 if any 2-byte slot is "0000"
        chunks = [value[i:i+4] for i in range(0, len(value), 4)]
        out["ir_strings_zero"] = 1 if any(c == "0000" for c in chunks if len(c) == 4) else 0
        return
    if key == "canary":
        out["canary"] = value
        out["canary_ok"] = 1 if value == "ok" else 0
        return

    # apos=(x,y;x,y;...) — per-agent grid positions. Semicolon-separated
    # so commas inside (x,y) pairs don't ambiguate. Parsed into list of
    # (x, y) tuples under `apos`. Filtered out of numeric_metrics().
    if key == "apos" and value.startswith("(") and value.endswith(")"):
        inner = value[1:-1]
        pairs = []
        for piece in inner.split(";"):
            piece = piece.strip()
            if not piece: continue
            xy = piece.split(",")
            if len(xy) == 2:
                try: pairs.append((int(xy[0]), int(xy[1])))
                except ValueError: pass
        out["apos"] = pairs
        return

    # apc=(N,N,...) — per-agent script PC. Parsed into list[int].
    if key == "apc" and value.startswith("(") and value.endswith(")"):
        inner = value[1:-1]
        pcs = []
        for piece in inner.split(","):
            piece = piece.strip()
            if not piece: continue
            try: pcs.append(int(piece))
            except ValueError: pass
        out["apc"] = pcs
        return

    # aglitch=(0,1,...) — per-agent glitched flag (1 = benched by the
    # runaway-opcode guard). Parsed to list[int] under `aglitch`, plus
    # `aglitch_any` = 1 if any agent is glitched. The engine-wide
    # `glitches=N` counter is handled by the generic scalar path below
    # (lands as out["glitches"]); a nonzero glitches with a frozen
    # agid/apos snapshot is the confirmed wedge.
    if key == "aglitch" and value.startswith("(") and value.endswith(")"):
        inner = value[1:-1]
        flags = []
        for piece in inner.split(","):
            piece = piece.strip()
            if not piece: continue
            try: flags.append(int(piece))
            except ValueError: pass
        out["aglitch"] = flags
        out["aglitch_any"] = 1 if any(f for f in flags) else 0
        return

    # pileheap=(N,N,...) — heap-marker count at each boober "piles" cell.
    # Wedge hypothesis (2026-05-29): all cells >= 1 at wedge → deposit
    # seek `< 1` has no candidate → returning agent livelocks. Parsed to
    # list[int] under `pileheap`, plus convenience flags:
    #   pileheap_min — smallest count (0 refutes the hypothesis)
    #   pileheap_all_ge1 — 1 if every cell >= 1 (the predicted wedge state)
    if key == "pileheap" and value.startswith("(") and value.endswith(")"):
        inner = value[1:-1]
        counts = []
        for piece in inner.split(","):
            piece = piece.strip()
            if not piece: continue
            try: counts.append(int(piece))
            except ValueError: pass
        out["pileheap"] = counts
        if counts:
            out["pileheap_min"] = min(counts)
            out["pileheap_all_ge1"] = 1 if min(counts) >= 1 else 0
        return

    m = _PAIR_RE.match(value)
    if m:
        a, b = int(m.group(1)), int(m.group(2))
        if key in ("interp", "astar"):
            out[f"{key}_avg_us"] = a
            out[f"{key}_max_us"] = b
        else:
            out[key] = (a, b)
        return

    # Bare scalar / string. Try int first; a failed parse means
    # it's a string field (script, net, fw).
    out[key] = _try_int(value)


def numeric_metrics(parsed: dict) -> dict:
    """Filter `parse_heartbeat` output to numeric metrics only.

    Detectors need pure numerics; this strips strings (script, fw,
    net, err_*, bri_sched) and the `_extra` bucket.
    """
    return {k: v for k, v in parsed.items()
            if isinstance(v, (int, float)) and not isinstance(v, bool)}

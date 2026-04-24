#!/usr/bin/env python3
"""Drift lint between critterchron.s2s.yaml and hal/particle/src/.

Two checks, both advisory-level (concrete actionable failures, not
philosophy):

  1. Every `get_int("k", ...)` / `get_float("k", ...)` site in the HAL
     source appears in the catalog, with a matching `type`. A new knob
     added to the device code without a catalog entry fails the lint
     — this is the primary forcing function keeping the catalog honest.

  2. For get_* sites whose default is a literal number, that literal
     matches the catalog's `default`. Sites whose default is a symbol
     (HEARTBEEP_DEFAULT, MIN_BRIGHTNESS, …) are resolved by looking for
     `#define <SYMBOL> N` in the same HAL tree. Only *unambiguous*
     literal resolutions participate in the check — a symbol with no
     `#define` found (or with conflicting definitions) is reported as
     "unresolved default" and skipped. Catalog entries marked
     `default: per-device` skip the check intentionally.

Reverse direction (catalog entries not read anywhere in HAL) warns
unless the entry is tagged `ops_only: true` — keys like `ir` are
read by Stra2usClient internals, not via the Config interface, and
shouldn't trip the lint.

Usage:
    python3 test_s2s_catalog.py
    python3 test_s2s_catalog.py -v

Exit 0 on clean, 1 on findings. No network traffic — static analysis only.
"""

from __future__ import annotations
import re
import sys
from pathlib import Path

try:
    import yaml  # type: ignore
except ImportError:
    print("error: pyyaml not installed", file=sys.stderr)
    sys.exit(2)

ROOT = Path(__file__).resolve().parent
# Every live HAL tree. Each platform's src/ owns its own copy of the
# engine-facing files (WobblyTimeSource, LightSensor*, the .ino/.cpp
# shim), so a new knob added on one platform but not mirrored to the
# others still trips the lint on the platform that *does* read it.
# Order matters for human output only — findings print sorted by path.
HAL_SRCS = [
    ROOT / "hal" / "particle" / "src",
    ROOT / "hal" / "esp32"    / "src",
]
CATALOG = ROOT / "critterchron.s2s.yaml"

# Match `get_int("key", <default-expr>)` / get_float across lines.
# The default expression is captured up to the closing paren on the
# same line — good enough for our call sites; if a future call splits
# the default over two lines this would need `re.DOTALL` + a paren
# balancer. Not worth it until we have one.
CALL_RE = re.compile(
    r'get_(?P<fn>int|float)\s*\(\s*"(?P<key>[^"]+)"\s*,\s*(?P<default>[^)]+?)\)'
)

# Match `#define SYMBOL <literal>` anywhere in the HAL tree (both
# hal/particle/src and hal/devices for per-device header overrides).
DEFINE_RE = re.compile(
    r'^\s*#\s*define\s+(?P<sym>[A-Z_][A-Z0-9_]*)\s+(?P<val>[-+0-9.]+f?)\s*$',
    re.MULTILINE,
)


def collect_get_calls() -> list[dict]:
    """Walk every HAL source tree, return each get_int/get_float site."""
    calls = []
    for hal_src in HAL_SRCS:
        if not hal_src.is_dir():
            continue  # platform HAL not checked out / doesn't exist
        for p in sorted(hal_src.rglob("*")):
            if p.suffix not in (".h", ".cpp", ".c", ".hpp", ".ino"):
                continue
            text = p.read_text()
            for m in CALL_RE.finditer(text):
                line = text.count("\n", 0, m.start()) + 1
                calls.append({
                    "file": p.relative_to(ROOT),
                    "line": line,
                    "fn": m.group("fn"),           # int | float
                    "key": m.group("key"),
                    "default_expr": m.group("default").strip(),
                })
    return calls


def collect_defines() -> dict[str, list[tuple[Path, str]]]:
    """Grep `#define SYMBOL <literal>` across every HAL src tree.

    Scoped to app-level source (NOT hal/devices/*.h) on purpose: the
    catalog's `default` field represents the app-level fallback a device
    sees when its header doesn't override. Per-device overrides are a
    separate concept from the catalog; treating them as conflicting
    sources of truth here would produce noise for exactly the keys we
    intentionally scope per-device (brightness, night thresholds).

    Symbols that differ between platforms (e.g. if Particle defines
    HEARTBEEP_DEFAULT=300 and ESP32 defines it=60) land as "ambiguous"
    below and are skipped — the current parity is lockstep but the
    machinery handles drift gracefully if it appears."""
    out: dict[str, list[tuple[Path, str]]] = {}
    for hal_src in HAL_SRCS:
        if not hal_src.is_dir():
            continue
        for p in sorted(hal_src.rglob("*")):
            if p.suffix not in (".h", ".cpp", ".c", ".hpp", ".ino"):
                continue
            text = p.read_text()
            for m in DEFINE_RE.finditer(text):
                out.setdefault(m.group("sym"), []).append(
                    (p.relative_to(ROOT), m.group("val"))
                )
    return out


LITERAL_RE = re.compile(r'^[-+]?(\d+\.\d*|\.\d+|\d+)f?$')


def parse_default(expr: str, defines: dict) -> tuple[str, object]:
    """Classify a default expression. Returns:
      ("literal", number)    — parsed literal, numeric
      ("symbol", number)     — symbol resolved uniquely to a literal
      ("ambiguous", list)    — symbol with multiple #define sites
      ("unresolved", expr)   — symbol not found, or a non-numeric expression
    """
    s = expr.strip()
    # Strip trailing `f` on a raw literal.
    if LITERAL_RE.match(s):
        return ("literal", float(s.rstrip("fF")) if "." in s or "f" in s.lower()
                else int(s))
    # Symbol lookup.
    sites = defines.get(s, [])
    if not sites:
        return ("unresolved", s)
    # Take unique values — multiple defines agreeing are fine.
    values = {v for _, v in sites}
    if len(values) == 1:
        v = next(iter(values))
        if "." in v or "f" in v.lower():
            return ("symbol", float(v.rstrip("fF")))
        return ("symbol", int(v))
    # Multiple conflicting values — per-device headers override main.
    return ("ambiguous", sites)


def main(argv: list[str]) -> int:
    verbose = "-v" in argv or "--verbose" in argv

    if not CATALOG.is_file():
        print(f"error: {CATALOG} not found", file=sys.stderr); return 2
    catalog = yaml.safe_load(CATALOG.read_text())
    vars_ = catalog.get("vars") or {}

    calls = collect_get_calls()
    defines = collect_defines()

    findings: list[str] = []
    keys_seen: set[str] = set()

    # Check 1 + 2: every C++ call site should line up with a catalog entry.
    for c in calls:
        key = c["key"]
        keys_seen.add(key)
        entry = vars_.get(key)
        if entry is None:
            findings.append(
                f"{c['file']}:{c['line']}: get_{c['fn']}({key!r}, …) has no "
                f"entry in {CATALOG.name}"
            )
            continue
        # Type match.
        if entry["type"] != c["fn"]:
            findings.append(
                f"{c['file']}:{c['line']}: get_{c['fn']}({key!r}, …) "
                f"but catalog `type: {entry['type']}`"
            )
        # Default cross-check. Skip keys whose default varies by scope
        # (per-device headers, per-platform drivers) or whose reader
        # doesn't go through the Config interface at all (ops_only).
        #   `default: per-device`           — string sentinel (legacy)
        #   `default_per_device: true`      — current convention for
        #                                     keys whose literal default
        #                                     lives in HAL source rather
        #                                     than the catalog. Covers
        #                                     both hal/devices/<name>.h
        #                                     overrides (brightness,
        #                                     night thresholds) and
        #                                     per-platform driver headers
        #                                     (light_exponent: 2.5 for
        #                                     the CDS path, 0.5 for
        #                                     BH1750, same key different
        #                                     sensor math). stra2us-cli's
        #                                     schema doesn't yet have a
        #                                     dedicated `per-platform`
        #                                     flag; reusing this one is
        #                                     fine because the lint's
        #                                     job here is just "don't
        #                                     cross-check a literal that
        #                                     doesn't exist in the YAML."
        catalog_default = entry.get("default")
        if (catalog_default == "per-device"
                or entry.get("default_per_device")
                or entry.get("ops_only")):
            continue
        kind, resolved = parse_default(c["default_expr"], defines)
        if kind == "literal" or kind == "symbol":
            if resolved != catalog_default:
                findings.append(
                    f"{c['file']}:{c['line']}: get_{c['fn']}({key!r}, "
                    f"{c['default_expr']}) resolves to {resolved!r} but "
                    f"catalog `default: {catalog_default!r}`"
                )
        elif kind == "ambiguous":
            if verbose:
                # Not an error — per-device overrides are normal. Only
                # flag if the main definition doesn't match the catalog.
                print(f"note: {c['file']}:{c['line']}: default "
                      f"{c['default_expr']} has multiple #define sites "
                      f"(per-device overrides expected). Not cross-"
                      f"checked against catalog.")
        else:  # unresolved
            if verbose:
                print(f"note: {c['file']}:{c['line']}: default "
                      f"{c['default_expr']!r} not a literal and not a "
                      f"resolvable #define; skipping cross-check.")

    # Check 3: catalog entries with no C++ reader, unless ops_only.
    for key, entry in vars_.items():
        if entry.get("ops_only"):
            continue
        if key not in keys_seen:
            findings.append(
                f"{CATALOG.name}: `{key}` is in the catalog but no "
                f"get_int/get_float call references it. Tag `ops_only: "
                f"true` if intentional."
            )

    if findings:
        print(f"catalog drift — {len(findings)} finding(s):", file=sys.stderr)
        for f in findings:
            print(f"  {f}", file=sys.stderr)
        return 1
    print(f"clean: {len(calls)} call site(s), {len(vars_)} catalog entries.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

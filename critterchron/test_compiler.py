#!/usr/bin/env python3
"""Compile-time test harness for .crit scripts.

Two sources of tests:
  1. `agents/*.crit` — production scripts. All must compile cleanly.
  2. `agents/tests/*.crit` — edge cases. Filename prefix encodes the expected
     outcome per `agents/tests/README.md`:
        `ok_<name>.crit`          -> must compile cleanly
        `<category>_<name>.crit`  -> must raise ValueError matching the category

Usage:
    python3 test_compiler.py          # run all, exit non-zero on any failure
    python3 test_compiler.py -v       # verbose: show each case's outcome line
"""

import sys
from pathlib import Path

from compiler import CritCompiler

ROOT = Path(__file__).parent
PROD_DIR = ROOT / "agents"
TESTS_DIR = ROOT / "agents" / "tests"

# Each rejection category maps to a stable substring that must appear in the
# raised ValueError. Keep these in lockstep with the error messages emitted by
# `compiler.py`. If you change an error message, update the matcher here and
# mention the change in `agents/tests/README.md`.
REJECTION_CATEGORIES = {
    "no_yield":            "never yields",
    "fall_off":            "fall off the bottom",
    "unknown_name":        "not a landmark, agent, or tile predicate",
    "unknown_instruction": "unrecognized instruction",
    "despawn_landmark":    "is a landmark, not an agent",
    "standing_on_name":    "'standing on' only accepts tile predicates",
    "diagonal":            "diagonal",
    "missing_color":       "has no color defined",
}

# Longest-first so e.g. "despawn_landmark" wins over a hypothetical "despawn".
_PREFIXES_SORTED = sorted(REJECTION_CATEGORIES.keys(), key=len, reverse=True)


def classify(path):
    """Return ('ok', None) or ('<category>', expected_substring) for a tests/ file."""
    stem = path.stem
    if stem == "ok" or stem.startswith("ok_"):
        return "ok", None
    for prefix in _PREFIXES_SORTED:
        if stem == prefix or stem.startswith(prefix + "_"):
            return prefix, REJECTION_CATEGORIES[prefix]
    raise ValueError(
        f"Unrecognized test-filename prefix in {path.name!r}. Expected 'ok_' "
        f"or one of: {sorted(REJECTION_CATEGORIES)}. See agents/tests/README.md."
    )


def run_case(path, expect_category, expect_substring):
    """Compile `path` and check the outcome. Returns (passed: bool, message)."""
    c = CritCompiler()
    try:
        c.compile(str(path))
    except ValueError as e:
        err = str(e)
        if expect_substring is None:
            return False, f"expected clean compile, got rejection: {err}"
        if expect_substring in err:
            return True, f"rejected ({expect_category}) \u2014 ok"
        return False, (
            f"rejected but error message missing token "
            f"{expect_substring!r}: {err}"
        )
    # compile() succeeded:
    if expect_substring is None:
        return True, "compiled clean \u2014 ok"
    return False, f"expected rejection ({expect_category}) but compile succeeded"


def main(argv):
    verbose = "-v" in argv or "--verbose" in argv

    cases = []
    for path in sorted(PROD_DIR.glob("*.crit")):
        cases.append((path, "ok", None))
    for path in sorted(TESTS_DIR.glob("*.crit")):
        try:
            category, substring = classify(path)
        except ValueError as e:
            print(f"  [SKIP] {path.relative_to(ROOT)}: {e}")
            continue
        cases.append((path, category, substring))

    if not cases:
        print("No test cases found.")
        return 0

    passed = failed = 0
    for path, category, substring in cases:
        ok, msg = run_case(path, category, substring)
        mark = "PASS" if ok else "FAIL"
        rel = path.relative_to(ROOT)
        if verbose or not ok:
            print(f"  [{mark}] {rel} ({category}): {msg}")
        if ok:
            passed += 1
        else:
            failed += 1

    total = passed + failed
    print(f"\n{passed}/{total} passed" + ("" if failed == 0 else f" \u2014 {failed} failed"))
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

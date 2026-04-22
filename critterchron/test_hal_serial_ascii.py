#!/usr/bin/env python3
"""Guard against Unicode sneaking into Log.* calls on the device.

Particle's SerialLogHandler emits raw bytes; the serial monitor on the
other end doesn't decode UTF-8. A `…` / `—` / smart quote in a
`Log.info(...)` format string renders as three replacement glyphs
(`���` or similar) and obscures the diagnostic. Regression-vector:
copy-paste from prose in the TODO / a chat transcript into a Log
format string.

This test walks `hal/particle/src/` (the canonical tree; the other
platform dirs are wildcard-copied from it at build time), finds every
`Log.<level>(...)` call, and flags any non-ASCII byte between the
opening `(` and its matching `)`. Comments outside the call are fine
— the lint only cares what lands in the binary's serial output.

Usage:
    python3 test_hal_serial_ascii.py

Exit code 0 on clean, 1 on findings. Findings print as
`<path>:<line>: <offending-byte-preview>` so the offender is a click
away in any editor.
"""

from __future__ import annotations
import os
import re
import sys
from pathlib import Path

LOG_CALL_RE = re.compile(rb'\bLog\.(info|warn|error|trace)\s*\(')


def scan_log_calls(data: bytes):
    """Yield (start_offset, end_offset) for each Log.*(...) call in `data`.

    Tracks paren depth with string-literal awareness so a `)` inside a
    quoted format string doesn't close the call early. Handles `"..."`
    (C++ string literal with backslash escapes) and `'...'` (char
    literal). Raw string literals (`R"delim(...)delim"`) aren't used
    in this codebase — if they ever are, this scanner would need to
    grow a raw-string branch.
    """
    for m in LOG_CALL_RE.finditer(data):
        start = m.end() - 1  # position of the `(`
        i = start
        depth = 0
        n = len(data)
        in_dq = False  # inside "..."
        in_sq = False  # inside '...'
        while i < n:
            c = data[i:i+1]
            if in_dq:
                if c == b'\\' and i + 1 < n:
                    i += 2
                    continue
                if c == b'"':
                    in_dq = False
            elif in_sq:
                if c == b'\\' and i + 1 < n:
                    i += 2
                    continue
                if c == b"'":
                    in_sq = False
            else:
                if c == b'"':
                    in_dq = True
                elif c == b"'":
                    in_sq = True
                elif c == b'(':
                    depth += 1
                elif c == b')':
                    depth -= 1
                    if depth == 0:
                        yield (m.start(), i + 1)
                        break
            i += 1
        # Unterminated call (EOF before closing paren): ignore.
        # Parse error would already fail the build; we don't need to
        # reinvent that diagnostic here.


def line_of(data: bytes, offset: int) -> int:
    return data.count(b'\n', 0, offset) + 1


def find_non_ascii(call_bytes: bytes):
    """Yield (relative_offset, byte) for each non-ASCII byte."""
    for i, b in enumerate(call_bytes):
        if b > 0x7F:
            yield (i, b)


def main() -> int:
    here = Path(__file__).resolve().parent
    root = here / "hal" / "particle" / "src"
    if not root.is_dir():
        print(f"error: {root} not found", file=sys.stderr)
        return 2

    findings: list[tuple[str, int, str]] = []
    scanned = 0
    for path in sorted(root.rglob("*")):
        if path.suffix not in (".h", ".cpp", ".c", ".hpp"):
            continue
        data = path.read_bytes()
        scanned += 1
        for start, end in scan_log_calls(data):
            call = data[start:end]
            bad = list(find_non_ascii(call))
            if not bad:
                continue
            rel = path.relative_to(here)
            line = line_of(data, start)
            # Preview: the call itself, with non-ASCII bytes escaped so
            # the printout is terminal-safe. Trim long calls to keep
            # the output readable.
            preview = call.decode("utf-8", errors="backslashreplace")
            if len(preview) > 140:
                preview = preview[:137] + "..."
            preview = preview.replace("\n", "\\n")
            findings.append((str(rel), line, preview))

    if findings:
        print(f"non-ASCII byte(s) in Log.* call(s) — serial output is "
              f"ASCII-only:\n", file=sys.stderr)
        for rel, line, preview in findings:
            print(f"  {rel}:{line}: {preview}", file=sys.stderr)
        print(f"\n{len(findings)} finding(s) across {scanned} file(s).",
              file=sys.stderr)
        return 1

    print(f"clean: scanned {scanned} file(s) under hal/particle/src/")
    return 0


if __name__ == "__main__":
    sys.exit(main())

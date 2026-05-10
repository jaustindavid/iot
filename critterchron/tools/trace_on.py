"""Turn FAILURE_TRIAGE.md §2 trace mode on for one device, with TTL.

Trace mode publishes a multi-frame snapshot every N seconds to a
per-device topic — useful for hands-on debugging but extremely easy
to forget on. THIS WRAPPER ALWAYS SETS A TTL: when the operator walks
away the server expires the `trace_mode` KV and the device reverts
to off (default 0) on its next poll cycle.

Per-device topic to tail:
    critterchron/trace/<device>

Use `tools/snapshot_dump.py --topic critterchron/trace/<device>` to
follow the stream — the wire format is identical to §1 dumps so the
existing parser handles it.

Usage:
    source ./analyzer.env  # or whatever credentials env is in use

    # Default: cadence=4s, ttl=30min — sensible for most debugging
    python3 -m tools.trace_on tommy_tanuki

    # Faster cadence (every 1s), shorter TTL
    python3 -m tools.trace_on tommy_tanuki --cadence 1 --minutes 10

    # Turn it off explicitly (sets to 0, server stops the device)
    python3 -m tools.trace_on tommy_tanuki --off
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

_HERE = Path(__file__).resolve().parent
_REPO = _HERE.parent
if str(_REPO) not in sys.path:
    sys.path.insert(0, str(_REPO))

from tools.s2s_client import client_from_env, Stra2usError  # noqa: E402


# Server caps TTL at 1 week (604800s); we won't touch the cap, but
# refuse to issue something stupid like an 8-hour debug session
# without a deliberate flag.
SOFT_MAX_MINUTES = 480  # 8 hours; longer needs --i-mean-it


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(prog="trace_on",
                                 description="enable §2 trace mode with TTL safety net")
    ap.add_argument("device", help="target device client_id")
    ap.add_argument("--app", default="critterchron",
                    help="app prefix for the KV path")
    ap.add_argument("--cadence", type=int, default=4,
                    help="publish every N seconds (1-60). Default 4.")
    ap.add_argument("--minutes", type=int, default=30,
                    help="how long the trace_mode KV stays set before "
                         "server-TTL expires it. Default 30.")
    ap.add_argument("--off", action="store_true",
                    help="explicitly turn trace mode off (writes 0 with no TTL)")
    ap.add_argument("--i-mean-it", action="store_true",
                    help="bypass the SOFT_MAX_MINUTES safety check")
    ap.add_argument("--server", default=None)
    ap.add_argument("--client-id", default=None)
    ap.add_argument("--secret", default=None)
    args = ap.parse_args(argv)

    try:
        client = client_from_env(args.server, args.client_id, args.secret)
    except Stra2usError as e:
        print(f"client error: {e}", file=sys.stderr)
        return 2

    key = f"{args.app}/{args.device}/trace_mode"

    if args.off:
        client.put(key, 0)  # no TTL — explicit off, persists
        print(f"set {key} = 0 (off)")
        return 0

    if not (1 <= args.cadence <= 60):
        print(f"error: --cadence must be 1..60 (got {args.cadence})",
              file=sys.stderr)
        return 2
    if args.minutes < 1:
        print(f"error: --minutes must be >= 1 (got {args.minutes})",
              file=sys.stderr)
        return 2
    if args.minutes > SOFT_MAX_MINUTES and not args.i_mean_it:
        print(f"error: --minutes {args.minutes} > soft cap {SOFT_MAX_MINUTES}. "
              f"Pass --i-mean-it if you really want to leave trace on that long.",
              file=sys.stderr)
        return 2

    ttl_seconds = args.minutes * 60
    client.put(key, args.cadence, ttl=ttl_seconds)
    print(f"set {key} = {args.cadence} (every {args.cadence}s, "
          f"TTL {args.minutes} min)")
    print(f"  tail: python3 -m tools.snapshot_dump "
          f"--topic {args.app}/trace/{args.device}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

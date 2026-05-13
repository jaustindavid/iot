"""Tail and pretty-print snapshot dumps from FAILURE_TRIAGE.md §1.

Devices publish ASCII dumps to `<app>/public/snapshots`; this tool tails
that queue (each consumer client_id has its own server-side cursor, so
running it alongside the analyzer is fine) and prints each dump with
trigger context + frame timeline.

Also offers a one-shot `--fire` mode that writes a fresh generation
token to a device's `dump_now` KV — the manual operator path. Saves
remembering the `int(time.time())` idiom.

Usage:
    # Authenticate with the same env vars the analyzer uses
    source ./analyzer.env

    # Tail the staging snapshot stream
    python3 -m tools.snapshot_dump --topic critterchron/public/snapshots

    # Manually trigger a dump on tommy and exit
    python3 -m tools.snapshot_dump --fire --device tommy_tanuki

    # Tail and print only specific devices
    python3 -m tools.snapshot_dump --device tommy_tanuki --device tammy_tanuki
"""

from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

# Allow `python3 tools/snapshot_dump.py` (script invocation) and
# `python3 -m tools.snapshot_dump` (module invocation).
_HERE = Path(__file__).resolve().parent
_REPO = _HERE.parent
if str(_REPO) not in sys.path:
    sys.path.insert(0, str(_REPO))

from tools.s2s_extras import client_from_env, Stra2usError  # noqa: E402


def _parse_dump(body: str) -> dict:
    """Parse one snapshot publish body into {header, frames}.

    Wire format (see hal/SnapshotBuffer.cpp):
      v=1 device=X ts=Y trigger=Z detail=... frames=N
      tick=A millis=B agents=C heap=D seeks_d=E phys_max=F rend_max=G
      ...
    """
    if isinstance(body, bytes):
        body = body.decode("utf-8", errors="replace")
    lines = [l for l in body.split("\n") if l.strip()]
    if not lines:
        return {"header": {}, "frames": []}
    header = _parse_kv_line(lines[0])
    frames = [_parse_kv_line(l) for l in lines[1:]]
    return {"header": header, "frames": frames}


def _parse_kv_line(line: str) -> dict:
    """k=v split on whitespace, with `detail=` consuming line tail."""
    out = {}
    parts = line.split(" ")
    i = 0
    while i < len(parts):
        tok = parts[i]
        if not tok or "=" not in tok:
            i += 1
            continue
        k, _, v = tok.partition("=")
        if k == "detail":
            v = " ".join([v] + parts[i + 1:])
            out[k] = v
            return out
        out[k] = _coerce(v)
        i += 1
    return out


def _coerce(v: str):
    try:
        return int(v)
    except ValueError:
        return v


def _print_dump(parsed: dict) -> None:
    h = parsed["header"]
    print(f"\n══ {h.get('device','?')}  ts={h.get('ts','?')}  "
          f"trigger={h.get('trigger','?')}  "
          f"detail={h.get('detail','')}  "
          f"frames={len(parsed['frames'])}")
    for f in parsed["frames"]:
        print(f"  tick={f.get('tick','?'):>6}  "
              f"ms={f.get('millis','?'):>10}  "
              f"agents={f.get('agents','?'):>3}  "
              f"heap={f.get('heap','?'):>7}  "
              f"seeks_d={f.get('seeks_d','?'):>4}  "
              f"phys={f.get('phys_max','?'):>6}us  "
              f"rend={f.get('rend_max','?'):>6}us")


def cmd_tail(args, client):
    devices = set(args.device or [])
    print(f"tailing {args.topic} ...", file=sys.stderr)
    while True:
        try:
            msg = client.consume(args.topic, envelope=True)
        except Stra2usError as e:
            print(f"consume failed: {e}", file=sys.stderr)
            time.sleep(args.poll)
            continue
        if msg is None:
            time.sleep(args.poll)
            continue
        body = msg.get("data")
        if isinstance(body, str):
            parsed = _parse_dump(body)
            dev = parsed["header"].get("device") or msg.get("client_id", "?")
            if devices and dev not in devices:
                continue
            _print_dump(parsed)


def cmd_fire(args, client):
    if not args.device or len(args.device) != 1:
        print("--fire requires exactly one --device", file=sys.stderr)
        return 2
    device = args.device[0]
    gen = int(time.time())
    key = f"{args.app}/{device}/dump_now"
    client.put(key, gen)
    print(f"set {key} = {gen}")
    return 0


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(prog="snapshot_dump")
    ap.add_argument("--topic", default="critterchron/public/snapshots",
                    help="snapshot queue to tail")
    ap.add_argument("--device", action="append",
                    help="filter to specific device(s); repeat for multiple")
    ap.add_argument("--app", default="critterchron",
                    help="app prefix for KV writes (--fire mode)")
    ap.add_argument("--fire", action="store_true",
                    help="manually trigger one dump on --device and exit "
                         "(writes a fresh generation token to dump_now)")
    ap.add_argument("--poll", type=float, default=2.0,
                    help="seconds between consume polls when queue is empty")
    ap.add_argument("--server", default=None)
    ap.add_argument("--client-id", default=None)
    ap.add_argument("--secret", default=None)
    args = ap.parse_args(argv)

    try:
        client = client_from_env(args.server, args.client_id, args.secret)
    except Stra2usError as e:
        print(f"client error: {e}", file=sys.stderr)
        return 2

    if args.fire:
        return cmd_fire(args, client)
    try:
        cmd_tail(args, client)
    except KeyboardInterrupt:
        print()
        return 0
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

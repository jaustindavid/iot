"""Replicate a device-side snapshot publish to see the actual 401 body.

The on-device `read_response_` discards 4xx response bodies, so the
heartbeat error channel only carries `publish=401` with no `detail`
text. This script signs and POSTs the same request shape and prints
the raw response so we can see which 401 sub-type fires.

Auth uses tommy's credentials from the device header — read them out
of `hal/devices/tommy_tanuki.h` so we're authenticating identically.

Usage:
    python3 scratch_snapshot_repro.py
"""
from __future__ import annotations

import hashlib
import hmac
import re
import sys
import time
from pathlib import Path

import requests

DEVICE_HEADER = Path(__file__).resolve().parent / "hal/devices/tommy_tanuki.h"


def read_creds() -> dict:
    """Two-pass parse so preprocessor indirections like
    `#define STRA2US_CLIENT_ID DEVICE_NAME` resolve regardless of the
    order keys appear in the header."""
    text = DEVICE_HEADER.read_text()
    raw: dict = {}
    for m in re.finditer(r'#define\s+(\w+)\s+([^\s/].*?)(?:\s|$)', text):
        raw[m.group(1)] = m.group(2).strip().strip('"')
    # Resolve any value that's also a #define name (one level — that's
    # all critterchron's headers do).
    for k, v in list(raw.items()):
        if v in raw:
            raw[k] = raw[v]
    needed = ("STRA2US_HOST", "STRA2US_PORT", "STRA2US_SECRET_HEX",
              "STRA2US_CLIENT_ID", "DEVICE_NAME")
    missing = [k for k in needed if k not in raw]
    if missing:
        raise RuntimeError(f"creds: missing {missing} in {DEVICE_HEADER}")
    return {k: raw[k] for k in needed}


def sign(secret_hex: str, uri: str, body: bytes, ts: int) -> str:
    secret = bytes.fromhex(secret_hex)
    payload = uri.encode("utf-8") + body + str(ts).encode("utf-8")
    return hmac.new(secret, payload, hashlib.sha256).hexdigest()


def main() -> int:
    creds = read_creds()
    base_url = f"http://{creds['STRA2US_HOST']}:{creds['STRA2US_PORT']}"
    topic = "critterchron/public/snapshots"
    uri = f"/q/{topic}"

    # Realistic snapshot body — same shape as SnapshotBuffer.cpp::encode().
    body = (
        f"v=1 device={creds['DEVICE_NAME']} ts={int(time.time())} "
        "trigger=manual detail=repro frames=2\n"
        "tick=1 millis=1000 agents=5 heap=100000 seeks_d=0 phys_max=2700 rend_max=0\n"
        "tick=2 millis=1125 agents=5 heap=100000 seeks_d=0 phys_max=2800 rend_max=0"
    )
    body_b = body.encode("utf-8")

    ts = int(time.time())
    sig = sign(creds["STRA2US_SECRET_HEX"], uri, body_b, ts)

    print(f"POST {base_url}{uri}")
    print(f"  client_id  = {creds['STRA2US_CLIENT_ID']}")
    print(f"  body bytes = {len(body_b)}")
    print(f"  ts         = {ts}")
    print(f"  sig (8)    = {sig[:8]}...")
    print()

    headers = {
        "X-Client-ID": creds["STRA2US_CLIENT_ID"],
        "X-Timestamp": str(ts),
        "X-Signature": sig,
        "Content-Type": "text/plain",
    }
    r = requests.post(base_url + uri, data=body_b, headers=headers, timeout=10)
    print(f"-> {r.status_code} {r.reason}")
    print(f"   headers : {dict(r.headers)}")
    print(f"   body    : {r.text[:800]}")
    return 0 if 200 <= r.status_code < 300 else 1


if __name__ == "__main__":
    sys.exit(main())

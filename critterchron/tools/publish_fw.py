"""Stage an ESP32 firmware binary into Stra2us KV for pull-OTA.

    python3 tools/publish_fw.py path/to/firmware.bin --target esp32c3
        [--dry-run]               # print summary, no network
        [--force]                 # upload even if remote sidecar matches

Auth comes from `stra2us_cli.client_from_env`, same resolution path the
`stra2us` CLI uses interactively (env vars or a `~/.stra2us` profile).

Key layout: `critterchron/fw/<target>` holds the binary blob;
`critterchron/fw/<target>/sha` holds the sidecar string
`<sha256>:<size>`. Phase 2 (device-side pull-OTA) will add pointer keys
`critterchron/<device>/fw_target` (with `critterchron/fw_target` as the
fleet-wide fallback) to direct each device at a target name; for
Phase 1 this tool just stages the binary at the target slot.

Idempotency: probes the remote sidecar and skips upload when it matches.
Same logic as publish_ir.py.

Tear-safety: blob first, sidecar second. If the process dies between the
two writes, the sidecar still points at the *previous* sha so devices
see no change and skip the apply — fail-safe.

Wire-format notes (Phase 1 is verifying these):
  * blob is sent as Python `bytes`, which stra2us_cli encodes as msgpack
    `bin`. For an actual binary that's the *correct* shape — the
    IR-OTA bytes-to-str fix (2026-04-28) was specifically for the IR
    blob which is semantically text. Firmware binaries are binary.
    The on-device `kv_fetch_str_` widening (2026-04-28) accepts both
    str and bin types; large binaries land as `bin32` (0xc6) since
    they exceed 64KB.
  * sidecar is a string `<sha256_hex>:<size_decimal>`, same format
    as the IR sidecar.
"""

from __future__ import annotations
import argparse
import hashlib
import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
_REPO = os.path.abspath(os.path.join(_HERE, ".."))
if _REPO not in sys.path:
    sys.path.insert(0, _REPO)

from stra2us_cli import client_from_env              # noqa: E402


def _read_sidecar(client, sha_key: str) -> str | None:
    """Mirror of publish_ir.py's _read_sidecar — degrade to None on any
    failure so the caller's idempotency check just falls through to
    "proceed with upload" instead of branching on error types."""
    try:
        v = client.get(sha_key)
    except Exception:
        return None
    if v is None:
        return None
    if isinstance(v, bytes):
        try:
            return v.decode("utf-8")
        except UnicodeDecodeError:
            return None
    return v


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("binary", help="Path to a built firmware .bin")
    ap.add_argument("--target", required=True,
                    help="Target platform identifier (e.g. esp32c3, esp32s3). "
                         "Becomes the KV key suffix: critterchron/fw/<target>.")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--force", action="store_true",
                    help="Upload even if the remote sidecar already matches")
    args = ap.parse_args()

    # Read the binary into memory. ESP32 firmware is ~1MB; trivially fits.
    # No streaming on the publish side — we need the full bytes anyway to
    # compute the SHA, and stra2us_cli.put() takes a single value.
    try:
        with open(args.binary, "rb") as f:
            blob_bytes = f.read()
    except Exception as e:
        print(f"error: read failed: {args.binary}: {e}", file=sys.stderr)
        return 1

    size       = len(blob_bytes)
    sha        = hashlib.sha256(blob_bytes).hexdigest()
    sidecar    = f"{sha}:{size}"
    key        = f"critterchron/fw/{args.target}"
    sha_key    = f"critterchron/fw/{args.target}/sha"

    print(f"binary:  {args.binary}")
    print(f"target:  {args.target}")
    print(f"key:     {key}")
    print(f"size:    {size} bytes ({size / 1024:.1f} KiB)")
    print(f"sha256:  {sha}")
    print(f"sidecar: {sidecar}")

    if args.dry_run:
        print("dry-run: not uploading")
        return 0

    try:
        client = client_from_env()
    except Exception as e:
        print(f"error: stra2us client init failed: {e}", file=sys.stderr)
        return 2

    if not args.force:
        remote = _read_sidecar(client, sha_key)
        if remote == sidecar:
            print("up-to-date: remote sidecar matches "
                  "(use --force to re-upload)")
            return 0

    # Blob first, sidecar second. Same tear-safe ordering as publish_ir.py.
    # Pass blob_bytes (NOT a decoded str): firmware is binary, msgpack
    # `bin` is the correct wire shape. Contrast with publish_ir.py which
    # pre-2026-04-28 was passing bytes for the IR text blob and got it
    # wrong; that fix doesn't apply here because the value really is
    # bytes-not-text.
    try:
        client.put(key, blob_bytes)
    except Exception as e:
        print(f"error: blob upload failed: {e}", file=sys.stderr)
        return 3
    try:
        client.put(sha_key, sidecar)
    except Exception as e:
        print(f"error: blob uploaded but sidecar upload failed: {e}",
              file=sys.stderr)
        print( "       retry with --force to restore the sidecar",
              file=sys.stderr)
        return 3

    print(f"published: {key}  ({size} bytes)")
    print(f"sidecar:   {sha_key}  -> {sidecar}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

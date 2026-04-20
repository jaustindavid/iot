"""Compile a .crit, encode to the OTA wire format, PUT into Stra2us KV.

    python3 tools/publish_ir.py agents/thyme.crit
        [--name thyme]            # override script name (default: file stem)
        [--server http://host:p]  # override server (else env STRA2US_HOST)
        [--client-id <id>]        # else env STRA2US_CLIENT_ID
        [--secret <hex>]          # else env STRA2US_SECRET_HEX
        [--dry-run]               # encode + print summary, no network
        [--force]                 # upload even if src_sha256 matches remote

Key layout: `critterchron/scripts/<name>` holds the blob. Separate tool
will set `critterchron/<device>/ir = <name>` to point a device at it.

Idempotency: decodes the remote blob's metadata and compares src_sha256.
If identical, skips the upload unless --force is given. encoded_at drifts
every invocation, so byte-exact comparison would always miss.
"""

from __future__ import annotations
import argparse
import datetime as _dt
import hashlib
import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
_REPO = os.path.abspath(os.path.join(_HERE, ".."))
if _REPO not in sys.path:
    sys.path.insert(0, _REPO)

from compiler import CritCompiler                    # noqa: E402
from hal.ir import ir_text                           # noqa: E402
from tools.s2s_client import client_from_env, Stra2usError  # noqa: E402


ENCODER_VERSION = "publish_ir/1"


def _now_iso() -> str:
    return _dt.datetime.now(_dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def _build_blob(script_path: str, name: str) -> tuple[str, str, int]:
    with open(script_path) as f:
        src = f.read()
    ir = CritCompiler().compile(script_path)
    src_sha = hashlib.sha256(src.encode("utf-8")).hexdigest()
    meta = {
        "name":            name,
        "src_sha256":      src_sha,
        "encoded_at":      _now_iso(),
        "encoder_version": ENCODER_VERSION,
        "ir_version":      ir.get("ir_version", 1),
        "source":          src,
    }
    blob = ir_text.encode(ir, meta)
    return blob, src_sha, ir.get("ir_version", 1)


def _remote_sha(client, key: str) -> str | None:
    """Return src_sha256 from the remote blob's metadata, or None if no
    remote blob (or the remote blob can't be parsed)."""
    value = client.get(key)
    if value is None:
        return None
    if isinstance(value, bytes):
        try:
            value = value.decode("utf-8")
        except UnicodeDecodeError:
            return None
    if not isinstance(value, str):
        return None
    try:
        _, meta = ir_text.decode(value)
    except ir_text.DecodeError:
        return None
    return meta.get("src_sha256")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("script", help="Path to a .crit file")
    ap.add_argument("--name", help="Script name (default: file stem)")
    ap.add_argument("--server")
    ap.add_argument("--client-id", dest="client_id")
    ap.add_argument("--secret", dest="secret_hex")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--force", action="store_true",
                    help="Upload even if src_sha256 matches remote")
    args = ap.parse_args()

    name = args.name or os.path.splitext(os.path.basename(args.script))[0]
    key  = f"critterchron/scripts/{name}"

    try:
        blob, src_sha, ir_ver = _build_blob(args.script, name)
    except Exception as e:
        print(f"error: compile/encode failed: {e}", file=sys.stderr)
        return 1

    size = len(blob.encode("utf-8"))
    print(f"script:    {args.script}")
    print(f"name:      {name}")
    print(f"key:       {key}")
    print(f"size:      {size} bytes")
    print(f"src_sha:   {src_sha}")
    print(f"ir_ver:    {ir_ver}")

    if args.dry_run:
        print("dry-run: not uploading")
        return 0

    try:
        client = client_from_env(
            base_url=args.server,
            client_id=args.client_id,
            secret_hex=args.secret_hex,
        )
    except Stra2usError as e:
        print(f"error: {e}", file=sys.stderr)
        return 2

    if not args.force:
        try:
            remote_sha = _remote_sha(client, key)
        except Stra2usError as e:
            print(f"warning: remote probe failed ({e}); proceeding with upload")
            remote_sha = None
        if remote_sha == src_sha:
            print(f"up-to-date: remote src_sha matches (use --force to re-upload)")
            return 0

    try:
        client.put(key, blob)
    except Stra2usError as e:
        print(f"error: {e}", file=sys.stderr)
        return 3

    print(f"published: {client.base_url}/kv/{key}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

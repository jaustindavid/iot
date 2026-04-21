"""Compile a .crit, encode to the OTA wire format, PUT into Stra2us KV.

    python3 tools/publish_ir.py agents/thyme.crit
        [--name thyme]            # override script name (default: file stem)
        [--server http://host:p]  # override server (else env STRA2US_HOST)
        [--client-id <id>]        # else env STRA2US_CLIENT_ID
        [--secret <hex>]          # else env STRA2US_SECRET_HEX
        [--dry-run]               # encode + print summary, no network
        [--force]                 # upload even if src_sha256 matches remote
        [--no-source]             # omit the SOURCE trailer (smaller blob;
                                  #   needed to OTA onto RAM-tight devices
                                  #   like rico where buffer < full blob)

Key layout: `critterchron/scripts/<name>` holds the blob. Separate tool
will set `critterchron/<device>/ir = <name>` to point a device at it.

Idempotency: fetches the remote blob and compares bytes against what
we'd upload, with `encoded_at` normalized out (wall-clock drift would
otherwise defeat the check). Catches both source edits and encoder
changes — a src_sha-only comparison would miss the latter, e.g. when
the encoder starts emitting symbolic coords for an unchanged .crit.
"""

from __future__ import annotations
import argparse
import datetime as _dt
import hashlib
import os
import re
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
_REPO = os.path.abspath(os.path.join(_HERE, ".."))
if _REPO not in sys.path:
    sys.path.insert(0, _REPO)

from compiler import CritCompiler                    # noqa: E402
from hal.ir import ir_text                           # noqa: E402
from tools.s2s_client import client_from_env, Stra2usError  # noqa: E402


ENCODER_VERSION = "publish_ir/1"

# `encoded_at` is a publish-time timestamp and `END <hex>` is the Fletcher
# checksum over all bytes before it (so it drifts whenever encoded_at
# drifts). Neither carries semantic content — strip both before hashing
# so the resulting content_sha only moves when the source text or the
# encoder's output semantics change.
#
# Device-side compute_content_sha_ in Stra2usClient.cpp mirrors this
# normalization. If you edit either side, edit both.
_ENCODED_AT_RE = re.compile(r"^encoded_at [^\n]*", flags=re.MULTILINE)
_END_LINE_RE   = re.compile(r"^END [0-9a-fA-F]+\s*$", flags=re.MULTILINE)


def _normalize_for_hash(blob_text: str) -> str:
    s = _ENCODED_AT_RE.sub("encoded_at <stripped>", blob_text, count=1)
    return _END_LINE_RE.sub("END <stripped>", s, count=1)


def _content_sha(blob_text: str) -> str:
    """SHA256 of the blob with wall-clock drift (encoded_at) and the
    Fletcher END line normalized out. Stable across republishes of the
    same source with the same encoder, shifts on source edits OR on
    encoder behavior changes — so devices pull a new blob in both cases
    without pulling one when nothing actually changed."""
    norm = _normalize_for_hash(blob_text)
    return hashlib.sha256(norm.encode("utf-8")).hexdigest()


def _now_iso() -> str:
    return _dt.datetime.now(_dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def _build_blob(script_path: str, name: str,
                include_source: bool = True) -> tuple[str, str, int]:
    with open(script_path) as f:
        src = f.read()
    # Preserve max_x / max_y in the output — the device resolves them
    # against its own geometry at load time. A single published blob
    # works on any grid size; compile-time substitution would silently
    # bake the publisher's 16x16 default into the IR.
    ir = CritCompiler(preserve_symbolic_coords=True).compile(script_path)
    src_sha = hashlib.sha256(src.encode("utf-8")).hexdigest()
    meta = {
        "name":            name,
        "src_sha256":      src_sha,
        "encoded_at":      _now_iso(),
        "encoder_version": ENCODER_VERSION,
        "ir_version":      ir.get("ir_version", 1),
    }
    # SOURCE trailer is optional. Omitting it roughly halves the blob for
    # most scripts (trailer is ~same size as body), at the cost of losing
    # the human-readable source in the KV store. Needed for OTA onto
    # devices whose IR_OTA_BUFFER_BYTES is smaller than the full blob.
    if include_source:
        meta["source"] = src
    blob = ir_text.encode(ir, meta)
    return blob, src_sha, ir.get("ir_version", 1)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("script", help="Path to a .crit file")
    ap.add_argument("--name", help="Script name (default: file stem)")
    ap.add_argument("--server")
    ap.add_argument("--client-id", dest="client_id")
    ap.add_argument("--secret", dest="secret_hex")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--force", action="store_true",
                    help="Upload even if the remote blob already matches")
    ap.add_argument("--no-source", dest="include_source",
                    action="store_false", default=True,
                    help="Omit the SOURCE trailer from the published blob "
                         "(smaller; needed for RAM-tight OTA targets)")
    args = ap.parse_args()

    name    = args.name or os.path.splitext(os.path.basename(args.script))[0]
    key     = f"critterchron/scripts/{name}"
    sha_key = f"critterchron/scripts/{name}/sha"

    try:
        blob, src_sha, ir_ver = _build_blob(args.script, name,
                                            include_source=args.include_source)
    except Exception as e:
        print(f"error: compile/encode failed: {e}", file=sys.stderr)
        return 1

    size        = len(blob.encode("utf-8"))
    content_sha = _content_sha(blob)
    print(f"script:     {args.script}")
    print(f"name:       {name}")
    print(f"key:        {key}")
    print(f"size:       {size} bytes")
    print(f"src_sha:    {src_sha}")
    print(f"content_sha:{content_sha}")
    print(f"ir_ver:     {ir_ver}")

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
        # Compare *content_sha*, not source sha. The encoder's output can
        # change without a source edit (e.g. new preserve_symbolic_coords
        # mode), and a src-sha-only check would silently skip the upload
        # and leave devices running the old encoding. One extra GET on
        # the happy path is cheap compared to a debug session chasing a
        # "but I published it" ghost.
        try:
            remote_blob = client.get(key)
        except Stra2usError as e:
            print(f"warning: remote probe failed ({e}); proceeding with upload")
            remote_blob = None
        if isinstance(remote_blob, bytes):
            try:
                remote_blob = remote_blob.decode("utf-8")
            except UnicodeDecodeError:
                remote_blob = None
        if isinstance(remote_blob, str):
            if _content_sha(remote_blob) == content_sha:
                print(f"up-to-date: remote content_sha matches (use --force to re-upload)")
                return 0

    # Upload order matters: blob first, sidecar second. If the process dies
    # between the two writes the sidecar still points at the *previous* sha,
    # so devices see no change and skip the apply — fail-safe. The device
    # recomputes content_sha on the fetched blob and checks it matches the
    # sidecar, catching the reversed tear (sidecar updated but blob stale)
    # by refusing to apply.
    try:
        client.put(key, blob)
    except Stra2usError as e:
        print(f"error: {e}", file=sys.stderr)
        return 3
    try:
        client.put(sha_key, content_sha)
    except Stra2usError as e:
        print(f"error: blob uploaded but sidecar sha upload failed: {e}",
              file=sys.stderr)
        print(f"       retry with --force to restore sidecar", file=sys.stderr)
        return 3

    print(f"published: {client.base_url}/kv/{key}")
    print(f"sidecar:   {client.base_url}/kv/{sha_key}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

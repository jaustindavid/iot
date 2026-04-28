"""Compile a .crit, encode to the OTA wire format, PUT into Stra2us KV.

    python3 tools/publish_ir.py agents/thyme.crit
        [--name thyme]            # override script name (default: file stem)
        [--dry-run]               # encode + print summary, no network
        [--force]                 # upload even if remote sidecar matches
        [--source]                # include SOURCE trailer (roughly doubles
                                  #   blob size; for human-readable inspection
                                  #   via the KV store). Default: omit —
                                  #   devices never read SOURCE at runtime and
                                  #   the extra bytes regularly push blobs past
                                  #   IR_OTA_BUFFER_BYTES, which silently kills
                                  #   OTA on buffer-tight devices like rico.

Auth comes from `stra2us_cli.client_from_env`, which reads either the
`STRA2US_*` env vars or a `~/.stra2us` profile — same resolution the
`stra2us` CLI uses interactively. No `--server` / `--client-id` /
`--secret` flags here; configure via that path instead.

Key layout: `critterchron/scripts/<name>` holds the blob bytes;
`critterchron/scripts/<name>/sha` holds the sidecar string
`<content_sha>:<size>`. A separate tool (`set_ir_pointer.py`) sets
`critterchron/<device>/ir = <name>` to point a device at a script.

Idempotency: probes the remote sidecar and skips the upload when it
matches what we'd write. Sidecar comparison is enough — it IS the
content_sha, and we always upload blob then sidecar (so a stale
sidecar implies a stale blob). One small GET vs the original's full-
blob fetch.

Tear-safety: blob first, sidecar second. If the process dies between
the two writes, the sidecar still points at the *previous* sha so
devices skip the apply (fail-safe). Reverse tear (sidecar updated,
blob stale) is caught device-side by the recompute-on-fetch check.
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
from stra2us_cli import client_from_env              # noqa: E402


ENCODER_VERSION = "publish_ir/1"

# Mirrors the default `IR_OTA_BUFFER_BYTES` in
# `hal/particle/src/Stra2usClient.h`. Devices at that default silently
# reject blobs whose HTTP body exceeds this cap (the rejection at
# `kv_fetch_str_` line 472 only surfaces via serial as "ir_poll: fetch
# failed"). Blobs over this threshold get a loud stderr warning here so
# the operator at least sees it publisher-side. Device headers can
# override IR_OTA_BUFFER_BYTES smaller or larger; without device-reported
# telemetry we can't know per-device, so we warn against the default and
# let the operator judge. Keep in lockstep with the device header.
DEFAULT_OTA_BUFFER_BYTES = 8192

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
    without pulling one when nothing actually changed.

    Re-exported because `set_ir_pointer.py` imports it for the same
    normalization on the pointer side; keep the symbol stable."""
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


def _read_sidecar(client, sha_key: str) -> str | None:
    """Fetch the sidecar value, normalizing bytes→str. Returns None on
    any failure (missing key, network blip, decode error) so the caller
    can degrade to "proceed with upload" without a special-case branch."""
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
    ap.add_argument("script", help="Path to a .crit file")
    ap.add_argument("--name", help="Script name (default: file stem)")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--force", action="store_true",
                    help="Upload even if the remote sidecar already matches")
    ap.add_argument("--source", dest="include_source",
                    action="store_true", default=False,
                    help="Include the SOURCE trailer in the published blob "
                         "(human-readable inspection via the KV store; "
                         "roughly doubles blob size). Default: omit — "
                         "devices never read SOURCE at runtime.")
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

    blob_bytes  = blob.encode("utf-8")
    size        = len(blob_bytes)
    content_sha = _content_sha(blob)
    sidecar     = f"{content_sha}:{size}"
    source_tag  = "with source" if args.include_source else "no source"

    print(f"script:     {args.script}")
    print(f"name:       {name}")
    print(f"key:        {key}")
    print(f"size:       {size} bytes ({source_tag})")
    print(f"src_sha:    {src_sha}")
    print(f"content_sha:{content_sha}")
    print(f"ir_ver:     {ir_ver}")

    if size > DEFAULT_OTA_BUFFER_BYTES:
        overshoot = size - DEFAULT_OTA_BUFFER_BYTES
        # Branch on whether SOURCE is on: if it is, suggest dropping it
        # first (free ~halving). If it's already off, only fix is bumping
        # IR_OTA_BUFFER_BYTES on the device.
        if args.include_source:
            remedy = (
                "         Drop the SOURCE trailer (remove --source) —\n"
                "         roughly halves blob size for most scripts — or\n"
                "         raise IR_OTA_BUFFER_BYTES in the relevant\n"
                "         device header.\n"
            )
        else:
            remedy = (
                "         Blob is already published without SOURCE, so no\n"
                "         publisher-side fix is available. Raise\n"
                "         IR_OTA_BUFFER_BYTES in the relevant device header.\n"
            )
        print(
            f"\nWARNING: size exceeds default OTA buffer "
            f"({DEFAULT_OTA_BUFFER_BYTES} bytes) by {overshoot} bytes.\n"
            f"         Devices running the default IR_OTA_BUFFER_BYTES will\n"
            f"         reject this blob at fetch time and silently stay on\n"
            f"         the previously-loaded script (visible on serial only\n"
            f"         as 'ir_poll: fetch failed').\n"
            f"{remedy}",
            file=sys.stderr,
        )

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

    # Upload order: blob first, sidecar second. If the process dies
    # between the two writes, the sidecar still points at the *previous*
    # sha so devices see no change and skip the apply — fail-safe. The
    # device recomputes content_sha on the fetched blob and checks it
    # matches the sidecar, catching the reversed tear.
    #
    # Sidecar format: "<64-char content_sha>:<size_bytes>". The size suffix
    # lets the device skip the big blob fetch entirely when the blob would
    # overrun IR_OTA_BUFFER_BYTES, avoiding the oversize-crash path.
    # Legacy devices that expect bare sha get a 64-hex prefix and ignore
    # the suffix (the `sha_len == 64` gate fails closed on length != 64,
    # which degrades to "miss this poll, retry next cycle" — fail-safe).
    try:
        # Pass the str, not blob_bytes: stra2us_cli's `put()` calls
        # `msgpack.packb(value, use_bin_type=True)`, which serializes
        # Python `bytes` as msgpack `bin` and Python `str` as msgpack
        # `str`. The IR blob is semantically text (UTF-8, LF-terminated
        # per OTA_IR.md), and on-device `kv_fetch_str_` originally only
        # accepted msgpack `str` types — passing bytes here got the
        # value stored as bin and made the device reject the fetch.
        # Devices now also accept bin defensively, but storing as str
        # is the semantically correct shape and keeps inspection
        # tools that print the value (admin UI, `stra2us get`) showing
        # the text rather than a hex dump.
        client.put(key, blob)
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

    print(f"published: {key}  ({size} bytes, {source_tag})")
    print(f"sidecar:   {sha_key}  -> {sidecar}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

"""Set the OTA IR pointer for a device.

    python3 tools/set_ir_pointer.py <device> <script-name>
        [--server http://host:p]
        [--client-id <id>]
        [--secret <hex>]
        [--clear]          # delete the pointer (revert to compiled-in default)

Key written: `critterchron/<device>/ir = <script-name>` (msgpack str). The
device polls this on its heartbeat cycle; on change it fetches
`critterchron/scripts/<script-name>` and hot-swaps the engine.

Script must already be published via tools/publish_ir.py. This tool
verifies the target exists before writing the pointer (a GET on the
sidecar sha key, with a fallback probe of the blob key for pre-sidecar
publishes). Pass --force to skip the check.
"""

from __future__ import annotations
import argparse
import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
_REPO = os.path.abspath(os.path.join(_HERE, ".."))
if _REPO not in sys.path:
    sys.path.insert(0, _REPO)

from tools.s2s_client import client_from_env, Stra2usError  # noqa: E402
from tools.publish_ir import _content_sha  # noqa: E402


def _as_text(value) -> str | None:
    """Normalize a KV response to str, or None if the server returned
    nothing usable. Empty strings count as 'missing' — our publish flow
    never writes an empty blob or sidecar, so empty == absent.

    **Strips leading/trailing whitespace.** Fine for sidecar-like values
    (sha is hex, any whitespace is noise). NOT fine for the blob itself,
    whose trailing `\\n` is a real byte that counts toward `size`. Use
    `_as_raw_text` for the blob."""
    if value is None:
        return None
    if isinstance(value, bytes):
        try:
            value = value.decode("utf-8")
        except UnicodeDecodeError:
            return None
    if not isinstance(value, str):
        return None
    v = value.strip()
    return v if v else None


def _as_raw_text(value) -> str | None:
    """Like `_as_text` but without the `.strip()`. The OTA blob ends with
    `END <fletcher>\\n` — that trailing newline is counted in the
    `size` the publisher records in the sidecar, so stripping it would
    make the byte-length cross-check off by one."""
    if value is None:
        return None
    if isinstance(value, bytes):
        try:
            value = value.decode("utf-8")
        except UnicodeDecodeError:
            return None
    if not isinstance(value, str):
        return None
    return value if value else None


def _script_exists(client, name: str) -> tuple[bool, str]:
    """Verify a published script exists *and is internally consistent*.
    Fetches both the sidecar and the blob, then cross-checks: blob size
    matches the sidecar's declared size, and blob's recomputed
    content_sha matches the sidecar's sha. Either mismatch is a "reverse
    tear" (sidecar updated but blob stale or missing) and blocks the
    repoint — the device would detect the same mismatch later and refuse
    to apply, so flagging it here saves a confusing round-trip.

    Presence-only checks aren't enough — the server can hand back empty
    or non-string bodies for missing keys, which would pass a naive
    `is None` check.

    Legacy sidecars (pre-2026-04-22, bare 64-hex sha with no size suffix)
    are accepted — the size check is skipped when sidecar size is absent,
    but the content_sha cross-check still runs."""
    sha_key = f"critterchron/scripts/{name}/sha"
    blob_key = f"critterchron/scripts/{name}"

    # 1. Fetch sidecar.
    try:
        sha = _as_text(client.get(sha_key))
    except Stra2usError as e:
        sha = None
        sha_err = str(e)
    else:
        sha_err = None
    # Sidecar format: "<64-hex sha>" (legacy) or
    # "<64-hex sha>:<decimal size_bytes>" (current).
    sidecar_sha: str | None = None
    sidecar_size: int | None = None
    if sha:
        sha_part, _, size_part = sha.partition(":")
        if (len(sha_part) == 64
                and all(c in "0123456789abcdefABCDEF" for c in sha_part)
                and (size_part == "" or size_part.isdigit())):
            sidecar_sha = sha_part.lower()
            sidecar_size = int(size_part) if size_part else None

    # 2. Fetch blob. Multi-KB but still cheap over HTTP — this runs
    # once per repoint, not per heartbeat. Use `_as_raw_text` (no
    # strip) so the trailing `\n` counts toward byte-length —
    # publisher-side size includes it.
    try:
        blob = _as_raw_text(client.get(blob_key))
    except Stra2usError as e:
        if sidecar_sha is None:
            return False, f"sidecar missing ({sha_err or 'empty'}); blob probe failed: {e}"
        return False, f"sidecar ok but blob fetch failed: {e}"

    if not blob:
        if sidecar_sha is None:
            return False, "no sidecar and no blob"
        return False, (f"reverse tear: sidecar sha={sidecar_sha[:8]}… "
                       f"present but blob missing")
    if not blob.startswith("CRIT "):
        return False, "blob present but missing CRIT header"

    # 3. Cross-check (only if we have a usable sidecar).
    if sidecar_sha is None:
        # Legacy / malformed sidecar. Blob is parseable, trust it — this
        # is the same fallback the old code did, just after a blob fetch
        # that didn't cost us anything meaningful.
        return True, f"blob {blob_key} present ({len(blob)} bytes, no sidecar)"

    blob_bytes = blob.encode("utf-8")
    blob_size  = len(blob_bytes)
    if sidecar_size is not None and sidecar_size != blob_size:
        return False, (f"reverse tear: sidecar size={sidecar_size}B but blob "
                       f"is {blob_size}B (sha={sidecar_sha[:8]}…)")

    actual_sha = _content_sha(blob)
    if actual_sha != sidecar_sha:
        return False, (f"reverse tear: sidecar sha={sidecar_sha[:8]}… "
                       f"but blob content_sha is {actual_sha[:8]}…")

    detail = f"sidecar {sha_key} sha={sidecar_sha[:8]}…"
    if sidecar_size is not None:
        detail += f" size={sidecar_size}B"
    detail += " (sha+size cross-checked against blob)"
    return True, detail


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("device", help="Device name, e.g. rachel_raccoon")
    ap.add_argument("script", nargs="?",
                    help="Script name (omit with --clear to unset)")
    ap.add_argument("--server")
    ap.add_argument("--client-id", dest="client_id")
    ap.add_argument("--secret", dest="secret_hex")
    ap.add_argument("--clear", action="store_true",
                    help="Unset the pointer (device reverts to its compiled-in default)")
    ap.add_argument("--force", action="store_true",
                    help="Skip the existence check and write the pointer anyway")
    args = ap.parse_args()

    if args.clear and args.script:
        ap.error("--clear takes no script argument")
    if not args.clear and not args.script:
        ap.error("script name required (or pass --clear)")

    key = f"critterchron/{args.device}/ir"

    try:
        client = client_from_env(
            base_url=args.server,
            client_id=args.client_id,
            secret_hex=args.secret_hex,
        )
    except Stra2usError as e:
        print(f"error: {e}", file=sys.stderr)
        return 2

    if args.clear:
        # No DELETE on the server — write an empty string. Device side sees
        # len==0 on ir_poll and skips the fetch, so the currently-loaded
        # script sticks. A reboot then comes up on the compiled-in default.
        try:
            client.put(key, "")
        except Stra2usError as e:
            print(f"error: {e}", file=sys.stderr)
            return 3
        print(f"cleared: {client.base_url}/kv/{key}")
        return 0

    if not args.force:
        ok, detail = _script_exists(client, args.script)
        if not ok:
            print(f"error: {args.script!r} not found on {client.base_url}: {detail}",
                  file=sys.stderr)
            print(f"       publish it first (tools/publish_ir.py) or pass --force",
                  file=sys.stderr)
            return 4
        print(f"verified: {detail}")

    try:
        client.put(key, args.script)
    except Stra2usError as e:
        print(f"error: {e}", file=sys.stderr)
        return 3
    print(f"set: {client.base_url}/kv/{key} → {args.script}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

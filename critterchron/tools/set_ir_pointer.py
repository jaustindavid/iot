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


def _as_text(value) -> str | None:
    """Normalize a KV response to str, or None if the server returned
    nothing usable. Empty strings count as 'missing' — our publish flow
    never writes an empty blob or sidecar, so empty == absent."""
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


def _script_exists(client, name: str) -> tuple[bool, str]:
    """Verify a published script exists by inspecting content, not just
    presence. `is None` isn't enough — the server can hand back empty or
    non-string bodies for missing keys, which would pass a naive check.

    Prefer the sidecar (64-byte fetch); fall back to the blob for
    pre-sidecar publishes."""
    sha_key = f"critterchron/scripts/{name}/sha"
    blob_key = f"critterchron/scripts/{name}"

    try:
        sha = _as_text(client.get(sha_key))
    except Stra2usError as e:
        sha = None
        sha_err = str(e)
    else:
        sha_err = None
    if sha and len(sha) == 64 and all(c in "0123456789abcdefABCDEF" for c in sha):
        return True, f"sidecar {sha_key} sha={sha[:8]}…"

    try:
        blob = _as_text(client.get(blob_key))
    except Stra2usError as e:
        return False, f"sidecar missing ({sha_err or 'empty'}); blob probe failed: {e}"
    if blob and blob.startswith("CRIT "):
        return True, f"blob {blob_key} present ({len(blob)} bytes, no sidecar)"

    reason = "no sidecar and no blob" if not blob else "blob present but missing CRIT header"
    return False, reason


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

"""Stra2us catalog / inspect / set CLI.

Reads `critterchron.s2s.yaml` at the repo root and offers three verbs
against the live Stra2us KV store:

    python3 tools/s2s.py catalog
        Print the variable table from the catalog.

    python3 tools/s2s.py show <device> [<key>]
        Show the resolution chain for one key or all keys:
        device-scope → app-scope → compiled-in default → effective.

    python3 tools/s2s.py set <device> <key> <value>
    python3 tools/s2s.py set --app <key> <value>
        Write a value after validating type, scope, and range against the
        catalog. Use `--unset` to write an empty string (device-side
        effect depends on key — see the catalog help text per key).

Shared flags: --server / --client-id / --secret, same shape as
tools/publish_ir.py / tools/set_ir_pointer.py (fall back to env).

This is the sketch version — minimal parsing, minimal formatting,
explicit about its assumptions. The catalog is hand-maintained;
test_s2s_catalog.py enforces non-drift between catalog and C++.
"""

from __future__ import annotations
import argparse
import os
import sys
from pathlib import Path

_HERE = Path(__file__).resolve().parent
_REPO = _HERE.parent
if str(_REPO) not in sys.path:
    sys.path.insert(0, str(_REPO))

try:
    import yaml  # type: ignore
except ImportError:
    print("error: pyyaml not installed. `pip install pyyaml` or see "
          "requirements.txt.", file=sys.stderr)
    sys.exit(2)

from tools.s2s_client import client_from_env, Stra2usError  # noqa: E402


CATALOG_PATH = _REPO / "critterchron.s2s.yaml"


# ---------- catalog loading + validation ----------

class CatalogError(RuntimeError):
    pass


def load_catalog(path: Path = CATALOG_PATH) -> dict:
    """Load + shallow-validate the catalog. Returns the parsed dict.

    Doesn't schema-check everything — enough to catch typos so the tool
    fails loudly instead of silently mis-setting a value."""
    if not path.is_file():
        raise CatalogError(f"catalog not found: {path}")
    with path.open("r") as fh:
        doc = yaml.safe_load(fh)
    if not isinstance(doc, dict) or "app" not in doc or "vars" not in doc:
        raise CatalogError(f"{path}: missing `app` or `vars`")
    vars_ = doc["vars"]
    if not isinstance(vars_, dict):
        raise CatalogError(f"{path}: `vars` must be a mapping")
    for name, entry in vars_.items():
        if not isinstance(entry, dict):
            raise CatalogError(f"{path}: var `{name}` must be a mapping")
        if "type" not in entry:
            raise CatalogError(f"{path}: var `{name}` missing `type`")
        if entry["type"] not in ("int", "float", "string"):
            raise CatalogError(f"{path}: var `{name}` type must be "
                               f"int|float|string, got {entry['type']!r}")
        scopes = entry.get("scope", [])
        if not scopes:
            raise CatalogError(f"{path}: var `{name}` missing `scope`")
        for s in scopes:
            if s not in ("app", "device"):
                raise CatalogError(f"{path}: var `{name}` scope entry "
                                   f"`{s}` must be app|device")
    return doc


def coerce_value(entry: dict, raw: str, key: str) -> object:
    """Parse a CLI string against the catalog entry's declared type + range."""
    t = entry["type"]
    if t == "int":
        try:
            v = int(raw)
        except ValueError as e:
            raise CatalogError(f"{key}: expected int, got {raw!r}") from e
    elif t == "float":
        try:
            v = float(raw)
        except ValueError as e:
            raise CatalogError(f"{key}: expected float, got {raw!r}") from e
    else:
        v = raw
    rng = entry.get("range")
    if rng and t in ("int", "float"):
        lo, hi = rng
        if v < lo or v > hi:
            raise CatalogError(
                f"{key}: value {v} outside catalog range [{lo}, {hi}]"
            )
    return v


# ---------- verbs ----------

def cmd_catalog(args, catalog: dict) -> int:
    """Tabulate the catalog — no network traffic."""
    print(f"# {catalog['app']} — Stra2us catalog ({CATALOG_PATH.name})\n")
    vars_ = catalog["vars"]
    # Column widths sized to content.
    name_w  = max(len("key"), max(len(k) for k in vars_))
    type_w  = max(len("type"),
                  max(len(e["type"]) for e in vars_.values()))
    scope_w = max(len("scope"),
                  max(len(",".join(e.get("scope", []))) for e in vars_.values()))
    def_w   = max(len("default"),
                  max(len(str(e.get("default", "—"))) for e in vars_.values()))
    header = (f"  {'key':<{name_w}}  {'type':<{type_w}}  "
              f"{'scope':<{scope_w}}  {'default':<{def_w}}  help")
    print(header)
    print("  " + "-" * (len(header) - 2))
    for name, e in vars_.items():
        default = e.get("default", "—")
        scope = ",".join(e.get("scope", []))
        help_line = (e.get("help") or "").strip().splitlines()
        help_first = help_line[0] if help_line else ""
        print(f"  {name:<{name_w}}  {e['type']:<{type_w}}  "
              f"{scope:<{scope_w}}  {str(default):<{def_w}}  {help_first}")
    return 0


def _fetch_scope(client, key: str):
    """Wrap client.get with a uniform "missing" return. A key may be
    absent (404 → None) or explicitly empty (200 "" or 200 None). Both
    are "unset" for display purposes."""
    try:
        v = client.get(key)
    except Stra2usError as e:
        return ("error", str(e))
    if v is None or v == "":
        return ("unset", None)
    return ("set", v)


def cmd_show(args, catalog: dict) -> int:
    app = catalog["app"]
    vars_ = catalog["vars"]
    device = args.device  # None when --app
    target_key = args.key  # may be None → show all

    if target_key and target_key not in vars_:
        print(f"error: {target_key!r} not in catalog. "
              f"Keys: {', '.join(vars_)}", file=sys.stderr)
        return 3

    try:
        client = client_from_env(
            base_url=args.server, client_id=args.client_id,
            secret_hex=args.secret_hex,
        )
    except Stra2usError as e:
        print(f"error: {e}", file=sys.stderr); return 2

    keys = [target_key] if target_key else list(vars_)
    # Simple indented report; good enough for a sketch.
    for k in keys:
        e = vars_[k]
        scopes = e.get("scope", [])
        default = e.get("default", "—")
        print(f"{k}  (type={e['type']}, scope={','.join(scopes)}, "
              f"default={default})")
        dev_state, dev_val = ("n/a", None)
        app_state, app_val = ("n/a", None)
        if device and "device" in scopes:
            dev_state, dev_val = _fetch_scope(
                client, f"{app}/{device}/{k}"
            )
        if "app" in scopes:
            app_state, app_val = _fetch_scope(client, f"{app}/{k}")
        # Resolution: device (if set) → app (if set) → default
        if dev_state == "set":
            effective = dev_val; source = f"device ({device})"
        elif app_state == "set":
            effective = app_val; source = "app"
        else:
            effective = default; source = "default"
        if device:
            print(f"    device  = {_fmt_scope(dev_state, dev_val)}")
        print(f"    app     = {_fmt_scope(app_state, app_val)}")
        print(f"    → effective = {effective}  (from {source})")
    return 0


def _fmt_scope(state: str, value) -> str:
    if state == "n/a":
        return "(not valid at this scope)"
    if state == "unset":
        return "(unset)"
    if state == "error":
        return f"(error: {value})"
    return f"{value!r}"


def cmd_set(args, catalog: dict) -> int:
    app = catalog["app"]
    vars_ = catalog["vars"]
    key = args.key
    if key not in vars_:
        print(f"error: {key!r} not in catalog. Add an entry to "
              f"{CATALOG_PATH.name} first.", file=sys.stderr)
        return 3
    entry = vars_[key]
    scopes = entry.get("scope", [])
    # Scope target: --app sets <app>/<key>; otherwise <app>/<device>/<key>.
    if args.app_scope:
        if "app" not in scopes:
            print(f"error: {key!r} is not app-scoped (catalog scope="
                  f"{scopes})", file=sys.stderr)
            return 3
        full_key = f"{app}/{key}"
    else:
        if not args.device:
            print("error: pass <device>, or use --app for app scope",
                  file=sys.stderr)
            return 3
        if "device" not in scopes:
            print(f"error: {key!r} is not device-scoped (catalog scope="
                  f"{scopes})", file=sys.stderr)
            return 3
        full_key = f"{app}/{args.device}/{key}"

    # --unset writes an empty string (server has no DELETE). Semantic is
    # per-key: for int/float, device kv_fetch rejects unparseable and
    # falls through (effective = app-scope or default). For string keys,
    # "" may be a real value — see each key's `help:` in the catalog.
    if args.unset:
        value: object = ""
    else:
        if args.value is None:
            print("error: value required (or pass --unset)", file=sys.stderr)
            return 3
        try:
            value = coerce_value(entry, args.value, key)
        except CatalogError as e:
            print(f"error: {e}", file=sys.stderr); return 3

    try:
        client = client_from_env(
            base_url=args.server, client_id=args.client_id,
            secret_hex=args.secret_hex,
        )
    except Stra2usError as e:
        print(f"error: {e}", file=sys.stderr); return 2

    try:
        client.put(full_key, value)
    except Stra2usError as e:
        print(f"error: {e}", file=sys.stderr); return 4
    print(f"set: {client.base_url}/kv/{full_key} → {value!r}")
    return 0


# ---------- entrypoint ----------

def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--server")
    ap.add_argument("--client-id", dest="client_id")
    ap.add_argument("--secret", dest="secret_hex")
    sub = ap.add_subparsers(dest="verb", required=True)

    sub.add_parser("catalog", help="Print the catalog table.")

    sp_show = sub.add_parser("show", help="Show key resolution for a device.")
    sp_show.add_argument("--app", dest="app_scope", action="store_true",
                         help="Show only app scope (no device).")
    sp_show.add_argument("device", nargs="?")
    sp_show.add_argument("key", nargs="?")

    sp_set = sub.add_parser("set", help="Set a KV value via the catalog.")
    grp = sp_set.add_mutually_exclusive_group()
    grp.add_argument("--app", dest="app_scope", action="store_true",
                     help="Write at app scope instead of device scope.")
    sp_set.add_argument("device", nargs="?")
    sp_set.add_argument("key")
    sp_set.add_argument("value", nargs="?")
    sp_set.add_argument("--unset", action="store_true",
                        help="Write empty string (no server DELETE).")

    args = ap.parse_args(argv)

    try:
        catalog = load_catalog()
    except CatalogError as e:
        print(f"error: {e}", file=sys.stderr); return 2

    if args.verb == "catalog":
        return cmd_catalog(args, catalog)
    if args.verb == "show":
        # `--app KEY` parses as device=KEY, key=None. Shift.
        if args.app_scope:
            if args.key is None and args.device is not None:
                args.key, args.device = args.device, None
        elif not args.device:
            ap.error("show: <device> required (or use --app)")
        return cmd_show(args, catalog)
    if args.verb == "set":
        # `--app foo bar` parses as device=foo key=bar. Normalize.
        if args.app_scope:
            # Expected args: --app KEY VALUE. Argparse ate KEY as
            # `device`. Shift.
            if args.key is None and args.device is not None:
                args.key, args.device = args.device, None
            elif args.value is None and args.device is not None and args.key is not None:
                args.value, args.key, args.device = args.key, args.device, None
        return cmd_set(args, catalog)
    ap.error(f"unknown verb {args.verb!r}")
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

# Catalog — worklist & open questions

Running ledger for catalog work. Kept concise: items either have a
decision (date + note) or an owner + ask. When a section grows beyond
"active", split it into its own doc.

Pointer back: [catalog_spec.md](catalog_spec.md) is canonical; this
file is where we capture the in-flight thinking that shapes it.

---

## Decisions log

| Date       | Decision | Why |
|------------|----------|-----|
| 2026-04-22 | Accept FR, ship M1 as spec + reference CLI in `tools/`. | FR was thorough; pattern is proven in CritterChron; surface is small. |
| 2026-04-22 | `default: per-device` magic string → `default_per_device: true` flag. | Keeps `default:` strictly typed; lint/CLI don't need to special-case a sentinel string. |
| 2026-04-22 | Add `bool` and `enum` types (plus `format:` hint) in M1, not later. | FR open question #5; retrofitting after UI exists is painful. |
| 2026-04-23 | **Variant C: publish via existing `/kv/_catalog/{app}`, raw YAML text.** Replaces the earlier "bespoke `/catalog/{app}` endpoint + JSON projection" plan. | Zero new server code for M2; reuses HMAC + ACLs; YAML comments round-trip. |
| 2026-04-23 | Format on the wire is YAML text (not JSON). | Follows from Variant C; UI pays ~40 KB for js-yaml, acceptable for admin-only tool. |
| 2026-04-22 | M2 landed: `catalog publish` + `catalog fetch` via `/kv/_catalog/{app}`. | Smoke-tested round-trip byte-equal against local redis+uvicorn; 29 offline + 3 live tests green. |
| 2026-04-22 | `STRA2US_FIRMWARE_DIR` env override in backend (default still `/firmware`). | Hardcoded `/firmware` blocked non-Docker local dev; one-line, zero behavior change for the compose path. |
| 2026-04-23 | Static catalog-drift lint stays in critterchron for now; no upstream to `stra2us_cli`. | CritterChron is the only consumer today; its `test_s2s_catalog.py` (~210 lines) is the proven shape. Wait for a 2nd app before generalizing. |

## Active — M2 worklist

**Goal of M2:** app dev can publish their catalog to stra2us with a
single CLI command; the stashed copy is retrievable via the existing
`/kv/` endpoint so M3's UI has something to read.

M2 is **shipped** as of 2026-04-22. All residual follow-ups closed:

- [x] `_catalog` reservation test — `test_app_name_rejects_underscore_prefix`
      in `tools/tests/test_catalog.py` pins that apps can't start with `_`.
- [x] CritterChron hand-back — migrated catalog + adoption note live at
      `docs/handoff/critterchron.{s2s.yaml,md}`. Validates clean.

## Open questions (for when they come up, not now)

- **Versioning / history.** `POST /kv/` overwrites. If we decide we
  want "last N catalog revisions per app," cheapest path is a
  parallel publish to `/q/_catalog_history/{app}` (each publish is a
  queue message, TTL gives automatic retention). Defer until someone
  asks for it.
- **Discovery — "which apps have catalogs?"** Not answerable with
  existing server surface; needs a narrow prefix-scan endpoint
  (`GET /kv/?prefix=_catalog/`, admin-auth) **or** the UI carries a
  configured list of apps. Revisit when M3 lands; MVP-UI can hardcode.
- **ACLs on `_catalog/*`.** Should only privileged clients be able to
  write there. Uses existing `check_acl` machinery. Concrete ACL
  entries to add are an ops decision, not a design one.
- **Server-side schema validation.** Currently CLI-only. If we ever
  want advisory-reject on upload (invariant 2 + FR open-question-#2's
  "enforce" concept), the server would need pyyaml + pydantic — adds
  deps, worth a discussion. Low priority; CLI validation is already
  tight.
- **Server 200-vs-404 on missing KV.** See `catalog_fr.md` team-note
  #1. Candidate cleanup for M2 since we'll be touching this path
  anyway. Non-blocking — CLI already handles both shapes.
- **CritterChron migration.** When M2 is stable, hand back a migrated
  `critterchron.s2s.yaml` (the 4 keys using `default: per-device`
  need to flip to `default_per_device: true`). Ship with a short
  "here's how to publish it" note.

## Parking lot (not for M2, not forgotten)

- Web UI (M3) — four views per FR §"Ask 2". Separate doc when we get
  there.
- Per-key `enforce: true` opt-in for server-side advisory-reject
  (FR open-question #2). Depends on server-side validation existing.
- `read_cadence` UI rendering — schema field exists in M1; actual UI
  hint consumption lands in M3.
- Catalog diff viewer / history stream — predicated on versioning
  decision above.
- **Catalog → Devices tab stale after edit.** When a key is edited
  via the modal from inside the Devices tab, closing the dialog
  doesn't refresh the Devices view — the effective-value table keeps
  showing the pre-edit values. The write lands (leaving and
  re-entering the Devices tab reflects it correctly). Likely the
  editor's success handler only refreshes the Variables tab or the
  top-level catalog data, not the per-device effective table. Fix
  will probably live in the `saveKv`/editor close path in
  [backend/src/static/app.js](../backend/src/static/app.js) —
  re-call `fetchCatalogDevices(app)` (or equivalent) when the
  active catalog tab is Devices. Filed 2026-04-23, light severity.
- **Catalog drift lint as `stra2us_cli lint` subcommand.** Today
  critterchron ships its own `test_s2s_catalog.py` that greps
  `get_int("k", default)` / `get_float(...)` across HAL C++, resolves
  `#define SYMBOL N` defaults, and cross-checks against the YAML
  catalog (forward: call sites without entries; reverse: entries with
  no reader unless `ops_only`). When a 2nd app appears, upstream into
  `stra2us_cli lint`. Open design points at that time:
    - Stra2us should define a **canonical catalog-read API** (e.g.
      `Stra2usConfig::get_int(key, default)` on the official client).
      Apps that bypass it (raw HTTP, wrapper libraries, etc.) are out
      of scope for lint — that's an acknowledged gap, not a bug.
    - Language adapter shape: at minimum C++ (critterchron) and
      Python (for stra2us_cli + app scripts). JS/TS later if needed.
    - `#define`-resolution is C++-specific; the Python adapter will
      need its own "default literal" discovery (module constants).
  Filed 2026-04-23.

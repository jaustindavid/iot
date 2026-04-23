# Admin UI — worklist

Light polish items for the admin dashboard. Catalog-specific UI items
live in [catalog_todo.md](catalog_todo.md) instead.

## Active

- **Device-scoped variable editor: drop the app-scope field.** When
  the variable editor is opened from the Catalog → Device detail
  view (via `openKeyEditorForDevice` in
  [backend/src/static/app.js](../backend/src/static/app.js)), the
  modal currently exposes both the device-override and app-scope
  inputs. The app-scope control is a foot-gun in that context — the
  operator came in via a specific device and probably doesn't mean
  to change every device. Replace it with an inline note along the
  lines of "editing the device-level override for `<device>`. To
  make an app-wide change, close this and edit from the catalog's
  Variables tab." Keep the full editor (both scopes) reachable from
  the Variables tab where it makes sense. Filed 2026-04-23.
- **First-call lag on `/kv_scan` (~5s).** Low priority. First click on
  Catalogs (fetches `_catalog/` prefix) and first click on a
  catalog's Devices tab (fetches `<app>/` prefix) each take ~5s;
  subsequent calls are snappy. Classic cold-path pattern. Most
  likely: cold Redis connection/auth on first use, or cold OS page
  cache for the keyspace. Diagnostics before fixing: instrument
  `scan_kv` in [backend/src/api/routes_admin.py](../backend/src/api/routes_admin.py)
  with per-phase timing (total vs. `redis.keys` vs. `strlen` loop).
  Incidental cleanups worth doing anyway: replace the per-key
  `redis.strlen` serial loop with a pipeline, and skip `strlen`
  entirely for callers that don't consume `bytes` (the Devices tab
  doesn't — only the Raw tab does). Also: a batched
  `peek/kv_batch` endpoint would kill the peek-storm in
  `openDeviceDetail` (separately slow, not this bug). Filed
  2026-04-23.
- **Topic Monitor: add a client-id / host filter.** Mirror the
  Activity Logs view, which already supports repeatable
  `?client_id=…` filtering. Target the Topic Monitor section in
  [backend/src/static/app.js](../backend/src/static/app.js) — the
  server-side filter would piggyback on whatever client_id field the
  monitored stream entries carry. Filed 2026-04-23.

## See also

- [catalog_todo.md](catalog_todo.md) parking lot — Catalog → Devices
  tab stale after edit, Raw tab column widths for long device IDs.

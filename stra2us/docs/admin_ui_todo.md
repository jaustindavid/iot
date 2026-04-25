# Admin UI — worklist

Light polish items for the admin dashboard. Catalog-specific UI items
live in [catalog_todo.md](catalog_todo.md) instead.

## Active

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

## See also

- [catalog_todo.md](catalog_todo.md) parking lot — Catalog → Devices
  tab stale after edit, Raw tab column widths for long device IDs.

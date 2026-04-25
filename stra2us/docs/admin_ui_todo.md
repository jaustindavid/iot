# Admin UI — worklist

Light polish items for the admin dashboard. Catalog-specific UI items
live in [catalog_todo.md](catalog_todo.md) instead.

## Active

_(none)_

## Closed

- **First-call lag on `/kv_scan` (~5s).** Filed 2026-04-23, fixed
  2026-04-25. Diagnosis via the new `system:perf_log` stream
  ([core/perf_log.py](../backend/src/core/perf_log.py)) showed every
  device endpoint (not just `/kv_scan`) was paying 100-300ms per
  request because [core/redis_client.py](../backend/src/core/redis_client.py)
  was creating a fresh `Redis()` instance per `get_redis_client()`
  call, throwing away the warm pool every time. Cached the client at
  module scope; cold/warm `/kv_scan` now indistinguishable (~3ms),
  30-parallel peek fan-out drops from `max(per-request)` of ~300ms to
  ~23ms total. Also dropped the redundant `xtrim` from the activity-log
  middleware — `MAXLEN ~ 150000` already bounds the stream.

## See also

- [catalog_todo.md](catalog_todo.md) parking lot — Catalog → Devices
  tab stale after edit, Raw tab column widths for long device IDs.

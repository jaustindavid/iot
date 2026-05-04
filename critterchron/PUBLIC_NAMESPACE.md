# Public-namespace migration

Critterchron-side companion to Stra2us's
[`fr_application_view.md`](../stra2us/docs/fr_application_view.md).
Documents what the firmware, publish tools, and catalog have to change
to adopt the new `<app>/public/...` convention for cross-device-visible
data, and the order to do it in without breaking the live fleet.

## Why this is happening

Stra2us is gaining a customer-facing `/app/<app>/<device>/` UI. To make
multi-tenant ACLs structurally safe — i.e. customer A's grants cannot
match customer B's data — Stra2us's flat-prefix matcher requires that
shared data live under a sibling namespace from per-device data.

The convention chosen:

- **`<app>/<device>/...`** — per-device, customer-private data.
  Unchanged.
- **`<app>/public/...`** — anything cross-device-visible (shared
  scripts, shared firmware blobs, app-scope catalog defaults, the
  shared telemetry queue). NEW location.

A customer ACL becomes `<app>/<device>:rw` + `<app>/public:r`, and the
prefix matcher's segment-boundary rule (`<app>/public` matches
`<app>/public/anything` but not `<app>/anything_else`) makes
cross-device leak impossible. Stra2us itself doesn't change — the
isolation falls out of the path naming.

For critterchron specifically: today our shared queue is exactly
`q:critterchron`, our shared scripts live at `kv:critterchron/scripts/...`,
our app-scope catalog defaults live at `kv:critterchron/<varname>`.
All of these need to slide one segment deeper under `/public/`.

## What changes

### Firmware reads (Stra2usClient.cpp, both shims)

Every place that constructs an app-scope KV path needs `/public/`
inserted between `<app>` and the rest. Sites enumerated:

**Particle** (`hal/particle/src/Stra2usClient.cpp`):

| Line | Current | New |
|------|---------|-----|
| 536  | `%s/%s` (poll_key fallback)              | `%s/public/%s` |
| 570  | `%s/brightness_schedule`                 | `%s/public/brightness_schedule` |
| 593  | `%s/wifi_ssid`                           | `%s/public/wifi_ssid` |
| 609  | `%s/wifi_password`                       | `%s/public/wifi_password` |
| 912  | `%s/scripts/%s/sha`                      | `%s/public/scripts/%s/sha` |
| 1032 | `%s/scripts/%s`                          | `%s/public/scripts/%s` |

**ESP32** (`hal/esp32/src/Stra2usClient.cpp`):

| Line | Current | New |
|------|---------|-----|
| 583  | `%s/%s` (poll_key fallback)              | `%s/public/%s` |
| 614  | `%s/brightness_schedule`                 | `%s/public/brightness_schedule` |
| 637  | `%s/wifi_ssid`                           | `%s/public/wifi_ssid` |
| 653  | `%s/wifi_password`                       | `%s/public/wifi_password` |
| 1239 | `%s/scripts/%s/sha`                      | `%s/public/scripts/%s/sha` |
| 1353 | `%s/scripts/%s`                          | `%s/public/scripts/%s` |
| 1498 | `%s/fw_target`                           | `%s/public/fw_target` |
| 1509 | `%s/fw/%s/sha`                           | `%s/public/fw/%s/sha` |
| 1556 | `%s/fw/%s`                               | `%s/public/fw/%s` |

Per-device paths (`%s/%s/%s` with `app_, device_, key`) are
**unchanged** at every site. Per-device queue topics are also
unchanged (none currently used in critterchron, but if/when they are,
the path stays `<app>/<device>`).

### Firmware publish topics (heartbeat + OTA events)

All `g_cfg.publish(STRA2US_APP, ...)` calls today publish to the
literal topic `"critterchron"`. Post-migration they should publish to
`"critterchron/public/heartbeep"` (the customer-facing app view tails
this single topic; OTA events ride along). Sites:

**Particle** (`hal/particle/src/critterchron_particle.cpp`):
lines 655, 917, 929, 941, 992 — heartbeat + ota_detected/matrix/loaded
+ boot_light. Five publish sites.

**ESP32** (`hal/esp32/src/critterchron_esp32.ino`):
line 927 (heartbeat). Plus the OTA-event publishes — same shape.

Cleanest implementation: introduce a new `STRA2US_TELEMETRY_TOPIC`
constant (`"critterchron/public/heartbeep"`) and replace `STRA2US_APP`
in all publish() calls with that. Keeps `STRA2US_APP` semantically the
catalog-app-name (still `"critterchron"`, used for catalog reads,
register_procyon_credential_, etc.), and `STRA2US_TELEMETRY_TOPIC` for
the publish path. Two distinct concerns get distinct names.

Alternative (split per event type): publish to
`/public/ota_detected`, `/public/heartbeep`, etc. The customer-facing
app view would then have to tail multiple topics. Less appealing
without a concrete reason to want event-level filtering on the app
view side.

### Publish tools

| Tool | Site | Current | New |
|------|------|---------|-----|
| `tools/publish_fw.py`  | L92  | `critterchron/fw/<target>`        | `critterchron/public/fw/<target>` |
| `tools/publish_fw.py`  | L93  | `critterchron/fw/<target>/sha`    | `critterchron/public/fw/<target>/sha` |
| `tools/publish_ir.py`  | L163 | `critterchron/scripts/<name>`     | `critterchron/public/scripts/<name>` |
| `tools/publish_ir.py`  | L164 | `critterchron/scripts/<name>/sha` | `critterchron/public/scripts/<name>/sha` |

Update docstrings + the `--help` text accordingly. `set_ir_pointer.py`
writes to `<app>/<device>/ir`, which is per-device, **unchanged**.

### Catalog YAML (`critterchron.s2s.yaml`)

Two new top-level fields per Stra2us's
[`catalog_spec.md`](../stra2us/docs/catalog_spec.md):

```yaml
app: critterchron
telemetry_topic: "{app}/public/heartbeep"
heartbeat_interval_seconds: 30
vars:
  # existing var declarations unchanged
```

`{app}` and `{device}` are placeholders the app view substitutes at
runtime. `heartbeat_interval_seconds` should match the actual cadence
used by the firmware (default 60s; lower if devices use a faster
heartbeep).

While you're in the YAML, also add `label:` to any var that should
appear in the customer-facing app view (presence of the field is the
visibility gate). A few human-friendly words per visible var. Internal
ops-only knobs (`ir`, `fw_target`, etc., already marked `ops_only:
true`) shouldn't get a label — they stay invisible to customers.

### What does NOT change

- **Per-device KV reads**: `kv:<app>/<device>/<varname>` is the same
  before and after. No firmware change for the per-device path, which
  is the bulk of what the device reads.
- **HMAC signing protocol**: identical. Request/response signing,
  timestamp drift window, encrypted-value cipher — all unchanged.
- **Client_id**: a device's id stays its id. ACL grants reference it,
  the URL bookmark uses it, `client.put` signs against it.
- **Catalog YAML location**: still served from `_catalog/critterchron`.
- **Encrypted-value wire format**: ext type 0x21 + HMAC-keystream
  cipher unchanged. The encrypted-flag sidecar moves with the value
  on the Redis side (`SET kv:critterchron/heartbeep:enc` becomes
  `SET kv:critterchron/public/heartbeep:enc` if the value is
  encrypted; nothing the firmware sees).

## Migration sequence

The transition has three independent moving parts: server-side data
location, firmware code paths, and per-device ACLs. Order matters less
than usual because the worst case at any intermediate step is "shared
script fetch returns 404 for a few minutes" or "telemetry isn't
captured for a few minutes" — both recoverable, both transient. Per
the FR's "Suggested cutover sequence":

1. **Stra2us data move** (operator-side, Redis CLI):
   ```bash
   redis-cli rename kv:critterchron/scripts/<each>   kv:critterchron/public/scripts/<each>
   redis-cli rename kv:critterchron/fw/<each>        kv:critterchron/public/fw/<each>
   redis-cli rename kv:critterchron/heartbeep        kv:critterchron/public/heartbeep
   redis-cli rename kv:critterchron/cloud_heartbeep  kv:critterchron/public/cloud_heartbeep
   redis-cli rename kv:critterchron/ir_poll_interval kv:critterchron/public/ir_poll_interval
   redis-cli rename kv:critterchron/timezone_offset  kv:critterchron/public/timezone_offset
   ```
   Also rename the queue topic if you want stream history preserved:
   `RENAME q:critterchron q:critterchron/public/heartbeep`.

2. **Add `critterchron/public:rw` to every device's ACL** *before*
   removing the broader `critterchron:rw`. Devices keep working under
   both grants during the transition (prefix matcher accepts the
   first match; put `critterchron/public` first in the JSON list so
   it's preferred when both apply).

3. **Push firmware update.** New firmware reads/writes the new paths.
   Devices that haven't updated yet keep publishing to the legacy
   `q:critterchron` (which the app view isn't tailing — telemetry
   from old-firmware devices appears as "offline" in the app view
   for the duration of the rollout).

4. **Once all devices on new firmware**, drop the broader
   `critterchron:rw` from each device's ACL — only the narrower
   `critterchron/public:rw` remains. Sweep any leftover data at
   legacy paths.

The compatibility window between (1) and (3) means devices on old
firmware appear offline in the app view but remain functional.
Acceptable since the customer-facing app view is new — no existing
customer expectation to break.

## Failure modes / edge cases

- **Old firmware after data move (pre-step 3 firmware roll).** Device
  reads `<app>/heartbeep` (legacy path), gets 0x81 fixmap "not found,"
  silently skips. Falls back to compiled-in default. Functionally
  fine, just doesn't pick up operator overrides until the firmware
  update.

- **New firmware before data move (impossible if order is followed).**
  Device reads `<app>/public/heartbeep`, gets 0x81 fixmap, falls back
  to compiled-in default. Same shape as above. Avoided by doing
  step 1 before step 3.

- **Operator forgets ACL update (step 2)**. Device gets 403 on
  `<app>/public/...` paths until ACL fixed. Fail-loud — heartbeat
  surfaces `err=ota_fetch:kvs status=403` so it's noticeable.

- **Mid-rollout encrypted record migration.** If `wifi_password` was
  set with `--encrypted`, the Redis sidecar `kv:critterchron/wifi_password:enc`
  also needs to migrate to `kv:critterchron/public/wifi_password:enc`
  alongside the value. `RENAME` both keys atomically (Redis script
  or two consecutive RENAMEs — the small race is bounded blast
  radius, fixed by re-setting if it bites).

## Drift-test recommendations

Worth automating in `test_s2s_catalog.py` (or a sibling test) once
the new convention is live:

- **No KV path under `<app>/public/<known_device_id>/...`** — catches
  the operator footgun of putting per-device data inside the public
  namespace. Lint walks the device list against an `--inspect`
  scan of KV.
- **No publish to bare `<app>` topic** — catches a firmware regression
  that forgets to use `STRA2US_TELEMETRY_TOPIC`. Could be a grep-
  level lint over the source tree; or a runtime check in the
  Stra2usClient that warns if `publish(STRA2US_APP, ...)` is called
  with the bare app name.

## Reference

- Stra2us-side FR: `../stra2us/docs/fr_application_view.md` —
  in particular the "Critterchron firmware-team brief" section.
- Catalog spec (top-level fields like `telemetry_topic`):
  `../stra2us/docs/catalog_spec.md`.
- Encrypted values still in scope post-migration:
  `../stra2us/docs/fr_encrypted_values.md` and `PROCYON.md`.

## Revision history

- 2026-05-04 — Initial draft. Migration not yet executed.

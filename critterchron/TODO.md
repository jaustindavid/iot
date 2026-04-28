# TODO

Items actively tracked. Completed items move to the bottom with a timestamp.

## Near-term

- **Time-of-day knobs for brightness / night mode.** Today brightness
  is purely a function of the ambient light sensor: lux (or CDS raw)
  → curve → bri target → Schmitt-triggered night mode. Two real-world
  failure modes argue for a time-of-day overlay on top:
  1. **Bedside display at night.** A nightstand panel may sit in a
     dim-but-not-pitch-black room — sensor reads bri=4, panel renders
     at perceptible levels, but the human asleep next to it wants a
     *much* dimmer or off panel during sleep hours regardless of
     ambient. Today there's no way to say "between 11pm and 7am, cap
     max_bri at 1, regardless of sensor."
  2. **Workspace where bright-natural-light-via-windows during the
     day biases the curve unrealistically.** A panel facing a window
     during morning sun saturates lux high and then never adjusts
     back down through the working day as the sun moves. Wanting
     "max_bri=24 1pm-6pm regardless of sensor" is a different shape
     of need but the same plumbing.
  Both cases are solved by letting the panel's effective `max_bri`
  (and possibly `min_bri`, `night_enter`, `night_exit`) be
  time-of-day-modulated — the sensor still controls the *position*
  on the curve but the curve's endpoints can shift through the day.

  *Sketch of an interface, not a commitment.* A schedule string in
  Stra2us KV, parsed once on cache update, evaluated against
  `clock_.wall_now()` each time the brightness loop reads
  `max_brightness`:
  ```
  brightness_schedule = "23:00-07:00:1, 07:00-09:00:8, 09:00-23:00:64"
  ```
  Each segment is `HH:MM-HH:MM:max_bri`; gaps fall through to the
  device-header default. Wraps midnight via the segment ordering. A
  parallel `night_force_schedule` could pin the engine into night
  mode for a window even if bri stays above the Schmitt threshold,
  for the bedroom case.

  *Open questions.*
  - Granularity: per-device only, per-device-with-app-fallback, or a
    single fleet-wide schedule? Per-device most flexible but a pain
    to manage; the heartbeep / cloud_heartbeep precedent suggests
    `<app>/<device>/<key>` first then `<app>/<key>`.
  - Edge transitions: hard cut at the segment boundary, or smooth
    interpolation between segments? EMA on top of the light sensor
    smoothing already (50× damping); a hard schedule cut would
    propagate as a 50-tick fade naturally. Probably fine.
  - DST / timezone: schedule is local-wall-clock, evaluated through
    the existing `CritTimeSource::zone_offset_hours()`. WobblyTime
    would wobble the schedule too — undesirable for "11pm bedtime"
    semantics. Probably want to evaluate against the *real* time
    source (un-wobbled), which means exposing the inner clock from
    WobblyTimeSource or routing the schedule check around it.
  - Discoverability: today a confused operator can grep `bri=` in a
    heartbeat and see what the panel thinks. With a schedule active,
    a richer heartbeat field like `bri=(min<cur<max @schedid)` or
    `bri_src=schedule|sensor|override` would name *why* the panel is
    where it is.

  Loosely related to TODO entry "Light-sensor calibration poisons
  itself; only reflash recovers" — that's about the sensor mapping
  collapsing in stable lux conditions; this is about the operator
  wanting human-clock-driven authority over the result. Both could
  share a "what's actually driving bri right now?" diagnostic in the
  heartbeat.

- **`fade` should clamp non-zero channels to 1 until age=0, not
  truncate them mid-fade.** The render pass at
  `hal/CritterEngine.cpp:2058-2064` does a Q8 quantization
  `(t.r * q8) >> 8` over each channel and stores the result back
  to the framebuffer. For low-channel-value night colors like
  `brick: (0, 1, 0)`, channel value 1 truncates to 0 for any
  `q8 < 256` — i.e., every fade tick except the very first.
  `scale_ch` at the sink-side has a 1-floor for exactly this
  reason ("if input was non-zero, keep at least 1"); the fade
  pass should mirror that contract. Proposed shape:
  ```cpp
  auto fade_ch = [q8](uint8_t c) -> uint8_t {
      if (c == 0) return 0;
      uint16_t s = ((uint16_t)c * q8) >> 8;
      return s ? (uint8_t)s : 1;
  };
  put(x, y, fade_ch(t.r), fade_ch(t.g), fade_ch(t.b));
  ```
  Trade: a tile painted with `draw <name> fade N` and channel
  value 1 will sit at intensity 1 for the full N ticks then
  vanish at age=0 — visually the same as `hold`. That's the
  right answer for night palettes where channel-1 means "this
  is the dim version, don't optimize me away." Bright-channel
  fades behave unchanged.
  Mirror to `engine.py` for sim parity. Add a fixture in
  `agents/tests/` that paints a `(0, 1, 0)` tile with
  `fade 100` and asserts the cell remains lit through ticks
  1..99. (The corresponding fixture for the day case ought to
  show the gradient — same opcode, different visual outcome
  by design.)

- **Mirror ESP32's Connection:close handling + granular OTA breadcrumbs to
  Particle.** Defensive consistency, not currently bug-fixing. The
  2026-04-28 ESP32 triage uncovered that arduino-esp32's `WiFiClient::
  connected()` returns true for a window after the server FINs (uvicorn
  ASGI sends `Connection: close` after every response), causing
  `read_response_` to write into a half-closed socket and time out.
  Fix landed in `hal/esp32/src/Stra2usClient.cpp` — parse the response's
  Connection header, call our own `close()` after reading the body when
  the server says `close`, so `ensure_connected_()` opens a fresh socket
  on the next request.

  Particle's TCPClient currently propagates FIN to `connected()` fast
  enough that the bug doesn't repro there (rachel pulls cleanly), but the
  protocol stimulus is identical and a future DeviceOS update or odd
  network condition could expose it. Same fix applied symmetrically would
  cost ~10 lines in `hal/particle/src/Stra2usClient.cpp` `read_response_`
  and gives us identical OTA-fetch semantics across platforms.

  While there, also mirror the granular failure-path breadcrumbs:
  `kvs req too big`, `kvs ensure_connected failed (2x)`, `kvs send_all
  failed (2x)`, `kvs status=N`, `kvs msgpack hdr=0xXX`, `kvs payload=N>
  cap=M` in `kv_fetch_str_`, plus `rr hdr timeout`, `rr bad status line`,
  `rr body EOF`, `rr 2xx unsigned`, `rr ts drift`, `rr HMAC fail` in
  `read_response_`. Each `g_errlog.record(ErrCat::OtaFetch, ...)` paired
  with the matching `Log.warn(...)` (Particle uses Log directly, not
  the LOG_WARN macro from ESP32). Heartbeat surfaces the specific
  reason as `err=ota_fetch:<reason>` so a future Photon-2 OTA mystery
  doesn't have to start from "fetch failed: <key>" again. See
  `hal/esp32/src/Stra2usClient.cpp` for the exact set of sites and the
  message format conventions.

  Memory ref: `debug_ota_diagnosis_2026-04-28.md` has the full
  rationale and failure→cause table.

- **ESP32 should report the compiled-in program name.** On Particle (bf64551)
  `ir_loaded_script()` / `ir_loaded_sha()` were migrated to fall back to
  `critter_ir::SCRIPT_NAME` / `SCRIPT_SHA` when the OTA-loaded fields are
  empty (i.e. pre-first-OTA), so the heartbeat shows `script=<real-name>@<sha>`
  instead of `default@00000000`. ESP32's `Stra2usClient.h` / `.cpp` still
  return the raw fields directly — observed 2026-04-27 with timmy publishing
  `ota_detected from=default@00000000 to=fraggle@...` instead of
  `from=<compiled-in>@<sha>`. Migration is mechanical: copy the accessor
  bodies from `hal/particle/src/Stra2usClient.h:140-150`, swap the four
  raw-field uses inside `hal/esp32/src/Stra2usClient.cpp` (lines ~712, 732,
  763, 815) for the accessor calls. While there, also fix the matching
  pre-first-OTA fast-path comparison so a freshly-flashed ESP32 whose
  compiled-in sha equals the sidecar's sha short-circuits instead of
  re-fetching the blob it already has burned in.

- **One-shot device provisioning script.** Breaking out a fresh device is
  currently a 3-step manual sequence that's easy to botch — the too-big
  OTA buffer on ricardo traces directly to copying a similar device's
  header and not thinking about RAM class. A `tools/provision.py` should
  bundle:
  1. **Name + claim to Particle**: accept `--name ronaldo_raccoon` (or
     whatever), invoke `particle device rename <id> <name>`. No product
     enrollment — we tried Particle Cloud products + group-based OTA
     and gave up (see "Particle products, abandoned" below); provisioning
     is strictly about getting the device bootable and personally
     claimed.
  2. **Seed device header** from a template keyed on hardware class
     (Photon / Photon 2 / Argon / P1), injecting name, geometry
     (`--grid 32x8`), pin (`--matrix-pin D0`), and defaults sized to the
     class — in particular `NO_IR_OTA` / `IR_OTA_BUFFER_BYTES` defaults
     baked into the P1 template so future P1s don't repeat
     ricardo/ronaldo's 8KB-buffer mistake. Hardware class drives the
     default knobs, not the device name.
  3. **Create Stra2us key + ACL**: generate `STRA2US_SECRET_HEX`,
     register it with the stra2us server under `<name>`, write into the
     device header alongside the identity block.
  Header-template approach probably wants a small templates dir
  (`hal/devices/templates/{photon,photon2,argon,p1}.h.j2` or plain .h
  with {{name}} markers) so the RAM-class defaults live in one place
  per class.

- **Particle products, abandoned.** Briefly built out Particle Cloud
  product/group OTA in `tools/particle_release.py` + `fleet.yaml`:
  canary/production groups, date-encoded versions, reconcile diff. Torn
  out on 2026-04-24 after realizing grid size is baked into each binary,
  so "one product per hardware platform" (the shape Particle's model
  enforces — one binary per product) would need one product per distinct
  geometry × platform combo. At five-plus geometries across three
  platforms that's ≥5 products for a handful of devices, with manual
  grid-aware tagging on top. Push-based `make DEVICE=<x> flash` from a
  laptop wins until the fleet is large enough to dwarf the setup cost.
  If we revisit: the pivot point is probably grid-aware binary
  selection, not the product machinery itself. Prior work lives in
  git history under the `particle_release.py` / `fleet.yaml` paths.

- **Guarded `step (dx, dy) if free` opcode.** The current opcode clamps to
  grid edges, but coati's bobbing at the pool is "literally move here, no
  questions asked" — if another agent is on the target cell, or the
  target happens to be off-grid, the animation is silently degraded.
  Fine for pool-bobbing at known-safe coordinates; wants a guarded form
  (`step (dx, dy) if free` or similar) before it's trusted in unknown
  contexts. `coati.crit:37` / `coati.crit:63` tag the locations.
- **Boot-window staleness flash (low priority).** *Re-scoped 2026-04-25
  after auditing what's actually unsolved. The original entry pitched a
  full "synchronous bootstrap" refactor with a new `IrBootstrapResult`
  enum and helper extraction; that turned out to be overkill once the
  rest of the OTA polish landed. Documenting the actual gap and the
  minimal fix here, plus why it's low priority.*

  **Original concern.** Fresh-flashed device boots, runs compiled-in
  (stale) IR for a stretch, then snaps to the intended script. Ugly
  UX, particularly when an operator is watching the device come up.

  **What's already solved (three-pronged "less surprising" goal):**
  1. **Signaling on the display** ✅ — `draw_spinner` (cyan) covers
     the cloud-wait window before any sim runs; `draw_ota_streamer`
     (5s of green vertical streamers) covers the post-boot OTA swap
     window. Both in `critterchron_particle.cpp:754, 778`.
  2. **Error messages remote** ✅ — heartbeat error channel landed
     2026-04-25. OTA fetch/apply failures (oversized, malformed, sha
     mismatch, parse-fail, reinit-fail) all surface as
     `err=<cat>:<msg>` in the next heartbeat. See `hal/ErrLog.{h,cpp}`.
  3. **Guarding the swap** ✅ — `g_ota_loading` state pauses physics
     and renders the streamer for `OTA_LOADING_MS=5000` before
     applying, so a swap is never silent or sudden.

  **The remaining gap.** Spinner stops at `Particle.connected()`, but
  the tel thread has a deliberate 15s `startup_delay_ms` (heap-pressure
  protection — was added to avoid SOS-1 deathblink on P1 when the tel
  thread allocates concurrently with the main-thread cloud handshake).
  So between cloud-connect and the first `ir_poll()`, the device
  renders compiled-in IR for ~15-20s with no visual cue it might still
  be catching up. *That window* is where the staleness flash lives. The
  signaling stops one stage too early.

  **Why this is low priority.** Reboots are infrequent (goal: never
  reboot). A device that's been up for weeks already converged to the
  current IR via the regular `ir_poll()` cadence; the staleness flash
  only hits on cold-boot or post-rescue-hold. So in the steady-state
  fleet, this window almost never appears.

  **Minimal fix (~5 lines) if it ever becomes worth closing.** Drop one
  inline `g_cfg.ir_poll()` call into the main thread between
  `Particle.connected() == true` and `Thread(...telemetry_worker...)`
  spawn. ir_poll is reentrant-safe and already does the right thing
  — stages a pending blob if the cloud has new IR, no-ops if we're
  current. The existing `g_ota_loading` state machine handles the
  visible swap on the next main-loop iteration with the established
  green-streamer cue. No new API, no helper extraction, no enum.
  ```cpp
  if (Particle.connected()) {
      g_cloud_seen = true;
      Particle.syncTime();
      last_sync_minute = -1;

      // First ir_poll inline on main thread — runs before tel thread
      // is armed, so no concurrency with tel's startup_delay_ms heap-
      // protection window. The existing g_ota_loading state takes
      // over on the next main-loop iter if a pending blob lands.
      g_cfg.connect();
      g_cfg.ir_poll();
      g_cfg.close();

      if (g_tel_thread == nullptr) { /* unchanged */ }
  }
  ```
  Spinner's last frame stays on the panel during the 1-3s blocking
  fetch (slight rotation pause; reads as "still working"). Closes the
  staleness window from ~15-20s to ~3s.

  **Why not the original ir_bootstrap_sync API?** The big refactor
  (helper extraction, `IrBootstrapResult` enum, separate orchestration)
  was solving a problem (server-distinguishable "bootstrapped" lifecycle
  event) that we don't actually have demand for. The 5-line tweak
  addresses the visible UX gap, reuses every existing mechanism, and
  is reversible. If the lifecycle-event distinction ever turns into
  real demand, the refactor can layer on top.
- ~~**Publish warns when blob exceeds OTA buffer size.**~~ Landed
  2026-04-22. `tools/publish_ir.py` now emits a multi-line stderr
  warning when the encoded blob exceeds 8192 bytes, naming the
  overshoot, explaining the failure mode ("devices silently stay on
  the previous script, visible only as 'ir_poll: fetch failed' on
  serial"), and suggesting either raising `IR_OTA_BUFFER_BYTES` in the
  device header or dropping the SOURCE trailer (`--source` is now
  opt-in and off by default; was `--no-source` opt-out at the time of
  this landing — see the 2026-04-23 flip below for the rename, and the
  2026-04-27 publish/point cleanup which removed the deprecated alias). The device-side error-channel-on-heartbeat follow-up is
  still the right-sized fix (see entry above) — this was the
  short-loop papercut close.
- **Re-publish doesn't trigger re-OTA.** ~~Investigation open — see
  three-branch diagnostic below.~~ **Root cause identified 2026-04-21**:
  blob size grew past `IR_OTA_BUFFER_BYTES` (8192), device rejected
  fetch at `Stra2usClient.cpp:472`, failure logged on serial only so
  publisher saw no signal. Short-loop fix captured in the "publish
  warns when blob exceeds OTA buffer size" entry above; right-sized
  fix is the device-side error channel on the heartbeat. Keeping the
  historical three-branch diagnostic below for future observers who
  hit the same symptom from a different cause:
  (a) content_sha actually identical — working as designed per the
  comment at `Stra2usClient.cpp:588` ("content_sha shifts on source
  edits AND on encoder behavior changes, but NOT on pure republishes
  of the same source"). A whitespace-only or comment-only edit that
  the compiler canonicalizes away genuinely produces identical content.
  Not a bug; surprising.
  (b) content_sha changed but sidecar didn't update — publisher bug,
  the sidecar write after the blob write either failed silently or
  raced with the blob.
  (c) sidecar updated but device didn't pick it up — either the poll
  interval is longer than the observer expected, the blob exceeded
  the OTA buffer (the 2026-04-21 case), or the fast-path compare at
  `ir_poll()` lines 603-607 is reading stale values.
  Diagnostic: publish, then directly inspect the three KV entries
  (`<app>/scripts/<name>`, `<app>/scripts/<name>/sha`,
  `<app>/<device>/ir`) via admin read and compare against the device's
  `ir_loaded_sha_` from a heartbeat. Three-way comparison names the
  branch. `publish_ir.py --force` smoke also useful — if `--force`
  re-OTAs but plain publish doesn't, it's a content-identity issue
  (branch a), not transport.
- **OTA freeze — "grid frozen, device online, colors still cycle".**
  Intermittent, affects all hardware types (shared HAL code, not
  platform glue). Signature: physics layer wedges (agents stop
  moving), but render pipeline keeps running (cycle colors still
  animate, pixel sink still flushing). Device remains Particle-online
  (cloud thread alive). Happens during OTA transitions. Working
  hypothesis: `engine.reinit()` returns false on the version gate
  (`CritterEngine.cpp:347`) AFTER `critter_ir::load()` has already
  mutated the global tables — agents still hold `type_idx` / `beh_idx`
  / `pc` indexing into freshly-overwritten slots, read garbage pointer
  from `BEHAVIORS[beh_idx].insns[pc]`, interpreter falls through the
  bottom-of-switch defaults, `color_idx` stays valid (cycle animates)
  but no step/seek opcode ever matches (agent never moves). Diagnostic
  to confirm: serial log for `engine.reinit() failed after OTA IR swap`
  (`critterchron_particle.cpp:667`) immediately following an `ir_apply:
  loaded` line. Fix, once confirmed: move the IR_VERSION gate into
  `critter_ir::load()` as a pre-install check so rejected blobs never
  mutate the tables, eliminating the state-tearing window. Secondary
  hardening regardless of confirmation: add `ApplicationWatchdog`
  coverage over the OTA path (load + reinit + first-tick) so a
  future-class hang resets the device with a clear panic code rather
  than wedging silently.

- **Device registers loaded sha back to Stra2us on successful OTA.**
  ~~Today the only fleet-visibility signal for "what sha is this
  device actually running" is the heartbeat (`script=<name>@<sha
  _prefix>`) which fires every heartbeep interval.~~ **Partially
  addressed 2026-04-23** via the lighter-weight heartbeat-nudge
  approach: on successful `ir_apply_if_ready()` → `engine.reinit()`
  (via the existing `g_ota_pub_loaded` flag path in
  `critterchron_particle.cpp:487-510`), the tel thread resets
  `last_attempt_ms` + sets `next_interval_ms = 5000` so the next
  heartbeat fires ~5s after load instead of up to `heartbeep`
  seconds later. The operator sees the confirming
  `script=<name>@<sha>` within seconds instead of waiting for
  cadence. Reuses the signal operators already grep — no new KV
  keys, no new tooling.

  What this doesn't cover (still open if the need arises):
  - **Single-KV-read fleet status.** The heartbeat-nudge still
    requires iterating every device's heartbeat stream to see who's
    on what sha; a dedicated `<app>/<device>/ir/loaded` key would
    let a single GET-per-device (or even a wildcard read) produce a
    fleet dashboard. Not urgent for small fleets.
  - **Parse-failure visibility.** The nudge only fires on successful
    reinit, so "fetched but rejected" is still only surfaced via
    serial. Overlaps with the device-side-error-channel TODO above —
    that's the right place for this signal, not a separate KV key.
  - **Payload richness.** Heartbeat carries `sha_prefix` (8 chars);
    a dedicated key could carry full sha + size + last error. Has
    mattered zero times so far; skip until it does.

  If a fleet dashboard becomes real, the original per-device KV
  proposal (`<app>/<device>/ir/loaded` containing `<sha>:<uptime_ms>`)
  is still the right shape — it just composes with the nudge rather
  than replacing it.
- **Light-sensor calibration poisons itself; only reflash recovers.**
  Observed 2026-04-21: rachel sat at `bri=(1<125<128)` in a room the
  operator describes as "very dark, for hours" — a state the current
  algorithm should recover from in a single tick via the instant-widen
  rule (`if raw > cal_dark: cal_dark = raw`, `LightSensor.h:54`).
  Reflashing cleared it (cal pair reset to the cold-boot `(3500, 2000)`
  defaults, came back dim). The sensor is physically isolated from the
  matrix so self-feedback is ruled out.

  For widen-in-dark to fail, raw must not be climbing above the learned
  `cal_dark`. Two suspected mechanisms, both rooted in the pull-in rule
  (`LightSensor.h:49-53`, 1 unit / 173s nudge of each bound toward
  current raw, floored at `MIN_CAL_RANGE=200` apart):

  (a) **Narrow physical range.** If a given CDS + resistor combination
  only produces raw in, say, 1200-2600 across the device's actual
  lifetime illumination, the cold-boot `cal_dark=3500` never gets
  reached and decays *down* at 1/173s toward whatever raw happens to
  sit at. Over a week the learned range collapses to the observed
  physical range, and from then on every reading maps across the full
  brightness span — a small drift in ambient pops bri from min to max.

  (b) **Asymmetric exposure history.** Long residency at a stable
  mid-level ambient drags both bounds toward the current raw. When the
  room then darkens *slightly*, raw dips below the converged midpoint,
  code reads that as "brighter than learned bright," bri goes to max.
  The widen rule can't help because raw never exceeds cal_dark — it
  just sits near cal_bright from the other direction.

  Both failure modes share the property that the learned range
  collapses toward *whatever the sensor actually produces over time*
  rather than tracking true physical extremes. The `MIN_CAL_RANGE=200`
  floor prevents total collapse but doesn't prevent collapse into a
  band that produces pathological mapping.

  **Observability first.** The heartbeat today reports `bri=(min<cur<max)`
  — the *output* of the mapping, with no visibility into the inputs.
  Add `light=(raw<cb<cd)` (or similar) to the heartbeat payload
  unconditionally when `LIGHT_SENSOR_TYPE` is defined, so this
  diagnosis is a one-line read off the server instead of a serial
  monitor session + reflash + wait-and-see. Cheap change, unblocks
  everything below.

  **Design directions, roughly in order of complexity:**
  - *Asymmetric decay toward cold-boot defaults, not toward raw.*
    Once per decay period, nudge `cal_dark` UP toward 3500 and
    `cal_bright` DOWN toward 2000 by 1 unit, gated on "no widen
    event this period." A genuine extreme still widens the bound
    out immediately; absent extremes, the pair slowly re-questions
    its learned range instead of collapsing around the current
    mid-level. Makes the algorithm forget slowly in the *safe*
    direction (toward conservative defaults) rather than the
    *dangerous* direction (toward whatever raw happens to be).
  - *Hard minimum range.* Raise `MIN_CAL_RANGE` from 200 to something
    like 800-1000. Smaller the floor, higher the pathological-mapping
    risk; larger the floor, less dynamic-range responsiveness. 200 is
    likely tuned for a photocell that happens to produce wide raw
    swings; other wiring / other CDS parts don't.
  - *Quantile-based learning over a long window.* Instead of min/max
    with decay, track e.g. p5 / p95 of raw over the last 24 hours.
    More code, more memory, more robust. Probably overkill.
  - *Non-volatile persist + manual re-learn.* Save cal pair to
    EEPROM every hour; on boot, load. Add a Stra2us KV trigger
    (`light_recal=1` or similar) to force a re-learn. Complementary
    to the decay fix — solves the "reboot loses good calibration"
    case at the cost of also preserving bad calibration across
    reboots (which is what prompted this TODO in the first place).
    Probably not the right fix, but worth mentioning.

  Start with the heartbeat exposure, live with it for a week, then
  pick a learning-rule fix based on what the raw traces actually
  show across the device fleet.

- ~~**Tile-paint fade on `draw`.**~~ Landed 2026-04-26. `draw <color>
  fade <N>` lerps brightness from full to off over N ticks then flips
  state→False; sibling `draw <color> hold <N>` (added same day, not in
  the original spec) keeps full brightness for N ticks then vanishes
  in one frame — both share the `age`/`age_max` countdown and the
  per-tick decay sweep. `Tile.hold_mode` (1 bit, fits HAL bitfield
  slack) selects flavor. No wire-format change — IR_VERSION stays at
  6, the fade/hold clause rides as free-form text on the existing
  `draw` instruction. Plain `draw <color>` (no fade/hold) behaves
  identically to pre-v6. Marker-rejection regex extended so `draw
  <ramp> fade N` / `draw <ramp> hold N` reject with the same
  message. RAM cost regression on OG Photon documented below
  ("Tile-fade RAM clawback") — the §369-374 "manageable on every
  platform" claim was empirically wrong; rico is currently
  dirtnapped pending one of the two clawback knobs. Tests:
  `agents/tests/ok_draw_fade.crit` (compile fixture covers both
  keywords + bare-color form), `agents/tests/ok_fade_basic.crit`
  and `ok_hold_basic.crit` (functional fixtures asserting lit→
  cleared trajectory and color-invariant for hold).

  The original spec follows for reference; everything below the
  rule is the as-designed shape (intent preserved on landing).

  ---

  Motivating use case: a busier
  tortoise whose clock tiles slowly dim and need periodic refresh,
  so stability manifests as brightness. Hour tiles get redrawn
  every hour; minute tiles get redrawn every minute; with (say) a
  15-minute fade, the hours complete a fade cycle while the minutes
  never visibly dim. The existing `seek missing, draw tile` tortoise
  loop handles the refresh mechanics — all that's missing is tiles
  that naturally expire after N ticks without being explicitly
  cleaned.

  This was briefly considered as a marker-system extension
  ("markers touch tile state" / "draw + incr freshness"). Rejected:
  fade is a *tile* property, not a *scalar-count* property. Markers
  are for additive ramp composition with predicate-observable counts
  — wrong abstraction for "this tile dims and then goes away."
  Burning a marker slot and cross-coupling two systems for what is
  structurally a per-tile countdown with a render-time lerp is the
  long way around.

  **Grammar.** Fade is a property of the `draw` opcode, not the
  color declaration. Color and cycle declarations stay untouched.
    - `draw <color>`            — no fade, tile persists until cleaned (today)
    - `draw tile`               — same, contextual color (today)
    - `draw <color> fade <N>`   — paint, tile auto-expires in N ticks
    - `draw tile fade <N>`      — same, contextual color
  `N` is a positive integer tick count — consistent with the rest of
  the DSL. No time-unit suffixes (`15m`, `30s`). If the author wants
  15 minutes at 200ms/tick they compute 4500 and move on. Cycle
  colors compose cleanly: phase resolves as today, the fade lerp
  scales the resolved RGB.

  **Redraw semantics: always reset, no side effects.** Calling
  `draw red fade 100` on a tile currently at `age=30` sets `age=100`.
  No addition, no max-of, no carry. "`draw red fade 100` has no
  side-effects and is not side-effected" — authored behavior is a
  pure function of the opcode. Relative forms (`fade +100` for "add
  100 to whatever's left") are a future FR if a use case emerges;
  not needed now.

  **State-transition model.** `t.state` stays strictly binary. Age
  is a purely visual dimmer + death timer:
    - At `draw ... fade N`: `t.state = true`, `t.age = N`, `t.age_max = N`.
    - Each tick with `state == true && age > 0`: `age--`.
    - At `age == 0`: `t.state = false`.
  Predicates (`current`, `missing`, `extra`) see binary state as
  today — no redefinition, no marker-awareness. A mid-fade intended
  tile reads `current` throughout, then flips to `missing` in a
  single tick when age hits zero. `seek missing` / `wander avoiding
  current` / etc. all work unchanged.

  **No-fade form (`draw red` without `fade N`) stays identical
  to today.** Internally represent "no fade" as a sentinel
  (`age_max = UINT16_MAX` with the tick sweep guarding `age_max !=
  UINT16_MAX` before decrementing). Backward-compatible: pre-v6
  blobs, and v6 blobs that never use the `fade` form, get identical
  behavior to today's binary paint.

  **Render-time lerp.** `final_rgb = base_rgb * (age / age_max)` as
  Q8 fixed-point (avoid the float). Runs in the tile-paint pass
  before marker composite (so markers on a fading tile still add on
  top). On a 16×16 grid that's 256 lerps/frame — noise compared to
  existing render cost.

  **Low-color footgun acknowledged as WAI.** Tiles painted at near-
  floor colors (e.g. `(1, 1, 1)`) don't appear to fade — the sink's
  brightness-scaling floor-clamp rounds sub-unit channel values back
  up to 1, so a half-faded `(1,1,1)` still renders `(1,1,1)`. At
  `age=0` the tile's `state` flips and rendering stops, so visually
  it's "full brightness → [holds] → black, one-tick transition."
  Not a bug — physics of integer color channels on a clamped sink.
  Worth calling out in the feature doc so operators don't file it.

  **RAM cost.** Two new fields per tile: `uint16_t age`, `uint16_t
  age_max`. 4 bytes × GRID_WIDTH × GRID_HEIGHT. On elk_cheetah's
  16×16 that's 1 KiB; on rachel's 32×8 also 1 KiB; on rico's 8×8
  256 bytes. ~~Manageable on every platform, including OG Photon.~~
  **Empirically wrong** (2026-04-26): the +256 B on rico's 8×8 was
  enough to dirtnap it on first boot. See "Tile-fade RAM clawback
  for OG Photon" below for the two remediation knobs
  (`IR_TILE_FADE_ENABLED` compile-out, `IR_TILE_AGE_BITS=8` packing).

  **Wire format.** `draw` opcode gains an optional fade parameter.
  Pre-v6 blobs encode as fade=0 (no fade). IR_VERSION bump to 6.
  Candidate for bundling with the night-markers v6 work below.

  **Convergence metric.** Unchanged — `computeConvergence` still
  counts `intended && state`. Fade doesn't affect it because state
  stays binary. At age=0 the tile drops out of `lit_intended` on
  the next tick, which is the intuitively correct behavior (a tile
  the author meant to be lit, that has expired, is not currently
  contributing to convergence).

  **Dependencies.** None. Natural to bundle with night-markers in a
  v6 bump, but can land independently.

- **Tile-fade RAM clawback for OG Photon.** The fade landing
  (2026-04-26) added 4 bytes/tile (`age` + `age_max`) plus the
  hold variant's 1-bit `hold_mode` (free in HAL bitfield slack).
  Spec §369-374 budgeted "manageable on every platform including
  OG Photon" — empirically wrong: flashing the new firmware to
  rico (OG Photon, 8×8 = 256 B added) dirtnapped it on the first
  boot. Pre-emptively cut the 4K IR_OTA buffer on ronaldo (P1) to
  keep it alive; rico is currently bricked-in-place pending one
  of the two remediations below. **Posture:** disabled-OTA route
  first (no code change), evaluate the knobs after rico is back.

  **Option 1 — `IR_TILE_FADE_ENABLED` compile-out switch.** Defaults
  to 1; a device header sets it to 0 to skip `age` / `age_max` /
  `hold_mode` from `Tile`, the per-tick decay loop in `tick()`, and
  the render-time lerp / hold-skip branch in `render()`. Scripts
  that contain `fade` or `hold` against a fade-disabled target
  silently behave like plain `draw` (tile persists until cleaned).
  Worth a compile-time warning on the device-side parser when an
  instruction with the `fade` / `hold` tail is seen on a build
  with the switch off — silent no-op is the worst kind of script
  divergence between sim and hardware. Saves the full 4 B/tile +
  the 1 bit. Right call for OG Photon (rico's 8×8 doesn't run
  anything fade-y) and any future Gen 1 device.

  **Option 2 — `IR_TILE_AGE_BITS=8` packing knob (TODO §373).**
  Halves `age` / `age_max` to `uint8_t` each, capping max fade
  at 255 ticks (~51s at 5Hz / 12.75s at 50Hz). Saves 2 B/tile
  (512 B on 16×16, 128 B on 8×8). Less invasive — fade still
  works everywhere, just with a tighter ceiling — but the cap
  is a footgun for authors who don't read the device header.
  Reasonable middle ground for P1 (ronaldo) if the IR_OTA cut
  doesn't give enough headroom long-term.

  **Both knobs are orthogonal.** A device can be `IR_TILE_FADE_ENABLED=0`
  (full opt-out) or `=1` with `IR_TILE_AGE_BITS=8` (packed). Photon 2
  and elk_cheetah stay on the defaults (fade enabled, 16-bit ages).

  **Triggering on the rico failure path.** `provision.py` (when it
  lands) should bake `IR_TILE_FADE_ENABLED=0` into the OG-Photon
  template by default — same hardware-class-drives-the-knobs
  pattern that the IR_OTA buffer template solves. Until then,
  manual flag in the rico device header.

- ~~**Cliff-fade variant (`draw <color> hold <N>`).**~~ Landed
  2026-04-26 alongside fade (already summarized inline in the fade
  entry above; recorded here as its own bullet so a future grep for
  `hold` finds something). Same `age` / `age_max` countdown plumbing
  as fade, plus a 1-bit `Tile.hold_mode` (free in the HAL bitfield
  slack) that gates the renderer: when set, the tile renders at full
  intensity for the entire countdown and vanishes in one frame at
  age=0; when clear, the original fade lerp runs. `fade` and `hold`
  are mutually exclusive on a single `draw` (regex-enforced). Bare
  `draw hold N` keeps the existing tile color and just resets the
  countdown — same shape as bare `draw fade N`. Use case: heartbeat
  / "still here" indicators where a smooth dim would misread as
  "this thing is dying." Fixture: `agents/tests/ok_hold_basic.crit`
  asserts the engine never mutates the stored color (the renderer's
  full-bright output depends on it).

- ~~**Negation symmetry across `if` and gradient seek.**~~ Landed
  2026-04-26. Audit found nine grammar sites where `X` was accepted
  but `not X` was rejected; closed cases #1-#6 (the meaningful ones).
  Concretely: gradient seek's landmark filter now accepts `not on
  landmark X` (the original motivating ask: "follow the marker but
  stay out of the heap zone"); board-scope tile predicates take
  `if not current` / `not extra` / `not missing`; positional sites
  take `if not standing on color X`, `if not agent X`, `if not
  landmark X`, `if not on agent X [with state == ...]`. Implemented
  via a generic recursive `not` peel in `_evaluate_if` (engine.py)
  and the matching HAL `evaluateIf` — strip the `not`, recurse on
  the de-negated body, invert. Compiler-side: name-resolution
  pre-pass (`_resolve_names_in_behaviors`) updated to allow a
  `(?:not )?` prefix so `if not <bare-name>` resolves to `if not
  <kind> <bare-name>` before the runtime peels it. Bonus: `if not
  state == X` and `if not random N%` work for free off the same
  recursion (case #7 / #8 from the audit, which the audit marked
  skip-worthy because of `!=` and `100-N` workarounds). Free side
  effect: `if not <marker>` canonicalizes to `if no marker <m>` via
  a new `_MARKER_IF_PATTERNS` rule, keeping marker references
  symmetric and avoiding a misleading "unknown name" error from
  the name-classifier. Cases #7/#8 from the audit are technically
  closed; case #9 (`wander not avoiding`) stays rejected — it's
  semantic gibberish. Fixtures: `agents/tests/ok_negation_symmetry.crit`
  (compile coverage of all six forms), `ok_negation_runtime.crit`
  + `_check_negation_runtime` (asserts the peel actually inverts —
  a regression would leave the painter's tile dark instead of blue).

- ~~**Night-mode marker ramps.**~~ Landed 2026-04-21. `ramp (r, g, b)`
  inside a `night:` block overrides the per-unit coefficient of the
  matching day marker while `night_mode_` is engaged; decay K/T is
  inherited (one counter ticks either mode — decay is a scheduler
  property, not a visual one) and a `decay` clause on a night entry
  is rejected at compile time. Wire format adds a NIGHT_MARKERS
  section (`name r g b`, 4 tokens — no index, no decay) after MARKERS;
  IR_VERSION bumped to 6, SUPPORTED_IR_VERSION raised on all HAL
  platforms and `engine.py`. Pre-v6 blobs leave NIGHT_MARKER_COUNT == 0
  and night mode transparently falls through to the day ramp. HAL
  render loop in `CritterEngine.cpp:1538` resolves the active ramp
  per-marker via a linear scan of NIGHT_MARKERS when night is engaged,
  falling through to the day RGB when no night override is declared
  for that marker (mirroring the day-fallback the color system does).
  `renderer.py` and `engine.py` do the same via
  `CritterEngine.marker_ramp(name)`. Validator warns on sub-unit ramp
  components (`0 < v < 1`) since uint8 truncation on device would
  render them as all-zero — approach (a) from the original TODO;
  Q4.4 storage (approach b) remains a separate potential bump.
  `agents/doozer3.crit` exercises the happy path (full-dim night
  heap); `test_compiler.py` covers decay-on-night rejection,
  unknown-day-marker rejection, and the sub-unit warning.

  **Grammar.** `ramp` inside a `night:` block. Today the parser
  rejects this at `compiler.py:331-333` — the `ramp` shunt is gated
  on `indent == 0`, and the nested-block path only accepts tuple /
  cycle / bare name via `_parse_color_value`. Extension: inside
  `night:`, also recognize `<marker>: ramp (r, g, b) decay K/T` and
  route to a night-marker parse. A name reference (`heap: dim_heap`)
  makes less sense here than for colors — markers aren't named by
  other markers — so probably ramp-literal only, and a bare name
  ref is either rejected or aliased to "copy day ramp but scaled by
  Stra2us-tunable factor." The decay K/T is worth thinking about
  too: likely inherited from the day declaration (decay is a
  *dynamics* property, not a visual one), with night-block `decay`
  specifically optional. Worth authoring a couple of target .crit
  snippets before freezing the grammar.

  **IR wire format.** New `NIGHT_MARKERS` section paralleling the
  existing `MARKERS` section, same shape minus the index (index
  comes from the day declaration it overrides). Bump IR_VERSION to
  6, update `SUPPORTED_IR_VERSION` on all platforms, update
  `ir_text.py` encode/decode, regenerate hw fixtures. Pre-v6 blobs
  leave the night-marker table empty and night mode reuses day
  coefficients — backward-compatible.

  **HAL tables + render.** Add `NIGHT_MARKERS[IR_MAX_MARKERS]` and
  `NIGHT_MARKER_COUNT` mirroring the day table. The render loop
  picks one table at the top based on `night_mode_`:
  ```cpp
  const Marker* M_table = night_mode_ && NIGHT_MARKER_COUNT
                        ? NIGHT_MARKERS : MARKERS;
  const uint16_t M_count = night_mode_ && NIGHT_MARKER_COUNT
                         ? NIGHT_MARKER_COUNT : MARKER_COUNT;
  ```
  A marker without a night override falls through to its day
  coefficients — mirror the day-fallback the color system does for
  colors not listed in `night:`. Decay sweep stays on the day table
  regardless (night mode is a visual state, not a dynamics state).

  **Sub-unit coefficient footgun — address either separately or
  alongside.** Marker RGB is stored as `uint8_t` on device
  (`IrRuntime.h:175`) and decoded via `(uint8_t)atof(...)`
  (`IrRuntime.cpp:514`), which truncates `0.25` to `0`. That makes
  the obvious first-intuition "dim version of a bright marker" —
  e.g. `heap: ramp (0.25, 0, 0)` in the night block — render as
  black, because the short-circuit at `CritterEngine.cpp:1542`
  skips any marker with all-zero RGB. Two options:
    (a) Validator warns on any ramp component `0 < v < 1` with
        "this will truncate to 0 on device." Cheap, unblocks
        night-markers shipping with current precision.
    (b) Switch coefficient storage to Q4.4 or similar fixed-point,
        paid in uint8_t (same wire size), with the render-time
        multiply widened accordingly. Enables `ramp (0.25, ...)`
        to actually render one R-unit per 4 count — the natural
        semantics most authors expect. Separate IR bump (or fold
        into v6 with the night-markers work).
  Recommendation: do both, bundled with the v6 bump. Warning costs
  nothing and protects anyone writing a day-marker with sub-unit
  coefficients too; fixed-point unlocks the full dim-marker use
  case night mode was asked for in the first place.

  **Dependencies.** None strictly blocking. Could land independently
  of every other Phase-2+ item. Natural to pair with any markers
  work already touching the IR version.

- ~~**Fix `light=(...)` heartbeat ordering.**~~ Landed 2026-04-22. Now
  prints `light=(cal_bright<raw<cal_dark)` to match the `min<cur<max`
  convention every other heartbeat triplet uses, so the `<` symbols
  read correctly at a glance. Old order `(raw<cb<cd)` and the
  "low-to-high in the mapping's sense" justification retired.
  Observation: an `<` violation in the output (e.g. `(2000<187<1500)`
  where raw is *below* cal_bright) now visibly indicates "raw outside
  learned range — widen incoming," which the old ordering obscured.

- **(low priority / for science) Reproduce the stuck-sensor bug.** On
  2026-04-21 rachel booted with raw=505 in a dark room and held it for
  minutes; a reflash unstuck her. We shipped a drain+settle in setup()
  (`critterchron_particle.cpp:538-547`) plus a boot_light OOB telemetry
  (one-shot publish of the first 30 raw reads, ~6s into boot) on the
  same day. First post-fix boot showed 30 raws flat at 4030±9, which is
  consistent with "fix worked" AND "there was never anything to fix" —
  we can't distinguish without a repro. When time permits, temporarily
  revert the drain+settle block (keep the boot_light publish so we get
  diagnostic data either way) and see if the stuck state comes back in
  normal use. If it does, the boot_light shape tells us whether it's
  hardware (RC curve) or software (flat at a wrong value, pointing at
  LightSensor auto-cal or config-cache poisoning — see the
  `light=(4029<2000<4045)` heartbeat triplet where the middle value
  didn't match the raw range, prime suspect for a persisted cal
  value). Not worth chasing while devices are behaving; revisit if
  another device shows the symptom or during a quiet period.

- **Hotspot-mode fallback when WiFi is unreachable (ESP32).** On
  Particle, WiFi creds live in DCT and DeviceOS handles the "can't
  associate" case by holding in listening mode for serial/BLE
  reconfiguration. ESP32 has no such service — the M2 plan is to
  compile `WIFI_SSID` / `WIFI_PASSWORD` into `hal/devices/<name>.h`,
  which means a moved/renamed/replaced home network turns the device
  into a brick that requires a reflash to recover. Want a fallback:
  when `WiFi.begin()` fails to associate within a budget (e.g. 60s),
  flip to SoftAP mode with a deterministic SSID (`critterchron-<device
  name>`, maybe a short PSK derived from `STRA2US_SECRET_HEX` so the
  owner can precompute it) and stand up a tiny HTTP endpoint that
  accepts new SSID/PSK. Write them to NVS; on reboot the station-mode
  path tries NVS first, falls back to compiled-in creds, falls back to
  hotspot. Visible state on the grid so an operator across the room
  knows which mode the device is in — maybe re-use the amber rescue
  chase for "hotspot up, awaiting config." Nice-to-have: advertise via
  mDNS (`critterchron-<name>.local`) while in hotspot mode so the user
  doesn't need to find its AP IP. Probably wants a physical trigger
  too (hold a boot button during reset) to force hotspot mode even
  when WiFi is fine, so you can rotate credentials without a flash.
  Scope this as its own milestone (M2.5?) between plain-WiFi bring-up
  and Stra2us integration — landing it before Stra2us means the very
  first field-deployable ESP32 build is credential-recoverable.

- **Pull-mode firmware OTA with signature verification (ESP32).** M5
  ships `ArduinoOTA` push-mode only: `arduino-cli upload --port
  <device>.local --protocol network` flashes over LAN. Fine for dev
  (trusted network, hands-on operator), wrong shape for fleet-wide
  rollouts — no authentication, no centralized "push new fw to every
  device in the swarm" story, and it assumes the developer is on the
  same LAN as every device. Want: a pull-OTA path where the device
  periodically checks a staged firmware URL (Stra2us sidecar?
  mirroring the IR-blob pattern — `<app>/fw/<target>/<sha>` pointer,
  `<app>/fw/<target>.bin` blob), verifies HMAC-SHA256 over the binary
  under the device's Stra2us secret, and flashes via
  `Update.writeStream`. Makefile gets an `ota-stage` target that SCPs
  the built .bin plus the sha sidecar to the staging server. Same
  device-secret that already signs Stra2us traffic; no new key
  material. Skip the trust-on-first-use shape — compiled-in secret is
  the anchor. Nice-to-have: a publish topic (`fw_staged
  target=esp32c3 sha=abcd1234`) so a tooling script can watch for
  devices to pick up the update and report back.

- **Auto-rollback on post-flash crash (ESP32).** M5's rescue-hold
  window buys time to OTA a replacement when a previous boot crashed,
  but doesn't roll back automatically. Arduino-ESP32's bootloader
  supports A/B validation: the app must call
  `esp_ota_mark_app_valid_cancel_rollback()` after a successful boot,
  otherwise the bootloader reverts to the previous partition on next
  reset. Want to wire this up — mark valid after N seconds of uptime
  with WiFi associated and SNTP synced (or some similar "this fw
  actually works" heuristic), and if we crash before that, the
  bootloader reverts for us. Closes the "bad flash bricked the
  device" failure mode without requiring an operator to notice and
  push a replacement through the rescue-hold window. Interaction with
  `esp_reset_reason()` / rescue mode needs thought: a device that
  boots, crashes, reverts, boots successfully on the old fw should
  probably still enter rescue mode (previous boot was a crash, even
  if the currently-running fw is fine) so an operator can investigate
  before re-attempting the bad flash.

## Phase 2+ (per HAL_SPEC)

- ~~**Phase 2 — Environmental polish:**~~ light sensor + brightness
  smoothing landed 2026-04-19; Stra2us KV overrides for the tuning
  knobs landed with Phase 4.
- ~~**Night mode palette.**~~ Landed 2026-04-20. Sparse `night:` block
  inside `--- colors ---` overrides day colors (static, cycle, or
  name-ref); optional `default:` catches unlisted colors. Trigger is
  device-side: Schmitt hysteresis on smoothed sink brightness, entering
  at `NIGHT_ENTER_BRIGHTNESS` (defaults to `MIN_BRIGHTNESS`, i.e. the
  floor-clamp regime itself), exiting a few units higher. Both
  thresholds are live-tunable via Stra2us KV
  (`night_enter_brightness` / `night_exit_brightness`, `<app>/<device>`
  + `<app>` fallback like every other key); exit is clamped to
  `enter + 1` on read so a misconfigured pair can't deadlock the
  trigger. Blobs without a `night:` block are untouched — `--night` /
  entering night-bri is a no-op at render time. Wire format adds
  optional `NIGHT_COLORS` / `NIGHT_DEFAULT` sections; IR_VERSION bumped
  to 4. See `agents/tests/night.crit` for the hello-world, `--night`
  flag on `main.py` and the host harness for forced-mode testing.
- ~~**Phase 3 — WobblyTime**~~ (landed 2026-04-19). `TimeSource` decorator
  around the Particle time source. Tuning via Stra2us KV lands with
  phase 4.
- **Phase 5 — OTA IR persistence.** KV pull + in-RAM apply is live on
  both HALs today (Particle pre-2026-04-22, ESP32 2026-04-23 via the
  `.ino`-ladder M4): devices poll `critterchron/<device>/ir`, fetch
  the blob, verify content_sha, and swap the in-RAM IR tables via
  `critter_ir::load()`. What's still missing: every cold boot snaps
  back to the compiled-in `critter_ir.h` default, so the first
  `ir_poll` after boot eats a fetch+apply cycle to re-converge on
  whatever the device was already running. Phase 5 closes that by
  persisting the OTA'd bytes to flash (NVS on ESP32, DCT/EEPROM on
  Particle) after a successful apply, and having
  `CritterEngine::begin()` try the flash copy first and only fall
  through to `loadDefault()` on empty / corrupt / parse-fail. *NB:
  the compiled-in blob stays pristine as the "never been online"
  fallback — we write to a separate flash region, not over `.rodata`.*
  *Numbering note: "Phase N" is the HAL_SPEC roadmap axis; "M N" in
  this file also refers to the ESP32-bring-up ladder (see the ESP32
  Hotspot / OTA-pull-mode / Auto-rollback entries above). They aren't
  1:1 — Phase 5 ≠ ESP32-ladder M5.*
- ~~**Phase 6 — ESP32 port.**~~ Closed 2026-04-24; see Completed.

# Completed

- **2026-04-28 — OTA pipeline triage on timmy: three bugs under one
  symptom.** After the 2026-04-27 stack fix unblocked rachel, ESP32
  (timmy) showed the same surface-level "OTA pull doesn't progress"
  but for entirely different reasons. Two distinct root causes shook
  out of the diagnostic round-trip, plus a publisher-side bug that
  affected every device class. Worth its own entry because the
  symptoms looked identical to the 2026-04-27 stack overflow at first
  pass and the troubleshooting path required several reflash cycles
  to disambiguate. Memory `debug_ota_diagnosis_2026-04-28.md` has the
  full red-herring list and failure→cause lookup table. The three
  bugs:

  *Bug A — ESP32 stale socket on server-sent `Connection: close`.*
  Stra2us (uvicorn ASGI) responds with `Connection: close` and FINs
  after every request. arduino-esp32's `WiFiClient::connected()`
  returns true for a window after the FIN, so `ensure_connected_()`
  reuses the half-closed socket; `tcp_.write()` succeeds (local TCP
  buffer accepts), the server never sees the request, and
  `read_response_` times out at total=0. Manifested as
  `err=ota_fetch:rr hdr timeout total=0` in the heartbeat error
  channel. Fix: `read_response_` parses the response's Connection
  header; on `close`, calls our own `close()` after returning the
  body. Next request reconnects fresh. Mirrored defensively to
  Particle, where the same protocol stimulus might bite under
  different DeviceOS or network conditions even though it currently
  doesn't repro.

  *Bug B — Publisher serialized blob as msgpack `bin`, devices
  only accepted `str`.* `tools/publish_ir.py:246` did
  `client.put(key, blob_bytes)`. `stra2us_cli.client.put()` calls
  `msgpack.packb(value, use_bin_type=True)`, so Python `bytes` →
  msgpack `bin` (0xc4/c5/c6) and Python `str` → msgpack `str`
  (0xd9/da/db). On-device `kv_fetch_str_` only accepted str types
  and silently rejected bin. The IR blob is semantically text
  (UTF-8 LF-terminated per OTA_IR.md), so the publisher should
  have been passing the str. Smaller values (the sidecar string)
  came back as str by virtue of being Python str, which is why
  sidecar fetch worked while blob fetch failed. Manifested as
  `err=ota_fetch:kvs msgpack hdr=0xc5`. Fixes (defense in depth):
  (a) `publish_ir.py` now passes `blob` (str) to `client.put`,
  (b) both Particle and ESP32 `kv_fetch_str_` widened to accept
  bin types alongside str — wire layout is identical, only the
  type byte differs, so the change is one extra `||` per branch.
  Already-published-as-bin scripts need a `--force` re-publish to
  flip their stored encoding to str; new firmware doesn't strictly
  need that since it now accepts both, but re-publishing keeps
  inspection tools (admin UI, `stra2us get`) showing the value as
  text rather than a hex dump.

  *Bug C (also resolved 2026-04-27, see entry below).* Tel-thread
  stack overflow on Photon 2 — referenced here for completeness;
  it manifested on rachel before timmy was even tested. ESP32
  didn't see this one because its task stack default is larger.

  *Diagnostic plumbing landed alongside the fixes.* Each `return
  false` in `kv_fetch_str_` and each `return -1` in `read_response_`
  on ESP32 now records a specific reason via `g_errlog.record()`
  (heartbeat-visible) and `LOG_WARN` (serial-visible). The granular
  breadcrumbs (`kvs req too big`, `kvs ensure_connected failed`,
  `kvs send_all failed`, `kvs status=N`, `kvs msgpack hdr=0xXX`,
  `kvs payload=N>cap=M`, `rr hdr timeout total=N`, `rr bad status
  line`, `rr body EOF cl=X filled=Y rem=Z`, `rr 2xx unsigned`,
  `rr ts drift now=A resp=B`, `rr HMAC fail cl=X filled=Y`)
  are deliberately verbose because the previous "everything maps
  to outer `fetch failed: <key>`" was useless for diagnosis. Cost
  ~12 lines of code; saved hours on this triage and will save
  more on the next.

  *ESP32-C3 USB-CDC fix as side-quest.* Native USB Serial was
  silent until we flipped the FQBN in `hal/esp32c3/Makefile` from
  `esp32:esp32:esp32c3` to `esp32:esp32:esp32c3:CDCOnBoot=cdc`.
  Default Arduino-ESP32 menu option targets UART0 pins, not native
  USB. Worth knowing for future ESP32-C3 / S3 device bring-up.

- **2026-04-27 — OTA HardFault on first pull (rachel) — tel-thread stack
  overflow.** Symptom: Photon 2 ran compiled-in scripts indefinitely
  but reset cleanly the moment the OTA pointer flipped. Last serial
  line was always `ir_poll: fetching blob critterchron/scripts/<name>`,
  never reaching `blob in (N bytes)`. No SOS — `SYSTEM_THREAD(ENABLED)`
  panics on a worker thread don't get a chance to render the SOS pattern
  before reset, so the symptom *looked* like a watchdog or a Particle
  cloud-side firmware-OTA push.

  *Diagnosis.* Added an unconditional `boot reset_reason=...` log in
  `loop()` (NOT `setup()` — early-boot Log.info gets dropped before USB
  CDC reattaches on a post-crash boot, which is why the first attempt to
  capture the reason saw nothing). Repro on rachel: `reset_reason=130
  data=1` = `RESET_REASON_PANIC` + HardFault. Hard pointer fault on the
  tel thread mid-`kv_fetch_str_`.

  *Root cause.* Call chain `telemetry_worker → ir_poll → kv_fetch_str_
  → read_response_` nests deep enough that the streaming-body branch
  (only reached on OTA — heartbeats have tiny bodies) overruns
  `TELEMETRY_STACK_BYTES=5120`. nRF52840 MPU stack guard fires the
  HardFault. Sum-of-locals estimate looked fine (~3.1KB), but libc +
  HMAC-SHA256 context allocations push it past the limit in practice.
  Buffer overrun and use-after-free were ruled out by code reading
  `read_response_`'s streaming branch — bounds and pointer lifetimes
  check out.

  *Fix.* `TELEMETRY_STACK_BYTES` default in
  `hal/particle/src/critterchron_particle.cpp:60` is now coupled to
  `NO_IR_OTA`: 8192 when OTA is enabled, 5120 when stubbed. OTA-capable
  devices (Photon 2 family, Argon) automatically get the bigger stack;
  RAM-tight devices (rico/OG Photon, ronaldo/P1) already define
  `NO_IR_OTA` so they keep the smaller stack and reclaim the 8KB
  `ir_ota_buf_` along with it. Either knob still device-overridable.
  The unconditional `boot reset_reason=` log in `loop()` is left in
  place as cheap permanent diagnostic for the next mystery reboot.

  *Related.* `device_name.h` template comment block on
  `TELEMETRY_STACK_BYTES` updated to reflect the auto-coupling. This
  is a third distinct OTA-first-pull failure class — the prior two
  (oversize blob 2026-04-22 Repro A, inline `ota_detected` POST wedging
  the tel socket 2026-04-22 Repro B) had different repro signatures
  (no panic, system thread down) and different fixes already in place.
  Keeping this entry separate so future grep on `panic data=1` lands
  here.

- **2026-04-25 — Device-side error channel on the heartbeat.** Landed
  across Particle (photon, photon2, argon) and ESP32-C3 in two phases.
  `hal/ErrLog.{h,cpp}` is a 4-entry ring (~256 B RAM) protected by a
  platform-conditional mutex (`Particle::Mutex` / `std::mutex`).
  Producers call `g_errlog.record(ErrCat::OtaFetch|OtaApply|Boot|...,
  fmt, ...)`; serial echo is preserved (`Log.error` on Particle,
  `Serial.println` on ESP32) so dev workflow is unchanged. Telemetry
  heartbeat drains one entry per cycle as ` err=<cat>:<msg>`,
  `mark_sent()` only on HTTP 200 so a transient publish failure
  requeues. Heartbeat report buffer bumped 320→384 to fit. Wire format
  is grep-friendly k=v, single-token snake_case categories so server
  splits cleanly without schema changes.

  Wired sites: 5 OTA paths in both Stra2usClient.cpp variants
  (oversized fetch / fetch failed / malformed blob / sidecar-blob sha
  mismatch / ir_apply parse failed) plus 4 boot/reinit paths in each
  platform's main shim (rescue-hold-on-crash / engine.begin failed /
  cloud wait timeout / engine.reinit failed after IR swap; ESP32 also
  ArduinoOTA error). Verified on real hardware: ricky's first
  heartbeat after a deliberately oversized publish carried
  `err=ota_fetch:megathyme size=4655>buf=4095`. ESP32 path verified on
  timmy.

  Mid-implementation bugfix worth remembering: first pass keyed
  `mark_sent` on `millis`. Multiple records hitting the same
  millisecond made peek (oldest-first) and mark_sent (linear) disagree
  — leaked entries plus emitted dupes. Fixed by adding a per-instance
  monotonic `seq` field as the identity. Smoke-tested with a 5-write
  overflow; correct oldest-first drain, no dupes, oldest dropped.

- **2026-04-24 — `seek nearest free` Python sim parity.** The C++ engine
  has had `want_free` for a while (parser, candidate filter, current-tile
  claim — all live in `CritterEngine.cpp:1761-1880`), and `swarm.crit:66`
  / `swarm-slower.crit:65` use `seek nearest free current timeout 50` in
  production. The Python sim was silently broken: parser at
  `engine.py:1008` skipped straight from `nearest` to target_kind, so
  `free` got read as the target type and the seek dispatched into
  garbage. No functional test caught it because swarm isn't in the
  fixture set. Closed by mirroring the C++ logic in three places —
  parser (added `want_free` after `nearest`), candidate builder (drop
  occupied cells, skip when target_kind=="agent"), claim site (extend
  to `current` when `want_free` set). Painters excluded from the
  occupancy check to match Python's existing `_occupied_positions`
  convention; this is a pre-existing C++/Python divergence
  (C++ counts painters as obstacles in path costs, Python doesn't) and
  not worth fixing in this pass. All 39+6+17 tests still green.
  Original-design discussion preserved in C++ engine comments at the
  parser site for future reference.

- **2026-04-24 — Color cycles closure.** The "blink/conditional color
  pattern" Near-term entry was effectively done as of 2026-04-19
  (`cycle` colors + `set color =`, hello-world in
  `agents/rainbow.crit`). Residual sub-bullet about "canned animated
  palettes (fire, rainbow-standard) if any script wants them" is
  hypothetical wishlist with no demand behind it — removed from the
  open list. If a future script genuinely wants a shared "fire"
  palette, define it inline in that script's `--- colors ---` block;
  if the same palette gets cribbed into a third script, that's the
  signal to add a stdlib of canned cycles, not now.

- **2026-04-24 — Drunken A* (C++ device engine).** Ported the Python
  sim's post-plan noise to `hal/CritterEngine.cpp::stepTowardTarget`.
  `PFConfig` gained `float drunkenness` + `has_drunkenness`; decoder
  (`IrRuntime.cpp`) parses `drunkenness=` alongside `diagonal_cost=`.
  `pfDrunkenness(type_idx)` mirrors `pfDiagonalCost`'s resolution chain
  (per-agent → `all` → 0.0 default); no top-level branch since the
  compiler restricts top-level pathfinding keys to `diagonal` /
  `diagonal_cost`. The perturb lambda inside `stepTowardTarget` fires
  on both the cached-plan step and the fresh-replan first step —
  invalidates `a.plan_len = 0` on any ortho stagger (we've walked off
  the cached path) and on freeze (cursor can't advance past an
  unexecuted step). Host harness A/B at drunkenness=0.1 over 200 ticks
  shows 193/200 ticks diverge from the sober run with freezes and
  ortho staggers both firing. RNG (`std::mt19937 rng_`) is shared with
  the existing `random N%` behavior evaluator; short-circuit at
  `drunk<=0.0` keeps the sober hot path RNG-free just like the Python
  side.

- **2026-04-24 — Drunken A* (Python sim).** Added `drunkenness` to the
  per-script pathfinding IR: float in `[0.0, 1.0]`, compile-time range
  check, encode/decode through `ir_text`. Injected post-plan noise at
  the seek-step site (`engine.py::_drunken_perturb` helper): at value
  `d`, probability `d/2` freezes the agent for a tick, `d/2` staggers
  perpendicular to the planned move, `1-d` takes the planned step
  unchanged. A* itself stays pure — goal convergence preserved, only
  the walk is noisy. Sober path (`drunkenness=0.0`, the default) is a
  single comparison and consumes no RNG state, so existing scripts are
  bit-identical to pre-change runs. Empirical distribution at
  `drunkenness=0.2` across 10k rolls: 79.8% planned, 10.0% freeze,
  10.2% ortho — matches theory. `agents/coati.crit` now sets 0.2 as a
  live demo. Past-me's third operator ("skip a tile", jump 2 along
  plan) is intentionally omitted — reads as a glitch more than a
  stagger; re-add if visual eval disagrees. Past-me's "catalog
  alongside `wobble_*` as a Stra2us knob" suggestion was dropped in
  favor of per-script IR: agent-type granularity ("tanuki drunker than
  cheetah") was the originally-stated fun knob, and it composes
  cleanly with the existing `pathfinding.per_agent[type]` resolution
  chain. C++ device engine parity landed same day — see entry above.

- **2026-04-24 — Night-mode disable sentinel landed.** `night_enter_brightness<=0`
  now force-exits night mode with an explicit early-out in
  `critterchron_particle.cpp:850-854`, rather than relying on the
  implicit "`bri` never reaches 0 because `min_brightness` floors at 1"
  behaviour. Also handles the "KV flipped live while already in night
  mode" case — exits on the next light tick instead of waiting for
  `bri >= nx`. `min_brightness` floor in HAL (line 814) is also in
  place as belt-and-suspenders, so the next entry below is stale too.

- **2026-04-24 — P1 RAM recovery + heartbeat script-identity FR.**
  Ronaldo was deathblinking (SOS+1, hard fault) seconds after the tel
  thread spun up, with `free=1400` in the last pre-crash log — WICED +
  cloud stack + the default 8KB `ir_ota_buf_` left no headroom for the
  tel thread's allocations. Two orthogonal fixes:

  (a) Buffer-sizing knobs in the device header. `NO_IR_OTA` opts out
  of IR-OTA entirely (reclaims the full 8KB, device runs whatever was
  flashed in at `make flash` time); `IR_OTA_BUFFER_BYTES=4096` keeps
  OTA alive for body-only blobs (the common case, since publish_ir
  flipped source to opt-in on 2026-04-23) and saves 4KB. Gated at the
  Stra2usClient member level with `#ifndef NO_IR_OTA` so the buffer
  vanishes from `.bss` under the opt-out; handoff fields stay in the
  class so the public API is uniform and every call site short-
  circuits naturally via `ir_pending_ready()` returning false.

  (b) Heartbeat now shows the real compiled-in script identity as a
  fallback when no OTA has yet applied. `critter_ir::SCRIPT_NAME` and
  `SCRIPT_SHA` are captured from the blob's `name` / `src_sha256`
  metadata at `load()` time; `Stra2usClient::ir_loaded_script()` /
  `ir_loaded_sha()` fall back to those when the private `ir_loaded_*`
  buffers are empty. Heartbeat renders `script=swarm@d466a095` instead
  of `script=default` on flash-only and pre-first-OTA devices. Same
  fallback threaded through `ir_poll`'s sidecar fast-path so a
  freshly-flashed OTA-capable device whose compiled-in sha matches
  the sidecar skips the redundant blob fetch on cold boot (used to
  re-fetch+re-apply every reboot).

  Soak evidence, both at `rst=70` / fresh boot, both running the
  compiled-in swarm script at different sha (visible demo of the
  OTA-lockout the knobs are supposed to provide):

  - ricardo (NO_IR_OTA): `up=5718 mem=8088 script=swarm@d466a095
    phys=(2400<4884) rend=(954<1199) agents=16 seeks_fail=0`
  - rico (IR_OTA_BUFFER_BYTES=4096): `up=6331 mem=3992
    script=swarm@6b25b67e phys=(2392<4045) rend=(911<1005) agents=16
    seeks_fail=14`

  Both stable past 90min uptime; `free` delta (8088 vs 3992) is the
  ~4KB cost of keeping OTA alive at 4K buffer. Different `@sha8`
  tails on the same `swarm` name are the operator-visible proof that
  the FR distinguishes "running the older doozer" from "running the
  latest" without server-side bookkeeping. Ronaldo (32x8, Drew's
  room) took `NO_IR_OTA` as the safe choice for a device that was
  actively dying; rico (16x16) stayed on 4K-buffer to keep the OTA
  train running for the common case. Files touched:
  `hal/ir/IrRuntime.{h,cpp}`, `hal/particle/src/Stra2usClient.{h,cpp}`,
  `hal/devices/{rico,ronaldo}_raccoon.h`,
  `hal/devices/device_name.h` (template knob docs).

- **2026-04-24 — ESP32 `make flash` default flipped to OTA.** On
  `hal/esp32c3/Makefile`, `flash` is now the OTA path (no `PORT=`
  required) and `flash-usb` is the cable path (requires `PORT=`).
  `flash-ota` stays as a one-line alias for `flash` so muscle memory
  and existing scripts keep working. Rationale is the common-case-is-
  default argument: deployed devices are the norm, cables are the
  exception. Verified via `make -n` dry-runs: `flash` → `espota.py`
  invocation, `flash-ota` → same, `flash-usb` → `arduino-cli upload
  --port ...`. `make list` output and usage-comment block at the top
  of the Makefile updated to match. Photon side untouched — Particle
  OTA goes through the cloud via `particle flash`, not a Makefile
  concern. The `flash-usb` recipe emits a stderr hint pointing at
  the OTA path when invoked, so a drive-by operator who types the
  old muscle-memory command sees the new convention.

- **2026-04-24 — Phase 6 ESP32 port closed.** timmy_tanuki
  (ESP32-C3-DevKitM-1, 32x8 WS2812B on GPIO10, BH1750 on I2C) runs
  the shared engine + IR tree clean via `hal/esp32/src/FastLEDSink.h`,
  a full-parity sink alongside `NeoPixelSink.h` (same rotation table,
  same serpentine math, same nonzero-preserving per-channel brightness
  scale). `hal/CritterEngine.*` and `hal/ir/*` stayed platform-gate-free
  — the "engine + device header format unchanged" clause of the
  milestone spec held through the port. Single reference device is the
  scope bar; future S3 / plain-ESP32 variants are post-Phase-6. M1-M4
  of the internal ESP32-bring-up ladder (pixels, WiFi/SNTP, Stra2us,
  OTA IR pull) are all landed; the ladder's M5 (ArduinoOTA push +
  rescue-hold) is also landed per the .ino, with residual follow-ups
  (pull-mode firmware OTA, auto-rollback on post-flash crash,
  hotspot-mode WiFi recovery) tracked as independent near-term items
  above — they outlive "the port" as a milestone.

- **2026-04-23 — `publish_ir.py` flips SOURCE to opt-in.** `--source`
  replaces `--no-source` as the way to include the SOURCE trailer; the
  new default drops it. Devices never read SOURCE at runtime — it's a
  debug artifact for `curl`-based inspection — and including it was
  the proximate cause of the 2026-04-21 silent-OTA-failure class (blob
  exceeds `IR_OTA_BUFFER_BYTES=8192`, device rejects fetch at
  `Stra2usClient.cpp:472`, publisher sees no signal). swarm.crit
  shrinks from 2245→850 bytes, foreman.crit from 8895→4442 — no
  longer trips the oversize warning on the default publish path.
  `--no-source` kept as a hidden deprecated alias (argparse.SUPPRESS)
  with a one-line stderr deprecation note; **retired 2026-04-27** in
  the publish/point cleanup that also rebased both tools on
  `stra2us_cli.client_from_env`. Oversize warning now branches on whether SOURCE
  was requested: if yes, recommend dropping it; if no (default path),
  recommend raising `IR_OTA_BUFFER_BYTES` since no publisher-side fix
  remains. Publish summary line names the path (`850 bytes (no
  source)` vs `(with source)`) so the behavior change doesn't
  surprise downstream `curl` inspection workflows. `OTA_IR.md`
  updated; the broader "device-side error channel on the heartbeat"
  item stays open as the right-sized fix for residual overflow cases
  on tight-buffer targets like rico.

- **2026-04-22 — OTA crash "first pull after boot freezes device hard"
  root-caused and fixed.** Symptom: system thread down, grid frozen,
  cloud presence gone, onboard LED stuck solid cyan (not breathing),
  no SOS, physical reboot required. Intermittent, all platforms.

  *Repro A (rachel, doozer3 oversize, 2026-04-22):* 8408-byte blob,
  216 bytes over `IR_OTA_BUFFER_BYTES=8192`. Defanged by the same-day
  publish-side warning + device-side sidecar size gate; the oversize
  path was *a* trigger, not the underlying fault.

  *Repro B (ricky, swarm-slower, 2026-04-22):* small script, well
  under buffer, first `ir_poll` after boot froze the device hard.
  Added phase-by-phase `Log.info` breadcrumbs through `ir_poll()`
  plus an `ota_detected` publish before the blob fetch (so the
  event would land on the server even if the device died mid-load).
  Next repro: device froze again with zero serial output and only
  the sidecar + blob GETs visible server-side — no breadcrumbs, no
  `ota_detected` publish reaching the stream.

  *Root cause:* the `ota_detected` publish, placed between the
  sidecar GET and the blob GET on the same keep-alive TCP socket
  managed by `Stra2usClient`, wedged the tel thread. Not a bug in
  the sidecar or blob fetch themselves — a POST interleaved into a
  GET→GET sequence on a shared socket is the fragility. Removing
  just that publish (keeping the breadcrumbs) cleared the freeze:
  swarm-slower pulled cleanly on first-pull-after-boot, and again
  after a reboot with the pointer already set (the exact repro
  condition).

  *Fix:* `ota_detected` restored via the same flag-and-snapshot
  pattern used for `ota_matrix` / `ota_loaded` — `ir_poll()` stages
  identity into member buffers and flips a flag, the tel worker
  main loop reads the flag on its next iteration (outside the
  GET→GET window) and does connect→publish→close in isolation.

  Mid-diagnosis we also captured one legitimate DeviceOS firmware
  OTA on serial (`[comm.ota] Received UpdateStart request ...`) and
  briefly chased it as the cause — Particle cloud does push firmware
  OTAs on first-connect when the on-device version diverges from the
  cloud's registered binary — but that path was unrelated to the
  IR-OTA freeze.

  Related items still open: `ApplicationWatchdog` coverage over
  the OTA path (future-class hang should reset cleanly instead of
  wedging); "device-side error channel on the heartbeat" (would
  surface the next weird thing without needing a serial cable);
  "synchronous bootstrap OTA before sim start" (would move
  first-boot OTA to the main thread, sidestepping this whole
  tel-thread socket-reuse class of bugs).

- **2026-04-21 — Palette re-resolves on day/night transition.** Tiles
  snapshot their paint-time RGB (engine-side choice: no live animation
  on painted cells), so before this a day-painted `draw orange` would
  keep its day RGB forever even after night mode engaged — and the
  same in reverse at dawn. Symptom on rachel: the leading hour digit
  painted in "orange" at daybreak kept its day color through the
  entire evening while fresh paint came up in the night green,
  splitting the clock face across two palettes. Fix: stash the color
  *name* on the tile (`Tile.color_ref` — `uint8_t` index on HAL with
  `0xFF` sentinel, `Optional[str]` on the Python sim) at `draw <name>`
  time; `set_night_mode()` detects the edge, sweeps the grid, and
  re-resolves every lit tile with a stashed ref against the target
  palette. Idempotent (no-op if the flag didn't change), skips unlit
  tiles and nameless-paint tiles, and the HAL's `color_ref` costs
  1B/tile (256B on a 16×16; negligible). **Caveat:** cycle colors
  re-resolve at frame 0 on the transition edge (per-tile cycle phase
  isn't snapshotted — would cost another `uint16_t` per tile). Result
  is a one-frame lockstep pulse right after the flip; revisit if it
  becomes visible in the wild.
- **2026-04-20 — Compiler rejects unknown day-color refs in night
  palette.** `_validate_night_palette` cross-checks every night
  override name, every bare name-ref value, and every cycle sub-entry
  against the day palette; unknown targets fail at compile time with
  a message listing the known day colors. Closes the sim-vs-HAL gap
  that let swarm.crit's stray `landed:` slip past `main.py`, pass
  encoding, and fail `IrRuntime::load` on rachel (whose `begin()` then
  returned false → black panel). Negative fixtures:
  `agents/tests/night_unknown_{override,ref}.crit`.
- **2026-04-19 — Compiler catches consecutive-seek fallthroughs.**
  `_is_yield` treats bare `seek` as non-yielding (only `seek ... timeout N`
  yields), so three chained `seek`→`if on ...`→`done` paths that all miss
  their target correctly land on the terminal `done` with `yielded=False`
  and are rejected. Error message now prints the full instruction trace of
  the failing path, so multi-`done` scripts point you straight at the
  broken branch. `agents/tests/no_yield.crit` is the fixture.
- **2026-04-19 — .crit language guide + annotated examples.**
  `agents/LANGUAGE.md` covers the grammar, opcodes, conditions, runtime
  model, and compile-time validation in one grep-able page; `rainbow.crit`
  doubles as the hello-world. `coati/ants/thyme/tortoise.crit` now carry
  inline comments explaining the per-script design choices, so the guide
  can stay tight and point at worked examples.
- **2026-04-19 — Cycle colors (`cycle`, `set color =`).** Colors block
  accepts `name: cycle c1 N, c2 N, ...`; agents can `set color = name` to
  switch/start animating. C++ engine stores a `color_idx` per agent
  (saves 2B over r/g/b), resolves via a shared `CYCLE_ENTRIES[]` table
  using a render-frame counter (50Hz visual). Dump uses tick_count so
  sim↔HW traces diff cleanly. +552B flash on photon2. Tiles snapshot
  cycle at paint time — no live animation on tiles (would look bad with
  the panel's floor-clamp).
- **2026-04-19 — Light sensor calibration decay.** `LightSensor::update`
  now pulls cal_dark / cal_bright in toward the observed working range
  one unit per 864 samples (~173s at 5Hz), so a 1000-unit excursion
  unlearns over ~48 hours. Widen-on-extreme still immediate — anomalies
  register for motion but don't pin forever. `MIN_CAL_RANGE=200` floor
  prevents collapse in a perfectly still environment. No flash persistence;
  cold boot re-learns within minutes from conservative defaults.
- **2026-04-19 — Particle cloud failsafe heartbeat.** `telemetry_worker`
  publishes to topic `stra2us` at `cloud_heartbeep` cadence (default 300s,
  floor 60s, tunable via Stra2us KV). Payload: `up=N s2s=STATUS url=HOST:PORT
  fw=VERSION`. Fires independent of Stra2us status — broken Stra2us can't
  silence the observability path. Answers "is the server down or is the
  device misconfigured" from the Particle event stream alone.
- **2026-04-19 — Render lerp landed as "co-SYRP" on hardware.** Sinusoidal
  dip weight curve (`sqrt(cos(π·t))`) via hardcoded Q8 LUT in
  `CritterEngine::render`; paint lambda blends agent over the underlying
  framebuffer so painter agents (tortoise) transition agent→tile color
  smoothly instead of flashing to black. True-zero midpoint models the
  5mm-pipe / 10mm-pitch panel geometry — agent physically goes dark
  mid-transit. Decisive improvement over linear+floor-clamp at low light
  (the dominant ambient regime); `sqrt(cos)` dwells high longer than pure
  cos so fast agents stay readable. Flash +12 bytes. First-cycle heartbeep
  bug also fixed by pre-registering the key before the telemetry thread's
  initial poll. See `hal/LERP_NOTES.md` for the full observation writeup.
- **2026-04-19 — Phase 4 live-tunable params verified end-to-end.**
  Walked the Stra2us KV loop on ricky_raccoon with a 15s debug
  heartbeep: all 6 keys auto-register client-side (heartbeep,
  wobble_{min,max}_seconds, wobble_{fast,slow}_rate, light_exponent),
  each poll cycle attempts `<app>/<device>/<key>` then falls back to
  `<app>/<key>` and short-circuits on hit. Int round-trip (heartbeep
  at both scopes) and float round-trip (wobble_fast_rate = 4.0, visibly
  sprints the minute display forward) both confirmed. Phase 4 closed.
- **2026-04-19 — `hal/photon/` OG Photon wrapper.** Near-clone of
  argon/photon2 Makefiles; `PLATFORM := photon`. rico_raccoon
  (16x16, D0) builds clean at 43444B flash / 11432B RAM against the
  shared `hal/particle/src/` tree. Ranier_raccoon (32x8 Photon 2)
  header also landed.
- **2026-04-19 — Hoisted Particle-generic shims to `hal/particle/src/`.**
  LightSensor, NeoPixelSink, ParticleTimeSource, WobblyTimeSource,
  Stra2usClient, sha256, hmac_sha256, and the DeviceOS entry point all
  live once under `hal/particle/src/`; `critterchron_photon2.cpp` and
  `critterchron_argon.cpp` collapsed into a single
  `critterchron_particle.cpp`. Per-platform Makefiles (`hal/photon2/`,
  `hal/argon/`) wildcard-copy the tree into local `src/` at build time
  — the only real delta between them is `PLATFORM`. Both targets still
  build clean (Photon 2 43250B/12382B, Argon 38556B/10506B). Ready for
  an OG Photon wrapper as soon as a device header lands.
- **2026-04-19 — Phase 4 Stra2us telemetry.** `Stra2usClient` implements
  the abstract `Config` interface with an internal KV cache; hot-path
  `get_int` / `get_float` are lock-free reads, refresh happens on the
  telemetry worker thread. Heartbeat payload now
  `bri=(min<cur<max) phys=(avg<max<budget)us rend=(avg<max<budget)us`
  for at-a-glance headroom. Retry policy: 2xx/4xx hold normal cadence
  (don't hammer the server over a config problem), 5xx/network back off
  10s→heartbeat exponentially, WiFi-reconnect edge fires immediately.
  WobblyTime + LightSensor tunables pulled live via the Config hook.
- **2026-04-19 — WobblyTime time-source decorator.**
  `hal/photon2/src/WobblyTimeSource.h` wraps `ParticleTimeSource`;
  engine reads wobbled time through the `TimeSource` interface. Defaults
  min=120s, max=300s, fast=1.3×, slow=0.7× — KISS, live tuning deferred
  to Stra2us. Shim's minute-rollover detector now reads the wobbled
  clock (was `Time.minute()`) so display updates match virtual time.
- **2026-04-19 — Light sensor + brightness smoothing.**
  `LightSensor` class in `hal/photon2/src/`, CDS voltage-divider layout
  (opt-in via `LIGHT_SENSOR_TYPE` in device header). Sampled at 5Hz,
  clamped to `[MIN_BRIGHTNESS, MAX_BRIGHTNESS]`, Q8 fixed-point EMA
  (α ≈ 1/50) smooths to the sink. Migrated MIN/MAX brightness from
  float 0..1 to uint8_t 0..255; sink now preserves nonzero channels at
  the floor so dim pixels don't round to black.
- **2026-04-19 — `step (dx, dy)` relative-move opcode.** Added to
  `compiler.py`, `engine.py`, and `hal/CritterEngine.cpp`. Literal /
  unguarded per spec; clamps to grid bounds.
- **2026-04-19 — Landmarks render as background on hardware.**
  `CritterEngine::render` now paints landmark colors first, then lit
  state, then agents. Fixed coati's trash/pool being invisible on rachel.
- **2026-04-19 — Rescue hold on crash-type reset.** Photon shim holds
  engine off for 60s after PANIC/WATCHDOG/UPDATE_ERROR/UPDATE_TIMEOUT so
  a replacement firmware can be OTA-flashed without physical reset.
  Amber chase animation signals the state.
- **2026-04-19 — A* scratch buffer reuse.** Per-call `std::vector`
  heap churn eliminated; A* reuses member-owned `pf_cost_` / `pf_from_`
  / `pf_heap_`. Photon 2 heap no longer fragments under ants @ 50ms.
- **2026-04-19 — 50Hz render + per-second diagnostic rollup** (physics/
  render timing, live agent count, `failed_seeks`, `System.freeMemory()`).
- **2026-04-19 — MAX_AGENTS bumped from 32 to 128.** Ants.crit wants 80;
  the 32 cap silently capped spawning.
- **2026-04-19 — `--widescreen` (32×8) support** lands across sim and
  HAL. `ir_encoder.py` takes `--width` / `--height` and emits them as
  guarded `#define`s into `critter_ir.h`; C++ engine's clock anchor
  layout aspect-switches (wide = single-row HH MM, square = stacked).
- **2026-04-19 — `assemble-engine` Makefile refresh.** Stale `creds.h`
  from a previous `DEVICE=` is detected via `cmp -s` + `.PHONY: FORCE`.

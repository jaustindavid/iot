# TODO

Items actively tracked. Completed items move to the bottom with a timestamp.

## Near-term

- **`step (dx, dy)` bounds-aware variant.** The current opcode clamps to
  grid edges, but coati's bobbing at the pool is "literally move here, no
  questions asked" — if another agent is on the target cell, or the
  target happens to be off-grid, the animation is silently degraded.
  Fine for pool-bobbing at known-safe coordinates; wants a guarded form
  (`step (dx, dy) if free` or similar) before it's trusted in unknown
  contexts. `coati.crit:37` / `coati.crit:63` tag the locations.
- **Device-side error channel on the heartbeat.** Today an OTA failure
  (malformed blob, sha mismatch, parse error) only surfaces as a
  `Log.error` on serial — useless for remote troubleshooting on deployed
  devices. Capture the most recent error string(s) in a small ring buffer
  on the device and pipe them into the heartbeat payload, one or two per
  heartbeat max so we don't flood the channel. Rate-limit: not more than
  1/heartbeat, drop oldest on overflow. Same hook probably useful for
  sensor read failures, watchdog resets, etc. — any non-fatal `Log.error`
  the device wants an operator to know about without shipping a serial
  cable.
- **blink** or other color pattern on conditions.  ~~Maybe add a type of
  color which blinks or changes, and let agents set themselves or a tile
  to the color?~~ Landed 2026-04-19 as `cycle` colors + `set color =` —
  `agents/rainbow.crit` is the hello-world. Still open: canned animated
  color palettes (fire, rainbow-standard) if any script wants them without
  redefining.

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
- **Phase 5 — OTA IR.** Ship IR bytes via Stra2us KV; swap the static
  `critter_ir.h` blob for a flash-loaded buffer.   *NB: compile in a
  default IR blob, in case stra2us is not available on first boot.*
- **Phase 6 — ESP32 port.** FastLED-backed `LedSink`. Engine + device
  header format unchanged.

# Completed

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

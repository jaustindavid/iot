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
- **Synchronous bootstrap OTA before sim start.** Today the boot flow is
  wifi → cloud → compiled-in default starts rendering → telemetry
  thread's `ir_poll()` eventually (up to `IR_POLL_INTERVAL_DEFAULT`
  seconds later) pulls the target blob → main thread applies on next
  tick boundary. Visible on elk_cheetah as "boot into swarm for a few
  seconds, then snap to the intended script" — ugly UX especially for
  freshly-flashed devices where the compiled-in default is stale.
  Desired: after cloud-connect succeeds, BEFORE dropping the
  spinner/breathing-cyan and starting the tick loop, drive one
  synchronous fetch+apply cycle with strict feedback. Proposed API on
  `Stra2usClient`:
  ```
  enum class IrBootstrapResult { Loaded, NoPointer, AlreadyLoaded,
                                 NetworkFailed, BlobCorrupt, Timeout };
  IrBootstrapResult ir_bootstrap_sync(uint32_t budget_ms);
  ```
  Runs on the main thread BEFORE the telemetry thread is armed — no
  locking needed. Does NOT call `engine.reinit()`; caller orchestrates
  so lifecycle stays visible at the call site (mirrors
  `critterchron_particle.cpp:665-674`). Implementation is mostly
  extraction — factor pointer+sidecar+blob+content_sha fetch logic out
  of `ir_poll()` into a shared helper, `ir_poll()` stages into the
  pending slot, `ir_bootstrap_sync()` calls `critter_ir::load()`
  inline. `g_engine.begin()` runs first unconditionally (compiled-in
  default) so a failed bootstrap never leaves the engine uninitialized
  — worst case is two loads + two reinits at boot (<20ms).

  **Open design wrinkle — visual feedback during OTA.** A sudden
  unannounced swap ("surprise! new clock") is jarring, particularly on
  fresh flashes where the user is watching the device come up. Want
  some on-grid indication that the OTA fetch is in progress — ideas
  to consider: keep the breathing-cyan spinner through the bootstrap
  fetch (Particle `RGB.control(true)`), paint a progress indicator on
  the grid (e.g. a small chevron or pulsing pixel), or flash the grid
  briefly on the transition. Same wrinkle applies to post-boot OTAs
  (normal operation): today the swap is invisible from the grid,
  which is fine when nothing's wrong but confusing when the user has
  just hit publish and is waiting for confirmation. Worth deciding
  the indication language once and using it in both contexts. The
  `RGB.control` LED is probably the right primary signal (always
  visible, doesn't steal grid real estate, matches Particle's own
  system-state conventions); a brief grid fade-to-black-and-back at
  the swap point would be a nice secondary cue.
- ~~**Publish warns when blob exceeds OTA buffer size.**~~ Landed
  2026-04-22. `tools/publish_ir.py` now emits a multi-line stderr
  warning when the encoded blob exceeds 8192 bytes, naming the
  overshoot, explaining the failure mode ("devices silently stay on
  the previous script, visible only as 'ir_poll: fetch failed' on
  serial"), and suggesting either raising `IR_OTA_BUFFER_BYTES` in the
  device header or re-running with `--no-source` to drop the SOURCE
  trailer. The device-side error-channel-on-heartbeat follow-up is
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

- **Tile-paint fade on `draw`.** Motivating use case: a busier
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
  256 bytes. Manageable on every platform, including OG Photon. If
  tightness ever bites, pack age into uint8_t (max fade ≈ 51 seconds
  at 5Hz) behind a `IR_TILE_AGE_BITS` knob.

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

- **blink** or other color pattern on conditions.  ~~Maybe add a type of
  color which blinks or changes, and let agents set themselves or a tile
  to the color?~~ Landed 2026-04-19 as `cycle` colors + `set color =` —
  `agents/rainbow.crit` is the hello-world. Still open: canned animated
  color palettes (fire, rainbow-standard) if any script wants them without
  redefining.

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

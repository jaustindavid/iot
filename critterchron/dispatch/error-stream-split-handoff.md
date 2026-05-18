# Handoff — split error stream from heartbeep

## Status

Done. Heartbeats no longer carry an `err=` trailer; ErrLog entries
publish individually to `critterchron/public/error` on the same
keep-alive socket as the heartbeat, immediately after the heartbeat
publish in the same tel cycle. As a follow-on within the same
session, the Particle-only `boot_light` one-shot was also moved off
the heartbeep stream onto the new error topic (see "boot_light
relocation" below) — it's an event-worth-knowing, not part of the
steady-state heartbeat.

## Files touched

- `hal/particle/src/critterchron_particle.cpp`
  - Added `STRA2US_ERROR_TOPIC` macro alongside the other topics
    (~L63).
  - `build_heartbeat_report()`: dropped the `tail_reserve` parameter
    entirely (no caller needs it anymore). Cloud-failsafe call site
    already passed nothing — unchanged at the call line.
  - `telemetry_cycle()`: removed `ERR_TAIL_RESERVE`, the inline
    `peek_oldest_unsent` + `snprintf(" err=...")` block, and the
    `if (have_err && pub_status == 200) mark_sent` line. Added a
    post-publish `while (peek_oldest_unsent)` drain that reuses
    `report[384]` (dead after the heartbeat log line) and breaks on
    non-200 so a transient failure leaves the remainder queued.
  - `boot_light` block (`LIGHT_SENSOR_TYPE`-gated): retopic'd to
    `STRA2US_ERROR_TOPIC`; payload reshaped to match the error-entry
    k=v form (`device=<name> cat=boot_light up=<s> wall=<unix>
    raws=<csv>`). Dropped the now-redundant `n=` field; kept all 30
    samples so the cap-discharge settling curve stays readable.
- `hal/esp32/src/critterchron_esp32.ino`
  - Added `STRA2US_ERROR_TOPIC` macro alongside the other topics
    (~L126), mirroring the Particle entry-point.
  - `telemetry_cycle()`: `safe_cap` collapsed to `sizeof(report)`
    (was `sizeof(report) - 80`); removed the inline err= append block
    and the `mark_sent` line. Added a mirror of the Particle drain
    loop after the publish + Serial.printf log line — same break-on-
    non-200 contract, ESP32 idioms for `up=` / `wall=` (`millis()/1000UL`
    and `time(nullptr)`).
  - No boot_light here (Particle-only feature; cap-discharge
    diagnostic that the ESP32 path doesn't share).
- `hal/ErrLog.h`
  - Updated the header doc-comment that described the (old) wire
    format. Now documents the per-entry `device=… cat=… seq=… up=…
    wall=… msg=…` shape published to `STRA2US_ERROR_TOPIC`, with a
    one-line note that the old inline `err=` trailer was split out
    2026-05-18 because the heartbeep stream was overloaded.

ErrLog API, ring depth (4), msg size (56), mutex — all unchanged.
Cloud-failsafe `Particle.publish("stra2us", …)` left untouched (it
already passed no `tail_reserve` and never drained errors).

## Wire format on the new topic

ErrLog entries (one message per ring entry per cycle):

```
device=<name> cat=<tag> seq=<n> up=<s> wall=<unix> msg=<text>
```

`<tag>` comes from `critterchron::err_cat_tag(ErrCat)`. Typical
≤180B, bounded by ErrLog's 56-byte msg + small fixed fields, well
inside the reused `report[384]`.

boot_light one-shot per boot (Particle only, `LIGHT_SENSOR_TYPE`
devices):

```
device=<name> cat=boot_light up=<s> wall=<unix> raws=<v1>,<v2>,...,<v30>
```

`cat=boot_light` is **not** in the `ErrCat` enum — the wire is just
k=v text and nothing device-side dispatches on `cat=`. A consumer
should treat `cat=` as a free-form discriminator string, not a
constrained enum. boot_light fits in the existing 512B static msg
buffer that the block already used.

## Verification actually run

1. Host harness build + run: `make -C hal/host DEVICE=ricky_raccoon
   thyme` — compiled clean, ran 200 ticks, produced the expected
   health report. (Note: the brief's `make -C hal host` doesn't
   match a target in the top-level Makefile; the working invocation
   is `make -C hal/host DEVICE=<dev> <script>`. Host harness doesn't
   compile the entry-point .cpp/.ino anyway — see below.)
2. Particle target builds (these *do* compile the entry point —
   numbers below are post-boot_light-relocation):
   - `make -C hal DEVICE=ricky_raccoon ants` — Photon 2 → compile
     succeeded, Flash 88554 / RAM 55698.
   - `make -C hal DEVICE=rico_raccoon thyme` — OG Photon (rico,
     `NO_IR_OTA`, tightest budget) → compile succeeded, Flash 92008
     / RAM 35536. No regression; the err= trailer removal is a small
     wash on stack (`ERR_TAIL_RESERVE` was a `constexpr`, no runtime
     cost; `pending_err` was the only saved bytes), and the
     boot_light reshape adds ~32B to flash from the longer format
     string. Both devices define `LIGHT_SENSOR_TYPE` (CDS) so both
     compile the boot_light block.
3. ESP32 target builds:
   - `make -C hal DEVICE=c3a_tanuki swarm` — ESP32-C3 → compile
     succeeded.
   - `make -C hal DEVICE=tammy_tanuki swarm` — ESP32-C6 → compile
     succeeded with the `FASTLED_RMT_WITH_DMA=0` override still in
     place from the C6 crashloop fix.
4. Grep guard `grep -rn ' err=' hal/`: only matches are doc-comment
   prose (the new ErrLog.h history note, the matching prose in both
   entry points, and a pre-existing "log the boot cause via the err=
   channel" comment near setup() in both files — still conceptually
   accurate). No format-string formatters remain. Build artifacts
   under `hal/esp32c6/build/` were stale until the C6 rebuild
   refreshed them; verified clean post-rebuild.

**On-device verification: not run.** No hardware in the loop for
this session. The verification steps in the brief (forced ErrLog
entry via a missing-IR push, black-hole test, burst test of 6) are
the right follow-up. boot_light's on-device check is a fresh boot
of any Particle device with `LIGHT_SENSOR_TYPE` — expect a single
message on `critterchron/public/error` ~6s post-boot carrying
`cat=boot_light` with 30 raws.

## Surprises vs. the brief's assumptions

- The brief instructs `make -C hal host`. There is no `host` target
  at the top of `hal/Makefile`; the host harness is invoked as
  `make -C hal/host DEVICE=<dev> <script>` (or `make -C hal
  PLATFORM=host DEVICE=<dev> <script>`). Worth fixing the brief
  and/or adding a top-level convenience target — followed up below.
- The brief's "default-0 parameter" fallback option turned out to be
  unnecessary — the cloud-failsafe caller already invoked
  `build_heartbeat_report(msg, sizeof(msg))` with no tail-reserve,
  so dropping the parameter entirely was clean. Took that route.
- `hal/esp32c6/` has no source of its own — it copies from
  `hal/esp32/src/` at build time. So the symmetric edit pair really
  is just the two entry-point files; `tammy_tanuki` (C6) inherits.
  Already documented in the C6 Makefile but worth knowing for
  future symmetry-rule reviews.
- The brief's "What NOT to change" line explicitly kept boot_light
  on `STRA2US_TELEMETRY_TOPIC`. The user redirected mid-session to
  move it to the error topic too; doing it anyway, with a `cat=`
  discriminator so it's distinguishable from ErrLog entries.

## Follow-ups

- **Server-side**: nothing reads `critterchron/public/error` yet.
  Need a consumer + dashboard panel; consumer should treat `cat=`
  as a free-form string (values today: each `ErrCat` tag, plus
  `boot_light`).
- **`hal/Makefile` top-level `host` target**: brief assumed it
  exists. Cheap to add as an alias for `make PLATFORM=host
  DEVICE=<dev> <script>`, or at least document the working
  invocation.
- **`ErrCat::Net` reconnect-kick chatter trimming**: explicitly
  noted out of scope; revisit once we see real volume on the
  dedicated stream.
- **`stick=`-based wedge detection**: now that the heartbeep stream
  is no longer competing for buffer space with error messages, the
  analyzer side can lean harder on the engine-wedge fields without
  worrying about err= shoving them out.

## Notes for the next agent

- The ErrLog drain loop deliberately runs the *entire* pending ring
  per cycle (was one-per-cycle in the old inline trailer).
  Reasoning: ring depth is 4, tel cadence floors at 10s, so peak
  publish rate is 0.4/s — bounded without a timer. If a future
  change widens the ring or unfloors cadence, revisit the rate-
  limit reasoning in the Particle drain comment.
- `report[384]` is reused for the drain after the heartbeat publish
  + log line. **Do not** slot anything between the publish's
  `Log.info` / `Serial.printf` and the drain that depends on
  `report` still holding the heartbeat body — the drain overwrites
  it on first iteration. Same constraint in the other direction:
  do not slot a heartbeat-body reader after the drain.
- boot_light uses its own static 512B `msg[]`, not the heartbeat's
  `report[]`. It runs in a separate block of the tel-thread loop
  (after the snapshot/trace block, before ir_poll) with its own
  connect/close, so there's no shared-buffer hazard with the
  drain.
- The Particle path uses `System.uptime()` for `up=`; note the OG
  Photon's ~77%-of-real-time SysTick drift (see memory:
  `debug_photon_gen2_systick_drift.md`). The new error-topic
  messages (both ErrLog entries and boot_light) carry both `up=`
  and `wall=` so the analyzer can cross-check, same as heartbeats
  already do.
- Pre-existing diagnostic fingerprints from memory still apply:
  `publish=-1` in any log line still means "didn't hit the wire"
  (Stra2usClient request buffer); a 401 on a >480B body would still
  point at the (since-fixed) sign_() truncation. Neither should
  trigger here — ErrLog messages are ≤180B and boot_light at 30
  samples × ~4 chars sits around ~200B.

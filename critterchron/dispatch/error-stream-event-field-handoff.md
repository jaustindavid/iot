# Handoff — split `cat=` (ErrLog) vs `event=` (one-shot) on the error topic

## Status

Done. Particle-only change. `critterchron/public/error` now carries
two disjoint discriminator fields:

- `cat=<errcat_tag>` — ErrLog drains, enum-bound via
  `err_cat_tag(ErrCat)`.
- `event=<name>` — non-ErrLog one-shots. Today: only `event=boot_light`.

A consumer branches on which field is present. ESP32 entry point
untouched (no equivalent boot_light block; nothing else on the topic
beyond the ErrLog drain).

## Files touched

- `hal/particle/src/critterchron_particle.cpp`
  - boot_light publish (inside the `LIGHT_SENSOR_TYPE`-gated block in
    `telemetry_cycle()`'s tel-thread loop): `snprintf` format string
    changed from `device=%s cat=boot_light up=%lu wall=%lu raws=` to
    `device=%s event=boot_light up=%lu wall=%lu raws=`. Nothing else
    in the payload moved.
  - Comment block above the publish updated: now explains that
    `event=` (not `cat=`) is used because `cat=` on this topic is
    reserved for ErrLog drains, and points at
    `dispatch/error-stream-event-field.md` by filename for the
    rationale.
  - `Log.info("boot_light publish=%d %s", bs, msg)` left as-is — it
    logs the full message which now includes `event=boot_light`.
- `hal/ErrLog.h`
  - Header doc-comment extended with a "Topic-sharing convention"
    bullet: non-ErrLog publishes on `STRA2US_ERROR_TOPIC` use
    `event=<name>`; `cat=` is reserved for
    `err_cat_tag(ErrCat)`-derived values; today's only non-ErrLog
    publisher is boot_light (Particle-only, `LIGHT_SENSOR_TYPE`-gated);
    future one-shots follow the same convention. Same voice as the
    surrounding comment.

Nothing else touched. No new helpers, no symbol renames, no enum
edits, no topic change, no ESP32 edits, no buffer/cadence/gating
changes.

## Wire format after this change

ErrLog drain (unchanged):

```
device=<name> cat=<errcat_tag> seq=<n> up=<s> wall=<unix> msg=<text>
```

boot_light one-shot (new):

```
device=<name> event=boot_light up=<s> wall=<unix> raws=<v1>,<v2>,...,<v30>
```

## Verification actually run

1. Host build: `make -C hal host` — passed clean, ran 200 ticks,
   produced the expected health report. (Brief noted this target
   might be missing; it now exists as
   `@$(MAKE) PLATFORM=host DEVICE=$(DEVICE) $(HOST_SCRIPT)` at
   `hal/Makefile:60`, presumably added as a follow-up from the prior
   dispatch. Worked first try with no `DEVICE=` override needed.)
2. Particle target builds, both compile the boot_light block (both
   devices define `LIGHT_SENSOR_TYPE`):
   - `make -C hal DEVICE=ricky_raccoon ants` (Photon 2) — clean.
     Flash 88554 / RAM 55698.
   - `make -C hal DEVICE=rico_raccoon thyme` (OG Photon, tightest
     budget) — clean. Flash 92008 / RAM 35536.
   - Identical footprint to the prior session's post-relocation
     numbers, which is what you'd expect from a `cat`/`event`
     character-for-character swap.
3. Sanity greps:
   - `grep -rn 'cat=boot_light' hal/` → **0 matches** (was one).
   - `grep -rn 'event=boot_light' hal/particle/` → **2 matches**: the
     format string at `critterchron_particle.cpp:1308` and the
     comment block above it at `critterchron_particle.cpp:1290`. The
     brief asked for "exactly one match" — that is true of format
     strings (the only publish-site formatter), but the rewritten
     comment now references the field name by design. Reading the
     brief's intent as "no stray publish sites," this is in spec;
     flagged here in case the reviewer reads the literal.
   - `grep -rn 'event=' hal/esp32/` → **0 matches**. ESP32 untouched.
4. **On-device verification: not run.** No hardware in the loop for
   this session. The right check is a fresh boot of any Particle
   device with `LIGHT_SENSOR_TYPE` (ricky_raccoon or rico_raccoon);
   expect one message on `critterchron/public/error` ~6s post-boot
   carrying `event=boot_light` (was `cat=boot_light`).

## Surprises vs. the brief's assumptions

- The `make -C hal host` target *does* exist now (see verification
  step 1), contra the prior dispatch's handoff note. Someone wired
  it up in between. No fallback needed.
- Brief's "exactly one match" for `event=boot_light` in
  `hal/particle/` is two when you count the comment that names the
  field — see verification note above. Not a real problem.

Everything else matched the brief exactly. No extra call sites, no
hidden coupling, no buffer reuse hazards — boot_light's static
512B `msg[]` is still independent of the ErrLog drain's reuse of
`report[384]`, and the `event=` swap doesn't change buffer math
(same field count, one character shorter than `cat=`).

## Follow-ups

- **Server-side consumers** of `critterchron/public/error` (the
  follow-up flagged in the prior handoff is still open) should now
  branch on `cat=` vs `event=` rather than treating `cat=` as
  free-form. If a consumer has already been written, it likely
  needs a small patch: ErrLog entries have `cat=<errcat_tag>` and
  `boot_light` entries have `event=boot_light` (no `cat=`). The
  enum-bound side can rely on `cat ∈ {other, ota_fetch, ota_apply,
  boot, sensor, net}` now.
- **Helper factoring** was deliberately deferred — boot_light is the
  only non-ErrLog one-shot today, YAGNI applies. The next non-ErrLog
  one-shot is the right moment to add an `emit_error_event(name, fmt,
  …)` helper; flag in that brief if the third caller is on the
  horizon. Candidates mentioned in the brief: procyon activation,
  OTA-started markers.

## Notes for the next agent

- The `event=` field is **only** valid on `STRA2US_ERROR_TOPIC`. Do
  not propagate the convention to other topics without thinking it
  through; `STRA2US_TELEMETRY_TOPIC` (heartbeep) and the snapshot /
  trace topics have their own contracts.
- `cat=` is now load-bearing as an enum-bound field on this topic.
  If you ever want to add a `cat=` value for a non-ErrLog reason,
  *don't*; use `event=` instead, or extend `ErrCat` if it's really
  an error category.
- boot_light still uses its own static 512B `msg[]`, separate from
  the heartbeat's `report[384]` that the ErrLog drain reuses. No
  shared-buffer hazard introduced or removed by this change.
- Photon Gen 2 (ricky) SysTick drift (~77% of real time) is still in
  effect on `up=`; boot_light carries `wall=` too so the analyzer
  can cross-check. Same as before this change.
- ESP32 path has no boot_light and no current non-ErrLog publisher
  on the error topic, so the `event=` convention is documented in
  `ErrLog.h` but not yet exercised on that target. If someone adds
  an ESP32-side one-shot diagnostic, follow the convention from day
  one — don't introduce a parallel `cat=<free-form>` literal.

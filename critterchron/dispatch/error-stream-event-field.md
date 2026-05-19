# Dispatch — split `cat=` (ErrLog) vs `event=` (one-shot) on the error topic

## Before you start

Read [../AGENTS.md](../AGENTS.md) for project shape, build recipes,
Stra2us conventions, the two-target symmetry rule, comment style, and
handoff conventions. Then read these task-specific files:

- [./error-stream-split.md](./error-stream-split.md) — the dispatch
  that created the `critterchron/public/error` topic and moved
  boot_light onto it. Required prior context.
- [./error-stream-split-handoff.md](./error-stream-split-handoff.md) —
  what actually shipped, including the boot_light relocation and the
  wire-format ambiguity this task closes.
- [hal/ErrLog.h](../hal/ErrLog.h) — header doc-comment documents the
  current ErrLog wire format on the error topic.

## Context

The previous dispatch consolidated all per-device error / one-shot
diagnostic publishes onto `critterchron/public/error`. As a side effect,
the `cat=` field on the wire now holds two unrelated value spaces:

- **ErrLog drains:** `cat=<tag>` where `<tag>` comes from
  `critterchron::err_cat_tag(ErrCat)` — strictly enum-bound
  (`other`, `ota_fetch`, `ota_apply`, `boot`, `sensor`, `net`).
- **boot_light one-shot:** `cat=boot_light` — a free-form literal with
  no enum backing.

This is fine for one event but won't scale; future one-shots (procyon
activation, OTA-started markers, etc.) would each accrete another
free-form `cat=` literal until the field is a grab-bag with no schema.
This task introduces the convention before that happens.

**Decision:** non-ErrLog publishes on `critterchron/public/error` use
`event=<name>` instead of `cat=<name>`. `cat=` stays reserved for
ErrLog-derived tags only. Both fields coexist on the topic; a consumer
branches on which one is present.

This is a Particle-only change today (boot_light is the only non-ErrLog
publisher and is `LIGHT_SENSOR_TYPE`-gated on the Particle entry point).
The ESP32 entry point has no equivalent block and is **not edited**.

## What to change

### 1. boot_light payload — `cat=boot_light` → `event=boot_light`

In `hal/particle/src/critterchron_particle.cpp`, locate the boot_light
publish by grepping for `cat=boot_light` (one match expected, in the
`LIGHT_SENSOR_TYPE`-gated block inside the tel-thread loop). Change the
`snprintf` format string so the field reads `event=boot_light` rather
than `cat=boot_light`. The rest of the payload (`device=`, `up=`,
`wall=`, `raws=`) stays identical.

Update the comment block immediately above the publish to reflect the
new field name and the reason: this topic's `cat=` is reserved for
ErrLog drains; one-shot diagnostic events use `event=` instead. Point
at this brief by filename so a future reader can find the rationale.

The `Log.info("boot_light publish=%d %s", bs, msg)` line below the
publish stays as-is — it logs the full message, which now includes
`event=boot_light`, still informative.

### 2. Document the convention in ErrLog.h

The existing doc-comment block at the top of [hal/ErrLog.h](../hal/ErrLog.h)
documents the ErrLog wire format on `STRA2US_ERROR_TOPIC`. Add a short
note inside that block (3-5 lines) covering:

- Non-ErrLog publishes on the same topic use `event=<name>` rather
  than `cat=<name>`.
- `cat=` is reserved for `err_cat_tag(ErrCat)`-derived values; a
  consumer can rely on that constraint.
- Today's only non-ErrLog publisher is boot_light
  (Particle-only, `LIGHT_SENSOR_TYPE`-gated); future one-shots follow
  the same convention.

Keep it in the same voice as the surrounding comment. This is where a
fresh reader will look for the contract — the comment is load-bearing.

## What NOT to change

- **ErrLog drain wire format.** `cat=<tag>` stays as-is for all entries
  drained from the ring. The drain loop in `telemetry_cycle()` is
  untouched.
- **The `ErrCat` enum.** No additions, no renames.
- **The topic.** Still `critterchron/public/error`.
- **The ESP32 entry point.** No boot_light there, nothing to edit.
- **The boot_light buffer / cadence / one-shot gating.** Format-string
  change only.
- **Anything else.** If you notice unrelated things worth fixing, list
  them in the closing handoff under "Follow-ups" — don't bundle.

## Wire format after this change

ErrLog drain (unchanged):

```
device=<name> cat=<errcat_tag> seq=<n> up=<s> wall=<unix> msg=<text>
```

boot_light one-shot (new):

```
device=<name> event=boot_light up=<s> wall=<unix> raws=<v1>,<v2>,...,<v30>
```

A consumer parsing the topic discriminates by which of `cat=` or
`event=` is present.

## Why we're not factoring a helper

Tempting to add `emit_error_event(name, fmt, …)` as a helper for
non-ErrLog publishes. We're not — boot_light is the only such call
today and YAGNI applies. The next non-ErrLog one-shot is the right
moment to factor; flag it as a follow-up in your handoff if you think
the second caller is near.

## Verification

Per AGENTS.md "Verification expectations":

1. **Host build:** `make -C hal host` must pass. (The `host` target
   was added in the prior session; if it's still missing for any
   reason, fall back to `make -C hal/host DEVICE=rachel_raccoon thyme`
   and call that out.)
2. **Particle target builds**, both with `LIGHT_SENSOR_TYPE` defined so
   the boot_light block compiles:
   - `make -C hal DEVICE=ricky_raccoon ants` (Photon 2)
   - `make -C hal DEVICE=rico_raccoon thyme` (OG Photon, tightest
     budget)
3. **Sanity grep:**
   - `grep -rn 'cat=boot_light' hal/` → zero matches (was one).
   - `grep -rn 'event=boot_light' hal/particle/` → exactly one match
     (the new format string).
   - `grep -rn 'event=' hal/esp32/` → zero matches (ESP32 untouched).
4. **No on-device run required** for a format-string change of this
   size, but if hardware is reachable: boot a Particle device with
   `LIGHT_SENSOR_TYPE` (e.g. ricky_raccoon or rico_raccoon); expect
   one message on `critterchron/public/error` ~6s post-boot carrying
   `event=boot_light` instead of `cat=boot_light`. Document what you
   actually ran.

## Closing handoff

Per AGENTS.md "Dispatch conventions": drop
`dispatch/error-stream-event-field-handoff.md` next to this brief
covering status, files touched, verification actually run (don't
fabricate), surprises vs. the brief's assumptions, follow-ups, and
notes for the next agent in this area.

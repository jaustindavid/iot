# AGENTS.md — orientation for cold-read agents

This file is the first thing a dispatched agent should read. It covers
the durable shape of the project. Anything that rots in weeks belongs
in `TODO.md` or `handoff.md`, not here.

## What this project is

CritterChron is a per-device LED-matrix runtime. Behaviors ("critters")
are authored in `.crit` scripts under `agents/`, compiled to a small IR,
and run on the same engine in two places:

1. A Python **gold-master simulator** (`engine.py`, `compiler.py`,
   `renderer.py`, `main.py`) — the reference implementation.
2. A C++ **HAL** (`hal/`) that runs on embedded targets via per-platform
   shims (Particle and ESP32 families today).

The sim defines correct behavior. The HAL exists to reproduce it on
hardware. When sim and device disagree, the sim wins unless there's a
documented reason otherwise.

## File layout

```
critterchron/
├── engine.py, compiler.py, renderer.py, main.py   # gold-master sim
├── agents/*.crit                                  # shared behavior scripts
├── tools/                                         # publish, trace, fleet helpers
├── hal/
│   ├── CritterEngine.{h,cpp}                      # cross-platform engine
│   ├── ErrLog.{h,cpp}                             # device-side error ring
│   ├── SnapshotBuffer.{h,cpp}                     # frame ring for failure triage
│   ├── interface/
│   │   ├── Config.h                               # live KV surface (Stra2us-backed)
│   │   ├── CritTimeSource.h                       # clock seam (NOT `TimeSource` —
│   │   │                                          #  collides with DeviceOS 6.x enum)
│   │   └── LedSink.h                              # render target seam
│   ├── ir/                                        # .crit → critter_ir.h encoder
│   ├── devices/<name>.h                           # per-device config (pins, geometry,
│   │                                              #  RAM overrides like NO_IR_OTA)
│   ├── host/                                      # smoke-test harness on dev machine
│   ├── particle/src/                              # Photon / Photon 2 / Argon shim
│   │   ├── critterchron_particle.cpp              # entry point, shared across all
│   │   │                                          #  WiFi-capable Particle devices
│   │   ├── Stra2usClient.{h,cpp}                  # KV + publish + OTA fetch
│   │   ├── NeoPixelSink.h, LightSensor.h, ...
│   └── esp32/src/                                 # ESP32 / C3 / C6 shim
│       ├── critterchron_esp32.ino                 # entry point, mirrors the .cpp
│       └── Stra2usClient.{h,cpp}, FastLEDSink.h, ...
├── dispatch/                                      # per-task briefs + closing handoffs
└── *.md                                           # see "Doc map" below
```

The two device entry points (`critterchron_particle.cpp` and
`critterchron_esp32.ino`) are **siblings** — see the symmetry rule
below.

## The two-target symmetry rule

Almost every change to the device firmware lands in **both** entry
points, plus their respective `Stra2usClient.{h,cpp}` if the transport
is involved. Comments in one file routinely reference the other (look
for "mirror of hal/esp32/..." and vice versa).

When writing a dispatch, assume **symmetric edits required** unless
explicitly scoped to one chip family. When reviewing your own work,
grep for the function/macro name in the other tree and confirm parity.

## Stra2us in 4 lines

Stra2us is a tiny KV-and-queues service the fleet talks to. KV is used
for **live-tunable config** (`<app>/<device>/<key>` with fallback to
`<app>/<key>`); topics are used for **device publishes** (telemetry,
snapshots, OTA lifecycle). Topics under `<app>/public/...` are the
fleet-visible aggregates ([PUBLIC_NAMESPACE.md](PUBLIC_NAMESPACE.md)).
All transport is HMAC-signed with a per-device shared secret; the
device runs all of this on a dedicated **telemetry thread**, never on
the engine thread.

Devices fetch with `g_cfg.get_int / get_float` (hot path, RAM-only) and
publish with `g_cfg.publish(topic, body)` (on the tel thread).

CLI shape: `stra2us set <device> <key> <value>` to write,
`stra2us get <full/path>` to read. `<full/path>` is e.g.
`critterchron/ricky_raccoon/heartbeep`.

## Build & flash

```bash
make -C hal DEVICE=ricky_raccoon ants        # Photon 2: encode IR, build, flash
make -C hal DEVICE=rico_raccoon  thyme       # OG Photon (rico)
make -C hal DEVICE=c3a_tanuki    swarm       # ESP32-C3 example
```

`DEVICE` matches a header in `hal/devices/`; the script name matches a
file in `agents/`. The host smoke target lives under `hal/host/`.

## RAM / stack budget reality

The fleet spans wildly different chips. The tightest is **rico_raccoon**
(OG Photon / P1) — ~80KB user RAM after WICED, and many design
decisions exist to fit it: `NO_IR_OTA`, `NO_SNAPSHOT_BUFFER`, smaller
buffers, tel-thread stack auto-couples to 5KB on `NO_IR_OTA` else 8KB.
Photon 2 / Argon / ESP32 have headroom; rico does not.

When designing changes: check `hal/devices/rico_raccoon.h` to see what
it opts out of, and prefer per-device overrides
(`#ifndef X #define X …`) over global constraints.

## Comment style

The codebase explains **why**, often at length. Multi-line comments
above non-obvious code blocks are the norm and load-bearing —
hard-won context lives in them (incident dates, alternative approaches
considered, RAM/stack tradeoffs, platform quirks). Don't strip them.
When adding code that has non-obvious motivation, write a comment in
the same spirit. WHAT is in the identifiers; WHY is in the comment.

## Dispatch conventions

- Briefs live in `dispatch/<task-name>.md` and are written for cold-read.
- Line numbers in briefs drift between when the brief is written and
  when it's executed. **Anchor on function names and macros**
  (`telemetry_cycle`, `STRA2US_TELEMETRY_TOPIC`, `build_heartbeat_report`)
  to find the real edit sites; treat brief line numbers as approximate.
- When done, drop a `dispatch/<task-name>-handoff.md` next to the brief
  covering: status, files touched, verification actually run (don't
  fabricate), surprises vs. the brief's assumptions, follow-ups, and
  anything the next agent should know. The handoff is how the work
  gets sealed.
- If the brief's assumptions break (an extra call site, a missing
  header, a buffer reused elsewhere), **stop and surface** rather than
  improvising silently. The brief was written from a snapshot and may
  be wrong about specifics.

## Verification expectations

- **Host build** (`make -C hal host`) is the cheapest gate and should
  always pass before declaring done.
- **At least one target build** for the affected chip family — both
  families if the change is symmetric.
- **On-device verification** is best-effort; many tasks ship without
  hardware in the loop. Say so honestly in the handoff rather than
  fabricating live tests.

## Doc map

- [README.md](README.md) — top-level intro.
- [HAL.md](HAL.md), [HAL_SPEC.md](HAL_SPEC.md) — HAL design + phasing.
- [PUBLIC_NAMESPACE.md](PUBLIC_NAMESPACE.md) — the `<app>/public/...`
  topic & KV convention.
- [STRA2US_CATALOG_FR.md](STRA2US_CATALOG_FR.md) — the KV catalog.
- [FAILURE_TRIAGE.md](FAILURE_TRIAGE.md) — snapshot/trace tooling for
  hard-to-reproduce field failures.
- [OTA_IR.md](OTA_IR.md) — over-the-air script swap pipeline.
- [PROCYON.md](PROCYON.md) — captive-WiFi rescue mode.
- [MARKERS_SPEC.md](MARKERS_SPEC.md) — visual markers in `.crit`.
- [TODO.md](TODO.md) — live punch list (rots fast; check date).
- [dispatch/](dispatch/) — per-task briefs + closing handoffs from
  prior cuttlefish work. Before touching an area, scan for a related
  brief/handoff pair — they carry rationale, surprises, and
  open follow-ups that a fresh agent would otherwise re-discover.
- [handoff.md](handoff.md) — last project-wide status snapshot
  (dated; check before relying on details).
- `hal/devices/<name>.h` — per-device config; read the one(s) your task
  touches before designing for RAM.

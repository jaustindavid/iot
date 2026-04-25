# CritterChron — handoff snapshot (2026-04-19)

Where things stand at the end of this session. See `TODO.md` for the
live punch list; see `HAL_SPEC.md` for the original phased plan.

## Status: Phase 1A and 1B are done.

Both grid shapes work end-to-end on Photon 2:

- **ricky_raccoon** (16×16): runs `ants.crit` cleanly. Steady-state ~73
  live agents, minute rollovers repaint correctly, heap bit-flat for
  hours.
- **rachel_raccoon** (32×8): runs `thyme.crit` and `coati.crit`. Aspect-
  switched clock layout (`HH MM` single row, vertically centered). Pool
  and dumpster landmarks render as the fixed background.

Telemetry rollup is emitted once per second over serial (physics/render
timings with max + avg, live agent count, `failed_seeks`, free heap) —
sufficient for at-a-glance health without Stra2us.

## Architecture reminders (what's where)

```
critterchron/
├── engine.py, compiler.py, renderer.py, main.py   # gold-master sim
├── agents/*.crit                                  # shared behavior scripts
├── hal/
│   ├── CritterEngine.{h,cpp}                      # cross-platform engine
│   ├── interface/{LedSink,CritTimeSource}.h       # platform seam (CritTimeSource not TimeSource — collides with DeviceOS 6.x's enum)
│   ├── ir/ir_encoder.py                           # .crit → critter_ir.h
│   ├── Makefile                                   # PLATFORM dispatcher
│   ├── devices/<name>.h                           # per-device config
│   ├── host/                                      # host harness (smoke test)
│   └── photon2/
│       ├── Makefile                               # assemble + flash
│       └── src/{critterchron_photon2.cpp,
│                NeoPixelSink.h, ParticleTimeSource.h}
```

Engine is one TU compiled into every platform. The Photon shim is the
only file that touches `Particle.h`.

## Build recipes

```bash
# Flash a script to a device (encodes IR + copies creds.h + particle flash):
make -C hal DEVICE=ricky_raccoon ants
make -C hal DEVICE=rachel_raccoon thyme
make -C hal DEVICE=rachel_raccoon coati

# Host smoke test at a given device's geometry:
make -C hal PLATFORM=host DEVICE=rachel_raccoon coati

# Serial follow:
particle serial monitor --follow   # only ricky has the serial cable attached
```

## Deferred work

See `TODO.md`. Summary: phase 2 (light sensor), phase 3 (WobblyTime),
phase 4 (Stra2us telemetry), phase 5 (ESP32), phase 6 (OTA IR). None
of these block current functionality. Device headers already carry the
Stra2us ID / HMAC key fields so phase 4 won't need a schema change.

## Things that bit us this session (keep in mind)

- **creds.h is device-specific.** Any time `DEVICE=` changes, the
  Makefile re-copies it via `cmp -s` + `.PHONY: FORCE`. If you ever
  see a device rendering at the wrong geometry, clean `hal/photon2/src/`
  and re-flash.
- **`critter_ir.h` is the script × geometry product.** The IR encoder
  bakes `GRID_WIDTH` / `GRID_HEIGHT` into guarded `#define`s at the top
  of the header, so `CritterEngine.cpp` (which includes `critter_ir.h`
  via `CritterEngine.h`) sees the right grid dims at compile time.
  Don't `-D` them from the Makefile — let the IR header own it.
- **Photon 2 rescue hold.** On crash-type reset (PANIC/WATCHDOG/
  UPDATE_ERROR/UPDATE_TIMEOUT), the shim holds the engine off for 60s
  with an amber chase animation, so a replacement firmware can be
  OTA-flashed without physical reset. Clean resets (power, pin, user)
  start immediately.
- **Landmarks draw as background in the C++ render**, unlike earlier
  phase-1A builds. If a new script hides the background behind agents,
  that's now correct; if something is invisible that shouldn't be,
  check the landmark color resolves against the `colors` block.

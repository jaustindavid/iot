# hal/ — build and deploy

This directory holds the cross-platform engine plus per-platform shims
and Makefiles for each target hardware family. All builds are driven
through the top-level `hal/Makefile`, which dispatches by device header
to the right per-platform subdirectory.

## How dispatch works

`make DEVICE=<name> <verb>` reads `hal/devices/<name>.h`, extracts
`#define DEVICE_PLATFORM "<platform>"`, and forwards the call to
`hal/<platform>/Makefile`. Adding a new platform is "drop a `<device>.h`
that names it, add `hal/<platform>/Makefile` if missing." From your
perspective the same `make DEVICE=…` line works regardless of whether
the device is a Photon, Photon 2, Argon, or ESP32-C3.

```
                            hal/Makefile
                                 │
                                 ▼
                     reads hal/devices/<name>.h
                                 │
                ┌────────┬───────┼────────┬────────┐
                ▼        ▼       ▼        ▼        ▼
            photon/  photon2/  argon/  esp32c3/  host/
            (P0)     (P2)     (Argon) (timmy)   (sim)
```

## Common verbs (work on all platforms)

```bash
make DEVICE=<device> <script>            # encode IR + compile (no flash)
make DEVICE=<device> <script> flash      # encode IR + compile + flash
make DEVICE=<device> flash               # re-flash whatever was last built
make DEVICE=<device> clean               # nuke build artifacts
make list                                # show devices + scripts available
```

`<device>` is any header in `hal/devices/` (without the `.h`):
`rachel_raccoon`, `ricky_raccoon`, `rico_raccoon`, `ronaldo_raccoon`,
`elk_cheetah`, `timmy_tanuki`, etc.

`<script>` is any `.crit` in `agents/` (without the extension):
`ants`, `coati`, `coati-fade`, `fraggle`, `swarm`, `thyme`, `tortoise`, etc.

A `<device>` named alone is shorthand for `flash DEVICE=<device>` —
e.g. `make rachel_raccoon` rebuilds and flashes rachel with the last
script she had.

## Platform-specific verbs

The cross-platform set above covers 90% of work. The remaining verbs
are platform-specific and reflect different hardware deployment models.

### Particle (photon, photon2, argon)

`flash` is the only deploy verb. It calls `particle flash <device>
<firmware.bin>`, which goes through Particle Cloud — the developer
laptop doesn't need to be on the same LAN as the device.

```bash
make DEVICE=rachel_raccoon coati flash    # encode + compile + cloud-flash
make DEVICE=rachel_raccoon flash          # re-flash whatever was last built
```

If the device is offline (lost cloud), the Cloud queues the flash and
delivers it on next reconnect. Useful, but means there's no
"this just hit the device" confirmation directly from the make
output — watch for the `fw=<date>` in the heartbeat to confirm.

### ESP32 (esp32c3 — currently just timmy_tanuki)

ESP32 has three deploy verbs because the hardware/network model is
different:

| verb | what it does | when to use |
|---|---|---|
| `flash` | OTA push to `<device>.local:3232` over LAN | most of the time |
| `flash-ota` | alias for `flash` | back-compat, pre-2026-04-24 muscle memory |
| `flash-usb` | wired flash via `arduino-cli upload` | fresh board, recovery, mDNS broken |
| `publish` | stage firmware to Stra2us KV (no flashing) | Phase 1 of pull-OTA — Phase 2 will let devices auto-pull |

```bash
# Common case — OTA push from the laptop:
make DEVICE=timmy_tanuki coati flash

# Fresh board / no WiFi yet — wired flash:
make DEVICE=timmy_tanuki coati flash-usb PORT=/dev/cu.usbmodem101

# OTA push to a specific IP (mDNS not resolving):
make DEVICE=timmy_tanuki coati flash OTA_HOST=192.168.1.42

# Stage the built firmware to Stra2us (does NOT flash any device):
make DEVICE=timmy_tanuki coati publish
```

Notes on ESP32 publish:
- Stages the firmware to `critterchron/fw/<platform>` (`esp32c3` today)
  plus a sidecar at `critterchron/fw/<platform>/sha`.
- Idempotent — re-runs with no source changes are a no-op. Pass
  `FORCE=1` to override.
- Does not point any device at the staged firmware. Phase 2 of the
  pull-OTA work (still in TODO) lets devices auto-pull on a cadence.
- Until Phase 2 lands, `publish` is mostly a wire-layer test
  vehicle — useful for confirming the staging round-trip works
  end-to-end against your Stra2us instance.

### Host harness (PLATFORM=host)

Run the C++ engine locally — no hardware needed. Useful for
smoke-testing engine changes before a flash round-trip, or for
reproducing engine bugs against a known IR.

```bash
make DEVICE=rachel_raccoon PLATFORM=host coati          # build + run
make DEVICE=rachel_raccoon PLATFORM=host coati-run      # re-run without rebuild
make DEVICE=rachel_raccoon PLATFORM=host coati \
    ARGS="--ticks 500 --seed 42"                        # custom run args
```

The host harness compiles the same engine TU as the on-device build,
parameterized by the device's grid geometry. So a host run at
`DEVICE=rachel_raccoon` exercises the engine at 32x8; at
`DEVICE=ricky_raccoon` it's 16x16. Catches geometry-dependent bugs
(fade quantization, marker indexing, A* clamping) without burning
flash cycles.

There's no `flash` for host. The build artifact is a Mac/Linux
executable in `hal/host/build/`.

## Cheat sheet

| goal | command |
|---|---|
| Flash rachel with thyme | `make DEVICE=rachel_raccoon thyme flash` |
| Re-flash rachel (last script) | `make rachel_raccoon` |
| Compile only, no flash | `make DEVICE=rachel_raccoon coati` |
| Wired flash to a fresh ESP32 | `make DEVICE=timmy_tanuki swarm flash-usb PORT=/dev/cu.usbmodem101` |
| Stage ESP32 firmware to Stra2us | `make DEVICE=timmy_tanuki coati publish` |
| Run thyme on the host harness at rachel's geometry | `make DEVICE=rachel_raccoon PLATFORM=host thyme` |
| List devices and scripts | `make list` |
| Clean a platform's build dir | `make DEVICE=<any-platform-device> clean` |

## Environment variables and overrides

| variable | default | applies to | what it does |
|---|---|---|---|
| `DEVICE` | `rachel_raccoon` | all | which device header (= which platform + geometry + secret) drives the build |
| `PLATFORM` | (auto from device header) | all | force a platform; useful for `PLATFORM=host` to run the engine locally |
| `PORT` | (none) | esp32c3 `flash-usb` | tty path for wired flash; required for `flash-usb` |
| `OTA_HOST` | `<DEVICE>.local` | esp32c3 `flash` | mDNS hostname or IP for OTA push |
| `OTA_PORT` | `3232` | esp32c3 `flash` | OTA port (matches the device's ArduinoOTA listener) |
| `FORCE` | (none) | esp32c3 `publish` | re-upload even if the remote sidecar matches |
| `ARGS` | (varies) | host | extra args passed to the host harness binary |

## Directory layout (orientation)

```
hal/
├── Makefile                # top-level dispatch (this README)
├── CritterEngine.{h,cpp}   # cross-platform engine (one TU, all platforms)
├── ErrLog.{h,cpp}          # cross-platform error ring buffer
├── interface/              # platform-seam abstract base classes
│   ├── LedSink.h
│   ├── CritTimeSource.h
│   └── Config.h
├── ir/
│   ├── ir_encoder.py       # .crit → critter_ir.h compile-time blob
│   ├── ir_text.py          # .crit ↔ wire-format encoder/decoder
│   └── IrRuntime.{h,cpp}   # device-side IR loader (parses both compile-in and OTA blobs)
├── devices/
│   ├── <name>.h            # per-device config (identity, secret, geometry, tuning)
│   └── device_name.h       # template / docs for new devices
├── photon/   photon2/   argon/                 # Particle ports
│   ├── Makefile            # platform-specific assembly + flash
│   └── src/                # populated at build time from particle/src/ + creds.h
├── particle/src/           # shared Particle (photon/photon2/argon) shims
├── esp32c3/                # ESP32 build (currently just timmy)
│   ├── Makefile
│   └── build/
├── esp32/src/              # shared ESP32 shims
└── host/                   # local sim / dev harness
    ├── Makefile
    ├── build/
    └── src/                # FakeTimeSource, DumpSink, main.cpp
```

The per-platform `src/` directories under `photon/`, `photon2/`,
`argon/`, `esp32c3/` are *generated* at build time — copies of the
shared shim files plus the chosen device's header as `creds.h`.
Don't hand-edit them; edit the canonical sources under
`particle/src/` or `esp32/src/` instead.

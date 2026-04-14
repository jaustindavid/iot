# Coati Clock — Particle Photon 2

A multi-agent LED matrix clock for the Particle Photon 2. Two autonomous agents
rearrange pixels on a 32×8 WS2812 matrix to display the current time.

The physics engine is **generated from the `.coati` script** — edit behavior in
`scripts/coaticlock.coati`, regenerate, and flash. No C++ editing required for
behavior changes.

---

## Quick Start

```bash
# 1. Regenerate the engine from the script (run from project root)
python3 -m coati codegen scripts/coaticlock.coati --output-dir coaticlock/src/

# 2. Flash all configured devices
cd coaticlock && make all
```

---

## Project Layout

```
coaticlock/
  src/
    CoatiEngine.h          ← GENERATED (do not edit manually)
    CoatiEngine.cpp        ← GENERATED (do not edit manually)
    coaticlock.cpp         ← HAL: main loop, rendering, Stra2us polling
    WobblyTime.h           ← Virtual clock with asymmetric drift
    LightSensor.h          ← CDS cell → brightness curve
    Stra2usClient.h/.cpp   ← HMAC-signed telemetry client
    hmac_sha256.h/.cpp     ← Standalone HMAC-SHA256 (no mbedtls)
    sha256.h/.cpp          ← SHA-256 primitive
    creds.h                ← Pulled in via device_name.h (see below)
  <device_name>.h          ← Per-device: grid dims, rotation, credentials

scripts/
  coaticlock.coati         ← Source of truth for agent behavior
```

---

## Device Configuration

Each physical device gets its own header file (e.g. `ricky_raccoon.h`).
`device_name.h` symlinks or includes the active device:

```cpp
// Example: ricky_raccoon.h
#define GRID_WIDTH   32
#define GRID_HEIGHT   8
#define GRID_ROTATION  0   // 0 | 90 | 180 | 270
#define STRA2US_CLIENT_ID  "ricky_raccoon"
#define STRA2US_SECRET_HEX "deadbeef..."
#define STRATUS_APP        "coaticlock"
```

The Makefile iterates all `*_raccoon.h` files and flashes each device in sequence.

---

## Hardware

| Component | Details |
|---|---|
| MCU | Particle Photon 2 (RTL8721DM, ARM Cortex-M33 @ 200 MHz) |
| Display | WS2812B 32×8 LED matrix, connected via SPI-DMA on **D2** |
| Light sensor | CDS cell voltage divider: **A0** = 3.3 V, **A2** = GND, **A1** = ADC input |
| ADC range | 0 (bright) → 4095 (dark) — inverted and mapped through `sqrtf` curve |

---

## Physics & Rendering

The firmware runs two independent loops:

| Loop | Rate | Purpose |
|---|---|---|
| Physics | 10 Hz / 100 ms | Agent tick, pathfinding, board updates |
| Render | 60 Hz / 16 ms | LED interpolation, motion blur, brightness |

Motion blur interpolates agent positions between physics ticks using the
`blend = (now - last_physics_tick) / 100.0f` factor, yielding smooth 6× oversampled
animation despite the 10 Hz physics rate.

---

## Time & WobblyTime

`WobblyTime` maintains a virtual clock that drifts relative to real wall time.
The physics tick rate is adjusted to reconcile virtual time with NTP.

Parameters (configurable via Stra2us KV):
- `wobble_min_seconds` / `wobble_max_seconds` — drift bounds
- `wobble_fast_rate` / `wobble_slow_rate` — asymmetric tick speeds
- `timezone_offset` — local timezone for `Time.zone()`

---

## Telemetry (Stra2us)

Every `heartbeep` seconds (default: 300 s), the firmware:
1. Publishes a heartbeat report (uptime, RSSI, memory, brightness, firmware version)
2. Polls KV pairs for live configuration updates

Signed with HMAC-SHA256. The standalone crypto implementation (`sha256.cpp`,
`hmac_sha256.cpp`) avoids the mildly unstable mbedtls header exposure in
Particle OS 6.x.

---

## Modifying Behavior

Edit `scripts/coaticlock.coati` in the project root, then regenerate:

```bash
python3 -m coati codegen scripts/coaticlock.coati --output-dir coaticlock/src/
```

Test in the simulator first:

```bash
python3 -m coati run scripts/coaticlock.coati
python3 -m coati fast scripts/coaticlock.coati --time 12:34 --max-ticks 500
```

The simulator and the generated C++ run the same IR, so behavior should be
identical. See `DESIGN.md §5` for the full IR→C++ mapping.

---

## Dependencies

- `neopixel` — WS2812 driver for Particle
- `Adafruit_GFX_RK` — (linked but not used directly; kept for compatibility)

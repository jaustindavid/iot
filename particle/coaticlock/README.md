# Coati Clock Particle Port

A port of the multi-agent LED matrix clock simulation from ESPHome to the Particle Photon 2 platform.

## Overview

The Coati Clock features a multi-agent physics engine where pixels act as autonomous agents that "clean" the clock digits and interact with a simulated environment (Dumpster and Pool). This port maintains the core C++ engine while optimizing it for the Photon 2's hardware and the Stra2us IoT backend.

## Implementation Details

### 1. Physics Engine (`CoatiEngine`)
- **A* Pathfinding**: Agents calculate paths to target digits, endpoints, or random "bored" walk targets. Pathfinding node limit expanded from 40 to 256 for thorough matrix traversal.
- **State Machine**: Manages transitions between walking, carrying, washing, and waiting. Agents aggressively escape if trapped under a digit ("Floor is Lava") and fetch excess pixels to the dumpster.
- **Manual Font Rendering**: Implements 5x6 numeric glyphs parsed from `megafont.bdf`. Render buffer is shifted downwards on the Y-axis to prevent agent stalemates.

### 2. Time & Drift (`WobblyTime`)
- Maintains a "virtual epoch" that drifts relative to the real wall-clock time.
- Adjusts the physics tick rate (20Hz) to reconcile the virtual time with NTP syncs using configurable "snapping" thresholds for massive drift.
- Timezone offsets natively utilize `Time.zone()` to cleanly map virtual UTC to local timezone rendering.

### 3. Telemetry & Configuration (`Stra2usClient`)
- **Connection**: Uses `TCPClient` for persistent, low-overhead communication with the Stra2us backend.
- **Security**: Signed requests using HMAC-SHA256 (standalone implementation in `src/`).
- **Sync**: Periodically polls KV pairs for configuration. Supports Msgpack `int`, `string`, `float32`, and `float64` decoding. Devices dynamically pull their `timezone_offset`, `min_brightness`, and `max_brightness` bounds directly from the Stra2us backend.

### 4. Hardware Integration
- **Display**: WS2812 32x8 matrix on pin **D2**. Uses **SPI/DMA** for efficient, non-blocking rendering.
- **Ambient Light Sensor**: CDS cell on **A1** with an automatic inverse-square-root dimming curve with calibrated `4040` (out of 4095) darkness noise floors.

## Installation & Build

1. **Hardware**: Connect your WS2812 data line to **D2** (MOSI) and your CDS cell voltage divider to **A1**.
2. **Credentials**:
   - Copy `device_name.h` to `rachel_raccoon.h` (or your chosen name).
   - Update `STRA2US_CLIENT_ID` and `STRA2US_SECRET_HEX`.
3. **Build**:
   - **Attention**: Before flashing, update the `#define APP_VERSION` macro at the very top of `src/coaticlock.cpp` using the `.HHMM` datecode standard (e.g., `"2026.04.09.0245"`). (We cannot use `project.properties` for this because the Particle Cloud compiler enforces Strict SemVer, silently mangling datecodes like `2026.04` into `1.0.0`).
   - Use the included `Makefile`:
     ```bash
     make flash DEVICE=rachel_raccoon
     ```
   - This automatically copies your config to `src/creds.h` and flashes the device.

## Dependencies

- **neopixel**: WS2812 driver for Particle.
- **Adafruit_GFX_RK**: Graphics core library.

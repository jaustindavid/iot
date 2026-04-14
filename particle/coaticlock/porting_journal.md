# Porting Journal: Coati Clock ESPHome to Particle Photon 2

This document tracks the technical challenges ("gotchas") and architectural decisions made during the migration of the Coati Clock to the Particle platform.

## Gotchas & Challenges

### 1. `mbedtls` Header Exposure
- **Issue**: The Particle Cloud compiler for Photon 2 (OS 6.x) did not expose the standard `mbedtls/md.h` or `mbedtls/sha256.h` headers to common user-space search paths, despite they being present in the HAL.
- **Solution**: To ensure long-term build stability and platform portability, I implemented a standalone **HMAC-SHA256** module in the project source. This removes the dependency on volatile system header exposures.

### 2. NeoPixel Initialization on Photon 2
- **Issue**: Standard bitbanging on the Photon 2 can be interrupted by system background tasks, leading to flickering on a 256-pixel matrix at high refresh rates.
- **Solution**: The `Adafruit_NeoPixel` library was configured to use the **SPI** constructor. This utilizes the hardware DMA on pin **D2** (MOSI), offloading the timing-critical signal generation to the hardware peripheral.

### 3. Coordinate System & Pixel Mapping
- **Issue**: The physical matrix is a 32x8 zigzag layout, but the engine logic operates on a standard Cartesian (X,Y) grid.
- **Solution**: Implemented a lambda-based pixel mapper `map_pixel(x, y)` that handles the zigzag inversion for odd columns:
  ```cpp
  if (x % 2 == 0) return (x * 8) + y;
  return (x * 8) + (7 - y);
  ```

### 4. Font Implementation
- **Issue**: The original ESPHome project relied on external BDF processing. I needed a lightweight way to render the 5x6 "megafont".
- **Solution**: Manually extracted the numeric glyphs from `megafont.bdf` and implemented a bitmask-based renderer in `CoatiEngine.cpp`. This keeps the binary small and avoids external font-processing libraries.

### 5. WiFi Credential Management
- **Issue**: Initial port included redundant `WIFI_SSID` defines in the headers.
- **Solution**: Purged these in favor of Particle's native out-of-band credential management. This prevents accidental credential leak if headers are checked into source control.

### 6. Animation Interpolation
- **Issue**: Standardizing the frame timings limited matrix drawing speeds to 20Hz synchronously coupled with `CoatiEngine.cpp`, leading to jerky physical rendering on the display structure.
- **Solution**: The physics simulation was explicitly uncoupled (clocking at 10Hz/100ms) from the hardware rendering system (60Hz/16ms). The pixel rendering sequence now interpolates Cartesian coordinate gradients internally, yielding seamlessly smooth $6\times$ oversampled LED animations as the firmware dynamically shades floating sub-pixel coordinates mathematically onto the physical hardware arrays.

## Architectural Notes
- **Memory Management**: Pre-allocated pixel boards and agent structures to prevent heap fragmentation during rapid physics looping.
- **Timing Constraint**: Matrix dimensioning heavily dictates processing limits. Dynamic target generation is carefully scaled into multiple internal geometry mapping rules via mathematical bounding box abstraction for scaling.

---

## Gotcha #7: Hand-Written Engine Replaced by Codegen

### Date
2026-04-13

### Issue

The hand-written `CoatiEngine.cpp` was developed as a monolithic port of the
ESPHome coaticlock logic. Over time the Python simulator accumulated behavior
fixes (pool starvation fix, `place` clearing `carrying`, `seek` no-op at dest,
Etc.) that needed careful manual backporting to keep the C++ in sync. Adding new
scripts (ladybugs, future effects) would require writing new C++ engine files by
hand each time.

### Solution

Replaced the hand-written engine with a code generator (`coati/codegen.py`).
The command:

```bash
python3 -m coati codegen scripts/coaticlock.coati --output-dir coaticlock/src/
```

produces `CoatiEngine.h` and `CoatiEngine.cpp` from the parsed IR. The generated
engine and the Python simulator now share the same IR as their source of truth,
so behavioral fixes applied to the Python engine are automatically reflected in
the next codegen run.

### Key design decisions

**`done` → lambda `return;`.**  The `.coati` language's `done` keyword exits the
entire behavior rule evaluation for the current agent. In generated C++, each
agent's behavior is wrapped in a `[&]()` lambda, so `done` maps cleanly to
`return;`. The outer physics loop still uses `continue` for movement/pause
short-circuits — the two levels are independent.

**IfNode semantics in the body loop.** When an IfNode's condition is true, the
Python engine breaks out of the body evaluation loop (skips subsequent body
items). In C++, the codegen appends `return;` at the end of a then-block, *unless*
the then-block already ends with `done` or `despawn` (which themselves generate
`return;`). A helper `_body_ends_with_return()` checks this to avoid unreachable
duplicate returns.

**Float literals.** Python's `f"{1.0:.6g}"` produces `"1"`. Appending `f` gives
`"1f"`, which is not standard C++ (requires `"1.0f"`). Fixed in `_cpp_literal()`
by inserting `.0` before the suffix when the formatted string contains neither
`.` nor `e`.

**No behavioral regression.** The seek-at-destination and pathfind-budget fixes
are baked into the generated `seek`/`seek_nearest` emitters, so the generated
code cannot re-introduce the starvation bug without also changing the codegen.

### `coaticlock.cpp` changes

The only changes to the HAL were three field renames in the render loop:
- `CoatiAgent&` → `AgentState&`
- `.is_bored` → `.bored`
- `.wait_ticks` → `.wait_counter`


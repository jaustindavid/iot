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

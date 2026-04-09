# Spec: Particle Photon 2 CoatiClock Port

This document outlines the requirements and technical approach for porting the "CoatiClock" (Multi-agent LED Matrix Simulation) from ESP32-C3/ESPHome to the **Particle Photon 2** platform.

## Hardware Specification

### 1. Processing
- **Target**: Particle Photon 2 (RTL8721DM).
- **Architecture**: ARM Cortex-M33 (200MHz), 2MB RAM (plenty for high-res physics/pathfinding).

### 2. LED Matrix (32x8)
- **Protocol**: WS2812B (NeoPixel)
- **Physical Connection**: 
    - **One-Wire (WS2812B)**: Connected to any GPIO with an RMT-equivalent driver (e.g., D2).
- **Requirements**: Supports 20Hz+ refresh rate for smooth "Coati" physics interpolation.

### 3. Ambient Light Sensor (CDS Cell)
- **Pin Mapping**: 
    - `A0`: Output HIGH (3.3V) — powers the voltage divider.
    - `A2`: Output LOW (GND).
    - `A1`: Analog Input.
- **Logic**: 12-bit ADC (0-4095).
- **Polarity**: 4095 (approx) = Darkest, 0 = Brightest. 
- **Mapping**: Must be inverted and normalized for the `sqrtf` brightness algorithm.

---

## Software Architecture

### 1. Framework Transition
ESPHome is a declarative framework; Particle Device OS is imperative (C++).

| Feature | ESPHome (Current) | Particle (Proposed) |
|---|---|---|
| **Entry Point** | `lambda` in `display` | `void loop()` |
| **Physics Ticks** | `interval: 50ms` | `SoftwareTimer` or `millis()` checking |
| **KV Polling** | `interval: 300s` | `SoftwareTimer` |
| **Storage** | `globals:` | Class member variables / `EEPROM` |

### 2. Component Porting

#### A. Coati Engine (`coati_engine.h`)
- **Portability**: High. The engine is standard C++.
- **Dependencies**: Remove `esphome.h`. Replace `display::DisplayBuffer` and `Color` with Particle-native equivalents (e.g. `Adafruit_GFX` + `FastLED`).
- **Memory**: The pre-allocated member vectors (scratch space) should remain to avoid heap fragmentation, though the Photon 2 is more resilient than the C3.

#### B. Stra2us Client (`IoTClientIDF.h`)
- **Challenge**: The current client uses `lwip/sockets.h` (ESP-IDF/POSIX).
- **Particle Fix**: Rewrite using Particle's `TCPClient` class.
- **Signing**: Use the `mbedtls` library (available in Device OS) for HMAC-SHA256.
- **Persistence**: Maintain the socket-reuse logic to save power/time on telemetry cycles.

#### C. Wobbly Time & Display
- **Time Sync**: Particle's `Time.now()` is automatic and resilient. 
- **Font Rendering**: Use `Adafruit_GFX` to render text into a `GFXcanvas1` (or equivalent) to emulate the `DummyDisplay` staging buffer.

---

## Implementation Plan

### Phase 1: Hardware Abstraction
- Initialize pins `A0`/`A2` and the `FastLED` strip.
- Implement the "Inverse Sqrt" dimming logic:
  ```cpp
  int raw = analogRead(A1);
  float normalized = (4095.0f - (float)raw) / 4095.0f; // 0=dark, 1=bright
  float lux_equiv = normalized * 500.0f; 
  float target = sqrtf(lux_equiv / 375.0f);
  // ... apply filter ...
  ```

### Phase 2: Core Engine Port
- Clean `coati_engine.h` of ESPHome-specific includes.
- Create a `ParticleCoatiEngine` wrapper that maps the physics output to a `FastLED` buffer.

### Phase 3: Networking
- Implement `Stra2usParticleClient` using `TCPClient`.
- Mirror the HMAC signing logic from `IoTClientIDF.h`.
- Setup `SoftwareTimer` for:
    - Heartbeat (300s).
    - KV variable synchronization (same 300s interval).

### Phase 4: Integration
- Combine physics, light sensing, and telemetry in `application.cpp`.
- Verify persistent socket behavior (server keep-alive).

---

## Verification Plan

### 1. Unit Tests (Engine)
- Run the pathfinding/collision logic on the Photon 2 and verify no stack overflows (using `System.freeMemory()`).

### 2. Connectivity
- Verify HMAC signatures match the backend expectations.
- Confirm KV variables are successfully retrieved and updated on the Photon 2 globals.

### 3. Aesthetics
- Verify the `sqrt` dimming curve provides the same "premium" feel as the ESP32 version.

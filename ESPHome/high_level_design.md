# Coati Clock: High-Level Design Overview

## Abstract
The **Coati Clock** is a custom firmware implementation for an ESP32-C3 microcontroller driving a 16x8 WS2812B LED matrix. It abandons traditional stateless digital clock rendering in favor of a localized, multi-agent game simulation. Instead of "blinking" to a new time layout each minute, autonomous AI agents (the "Coatis") physically carry and assemble the required pixels on the board, piece by piece, adhering to strict spatial constraints and pathfinding rules.

## Hardware Footprint
- **Controller:** Seeed Studio XIAO ESP32-C3
- **Display:** 256 WS2812B LEDs, physically wired as a snaking 16x8 matrix.
- **Virtual Layout:** The physical matrix is remapped through a custom XYZ pixel mapper to function as a seamless 32x8 logical display.

## Ecosystem Architecture
The project is built within **ESPHome**, leveraging its native networking (Wi-Fi, OTA) and integration (SNTP timezone sync) pipelines. However, typical ESPHome display pipelines render directly onto hardware buffers over rigidly defined update intervals. To support game logic and animation frames, the Coati project heavily injects standalone C++ engines that execute high refresh-rate logic via the `addressable_light` lambda hooks.

## Core Features
1. **Physical Font Layouts**
   The clock utilizes a highly customized 5x6 bitmap font (`CustomMerged.bdf`). Crucially, this font was specifically engineered with intentional gaps and "holes" in each glyph. This ensures that a single 1x1 sprite navigating orthagonally or diagonally can pass through numerals without getting trapped or creating sealed-off zones.
2. **Autonomous Multi-Agent AI**
   Multiple worker entities inhabit the matrix. They make independent, cooperative decisions every frame to disassemble stale time projections and assemble the fresh time representation. 
3. **The Pixel Economy (Fetch & Dispose)**
   The board respects pixel persistence. If a minute transitions from `12:59` to `01:00`, the agents calculate the exact pixel delta.
   - Usable left-over pixels are picked up and moved to new coordinates.
   - Dead pixels are carried to a designated **Blue Pool** for disposal.
   - Missing required pixels are fetched from a designated **Red Dumpster**.

## Re-Engineering Goals
If attempting to port or re-engineer this project from scratch on another platform (e.g., pure FastLED, MicroPython, or Arduino C++), the architecture must guarantee three things:
1. **Virtual Rendering Hooks:** The system must be able to rasterize the text font to an invisible backend array to determine the target pixel addresses, without overwriting the public display buffer.
2. **Array Comparison:** The system must maintain a rigid separation between the `CurrentState` (what the hardware currently shows) and the `TargetState` (what the off-screen renderer built).
3. **Collision Pathfinding:** A* or Dijkstra pathfinding is strictly required to navigate the jagged contours of the font layout. Simple straight-line interpolation will violate the simulation rules and break the visual illusion.

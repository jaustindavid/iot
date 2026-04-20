#pragma once
#include <cstdint>

// Engine ↔ platform seam. Engine writes FULL-BRIGHTNESS colors at logical
// (x, y); the sink owns brightness scaling AND hardware geometry (rotation,
// serpentine wiring, linear-index computation) on the way to the physical
// strip. See HAL_SPEC §4.1.
//
// Coordinates are pre-rotation and pre-serpentine: (0, 0) is the logical
// top-left corner of the grid as the engine sees it. 0 <= x < GRID_WIDTH,
// 0 <= y < GRID_HEIGHT.

struct LedSink {
    virtual void clear() = 0;
    virtual void set(int x, int y, uint8_t r, uint8_t g, uint8_t b) = 0;
    virtual void show() = 0;
    virtual ~LedSink() = default;
};

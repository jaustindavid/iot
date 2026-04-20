#pragma once

// NeoPixelSink — Photon 2 WS2812B LedSink impl.
//
// Owns:
//   - rotation (pre-rotation logical (x, y) → post-rotation (rx, ry))
//   - serpentine wiring (odd columns run bottom→top, even columns top→bottom)
//     matching the coaticlock convention: idx = rx*GRID_HEIGHT + ((rx & 1) ?
//     GRID_HEIGHT-1-ry : ry)
//   - global brightness scaling (float 0..1, applied per channel)
//
// Dimensions + rotation are compile-time constants pulled from creds.h
// (copied from hal/devices/<name>.h by the Makefile).

#if defined(PLATFORM_ID)

#include "Particle.h"
#include "neopixel.h"
#include "interface/LedSink.h"

#ifndef GRID_ROTATION
#define GRID_ROTATION 0
#endif
#ifndef PIXEL_TYPE
#define PIXEL_TYPE WS2812B
#endif
#ifndef MATRIX_PIN
#define MATRIX_PIN SPI
#endif

class NeoPixelSink : public LedSink {
public:
    NeoPixelSink()
        : strip_(GRID_WIDTH * GRID_HEIGHT, MATRIX_PIN, PIXEL_TYPE),
          brightness_(255) {}

    void begin() {
        strip_.begin();
        strip_.show();
    }

    // Brightness is a scale factor in [0, 255]; applied per channel. 255 = raw
    // engine color hits the LED; 0 = full black.
    void set_brightness(uint8_t b) { brightness_ = b; }

    void clear() override { strip_.clear(); }

    void set(int x, int y, uint8_t r, uint8_t g, uint8_t b) override {
        int rx, ry;
        rotate(x, y, rx, ry);
        if (rx < 0 || rx >= post_rot_w_() || ry < 0 || ry >= post_rot_h_()) return;
        int idx = rx * post_rot_h_() + ((rx & 1) ? (post_rot_h_() - 1 - ry) : ry);
        strip_.setPixelColor(idx, strip_.Color(
            scale_ch(r, brightness_),
            scale_ch(g, brightness_),
            scale_ch(b, brightness_)));
    }

    void show() override { strip_.show(); }

private:
    // Pre-rotation (x, y) in a GRID_WIDTH x GRID_HEIGHT frame →
    // post-rotation (rx, ry) in the physical strip's frame. For 90/270
    // the physical frame's width/height swap; serpentine indexing uses
    // post-rotation dims (see post_rot_w_/h_).
    static void rotate(int x, int y, int& rx, int& ry) {
#if GRID_ROTATION == 0
        rx = x;                   ry = y;
#elif GRID_ROTATION == 90
        rx = y;                   ry = GRID_WIDTH  - 1 - x;
#elif GRID_ROTATION == 180
        rx = GRID_WIDTH  - 1 - x; ry = GRID_HEIGHT - 1 - y;
#elif GRID_ROTATION == 270
        rx = GRID_HEIGHT - 1 - y; ry = x;
#else
#error "GRID_ROTATION must be 0, 90, 180, or 270"
#endif
    }

    static constexpr int post_rot_w_() {
#if GRID_ROTATION == 90 || GRID_ROTATION == 270
        return GRID_HEIGHT;
#else
        return GRID_WIDTH;
#endif
    }
    static constexpr int post_rot_h_() {
#if GRID_ROTATION == 90 || GRID_ROTATION == 270
        return GRID_WIDTH;
#else
        return GRID_HEIGHT;
#endif
    }

    // Per-channel scale that preserves visibility at the bottom of the
    // brightness range: if the original color had any light in this channel,
    // keep at least 1/255 of it after scaling. Prevents a dimmed (32, 64, 32)
    // from rounding to pure black — it fades to (1, 1, 1) instead. Hue is
    // distorted near the floor but pixels stay visible, which is the right
    // tradeoff for a clock. A future "night mode" can remap colors
    // deliberately (see TODO.md).
    static uint8_t scale_ch(uint8_t c, uint8_t bri) {
        if (c == 0) return 0;
        uint16_t s = (uint16_t)c * bri / 255;
        return s ? (uint8_t)s : 1;
    }

    Adafruit_NeoPixel strip_;
    uint8_t           brightness_;
};

#endif  // PLATFORM_ID

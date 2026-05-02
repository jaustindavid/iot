#pragma once

// FastLEDSink — ESP32-family WS2812B LedSink impl, backed by FastLED's
// RMT driver. Shape-compatible with hal/particle/src/NeoPixelSink.h:
// same rotation table, same serpentine wiring (odd cols bottom→top),
// same per-channel nonzero-preserving brightness scale — so a blob
// that renders correctly on Particle renders identically on ESP32.
//
// FastLED wants the data pin known at *template* time (CLEDController
// is a templated class), and GRID_WIDTH × GRID_HEIGHT known at pool-
// allocation time, so everything here is pulled from the compile-time
// macros in creds.h. Runtime-changing the pin or dims requires a rebuild
// — same constraint as NeoPixelSink.

#if defined(ARDUINO_ARCH_ESP32)

#include <Arduino.h>
#include <FastLED.h>
#include "interface/LedSink.h"

#ifndef GRID_ROTATION
#define GRID_ROTATION 0
#endif
#ifndef PIXEL_TYPE
#define PIXEL_TYPE WS2812B
#endif
#ifndef MATRIX_PIN
#error "creds.h must define MATRIX_PIN (GPIO number) for FastLEDSink"
#endif

class FastLEDSink : public LedSink {
public:
    FastLEDSink() : brightness_(255) {}

    void begin() {
        // FastLED.addLeds is a templated call — MATRIX_PIN and PIXEL_TYPE
        // must be compile-time constants. Color order GRB matches WS2812B
        // silicon; override by dropping a per-device palette fix later if
        // a variant ships different wiring.
        FastLED.addLeds<PIXEL_TYPE, MATRIX_PIN, GRB>(
            leds_, GRID_WIDTH * GRID_HEIGHT);
        // We own brightness scaling per-channel (matches NeoPixelSink so
        // blobs look identical across ports), so keep FastLED's global
        // multiplier at full and let our scale_ch do the floor-preserve.
        FastLED.setBrightness(255);
        // Disable temporal dithering. With BINARY_DITHER (FastLED's
        // default) low per-channel PWM values get pulsed off most frames
        // to time-average toward sub-PWM-1 targets. That suppresses the
        // pixels scale_ch's floor-preserve was designed to keep visible:
        // a value of 1 reads on Particle/NeoPixelBus (no dithering) but
        // dithers to mostly-off on FastLED, producing platform-divergent
        // night-palette behavior (observed 2026-04-28: rachel renders
        // night brick (0,1,0) as a clear green dot, timmy renders it as
        // dark — same blob, same scale_ch). Same fleet, same intent —
        // disable dithering so what scale_ch writes is what the LED gets.
        FastLED.setDither(DISABLE_DITHER);
        FastLED.clear(true);
    }

    void set_brightness(uint8_t b) { brightness_ = b; }

    void clear() override {
        // FastLED.clear() without `true` clears the framebuffer but
        // doesn't push to the strip — show() below handles that.
        FastLED.clear();
    }

    void set(int x, int y, uint8_t r, uint8_t g, uint8_t b) override {
        int rx, ry;
        rotate(x, y, rx, ry);
        if (rx < 0 || rx >= post_rot_w_() || ry < 0 || ry >= post_rot_h_()) return;
        int idx = rx * post_rot_h_() + ((rx & 1) ? (post_rot_h_() - 1 - ry) : ry);
        leds_[idx] = CRGB(
            scale_ch(r, brightness_),
            scale_ch(g, brightness_),
            scale_ch(b, brightness_));
    }

    void show() override {
        // Apply overlay row (if active) just before pushing to LEDs.
        // Engine writes its non-zero pixels via set() earlier this
        // frame; here we paint overlay pixels ONLY where no engine
        // output exists (leds_[idx] == 0). Net effect: sparkline is
        // background, agents on the overlay row render on top of it.
        // sink.clear() at frame start guarantees leds_ begins zeroed,
        // so "is zero" cleanly means "no agent here this frame."
        if (overlay_active_) {
            for (int x = 0; x < GRID_WIDTH; ++x) {
                int rx, ry;
                rotate(x, overlay_y_, rx, ry);
                if (rx < 0 || rx >= post_rot_w_() ||
                    ry < 0 || ry >= post_rot_h_()) continue;
                int idx = rx * post_rot_h_() +
                          ((rx & 1) ? (post_rot_h_() - 1 - ry) : ry);
                if (leds_[idx].r || leds_[idx].g || leds_[idx].b) continue;
                const uint8_t* p = &overlay_pixels_[x * 3];
                leds_[idx] = CRGB(
                    scale_ch(p[0], brightness_),
                    scale_ch(p[1], brightness_),
                    scale_ch(p[2], brightness_));
            }
        }
        FastLED.show();
    }

    // Latency-sparkline overlay. n MUST be GRID_WIDTH (each entry is
    // 3 bytes: r,g,b). The sink copies the pixel data so the caller's
    // buffer doesn't need to outlive the call. Brightness scaling +
    // grid rotation are applied in show() via the same set() path the
    // engine uses, so the overlay matches the rest of the panel's
    // brightness regime instead of overpowering low-light scenes.
    void set_overlay_row(int y, const uint8_t* rgb_pixels) {
        if (y < 0 || y >= GRID_HEIGHT) return;
        overlay_y_ = y;
        memcpy(overlay_pixels_, rgb_pixels, sizeof(overlay_pixels_));
        overlay_active_ = true;
    }

    void clear_overlay() { overlay_active_ = false; }

private:
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

    // Per-channel scale mirrors NeoPixelSink::scale_ch — see that file's
    // comment for the rationale. Keep behavior identical across sinks so
    // the night-palette regime triggers at the same sensor reading on
    // both ports.
    static uint8_t scale_ch(uint8_t c, uint8_t bri) {
        if (c == 0) return 0;
        uint16_t s = (uint16_t)c * bri / 255;
        return s ? (uint8_t)s : 1;
    }

    CRGB    leds_[GRID_WIDTH * GRID_HEIGHT];
    uint8_t brightness_;

    bool    overlay_active_              = false;
    int     overlay_y_                   = 0;
    uint8_t overlay_pixels_[GRID_WIDTH * 3] = {0};
};

#endif  // ARDUINO_ARCH_ESP32

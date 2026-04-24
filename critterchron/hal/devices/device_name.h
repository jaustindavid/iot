#pragma once

// Template device header — copy to hal/devices/<your_device>.h and fill in.
// Not consumed by any build; the Makefile filters this filename out of the
// devices list.

// --- Identity ---
// DEVICE_NAME is the canonical device id; STRA2US_CLIENT_ID derives from it
// so there's one string to keep in sync.
#define DEVICE_NAME         "your_device"
#define DEVICE_PLATFORM     "photon2"       // hal/<platform>/ subdir for this device
#define STRA2US_CLIENT_ID    DEVICE_NAME
#define STRA2US_SECRET_HEX  "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
#define STRA2US_HOST        "stra2us.austindavid.com"
#define STRA2US_PORT         8153
#define STRA2US_APP         "critterchron"

// --- Hardware geometry ---
#define GRID_WIDTH     16
#define GRID_HEIGHT    16
#define GRID_ROTATION  0                    // 0, 90, 180, 270
#define MATRIX_PIN     SPI                  // MOSI on Photon 2; override for pin-driven boards
#define PIXEL_TYPE     WS2812B

// --- Ambient light sensor ---
// Leave LIGHT_SENSOR_TYPE undefined for devices without a sensor — the shim
// will hold brightness at MAX_BRIGHTNESS. Currently the only supported type
// is CDS (photoresistor in a voltage divider, coaticlock hardware layout).
// Phototransistor / I2C options will slot in here when we build them.
// #define LIGHT_SENSOR_TYPE    CDS
// #define LIGHT_SENSOR_PWR_PIN A0
// #define LIGHT_SENSOR_SIG_PIN A1
// #define LIGHT_SENSOR_GND_PIN A2

// --- Tuning ---
// Brightness is a uint8_t 0..255 scale factor applied per-channel at the sink
// (nonzero input preserved as ≥1 at the bottom, so dim pixels don't vanish).
// 255 = engine colors hit the LED unscaled; 0 = full off.
#define MAX_BRIGHTNESS         102          // ~40% of full
#define MIN_BRIGHTNESS         3            // floor; only consulted when a sensor is present
#define TIMEZONE_OFFSET_HOURS -5.0f

// --- Night mode Schmitt thresholds ---
// The engine flips to its night palette (if the loaded blob defines one)
// when the smoothed sensor brightness is at or below NIGHT_ENTER_BRIGHTNESS,
// and flips back when it rises at or above NIGHT_EXIT_BRIGHTNESS. The gap
// is the hysteresis band — keeping it wider than one sensor unit avoids
// flicker on a flame/shadow edge. Defaults anchor to MIN_BRIGHTNESS so the
// night swap fires exactly when the NeoPixel floor-clamp starts distorting
// hues (the very regime the night palette exists to rescue). Devices
// without an ambient sensor never trip the Schmitt; night mode stays off.
#ifndef NIGHT_ENTER_BRIGHTNESS
#define NIGHT_ENTER_BRIGHTNESS (MIN_BRIGHTNESS)
#endif
#ifndef NIGHT_EXIT_BRIGHTNESS
#define NIGHT_EXIT_BRIGHTNESS  (MIN_BRIGHTNESS + 4)
#endif

// --- Memory / capacity overrides ---
// All optional. The defaults in the engine and HAL are tuned for a mid-range
// board (Photon 2). Override here to recover RAM on tighter platforms (OG
// Photon, see rico_raccoon.h) or to raise ceilings for more ambitious scripts.
//
// MAX_AGENTS — engine-wide cap across all agent types. Default 80, sized to
// ants.crit's `up to 80`. Agent struct is ~28B on-device, so each +1 is ~28B
// of .bss. Drop to recover memory if you know your scripts stay small; raise
// if you're authoring something denser than ants.
// #define MAX_AGENTS          80
//
// IR_OTA_BUFFER_BYTES — scratch for OTA-fetched IR blobs. Default 8192 in
// Stra2usClient.h. Largest body-only blob seen is ~3KB; the SOURCE trailer
// adds roughly another 1x. Drop on RAM-starved boards (rico is at 6144) at
// the cost of rejecting OTA of scripts with long source trailers. Raise if
// scripts grow past ~4KB.
// #define IR_OTA_BUFFER_BYTES 8192
//
// NO_IR_OTA — reclaim the full IR_OTA_BUFFER_BYTES (8192 default) as
// static RAM by dropping IR-OTA capability entirely. The device runs
// whatever script was compiled in at `make flash` time; remote IR swaps
// are disabled. Heartbeat + Stra2us config pull still work — only the
// ir_poll() fetch path is stubbed. Under this knob the heartbeat reports
// `script=default` (the ir_loaded_ptr_ fallback), which is how an
// operator spots flash-only devices in the fleet at a glance. Intended
// for P1-class / RAM-constrained specialist roles where the device's
// value is elsewhere (e.g. battery-backed RTC for blackout continuity)
// and OTA scripting isn't worth 8KB of BSS pressure on WICED. To go
// further and also drop heartbeat + config, omit STRA2US_HOST entirely
// from the build — but that loses operator visibility too.
// #define NO_IR_OTA
//
// TELEMETRY_STACK_BYTES — stack for the Stra2us telemetry thread. Default
// 5120. Lower if you've profiled the thread's peak usage and have headroom.
// #define TELEMETRY_STACK_BYTES 5120

// --- Debug ---
// Uncomment to override compiled-in defaults during bring-up. Live Stra2us
// KV overrides these at runtime once polled.
// #define HEARTBEEP_DEFAULT   15           // telemetry cadence in seconds (floor 10)

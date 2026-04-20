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

// --- Debug ---
// Uncomment to override compiled-in defaults during bring-up. Live Stra2us
// KV overrides these at runtime once polled.
// #define HEARTBEEP_DEFAULT   15           // telemetry cadence in seconds (floor 10)

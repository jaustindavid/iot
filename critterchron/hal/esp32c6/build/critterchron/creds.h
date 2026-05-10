#pragma once

// tommy_tanuki — headless ESP32-C6-DevKitC-1. First of the C6 critters,
// running as part of the FAILURE_TRIAGE.md §4 differential staging fleet.
// No LED strip, no light sensor; the engine + telemetry path runs as
// normal and the FastLEDSink emits to nothing.

// --- Identity ---
#define DEVICE_NAME         "tommy_tanuki"
#define DEVICE_PLATFORM     "esp32c6"

// --- WiFi credentials ---
#define WIFI_SSID           "turtlefarm"
#define WIFI_PASSWORD       "david fam"

// --- Stra2us creds (staging) ---
#define STRA2US_CLIENT_ID    DEVICE_NAME
#define STRA2US_SECRET_HEX  "2cb174a5ffee20a178971f91f0a11b041f78c4601fee33b02c2baba280916298"
#define STRA2US_HOST        "iot-staging.stra2us.austindavid.com"
#define STRA2US_PORT         8253
#define STRA2US_APP         "critterchron"

// --- Hardware geometry ---
// MATRIX_PIN is a plain GPIO. On ESP32-C6-DevKitC-1 the pins to avoid
// are different from C3 — strapping/USB-JTAG/UART moved:
//   GPIO4, GPIO5, GPIO8, GPIO9, GPIO15  — strapping pins (boot mode, debug)
//   GPIO12, GPIO13                      — native USB-Serial-JTAG D-/D+
//   GPIO16, GPIO17                      — default UART0 (serial console)
// GPIO10 is general-purpose, no strap/UART/USB conflict — matches the
// C3 convention so nothing has to think when the strip eventually does
// land. For now this device is headless so the line drives nothing.
#define GRID_WIDTH     32
#define GRID_HEIGHT    8
#define GRID_ROTATION  0                   // 0, 90, 180, 270
#define MATRIX_PIN     10                  // GPIO10 — safe GP pin on C6
#define PIXEL_TYPE     WS2812B

// --- Ambient light sensor ---
// Headless — no sensor wired. Leaving LIGHT_SENSOR_TYPE undefined makes
// the .ino skip the BH1750 init block (`#if defined(LIGHT_SENSOR_TYPE)`)
// so I2C never comes up.

// --- Failure-triage opt-in (FAILURE_TRIAGE.md §1) ---
// Staging-fleet member: ring buffer always-on at 32 frames. Override
// at runtime via the `snapshot_buffer_frames` KV if needed.
#define SNAPSHOT_BUFFER_FRAMES_DEFAULT 32

// --- Tuning ---
#define MAX_BRIGHTNESS         64           // moot when no strip is wired
#define MIN_BRIGHTNESS         1
#define TIMEZONE_OFFSET_HOURS -4.0f

// --- Night mode Schmitt thresholds ---
#ifndef NIGHT_ENTER_BRIGHTNESS
#define NIGHT_ENTER_BRIGHTNESS (MIN_BRIGHTNESS)
#endif
#ifndef NIGHT_EXIT_BRIGHTNESS
#define NIGHT_EXIT_BRIGHTNESS  (MIN_BRIGHTNESS + 4)
#endif

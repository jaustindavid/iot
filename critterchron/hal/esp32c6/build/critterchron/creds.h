#pragma once

// tammy_tanuki — second headless ESP32-C6-DevKitC-1, sibling of
// tommy_tanuki. Same role: FAILURE_TRIAGE.md §4 differential staging
// fleet member. No LED strip, no light sensor.

// --- Identity ---
#define DEVICE_NAME         "tammy_tanuki"
#define DEVICE_PLATFORM     "esp32c6"

// --- WiFi credentials ---
#define WIFI_SSID           "turtlefarm"
#define WIFI_PASSWORD       "david fam"

// --- Stra2us creds (staging) ---
// !!! PLACEHOLDER — NOT A REAL SECRET !!!
// Provision a real client_id+secret on the staging Stra2us instance
// and replace before flashing. The deadbeef pattern is intentionally
// obvious so it can't be mistaken for the real thing.
#define STRA2US_CLIENT_ID    DEVICE_NAME
#define STRA2US_SECRET_HEX  "57d45f2252f2841f344c452515139485bb1e990d43b7cfa7d72ffc2e9df5ec86"
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

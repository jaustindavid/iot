#pragma once

// --- Hardware Geometry ---
#define GRID_WIDTH  32
#define GRID_HEIGHT 8
#define GRID_ROTATION 0

// --- Hardware Configuration ---
// If your older devices wire matrix data to a GPIO pin instead of Hardware SPI DMA:
// #define MATRIX_PIN D0

// --- Stra2us Telemetry Configuration ---
// These are available from your Stra2us dashboard
#define STRA2US_HOST       "telemetry.yourdomain.com"
#define STRA2US_PORT       8153
#define STRA2US_CLIENT_ID  "your_device_id"
#define STRA2US_SECRET_HEX "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"

// --- App Identity ---
#define STRATUS_APP        "coaticlock"

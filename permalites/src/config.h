#pragma once
// Everything a human is likely to want to tune lives here.

// ---- hardware ----
#define NUM_LEDS        150
#define DATA_PIN        D0      // WS2812 / NeoPixel data
#define CDS_PIN         A1      // CdS photoresistor divider, 12-bit ADC (0..4095)
#define HEARTBEAT_PIN   D7      // onboard blue LED, toggles every 10 s so you can see loop() is alive

// ---- location: Charleston, SC ----
#define LATITUDE        32.7765
#define LONGITUDE      -79.9311
#define TZ_STD_OFFSET_H  (-5)   // US Eastern standard time; DST is applied automatically (calendar.h)

// ---- schedule ----
#define SUNSET_LEAD_MIN  30     // turn on this many minutes before sunset
#define SUNRISE_LAG_MIN   0     // turn off this many minutes after sunrise
#define SUN_ZENITH      ZENITH_OFFICIAL   // or ZENITH_CIVIL for twilight-based times

// ---- light-sensor fallback (only used when the clock is not valid) ----
// Lower ADC reading = darker. Hysteresis: turn on below CDS_DARK, off above CDS_LIGHT.
#define CDS_DARK        1500
#define CDS_LIGHT       3000

// ---- look ----
#define MAX_BRIGHTNESS   96     // 0..255. The old code's "override" level; it looked right on the porch.
#define FRAMES_PER_SECOND 120
#define WAVE_PERIOD_S    750    // seconds for the palette to drift one full lap of the string (old: 1 px / 5 s)
#define WAVE_REPEATS       1    // how many times the 4-color palette repeats across the string
#define BRIGHTNESS_STEP_MS 250  // fade speed at dusk/dawn: 1 brightness unit per this many ms (~24 s for 0->96)

// ---- cloud test overrides (auto-expire) ----
#define OVERRIDE_MINUTES  10

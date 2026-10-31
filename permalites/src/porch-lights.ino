/*
 * porch-lights: permanent, fully automatic seasonal LED string.
 *
 * ===== CONTEXT FOR FUTURE CLAUDE (or future Austin) =====
 *
 * Hardware: Particle Photon (gen 1, STM32F205, no FPU) driving a 150-pixel
 * WS2812 string on D0 via FastLED, mounted permanently on the front porch in
 * Charleston, SC. A CdS photoresistor divider is on A1. D7 is the onboard LED.
 * If the Photon dies the plan is a Photon 2 or an ESP32 (C3/S3); calendar.h
 * and sun.h are plain C++ for that reason, only this file and palettes.h
 * touch Particle/FastLED APIs.
 *
 * Intent (from the owner, Oct 2026): lights should be OFF during daylight and
 * show a slow, pleasant wave at night. The wave is so slow it is imperceptible
 * over a few seconds. Season differs ONLY by color palette. Everything is
 * automatic: season from the date, on/off from computed sunset/sunrise.
 * No manual state, no EEPROM.
 *
 * Design decisions:
 *  - SYSTEM_THREAD(ENABLED) so loop() keeps running when the cloud is down.
 *    The previous sketch lacked this and would freeze during reconnects.
 *  - Time comes from the Photon RTC, synced from the cloud. Local time, US
 *    DST, holidays and sun times are all computed on-device (calendar.h,
 *    sun.h) so nothing depends on the network after the first time sync.
 *  - The CdS sensor is only a fallback for when Time.isValid() is false
 *    (cold boot with no internet). It was never verified to work; see README.
 *  - Palette and brightness both move toward targets gradually, so a season
 *    rollover at midnight or dusk/dawn is a fade, not a jump.
 *  - Cloud function "command" exists only for testing and auto-expires:
 *      "season <0..8>"  preview a palette for OVERRIDE_MINUTES (indices in calendar.h)
 *      "on"             force lights on for OVERRIDE_MINUTES (test in daylight)
 *      "auto"           cancel overrides now
 *    Cloud variables: cds (live ADC), season (name), status (one-line summary).
 *
 * The previous sketch (ancestor of this one) had: per-season functions, a
 * "motion" state (drift/fast chaser/static), 0-5 brightness state in EEPROM,
 * fireworks/sparks/dumpster-fire effects, a hardcoded UTC-4, and a fixed
 * 17:00-01:00 window. All intentionally removed; git history or the owner's
 * backups have it if the chaser or fire effects are ever wanted again.
 */

#include "Particle.h"

#define FASTLED_ALLOW_INTERRUPTS 0
#include <FastLED.h>
FASTLED_USING_NAMESPACE;

#include "config.h"
#include "calendar.h"
#include "sun.h"
#include "palettes.h"

SYSTEM_MODE(AUTOMATIC);
SYSTEM_THREAD(ENABLED);

CRGB leds[NUM_LEDS];

CRGBPalette16 currentPalette;
CRGBPalette16 targetPalette;
uint8_t currentBrightness = 0;
uint8_t targetBrightness  = 0;

// Cloud-visible state. Keep these as simple types; Particle.variable needs
// the storage to live for the life of the program.
int    cdsValue = 0;
String seasonStr = "boot";
String statusStr = "boot";

// Test overrides, 0 = none. Compared against millis().
unsigned long seasonOverrideUntil = 0;
int           seasonOverride      = -1;
unsigned long forceOnUntil        = 0;

// CdS hysteresis state for the no-clock fallback.
bool cdsSaysDark = false;

ApplicationWatchdog *watchdog = nullptr;

// -------------------------------------------------------------------------

static void fmtHM(char *out, size_t n, int minuteOfDay) {
    snprintf(out, n, "%02d:%02d", minuteOfDay / 60, minuteOfDay % 60);
}

// Is `now` inside the window [on, off) on a circular 1440-minute clock?
static bool inNightWindow(int now, int on, int off) {
    if (on < off) return now >= on && now < off;   // window does not cross midnight
    return now >= on || now < off;                 // window crosses midnight
}

// Decide season + brightness target. Called every few seconds, not per frame.
static void updateSchedule() {
    cdsValue = analogRead(CDS_PIN);
    // Hysteresis so a cloudy dusk does not flicker the string.
    if (cdsValue < CDS_DARK)  cdsSaysDark = true;
    if (cdsValue > CDS_LIGHT) cdsSaysDark = false;

    const unsigned long nowMs = millis();
    if (seasonOverrideUntil && (long)(nowMs - seasonOverrideUntil) >= 0) { seasonOverrideUntil = 0; seasonOverride = -1; }
    if (forceOnUntil        && (long)(nowMs - forceOnUntil)        >= 0) { forceOnUntil = 0; }

    int season;
    bool night;
    char buf[160];

    if (!Time.isValid()) {
        // Cold boot, no time sync yet. Fall back to the light sensor and the
        // default palette. This path should be rare on a powered porch.
        season = SEASON_DEFAULT;
        night  = cdsSaysDark;
        snprintf(buf, sizeof buf, "NO CLOCK | cds %d -> %s", cdsValue, night ? "night" : "day");
    } else {
        const int64_t utc = (int64_t)Time.now();
        const int32_t off = localOffsetSec(utc, TZ_STD_OFFSET_H * 3600);
        const Civil loc   = civilFromEpoch(utc + off);
        const int nowMin  = loc.hour * 60 + loc.minute;

        season = seasonForDate(loc.year, loc.month, loc.day);

        int riseMin, setMin;
        if (sunTimesLocalMin(loc.year, loc.month, loc.day, LATITUDE, LONGITUDE, off, SUN_ZENITH, riseMin, setMin)) {
            const int onMin  = (setMin - SUNSET_LEAD_MIN + 1440) % 1440;
            const int offMin = (riseMin + SUNRISE_LAG_MIN) % 1440;
            night = inNightWindow(nowMin, onMin, offMin);
            char r[8], s[8], n[8];
            fmtHM(r, sizeof r, riseMin); fmtHM(s, sizeof s, setMin); fmtHM(n, sizeof n, nowMin);
            snprintf(buf, sizeof buf, "%04d-%02d-%02d %s %s | rise %s set %s | %s | cds %d",
                     loc.year, loc.month, loc.day, n, usDstActive(utc, TZ_STD_OFFSET_H * 3600) ? "EDT" : "EST",
                     r, s, night ? "night" : "day", cdsValue);
        } else {
            // Cannot happen at this latitude, but keep the lights sane anyway.
            night = cdsSaysDark;
            snprintf(buf, sizeof buf, "no sun event?! cds %d", cdsValue);
        }
    }

    if (seasonOverride >= 0) season = seasonOverride;
    if (forceOnUntil) night = true;

    targetPalette    = paletteFor(season);
    targetBrightness = night ? MAX_BRIGHTNESS : 0;

    seasonStr = seasonName(season);
    statusStr = String(buf);
    if (seasonOverride >= 0) statusStr += " [season override]";
    if (forceOnUntil)        statusStr += " [forced on]";
}

// Slow drift of the palette along the string. Offset is derived from millis()
// in 16-bit palette units so sub-pixel motion is smooth, unlike the old
// 1-pixel-every-5-seconds step.
static void paintWave() {
    const uint16_t offset = (uint16_t)((((uint64_t)millis()) << 16) / ((uint64_t)WAVE_PERIOD_S * 1000ULL));
    for (int i = 0; i < NUM_LEDS; i++) {
        const uint16_t idx16 = (uint16_t)(((uint32_t)i * 65536UL * WAVE_REPEATS) / NUM_LEDS) + offset;
        leds[i] = ColorFromPalette(currentPalette, idx16 >> 8, 255, LINEARBLEND);
    }
}

// -------------------------------------------------------------------------

int command(String cmd) {
    cmd.trim();
    Log.info("command: %s", cmd.c_str());
    if (cmd == "auto") {
        seasonOverride = -1; seasonOverrideUntil = 0; forceOnUntil = 0;
        updateSchedule();
        return 0;
    }
    if (cmd == "on") {
        forceOnUntil = millis() + OVERRIDE_MINUTES * 60000UL;
        if (forceOnUntil == 0) forceOnUntil = 1;
        updateSchedule();
        return 1;
    }
    if (cmd.startsWith("season")) {
        int s = cmd.substring(6).toInt();
        if (s < 0 || s >= SEASON_COUNT) return -2;
        seasonOverride = s;
        seasonOverrideUntil = millis() + OVERRIDE_MINUTES * 60000UL;
        if (seasonOverrideUntil == 0) seasonOverrideUntil = 1;
        updateSchedule();
        return s;
    }
    return -1;
}

// -------------------------------------------------------------------------

SerialLogHandler logHandler(LOG_LEVEL_INFO);

void setup() {
    pinMode(HEARTBEAT_PIN, OUTPUT);
    pinMode(CDS_PIN, INPUT);

    // We do our own local-time math; keep Particle's Time in UTC.
    Time.zone(0);

    FastLED.addLeds<NEOPIXEL, DATA_PIN>(leds, NUM_LEDS);
    FastLED.setBrightness(0);
    FastLED.clear(true);

    currentPalette = targetPalette = paletteFor(SEASON_DEFAULT);

    Particle.variable("cds", cdsValue);
    Particle.variable("season", seasonStr);
    Particle.variable("status", statusStr);
    Particle.function("command", command);

    // Reset if loop() stalls for a minute. Cheap insurance on a permanent install.
    watchdog = new ApplicationWatchdog(60000, System.reset, 1536);

    updateSchedule();
    Log.info("porch-lights up: %s", statusStr.c_str());
}

void loop() {
    ApplicationWatchdog::checkin();

    paintWave();
    FastLED.show();
    FastLED.delay(1000 / FRAMES_PER_SECOND);

    // Ease toward the target palette so a season rollover is a crossfade.
    EVERY_N_MILLISECONDS(40) {
        nblendPaletteTowardPalette(currentPalette, targetPalette, 4);
    }

    // Ease brightness so dusk/dawn is a fade.
    EVERY_N_MILLISECONDS(BRIGHTNESS_STEP_MS) {
        if (currentBrightness < targetBrightness)      currentBrightness++;
        else if (currentBrightness > targetBrightness) currentBrightness--;
        FastLED.setBrightness(currentBrightness);
    }

    EVERY_N_SECONDS(10) {
        updateSchedule();
        digitalWrite(HEARTBEAT_PIN, !digitalRead(HEARTBEAT_PIN));
    }

    EVERY_N_SECONDS(60) {
        Log.info("%s | %s | bright %d->%d", seasonStr.c_str(), statusStr.c_str(), currentBrightness, targetBrightness);
    }

    EVERY_N_SECONDS(3600) {
        if (Particle.connected()) Particle.syncTime();
    }
}

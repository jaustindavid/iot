//
// critterchron_esp32.ino — Arduino-ESP32 entry point.
//
// Milestone ladder:
//   M1         — FastLEDSink + MockTimeSource + static IR blob (offline)
//   M2         — WiFi STA + SNTP + EspTimeSource
//   M3         — Stra2usClient port + heartbeat task + live KV
//   M3.5       — WobblyTimeSource decorator
//   M5         — ArduinoOTA push-flash + rescue hold
//              — BH1750 light sensor + EMA brightness + night Schmitt
//   M8         — auto-rollback on bad flash (esp_ota_mark_app_valid)
//   M4 (here)  — OTA IR pull (ir_poll / ir_pending / ota_loading
//                streamer + ota_detected/matrix/loaded lifecycle)
//                [landed after M5 so devices can be uncabled during
//                 IR-pipeline development]
//
// Compile-time feature gates (all driven by hal/devices/<device>.h):
//   WIFI_SSID + WIFI_PASSWORD → WiFi STA + SNTP + EspTimeSource, else MockTimeSource
//                               Also enables ArduinoOTA push-flash listener.
//   STRA2US_HOST              → Stra2usClient (live KV + heartbeat task), else StaticConfig
//   LIGHT_SENSOR_TYPE=BH1750  → I2C lux sensor drives brightness + night Schmitt
//
// The gates compose independently enough: leaving both undefined is a
// purely-offline M1 build on a fresh board; defining only WiFi is M2
// (with OTA); defining both is M3. Each gate keeps the unused subsystem
// out of flash + RAM entirely, matching the Particle shim's model.
//

#if defined(ARDUINO_ARCH_ESP32)

// Include paths: creds.h sits at the sketch root; everything else lives
// under <sketch>/src/ per arduino-cli's "only src/ subdirectories get
// recursively compiled" rule (see hal/esp32c3/Makefile for the layout
// rationale). The `src/` prefix is the explicit spelling that doesn't
// depend on whether arduino-cli has added src/ to the quoted-include
// search path — works either way.
#include <Arduino.h>
#include "creds.h"
#include "src/CritterEngine.h"
#include "src/ErrLog.h"
#include "src/interface/Config.h"
#include "src/FastLEDSink.h"

#if defined(WIFI_SSID) && defined(WIFI_PASSWORD)
#define CRIT_HAVE_WIFI 1
#include <WiFi.h>
#include <time.h>
#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <esp_ota_ops.h>          // M8: partition-state inspection + mark-valid
#include "src/EspTimeSource.h"
#else
#define CRIT_HAVE_WIFI 0
#include "src/MockTimeSource.h"
#endif

// WobblyTimeSource is a decorator over whichever base clock we selected
// above — reads a Config for its drift bounds and tick-rate swings, so
// it's always on (works against StaticConfig defaults when Stra2us isn't
// compiled in, picks up live overrides when it is).
#include "src/WobblyTimeSource.h"

#if defined(STRA2US_HOST)
#if !CRIT_HAVE_WIFI
#error "STRA2US_HOST requires WIFI_SSID + WIFI_PASSWORD — Stra2us has no offline path"
#endif
#define CRIT_HAVE_STRA2US 1
#include "src/Stra2usClient.h"
#else
#define CRIT_HAVE_STRA2US 0
#endif

// Light sensor is opt-in per device header. Only BH1750 is supported on
// ESP32 today — the CDS path (analog ADC + divider) lives on Particle
// hardware where the ADC pins are the native interface. Any value of
// LIGHT_SENSOR_TYPE in the device header compiles the BH1750 driver;
// the intent is that the symbol's *presence* means "a sensor is wired"
// and the current ESP32 HAL only knows one way to talk to sensors. Add
// a dispatch here if a second sensor type ever shows up on ESP32.
#if defined(LIGHT_SENSOR_TYPE)
#define CRIT_HAVE_LIGHT 1
#include <Wire.h>
#include "src/LightSensorBH1750.h"
#ifndef LIGHT_SENSOR_ADDR
#define LIGHT_SENSOR_ADDR 0x23
#endif
#else
#define CRIT_HAVE_LIGHT 0
#endif

#if !defined(GRID_WIDTH) || !defined(GRID_HEIGHT)
#error "creds.h must define GRID_WIDTH and GRID_HEIGHT"
#endif

#define APP_VERSION __DATE__ " " __TIME__

static inline uint32_t physics_tick_ms() { return critter_ir::RUNTIME_TICK_MS; }

#ifndef RENDER_TICK_MS
#define RENDER_TICK_MS 20
#endif

#ifndef MAX_BRIGHTNESS
#define MAX_BRIGHTNESS 64
#endif
#ifndef MIN_BRIGHTNESS
#define MIN_BRIGHTNESS 1
#endif
#ifndef NIGHT_ENTER_BRIGHTNESS
#define NIGHT_ENTER_BRIGHTNESS (MIN_BRIGHTNESS)
#endif
#ifndef NIGHT_EXIT_BRIGHTNESS
#define NIGHT_EXIT_BRIGHTNESS  (MIN_BRIGHTNESS + 4)
#endif
#ifndef TIMEZONE_OFFSET_HOURS
#define TIMEZONE_OFFSET_HOURS -5.0f
#endif

#ifndef SNTP_SERVER_1
#define SNTP_SERVER_1 "pool.ntp.org"
#endif
#ifndef SNTP_SERVER_2
#define SNTP_SERVER_2 "time.google.com"
#endif
#ifndef SNTP_SERVER_3
#define SNTP_SERVER_3 "time.cloudflare.com"
#endif

// Heartbeat cadence — default when Stra2us has no live override yet.
// Mirrors the Particle shim. Floored at 10s in the task loop.
#ifndef HEARTBEEP_DEFAULT
#define HEARTBEEP_DEFAULT 300
#endif

// OTA IR poll cadence (seconds). Decoupled from the heartbeat: pointer
// changes are human-scale events and running ir_poll() every heartbeat
// would add 1-2 HTTP round trips to the hot path. 1200s = 20min. Floored
// at 60s in the task loop. Mirrors the Particle shim.
#ifndef IR_POLL_INTERVAL_DEFAULT
#define IR_POLL_INTERVAL_DEFAULT 1200
#endif

// Deliberate user-visible hold between "blob staged" and "engine swaps
// to it". The parse + reinit cost is <10ms on a C3 — the delay exists so
// an operator watching the grid after a publish sees a distinct
// "incoming" cue (green vertical streamers) before the new script takes
// over. Matches the Particle shim's OTA_LOADING_MS exactly so both
// platforms feel the same on a push.
#ifndef OTA_LOADING_MS
#define OTA_LOADING_MS 5000
#endif

// FreeRTOS task stack for the telemetry worker. Must cover: WiFiClient
// TLS-adjacent buffers (none in M3 — plain TCP), snprintf frames, HMAC
// contexts, msgpack parse. 8KB is generous; ESP32-C3 has 400KB SRAM so
// stack budget is not tight. Revisit (downward) if/when OTA IR on C3
// wants more headroom for ir_ota_buf_ on the task stack rather than
// globals — currently it's a member so it's in .bss, task unaffected.
#ifndef TELEMETRY_STACK_BYTES
#define TELEMETRY_STACK_BYTES 8192
#endif

// ---- Wiring ----

#if CRIT_HAVE_STRA2US
// Live-tunable KV + heartbeat publisher. Consumers reach it through the
// `Config` abstract — same contract as the Particle shim, so code that
// takes `const Config&` (WobblyTimeSource, LightSensor when they land)
// won't care which backend it got.
static Stra2usClient g_cfg(STRA2US_HOST, STRA2US_PORT,
                           STRA2US_CLIENT_ID, STRA2US_SECRET_HEX,
                           STRA2US_APP, DEVICE_NAME);
#else
static StaticConfig  g_cfg;
#endif

static FastLEDSink          g_sink;
#if CRIT_HAVE_LIGHT
// Ambient light driver. Holds a reference to g_cfg for live-tunable
// knobs (light_lux_full, light_exponent). Pure math; shim owns the
// sample cadence + EMA smoothing below.
static LightSensorBH1750    g_light(g_cfg, LIGHT_SENSOR_ADDR);
#endif
// Base clock: real SNTP-synced time if WiFi compiled in, else a fixed
// mock. WobblyTimeSource wraps whichever we picked — the engine and
// the local_minute helper read `g_clock`, which is always the wobbled
// decorator. Construction order matters (g_cfg above, g_real_clock,
// then g_clock) — WobblyTimeSource holds references to both and the
// compiler initializes globals top-to-bottom within this TU.
#if CRIT_HAVE_WIFI
static EspTimeSource        g_real_clock(TIMEZONE_OFFSET_HOURS);
#else
static MockTimeSource       g_real_clock(1776844800, TIMEZONE_OFFSET_HOURS);
#endif
static WobblyTimeSource     g_clock(g_real_clock, g_cfg);
static critterchron::CritterEngine g_engine(g_sink, g_clock);

static unsigned long last_physics_tick = 0;
static unsigned long last_render_tick  = 0;
static int           last_sync_minute  = -1;

// ---- OTA + rescue state ----

// Rescue hold: if the previous boot was a crash (panic, task-WDT, int-WDT,
// brownout), hold the engine off for RESCUE_HOLD_MS so a replacement
// firmware can be pushed without a physical reset. ArduinoOTA.handle()
// runs throughout, so the listener is live the whole window. Clean
// resets (power, software reset, deepsleep wake) start immediately.
// Mirrors the Particle shim's `is_crash_reset` gate.
#ifndef RESCUE_HOLD_MS
#define RESCUE_HOLD_MS 60000
#endif
static bool          g_rescue_mode       = false;
static unsigned long g_rescue_start_ms   = 0;

#if CRIT_HAVE_WIFI
// M8: auto-rollback. The stock arduino-esp32 C3 bootloader is built with
// CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y (verified in
// ~/Library/Arduino15/.../esp32c3-libs/3.3.8/sdkconfig). When an OTA-
// flashed image boots for the first time its partition state is
// ESP_OTA_IMG_PENDING_VERIFY — the bootloader watches for
// esp_ota_mark_app_valid_cancel_rollback() and if the app doesn't call
// it before the NEXT reset, the bootloader reverts to the previous
// partition. So "auto-rollback" is just: don't mark valid too early.
//
// Criteria for "this image works":
//   - Clean boot (not in rescue hold)
//   - Clock is valid (WiFi associated + SNTP synced)
//   - OTA_VALID_BUDGET_MS of post-clock-valid runtime without crashing
//
// If we crash before the budget elapses, reset_reason on the NEXT boot
// is still PANIC/WDT/etc. and rescue mode kicks in — but now on the
// ROLLED-BACK image (previous good fw). This is the desired end state:
// the bad fw is reverted automatically, and the rescue-mode amber
// chase flags the situation to any operator who happens to see it.
#ifndef OTA_VALID_BUDGET_MS
#define OTA_VALID_BUDGET_MS 60000   // 60s of post-SNTP runtime before commit
#endif
static bool          g_ota_marked_valid      = false;
static unsigned long g_ota_valid_deadline_ms = 0;  // 0 = not yet armed

static void ota_log_partition_state() {
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (!running) {
        Serial.println("[ota] no running partition?");
        return;
    }
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    esp_err_t err = esp_ota_get_state_partition(running, &state);
    const char* s = "?";
    if (err == ESP_OK) {
        switch (state) {
            case ESP_OTA_IMG_NEW:            s = "NEW";            break;
            case ESP_OTA_IMG_PENDING_VERIFY: s = "PENDING_VERIFY"; break;
            case ESP_OTA_IMG_VALID:          s = "VALID";          break;
            case ESP_OTA_IMG_INVALID:        s = "INVALID";        break;
            case ESP_OTA_IMG_ABORTED:        s = "ABORTED";        break;
            case ESP_OTA_IMG_UNDEFINED:      s = "UNDEFINED";      break;
            default:                         s = "?";              break;
        }
    }
    Serial.printf("[ota] partition=%s state=%s rollback_possible=%d\n",
                  running->label, s,
                  esp_ota_check_rollback_is_possible() ? 1 : 0);
}
#endif  // CRIT_HAVE_WIFI

#if CRIT_HAVE_WIFI
// OTA-in-progress gate. onStart sets it true; onEnd/onError clear it.
// While true, loop() skips everything (physics, render, tel-task
// check) — the progress bar is drawn from onProgress. Rationale: ~1MB
// firmware flash takes ~30s; we don't want the engine's RMT transfers
// fighting flash I/O or the tick pile-up on the other side.
static volatile bool g_ota_active         = false;
static unsigned int  g_ota_last_bar_cols  = 0;

// Progress bar on the grid while flashing. Centered 2-row band, pure
// green, fills left-to-right proportional to bytes written. Distinct
// from the blue spinner and amber rescue chase so at-a-glance state is
// unambiguous. Re-renders only when the column count changes — a 32-
// column bar at 32x8 moves a column every ~32KB, so this caps draws at
// ~32 over a 1MB flash and leaves flash I/O unblocked the rest of the
// time.
static void draw_ota_progress(unsigned int progress, unsigned int total) {
    if (total == 0) return;
    unsigned int cols = (progress * GRID_WIDTH) / total;
    if (cols > (unsigned)GRID_WIDTH) cols = GRID_WIDTH;
    if (cols == g_ota_last_bar_cols) return;
    g_ota_last_bar_cols = cols;

    g_sink.clear();
    int y0 = GRID_HEIGHT / 2 - 1;
    int y1 = GRID_HEIGHT / 2;
    for (unsigned int x = 0; x < cols; ++x) {
        g_sink.set((int)x, y0, 0, 48, 0);
        g_sink.set((int)x, y1, 0, 48, 0);
    }
    g_sink.show();
}
#endif  // CRIT_HAVE_WIFI

// Rescue-mode indicator: amber chase across row 0, matches the Particle
// shim's draw_rescue so an operator watching any platform's rescue hold
// sees the same visual language.
static void draw_rescue(unsigned long now) {
    g_sink.clear();
    int n = GRID_WIDTH;
    int phase = (now / 80) % n;
    for (int i = 0; i < 3; ++i) {
        int x = (phase + i) % n;
        uint8_t v = 64 - i * 20;
        g_sink.set(x, 0, v, v / 3, 0);
    }
    g_sink.show();
}

// Classify reset reason into crash / clean. Keep the list explicit so a
// future ESP-IDF adding a new reason defaults to "clean, run normally"
// rather than silently triggering a rescue hold.
static bool is_crash_reset(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_PANIC:       // unhandled fault / assert
        case ESP_RST_INT_WDT:     // interrupt watchdog
        case ESP_RST_TASK_WDT:    // task watchdog
        case ESP_RST_WDT:         // other/legacy watchdog
        case ESP_RST_BROWNOUT:    // VCC sagged below the brownout detector
            return true;
        default:
            return false;  // POWERON, SW, DEEPSLEEP, EXT, SDIO, USB, JTAG, ...
    }
}

#if CRIT_HAVE_STRA2US
// Task handle so we don't spawn twice. Nulled at boot; set once SNTP is
// valid and the task is launched.
static TaskHandle_t g_tel_task = nullptr;

// OTA IR lifecycle publish handoff (main thread → tel task) for the
// `ota_matrix` and `ota_loaded` events. The `ota_detected` event is
// handled symmetrically but its snapshot lives on Stra2usClient
// (ir_detected_*) because ir_poll itself captures it on the tel task.
//
// Two independent buffer pairs because matrix (entered loading window)
// and loaded (engine reinit done) can both be pending when tel drains
// — the main thread fires both within a ~5s window and a transient TCP
// failure can delay tel's first drain past the second flag-set. One
// shared buffer would race; two buffers keep each event's payload
// intact regardless of drain order. Volatile-flag-last ordering mirrors
// the has_live / cache_count fence pattern elsewhere in this TU.
static volatile bool g_ota_pub_matrix = false;
static volatile bool g_ota_pub_loaded = false;
static char          g_ota_matrix_name[IR_SCRIPT_NAME_MAX] = {0};
static char          g_ota_matrix_sha [65]                 = {0};
static char          g_ota_loaded_name[IR_SCRIPT_NAME_MAX] = {0};
static char          g_ota_loaded_sha [65]                 = {0};

// Telemetry-visible snapshots. Written by loop(), read by the task via
// atomic 32-bit reads — same coherency model as the Particle shim's
// volatile globals, valid on both RISC-V (C3) and Xtensa (S3) aligned
// 32-bit stores.
static volatile uint8_t g_bri     = MAX_BRIGHTNESS;
static volatile uint8_t g_bri_min = MIN_BRIGHTNESS;
static volatile uint8_t g_bri_max = MAX_BRIGHTNESS;
#if CRIT_HAVE_LIGHT
// Latest lux reading, captured by the sample loop and reported on the
// next heartbeat. Written exactly once per 200ms sample, read once per
// heartbeat — race-safe on 32-bit aligned float stores.
static volatile float   g_lux     = 0.0f;
#endif

// Engine-timing snapshots — same layout as the Particle shim. Accumulators
// live inside loop() (static locals); once per second the diag rollup
// closes the window, publishes avg/max to these volatiles, and resets.
// Tel task reads the last closed window so a publish never sees an in-
// progress accumulator.
//
// phys   = whole-tick wall time (tick body including syncTime + housekeeping)
// rend   = render() call only
// interp = opcode dispatch inside processAgent (engine-reported)
// astar  = pathfinding inside processAgent (engine-reported)
// interp + astar <= phys; the gap is spawn + compaction + convergence.
static volatile uint32_t g_phys_avg_us   = 0;
static volatile uint32_t g_phys_max_us   = 0;
static volatile uint32_t g_rend_avg_us   = 0;
static volatile uint32_t g_rend_max_us   = 0;
static volatile uint32_t g_interp_avg_us = 0;
static volatile uint32_t g_interp_max_us = 0;
static volatile uint32_t g_astar_avg_us  = 0;
static volatile uint32_t g_astar_max_us  = 0;
#endif

static int local_minute(const CritTimeSource& c) {
    time_t local = c.wall_now() + (time_t)(c.zone_offset_hours() * 3600.0f);
    struct tm tm;
    gmtime_r(&local, &tm);
    return tm.tm_min;
}

static void draw_spinner(unsigned long now) {
    g_sink.clear();
    int cx = GRID_WIDTH / 2;
    int cy = GRID_HEIGHT / 2 - 1;
    int phase = (now / 150) % 8;
    static const int DX[8] = { 0,  1, 1, 1, 0, -1, -1, -1};
    static const int DY[8] = {-1, -1, 0, 1, 1,  1,  0, -1};
    auto pix = [&](int x, int y, uint8_t r, uint8_t g, uint8_t b) {
        if (x < 0 || x >= GRID_WIDTH || y < 0 || y >= GRID_HEIGHT) return;
        g_sink.set(x, y, r, g, b);
    };
    pix(cx + DX[phase], cy + DY[phase], 0, 32, 64);
    pix(cx, cy, 0, 16, 32);
    g_sink.show();
}

// OTA IR loading streamer — drawn directly into the sink while the
// main loop holds off applying a staged IR blob. Visual: Matrix-rain,
// 4-px green tails flowing top-to-bottom in sparse columns. Chosen to
// be clearly distinct from the pre-SNTP spinner (full-grid vs.
// centered, green vs. blue, downward vs. orbit) and from the rescue
// chase (green vs. amber) so at-a-glance state is unambiguous across
// boot / wait / rescue / OTA-script / OTA-flash. Ported from the
// Particle shim's draw_ota_streamer — same constants so both platforms
// render the same shape.
static void draw_ota_streamer(unsigned long now) {
    g_sink.clear();
    // One streamer every 4 cols, starting at col 1 so the leftmost
    // column stays dark — pattern reads as "sparse" at a glance even
    // on narrow grids. 32x8 timmy → 8 streamers.
    constexpr int COL_STEP = 4;
    constexpr int TAIL_LEN = 4;       // pixels
    constexpr int STEP_MS  = 120;     // head advances 1 row per 120ms
    // Cycle > GRID_HEIGHT so each streamer has a dark gap between its
    // tail and the next head.
    int cycle = GRID_HEIGHT + TAIL_LEN + 2;
    unsigned long base_phase = now / STEP_MS;
    for (int col = 1; col < GRID_WIDTH; col += COL_STEP) {
        // Per-column phase offset so streamers don't march in lockstep.
        int col_phase = (col * 7) % cycle;
        int head_y    = (int)((base_phase + col_phase) % cycle);
        for (int t = 0; t < TAIL_LEN; ++t) {
            int y = head_y - t;
            if (y < 0 || y >= GRID_HEIGHT) continue;
            // Head bright, tail fading. 48 at head leaves headroom for
            // the sink's brightness multiplier; pure green so it can't
            // be confused with any script's intended palette.
            uint8_t g = (uint8_t)(48 - t * 12);
            g_sink.set(col, y, 0, g, 0);
        }
    }
    g_sink.show();
}

#if CRIT_HAVE_STRA2US
// Heartbeat + KV poll, one iteration. Returns the HTTP publish status so
// the caller can drive the retry cadence. Mirrors the Particle shim's
// telemetry_cycle — simplified for M3: no light-sensor diagnostic, no
// engine timing metrics yet, no cloud-failsafe heartbeat (ESP32 has no
// Particle-cloud equivalent — Stra2us IS the telemetry path).
static int telemetry_cycle() {
    if (!g_clock.valid()) return 0;  // punted before send

    // Payload is grep-friendly k=v tokens, same schema as the Particle
    // shim so the server-side parser stays device-agnostic.
    // Budget values in µs. Tick budget is the physics tick interval (past
    // that we're falling behind); render budget is the frame interval.
    // Printed alongside the observed avg/max so a glance tells you how
    // close to saturation the hot loop is running.
    const uint32_t PHYS_BUDGET_US = physics_tick_ms() * 1000UL;
    const uint32_t REND_BUDGET_US = (uint32_t)RENDER_TICK_MS * 1000UL;
    const auto& m = g_engine.metrics();

    // `script=<name>@<sha8>` so the server can tell at a glance which
    // IR is running. Truncated to 8 hex of the content_sha — name is
    // rarely unique after a rename so name+sha beats either alone, and
    // 8 hex at ~16-bit collision odds is fine at our swarm size.
    // Falls back to "default" when no OTA blob has been loaded yet
    // (compiled-in IR is running). Matches the Particle shim exactly.
    const char* script = g_cfg.ir_loaded_script();
    const char* sha    = g_cfg.ir_loaded_sha();
    char script_tag[IR_SCRIPT_NAME_MAX + 16];
    if (script && *script && sha && *sha) {
        snprintf(script_tag, sizeof(script_tag), "%.*s@%.8s",
                 (int)IR_SCRIPT_NAME_MAX, script, sha);
    } else {
        snprintf(script_tag, sizeof(script_tag), "default");
    }

    // Report buffer bumped to 384 to hold the full parity-with-Particle
    // field set. Current typical length is ~280 chars with script= in
    // the mix; 384 leaves slack for future additions without another
    // resize.
    char report[384];
    int  rssi = WiFi.isConnected() ? WiFi.RSSI() : -127;

#if CRIT_HAVE_LIGHT
    // `lux=X.X` is the BH1750 raw reading; bri=(min<cur<max) is the
    // mapped-and-smoothed output the sink is actually using. Together
    // they answer "is the room light what we think it is" (lux) and "is
    // the curve+clamp producing sensible brightness" (bri triplet).
    int rlen = snprintf(report, sizeof(report),
        "up=%lu rssi=%d heap=%lu rst=%d fw=%s script=%s bri=(%u<%u<%u) lux=%.1f "
        "phys=(%lu<%lu<%lu)us rend=(%lu<%lu<%lu)us "
        "interp=(%lu<%lu)us astar=(%lu<%lu)us "
        "agents=%u seeks_fail=%lu chip=%s",
        (unsigned long)(millis() / 1000),
        rssi,
        (unsigned long)ESP.getFreeHeap(),
        (int)esp_reset_reason(),
        APP_VERSION,
        script_tag,
        (unsigned)g_bri_min, (unsigned)g_bri, (unsigned)g_bri_max,
        (double)g_lux,
        (unsigned long)g_phys_avg_us,   (unsigned long)g_phys_max_us,   (unsigned long)PHYS_BUDGET_US,
        (unsigned long)g_rend_avg_us,   (unsigned long)g_rend_max_us,   (unsigned long)REND_BUDGET_US,
        (unsigned long)g_interp_avg_us, (unsigned long)g_interp_max_us,
        (unsigned long)g_astar_avg_us,  (unsigned long)g_astar_max_us,
        (unsigned)g_engine.liveAgentCount(),
        (unsigned long)m.failed_seeks,
        CONFIG_IDF_TARGET);
#else
    int rlen = snprintf(report, sizeof(report),
        "up=%lu rssi=%d heap=%lu rst=%d fw=%s script=%s bri=(%u<%u<%u) "
        "phys=(%lu<%lu<%lu)us rend=(%lu<%lu<%lu)us "
        "interp=(%lu<%lu)us astar=(%lu<%lu)us "
        "agents=%u seeks_fail=%lu chip=%s",
        (unsigned long)(millis() / 1000),
        rssi,
        (unsigned long)ESP.getFreeHeap(),
        (int)esp_reset_reason(),
        APP_VERSION,
        script_tag,
        (unsigned)g_bri_min, (unsigned)g_bri, (unsigned)g_bri_max,
        (unsigned long)g_phys_avg_us,   (unsigned long)g_phys_max_us,   (unsigned long)PHYS_BUDGET_US,
        (unsigned long)g_rend_avg_us,   (unsigned long)g_rend_max_us,   (unsigned long)REND_BUDGET_US,
        (unsigned long)g_interp_avg_us, (unsigned long)g_interp_max_us,
        (unsigned long)g_astar_avg_us,  (unsigned long)g_astar_max_us,
        (unsigned)g_engine.liveAgentCount(),
        (unsigned long)m.failed_seeks,
        CONFIG_IDF_TARGET);
#endif
    if (rlen >= (int)sizeof(report)) report[sizeof(report)-1] = '\0';

    // Error-channel drain. One entry per heartbeat (ring is 4 deep).
    // Mirror of telemetry_cycle() in hal/particle/src/critterchron_particle.cpp;
    // mark_sent only on successful publish so a transient network failure
    // requeues the entry for next cycle. See hal/ErrLog.h.
    critterchron::ErrEntry pending_err;
    bool have_err = critterchron::g_errlog.peek_oldest_unsent(pending_err);
    if (have_err && rlen > 0 && rlen < (int)sizeof(report) - 8) {
        int extra = snprintf(report + rlen, sizeof(report) - rlen,
                             " err=%s:%s",
                             critterchron::err_cat_tag(pending_err.cat),
                             pending_err.msg);
        if (extra > 0 && rlen + extra < (int)sizeof(report)) {
            rlen += extra;
        } else {
            report[sizeof(report) - 1] = '\0';
            have_err = false;
        }
    }

    g_cfg.connect();
    int pub_status = g_cfg.publish(STRA2US_APP, report);
    Serial.printf("[tel] publish=%d %s\n", pub_status, report);
    if (have_err && pub_status == 200) {
        critterchron::g_errlog.mark_sent(pending_err.seq);
    }
    g_cfg.poll_all();
    g_cfg.close();

#if !CRIT_HAVE_LIGHT
    // Pull live max_brightness so a remote tune reaches the sink even
    // without a light sensor in the pipeline. Clamp [0,255]; store
    // snapshots for the heartbeat payload. When a sensor IS wired, the
    // sample loop in loop() owns set_brightness/g_bri/g_bri_max — this
    // telemetry path stays out of its way so the two drivers don't fight.
    int max_b = g_cfg.get_int("max_brightness", MAX_BRIGHTNESS);
    if (max_b < 0)   max_b = 0;
    if (max_b > 255) max_b = 255;
    g_bri_max = (uint8_t)max_b;
    g_sink.set_brightness(g_bri_max);
    g_bri = g_bri_max;
#endif

    return pub_status;
}

// FreeRTOS task body. Never returns. Retry policy matches the Particle
// shim: 2xx/4xx = full cadence; 5xx / TCP / no-time = exponential
// backoff from 10s doubling up to hb. Task spawns only after SNTP is
// valid, so we know WiFi is associated + time is set.
static void telemetry_task(void*) {
    const uint32_t retry_floor_ms = 10000;
    uint32_t next_interval_ms     = 0;            // 0 = fire asap
    uint32_t last_attempt_ms      = 0;
    uint32_t backoff_ms           = retry_floor_ms;
    bool     first                = true;

    // OTA IR poll timer. Separate from the heartbeat cadence because
    // pointer changes propagate at human scale, not telemetry scale.
    // First pass fires on startup so a pointer set while the device was
    // offline is picked up on boot without a 20-min wait.
    uint32_t last_ir_poll_ms = 0;
    bool     ir_first        = true;

    // Pre-register heartbeep + ir_poll_interval so cycle 1's poll_all
    // pulls them live — otherwise a live override would only kick in
    // starting with cycle 3. Same rationale as the Particle shim.
    (void)g_cfg.get_int("heartbeep",        HEARTBEEP_DEFAULT);
    (void)g_cfg.get_int("ir_poll_interval", IR_POLL_INTERVAL_DEFAULT);
    (void)g_cfg.get_int("max_brightness",   MAX_BRIGHTNESS);

    for (;;) {
        uint32_t now = millis();

        // OTA lifecycle publishes. Run *before* the heartbeep `due` gate
        // so these fire as soon as the producer (ir_poll for detected,
        // the main loop for matrix/loaded) flips the flag — within
        // ~100ms of the event rather than "next heartbeat, which could
        // be `heartbeep` seconds away." Each is one-shot per event:
        // read flag, fetch snapshot, publish, clear flag.
        //
        // Ordering matters: detected → matrix → loaded. ir_poll (tel
        // task) sets `detected` before staging; the main loop sees the
        // pending blob, sets `matrix`, holds the streamer for
        // OTA_LOADING_MS, calls ir_apply_if_ready (main loop) and sets
        // `loaded` after engine.reinit. Publishing in that order keeps
        // the app stream legible: each event names its lifecycle phase.
        //
        // `detected` identity lives on Stra2usClient (captured there by
        // ir_poll); `matrix`/`loaded` use g_ota_{matrix,loaded}_*
        // snapshots because the main-thread handoff needs its own
        // freeze point against a racing OTA.
        //
        // Why a fresh connect()/close() per publish instead of batching:
        // a POST interleaved mid-GET-keep-alive on a shared socket
        // wedged the tel thread on Particle (post-mortem 2026-04-22).
        // Same WiFiClient semantics on ESP32; keep each POST on its own
        // socket until we have reason to trust otherwise.
        if (g_cfg.ir_detected_ready()) {
            static char msg[192];
            snprintf(msg, sizeof(msg),
                     "ota_detected from=%s@%.8s to=%s@%.8s size=%u up=%lu",
                     g_cfg.ir_detected_from_name(), g_cfg.ir_detected_from_sha(),
                     g_cfg.ir_detected_to_name(),   g_cfg.ir_detected_to_sha(),
                     (unsigned)g_cfg.ir_detected_size(),
                     (unsigned long)(millis() / 1000));
            g_cfg.connect();
            int s = g_cfg.publish(STRA2US_APP, msg);
            g_cfg.close();
            Serial.printf("[tel] ota_detected publish=%d %s\n", s, msg);
            g_cfg.ir_clear_detected();
        }
        if (g_ota_pub_matrix) {
            static char msg[128];
            snprintf(msg, sizeof(msg),
                     "ota_matrix name=%s@%.8s up=%lu",
                     g_ota_matrix_name, g_ota_matrix_sha,
                     (unsigned long)(millis() / 1000));
            g_cfg.connect();
            int s = g_cfg.publish(STRA2US_APP, msg);
            g_cfg.close();
            Serial.printf("[tel] ota_matrix publish=%d %s\n", s, msg);
            g_ota_pub_matrix = false;
        }
        if (g_ota_pub_loaded) {
            static char msg[128];
            snprintf(msg, sizeof(msg),
                     "ota_loaded name=%s@%.8s up=%lu",
                     g_ota_loaded_name, g_ota_loaded_sha,
                     (unsigned long)(millis() / 1000));
            g_cfg.connect();
            int s = g_cfg.publish(STRA2US_APP, msg);
            g_cfg.close();
            Serial.printf("[tel] ota_loaded publish=%d %s\n", s, msg);
            g_ota_pub_loaded = false;

            // Nudge the next heartbeat to fire ~5s from now so the
            // `script=<name>@<sha>` confirmation lands promptly instead
            // of waiting up to `heartbeep` seconds (300s default) for
            // the normal cadence. 5s (not 0s) gives `ota_loaded` room
            // to land on the event stream as its own distinct record
            // and avoids back-to-back publishes on the same socket.
            // Matches the Particle shim's nudge.
            last_attempt_ms  = now;
            next_interval_ms = 5000;
        }

        bool due = first || (now - last_attempt_ms >= next_interval_ms);
        if (!due) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }

        last_attempt_ms = now;
        first = false;
        int status = telemetry_cycle();

        // Read heartbeep AFTER the cycle so a newly-fetched override
        // schedules the next fire — same ordering as the Particle shim.
        int hb = g_cfg.get_int("heartbeep", HEARTBEEP_DEFAULT);
        if (hb < 10) hb = 10;
        uint32_t hb_ms = (uint32_t)hb * 1000UL;

        if (status >= 200 && status < 500) {
            next_interval_ms = hb_ms;
            backoff_ms       = retry_floor_ms;
        } else {
            next_interval_ms = backoff_ms;
            backoff_ms = (backoff_ms * 2 < hb_ms) ? backoff_ms * 2 : hb_ms;
        }

        // OTA IR poll on its own slow cadence. Runs AFTER telemetry_cycle
        // so a stalled ir_poll can't delay the next heartbeat — worst
        // case we skip an OTA check, not a heartbeat. connect()/close()
        // brackets keep the sidecar+blob GETs on a dedicated socket so
        // they can't interleave with a concurrent publish() on the same
        // TCP stream (see post-mortem note at the top of this task).
        int ir_int_s = g_cfg.get_int("ir_poll_interval", IR_POLL_INTERVAL_DEFAULT);
        if (ir_int_s < 60) ir_int_s = 60;
        uint32_t ir_int_ms = (uint32_t)ir_int_s * 1000UL;
        if (ir_first || (millis() - last_ir_poll_ms >= ir_int_ms)) {
            g_cfg.connect();
            g_cfg.ir_poll();
            g_cfg.close();
            last_ir_poll_ms = millis();
            ir_first = false;
        }
    }
}
#endif  // CRIT_HAVE_STRA2US

void setup() {
    Serial.begin(115200);
    unsigned long t0 = millis();
    while (!Serial && (millis() - t0) < 1500) { delay(10); }

    // Rescue-hold classification runs FIRST — before the engine starts,
    // before WiFi. If the previous boot was a crash we still want OTA up
    // (so a replacement fw can push), but we don't want the engine
    // running in case whatever crashed us is in the engine path. The
    // reason is latched at poweron and cleared on the next boot, so
    // reading it here gives the previous boot's exit status.
    esp_reset_reason_t reset_reason = esp_reset_reason();
    if (is_crash_reset(reset_reason)) {
        g_rescue_mode     = true;
        g_rescue_start_ms = millis();
        Serial.printf("[crit] rescue hold: reset_reason=%d, holding %lums for OTA\n",
                      (int)reset_reason, (unsigned long)RESCUE_HOLD_MS);
    } else {
        Serial.printf("[crit] clean boot: reset_reason=%d\n", (int)reset_reason);
    }

#if CRIT_HAVE_WIFI
    // Diagnostic only — actual mark-valid happens later in loop(), once
    // we've proven the image can reach WiFi + SNTP + engine runtime. The
    // bootloader has already booted us; this just surfaces what state it
    // sees for us. PENDING_VERIFY means "this is your first boot after
    // an OTA flash; mark valid or get rolled back."
    ota_log_partition_state();
#endif

    g_sink.begin();
    g_sink.set_brightness(MAX_BRIGHTNESS);

#if CRIT_HAVE_LIGHT
    // Start BH1750 before the engine so the first sample is queued by the
    // time loop() starts polling. 180ms continuous-mode warmup happens in
    // the background while the rest of setup runs — by the time we hit
    // loop() the sensor has valid data. A failed begin() is non-fatal:
    // update() returns max_bri on dropped reads, so the panel stays at
    // full brightness until the sensor comes online (or forever if it's
    // never going to — same graceful-degradation model as the Particle
    // CDS path).
    if (!g_light.begin(LIGHT_SENSOR_SDA_PIN, LIGHT_SENSOR_SCL_PIN)) {
        Serial.printf("[crit] BH1750 init FAILED (sda=%d scl=%d addr=0x%02x); "
                      "holding at MAX_BRIGHTNESS\n",
                      LIGHT_SENSOR_SDA_PIN, LIGHT_SENSOR_SCL_PIN,
                      (int)LIGHT_SENSOR_ADDR);
    } else {
        Serial.printf("[crit] BH1750 up (sda=%d scl=%d addr=0x%02x)\n",
                      LIGHT_SENSOR_SDA_PIN, LIGHT_SENSOR_SCL_PIN,
                      (int)LIGHT_SENSOR_ADDR);
    }
#endif

    if (!g_engine.begin()) {
        critterchron::g_errlog.record(critterchron::ErrCat::Boot,
                 "engine.begin() failed");
    }
    g_engine.seedRng(esp_random());

#if CRIT_HAVE_WIFI
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    configTime(0, 0, SNTP_SERVER_1, SNTP_SERVER_2, SNTP_SERVER_3);

    // ArduinoOTA: push-flash over LAN via espota.py (wrapped by
    // `arduino-cli upload --port <name>.local --protocol network`).
    // Hostname = DEVICE_NAME so mDNS resolves `<device>.local` without
    // tracking the DHCP-assigned IP. No password on the listener —
    // LAN-trust model; follow-up TODO filed for pull-mode OTA with
    // HMAC-SHA256 verification against the Stra2us device secret.
    //
    // Callbacks drive the visual + engine-pause state:
    //   onStart   — flip g_ota_active; clear the last-bar-cols cache so
    //               the first onProgress always re-draws.
    //   onProgress — repaint the green progress bar when its column
    //               count changes.
    //   onEnd     — success: listener's about to reboot. Paint the bar
    //               full one last time so the operator sees "done" for
    //               the brief window before reset.
    //   onError   — failure: log + clear the flag so the engine resumes.
    //               Flashing can be retried; the current running fw is
    //               unchanged (Update.h writes to the inactive partition
    //               and only swaps on successful end).
    ArduinoOTA.setHostname(DEVICE_NAME);
    ArduinoOTA.onStart([]() {
        g_ota_active = true;
        g_ota_last_bar_cols = (unsigned int)-1;  // force first draw
        Serial.printf("[ota] start: type=%s\n",
                      ArduinoOTA.getCommand() == U_FLASH ? "fw" : "spiffs");
    });
    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
        draw_ota_progress(progress, total);
    });
    ArduinoOTA.onEnd([]() {
        // Full bar on success — device resets within milliseconds, but
        // the bar stays painted until the reset actually happens so
        // there's a visible "100%" beat on the grid.
        draw_ota_progress(1, 1);
        Serial.println("[ota] end: flash complete, rebooting");
        // g_ota_active stays true — Update.h will reset us; no point
        // resuming the engine for the ~100ms window until it does.
    });
    ArduinoOTA.onError([](ota_error_t err) {
        critterchron::g_errlog.record(critterchron::ErrCat::OtaApply,
                 "ArduinoOTA error=%u", (unsigned)err);
        g_ota_active = false;
        g_ota_last_bar_cols = 0;
        // Sink may be mid-progress-bar; blank it so the engine's first
        // render after this isn't a weird merge.
        g_sink.clear();
        g_sink.show();
    });
    // ArduinoOTA.begin() calls MDNS.begin(hostname) internally, so the
    // device answers `<DEVICE_NAME>.local` immediately. No separate
    // MDNS.begin() call needed.
    ArduinoOTA.begin();
    Serial.printf("[crit] OTA listener up: %s.local:3232\n", DEVICE_NAME);
#endif

    Serial.printf("[crit] CritterChron up. %dx%d rot=%d fw=%s device=%s "
                  "wifi=%s stra2us=%s rescue=%s\n",
                  GRID_WIDTH, GRID_HEIGHT, GRID_ROTATION,
                  APP_VERSION, DEVICE_NAME,
                  CRIT_HAVE_WIFI    ? "enabled" : "disabled",
                  CRIT_HAVE_STRA2US ? "enabled" : "disabled",
                  g_rescue_mode     ? "HOLDING"  : "clean");
}

void loop() {
    unsigned long now = millis();

#if CRIT_HAVE_WIFI
    // ArduinoOTA.handle() polls the UDP listener for incoming flash
    // requests. Cheap when idle (single non-blocking recvfrom); must
    // run EVERY loop iteration, even in rescue mode, even during the
    // pre-SNTP spinner wait — the whole point of the rescue hold is
    // that OTA stays reachable when nothing else is.
    ArduinoOTA.handle();

    if (g_ota_active) {
        // Flash in progress. Everything else is off: render, physics,
        // tel task (its network traffic would collide with the flash
        // stream), heartbeat publishes. The progress bar is drawn
        // directly by the onProgress callback; this branch just
        // short-circuits the normal loop body.
        delay(1);  // yield to other tasks; ArduinoOTA.handle() keeps the
                   // protocol timer-loop moving via its internal work.
        return;
    }
#endif

#if CRIT_HAVE_LIGHT
    // Ambient light sample @ 5Hz. Placed above the rescue + clock gates so
    // the rescue chase and pre-SNTP spinner both dim with ambient — an
    // operator watching a dark room shouldn't get a face full of max-bright
    // rescue-amber while the engine is held off. g_light.update() gracefully
    // returns max_bri on dropped reads, so a dead sensor is equivalent to
    // compiled-in MAX_BRIGHTNESS and nothing else has to care.
    //
    // EMA (α ≈ 1/50) smooths the target so a flipped light switch fades
    // over ~10 seconds rather than popping. Q8 fixed-point mirrors the
    // Particle shim at critterchron_particle.cpp:785 — same visual
    // response across platforms.
    //
    // min_brightness / max_brightness are pulled live from Config each
    // tick (hot-path safe per Config.h contract). Floor at 1 — a bri=0
    // sink is reserved as a night-mode-disable sentinel in the Schmitt
    // below (matches the Particle convention).
    {
        static unsigned long last_light_ms = 0;
        static uint32_t      bri_q8 = (uint32_t)MAX_BRIGHTNESS << 8;
        if (now - last_light_ms >= 200) {
            last_light_ms = now;
            int min_b = g_cfg.get_int("min_brightness", MIN_BRIGHTNESS);
            int max_b = g_cfg.get_int("max_brightness", MAX_BRIGHTNESS);
            if (min_b < 1)   min_b = 1;
            if (max_b > 255) max_b = 255;
            if (min_b > max_b) min_b = max_b;
            uint8_t target = g_light.update((uint8_t)min_b, (uint8_t)max_b);
            uint32_t target_q8 = (uint32_t)target << 8;
            bri_q8 = bri_q8 + ((int32_t)(target_q8 - bri_q8) / 50);
            uint8_t bri = (uint8_t)(bri_q8 >> 8);
            g_sink.set_brightness(bri);
#if CRIT_HAVE_STRA2US
            g_bri     = bri;
            g_bri_min = (uint8_t)min_b;
            g_bri_max = (uint8_t)max_b;
            g_lux     = g_light.last_raw_lux;
#endif

            // Night-mode Schmitt trigger. Drive off the smoothed `bri`
            // rather than raw lux — a single dark sample shouldn't flip
            // the palette. Enter at the sink's floor (where color
            // rounding starts pushing channels toward white); exit a
            // few units above so a candle flicker doesn't oscillate.
            // Both thresholds are Stra2us-tunable; `ne + 1` floor on
            // `nx` keeps a misconfigured pair from deadlocking the
            // trigger. Matches critterchron_particle.cpp:792-805.
            int ne = g_cfg.get_int("night_enter_brightness", NIGHT_ENTER_BRIGHTNESS);
            int nx = g_cfg.get_int("night_exit_brightness",  NIGHT_EXIT_BRIGHTNESS);
            if (nx <= ne) nx = ne + 1;
            if (bri <= (uint8_t)ne && !g_engine.nightMode()) {
                g_engine.setNightMode(true);
            } else if (bri >= (uint8_t)nx && g_engine.nightMode()) {
                g_engine.setNightMode(false);
            }
        }
    }
#endif  // CRIT_HAVE_LIGHT

    // Rescue hold: previous boot was a crash. Keep OTA reachable (handled
    // above), render the amber chase so the state is visible across the
    // room, skip the engine. Exits automatically after RESCUE_HOLD_MS —
    // if no OTA landed in the window, we fall through and try to run the
    // engine anyway. That's better than holding forever on a device with
    // no one available to reflash.
    if (g_rescue_mode) {
        if (now - g_rescue_start_ms < RESCUE_HOLD_MS) {
            if (now - last_render_tick >= RENDER_TICK_MS) {
                last_render_tick = now;
                draw_rescue(now);
            }
            return;
        }
        g_rescue_mode = false;
        Serial.println("[crit] rescue hold elapsed, starting engine");
    }

    // Network + time gate.
    if (!g_clock.valid()) {
        if (now - last_render_tick >= 100) {
            last_render_tick = now;
            draw_spinner(now);
        }
        static unsigned long last_wait_log_ms = 0;
        if (now - last_wait_log_ms >= 5000) {
            last_wait_log_ms = now;
#if CRIT_HAVE_WIFI
            Serial.printf("[crit] waiting: wifi.status=%d time=%ld\n",
                          (int)WiFi.status(), (long)::time(nullptr));
#else
            Serial.println("[crit] waiting: no WiFi compiled in, "
                           "MockTimeSource should be valid already — "
                           "something is wrong");
#endif
        }
        return;
    }

#if CRIT_HAVE_WIFI
    // M8: mark the current image valid once it's demonstrated WiFi + SNTP
    // + OTA_VALID_BUDGET_MS of engine runtime. Before this fires, a
    // crash reset rolls us back to the previous partition automatically.
    // Cheap to evaluate once per loop iteration; the check is a bool
    // read after the first fire. No-op on non-OTA boots (serial-flashed
    // images have partition state VALID from the start; the SDK call
    // is a no-op in that case, just logs OK).
    if (!g_ota_marked_valid) {
        if (g_ota_valid_deadline_ms == 0) {
            g_ota_valid_deadline_ms = now + OTA_VALID_BUDGET_MS;
            Serial.printf("[ota] image verify armed: mark valid in %lums\n",
                          (unsigned long)OTA_VALID_BUDGET_MS);
        } else if ((long)(now - g_ota_valid_deadline_ms) >= 0) {
            esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
            Serial.printf("[ota] mark_app_valid=%s\n",
                          err == ESP_OK ? "OK" : esp_err_to_name(err));
            g_ota_marked_valid = true;
        }
    }
#endif

#if CRIT_HAVE_STRA2US
    // Lazy-start the telemetry task once the clock is valid. Putting this
    // in loop() (not setup) mirrors the Particle shim's cloud-first gate:
    // the task only makes sense once we've actually seen NTP, and waiting
    // until then means the first heartbeat carries a sane timestamp
    // instead of a 1970-era one that the server would reject on drift.
    if (g_tel_task == nullptr) {
        // Core pinning: on dual-core parts (ESP32, S3) WiFi/BT run on
        // PRO_CPU (core 0), so user tasks typically go on APP_CPU
        // (core 1) to avoid stack-starving the radio. On single-core
        // parts (C3, C6) there's only PRO_CPU and APP_CPU_NUM isn't
        // defined at all — fall back to PRO_CPU_NUM, which xTaskCreate-
        // PinnedToCore accepts on both families.
#ifdef APP_CPU_NUM
        const BaseType_t tel_core = APP_CPU_NUM;
#else
        const BaseType_t tel_core = PRO_CPU_NUM;  // single-core: only option
#endif
        BaseType_t ok = xTaskCreatePinnedToCore(
            telemetry_task, "tel",
            TELEMETRY_STACK_BYTES / sizeof(StackType_t),
            nullptr, 1 /* low prio */, &g_tel_task, tel_core);
        if (ok != pdPASS) {
            Serial.println("[crit] telemetry task spawn FAILED");
            g_tel_task = nullptr;  // retry next loop
        } else {
            Serial.printf("[crit] telemetry task started (stack=%u)\n",
                          (unsigned)TELEMETRY_STACK_BYTES);
        }
    }
#endif

#if CRIT_HAVE_STRA2US
    // OTA IR swap with a visual-delay loading screen.
    //
    // Flow: the tel task's ir_poll fetches a new blob and stages it in
    // ir_pending_*. We see ir_pending_ready(), enter the OTA loading
    // state for OTA_LOADING_MS, pause physics + render-over-the-grid,
    // snapshot identity for the `ota_matrix` lifecycle publish. When
    // the window elapses, ir_apply_if_ready() swaps the IR tables and
    // g_engine.reinit() reseats the engine. Parse + reinit cost runs
    // <10ms in practice — the user-visible delay is the loading window
    // itself, added deliberately so operators watching the grid after
    // a publish see a clear "incoming" cue before the new script takes
    // over.
    //
    // Runs BEFORE physics so we never swap tables mid-tick —
    // processAgent reads BEHAVIORS[beh_idx].insns[pc] and those
    // pointers must not shift under it. Physics is also skipped during
    // the window (render is fully overridden by the streamer; ticking
    // would only heat the CPU).
    //
    // Mirrors the Particle shim's OTA loading block at
    // critterchron_particle.cpp:934-1009 so both platforms feel the
    // same on an OTA push.
    static bool          g_ota_loading          = false;
    static unsigned long g_ota_loading_start_ms = 0;

    if (!g_ota_loading && g_cfg.ir_pending_ready()) {
        g_ota_loading          = true;
        g_ota_loading_start_ms = now;
        // Snapshot pending identity for lifecycle publish #2
        // (`ota_matrix`). Snapshot name/sha BEFORE the flag flip — tel
        // reads the names only after seeing the flag true, so these
        // writes must be in place first. Same volatile-flag-last fence
        // as the has_live ordering in Stra2usClient.
        strncpy(g_ota_matrix_name, g_cfg.ir_pending_script(),
                sizeof(g_ota_matrix_name) - 1);
        g_ota_matrix_name[sizeof(g_ota_matrix_name) - 1] = '\0';
        strncpy(g_ota_matrix_sha, g_cfg.ir_pending_sha(),
                sizeof(g_ota_matrix_sha) - 1);
        g_ota_matrix_sha[sizeof(g_ota_matrix_sha) - 1] = '\0';
        g_ota_pub_matrix = true;
        Serial.println("[crit] ota_loading: entered (pending blob ready)");
    }

    if (g_ota_loading) {
        if (now - g_ota_loading_start_ms >= OTA_LOADING_MS) {
            if (g_cfg.ir_apply_if_ready()) {
                if (!g_engine.reinit()) {
                    critterchron::g_errlog.record(critterchron::ErrCat::OtaApply,
                             "engine.reinit() failed after IR swap");
                } else {
                    // Fresh RNG entropy so two simultaneously-swapped
                    // devices diverge rather than march in lockstep.
                    g_engine.seedRng(esp_random());
                    last_sync_minute = -1;  // force syncTime next tick
                }
                // Snapshot loaded identity for lifecycle publish #3
                // (`ota_loaded`). Fire even if reinit failed — a
                // failed reinit is still an observable "this blob
                // landed on the device" event worth reporting; a gap
                // between ota_matrix and ota_loaded is a louder signal
                // than a missing ota_loaded.
                strncpy(g_ota_loaded_name, g_cfg.ir_loaded_script(),
                        sizeof(g_ota_loaded_name) - 1);
                g_ota_loaded_name[sizeof(g_ota_loaded_name) - 1] = '\0';
                strncpy(g_ota_loaded_sha, g_cfg.ir_loaded_sha(),
                        sizeof(g_ota_loaded_sha) - 1);
                g_ota_loaded_sha[sizeof(g_ota_loaded_sha) - 1] = '\0';
                g_ota_pub_loaded = true;
            }
            g_ota_loading = false;
        } else {
            // Hold here: render streamer at normal render cadence,
            // skip physics, short-circuit the rest of the loop.
            if (now - last_render_tick >= RENDER_TICK_MS) {
                last_render_tick = now;
                draw_ota_streamer(now);
            }
            return;
        }
    }
#endif  // CRIT_HAVE_STRA2US

    // Per-second diag rollup state. Accumulators live here (not globals)
    // because only the main loop touches them — the tel task reads the
    // snapshot volatiles published at the rollup boundary. Matches the
    // Particle shim's diag_* pattern at critterchron_particle.cpp:1061.
    static uint32_t      diag_phys_total   = 0;
    static uint32_t      diag_phys_count   = 0;
    static uint32_t      diag_phys_max     = 0;
    static uint32_t      diag_rend_total   = 0;
    static uint32_t      diag_rend_count   = 0;
    static uint32_t      diag_rend_max     = 0;
    static uint32_t      diag_interp_total = 0;
    static uint32_t      diag_interp_max   = 0;
    static uint32_t      diag_astar_total  = 0;
    static uint32_t      diag_astar_max    = 0;
    static unsigned long diag_next_ms      = 0;

    // Physics tick. `micros()` wrap is a non-issue for our window sizes
    // (uint32_t µs wraps every ~71 minutes; a single tick is <50ms).
    if (now - last_physics_tick >= physics_tick_ms()) {
        last_physics_tick = now;

        int m = local_minute(g_clock);
        if (m != last_sync_minute) {
            last_sync_minute = m;
            g_engine.syncTime();
        }
        uint32_t t0 = micros();
        g_engine.tick();
        uint32_t dt = micros() - t0;
        uint32_t interp_dt = g_engine.interpUsLastTick();
        uint32_t astar_dt  = g_engine.astarUsLastTick();
        diag_phys_total   += dt;
        diag_phys_count   += 1;
        if (dt > diag_phys_max) diag_phys_max = dt;
        diag_interp_total += interp_dt;
        if (interp_dt > diag_interp_max) diag_interp_max = interp_dt;
        diag_astar_total  += astar_dt;
        if (astar_dt > diag_astar_max) diag_astar_max = astar_dt;
    }

    // Render tick
    if (now - last_render_tick >= RENDER_TICK_MS) {
        last_render_tick = now;
        float blend = (float)(now - last_physics_tick) / (float)physics_tick_ms();
        if (blend > 1.0f) blend = 1.0f;
        uint32_t t0 = micros();
        g_engine.render(blend);
        uint32_t dt = micros() - t0;
        diag_rend_total += dt;
        diag_rend_count += 1;
        if (dt > diag_rend_max) diag_rend_max = dt;
    }

#if CRIT_HAVE_STRA2US
    // Per-second rollup. Close the window, publish avg/max to the tel-
    // visible volatiles, reset accumulators. Also emit a serial diag
    // line mirroring the Particle shim's Log.info — useful for local
    // debugging without waiting for a heartbeat cycle to hit the server.
    if ((long)(now - diag_next_ms) >= 0) {
        diag_next_ms = now + 1000;
        uint32_t phys_avg   = diag_phys_count ? (diag_phys_total   / diag_phys_count) : 0;
        uint32_t rend_avg   = diag_rend_count ? (diag_rend_total   / diag_rend_count) : 0;
        uint32_t interp_avg = diag_phys_count ? (diag_interp_total / diag_phys_count) : 0;
        uint32_t astar_avg  = diag_phys_count ? (diag_astar_total  / diag_phys_count) : 0;
        const auto& m = g_engine.metrics();
        Serial.printf("[diag] t=%lus phys=%lu(avg=%luus max=%luus) rend=%lu(avg=%luus max=%luus) "
                      "interp=(avg=%luus max=%luus) astar=(avg=%luus max=%luus) "
                      "agents=%u seeks_fail=%lu free=%lu\n",
                      (unsigned long)(now / 1000),
                      (unsigned long)diag_phys_count,
                      (unsigned long)phys_avg, (unsigned long)diag_phys_max,
                      (unsigned long)diag_rend_count,
                      (unsigned long)rend_avg, (unsigned long)diag_rend_max,
                      (unsigned long)interp_avg, (unsigned long)diag_interp_max,
                      (unsigned long)astar_avg,  (unsigned long)diag_astar_max,
                      (unsigned)g_engine.liveAgentCount(),
                      (unsigned long)m.failed_seeks,
                      (unsigned long)ESP.getFreeHeap());
        g_phys_avg_us     = phys_avg;
        g_phys_max_us     = diag_phys_max;
        g_rend_avg_us     = rend_avg;
        g_rend_max_us     = diag_rend_max;
        g_interp_avg_us   = interp_avg;
        g_interp_max_us   = diag_interp_max;
        g_astar_avg_us    = astar_avg;
        g_astar_max_us    = diag_astar_max;
        diag_phys_count = diag_phys_total = diag_phys_max = 0;
        diag_rend_count = diag_rend_total = diag_rend_max = 0;
        diag_interp_total = diag_interp_max = 0;
        diag_astar_total  = diag_astar_max  = 0;
    }
#endif
}

#endif  // ARDUINO_ARCH_ESP32

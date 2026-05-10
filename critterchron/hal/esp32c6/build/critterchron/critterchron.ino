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
#include "src/SnapshotBuffer.h"
#include "src/interface/Config.h"
#include "src/FastLEDSink.h"

#if defined(WIFI_SSID) && defined(WIFI_PASSWORD)
#define CRIT_HAVE_WIFI 1
#include <WiFi.h>
#include <Preferences.h>          // NVS-backed KV for procyon target persistence
#include <time.h>
#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <esp_ota_ops.h>          // M8: partition-state inspection + mark-valid
#include <esp_partition.h>        // running-image SHA256 for fw_sha heartbeat field
#include <esp_image_format.h>     // esp_image_get_metadata for true image_len
#include "src/sha256.h"           // SHA over raw partition bytes
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

// Telemetry publish topic. Derived from STRA2US_APP at preprocessor
// time so per-device headers don't have to duplicate it. The
// `<app>/public/heartbeep` shape is the post-public-namespace-
// migration topic that the customer-facing /app view tails — see
// PUBLIC_NAMESPACE.md and stra2us/docs/fr_application_view.md.
// Mirror of hal/particle/src/critterchron_particle.cpp.
#ifndef STRA2US_TELEMETRY_TOPIC
#define STRA2US_TELEMETRY_TOPIC STRA2US_APP "/public/heartbeep"
#endif

// Snapshot publish topic — sibling of heartbeep. Devices write here when
// FAILURE_TRIAGE.md §1's ring buffer dumps; the analyzer (or operator
// via `stra2us-cli follow`) tails to consume.
#ifndef STRA2US_SNAPSHOT_TOPIC
#define STRA2US_SNAPSHOT_TOPIC STRA2US_APP "/public/snapshots"
#endif

// Per-device trace topic (FAILURE_TRIAGE.md §2). Different prefix from
// snapshots: trace is hands-on debugging, not customer-aggregate, so
// keep it out of /public/. DEVICE_NAME comes from creds.h.
#ifndef STRA2US_TRACE_TOPIC
#define STRA2US_TRACE_TOPIC STRA2US_APP "/trace/" DEVICE_NAME
#endif

// Per-device default for the runtime ring depth. 0 = feature dormant
// even if compiled in (no append() cost). Staging fleet device headers
// override to 32 (4 seconds at the default 8Hz physics tick); prod
// devices stay at 0 until explicitly opted in via stra2us KV.
#ifndef SNAPSHOT_BUFFER_FRAMES_DEFAULT
#define SNAPSHOT_BUFFER_FRAMES_DEFAULT 0
#endif

// Max bytes a single snapshot publish can carry. Sized for the v1
// frame format (~80 bytes/frame) at the SNAPSHOT_MAX_FRAMES default
// of 32: ~2.6 KB body + header. 4 KB headroom is comfortable.
#ifndef SNAPSHOT_PUBLISH_BUF_BYTES
#define SNAPSHOT_PUBLISH_BUF_BYTES 4096
#endif

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

// Cadence for firmware OTA poll. Firmware updates are rare events
// (typically weekly at most), much rarer than IR script changes —
// borrowing `ir_poll_interval` here would generate hundreds of
// needless sidecar GETs per device per day. 86400s = 1 day. Floored
// at 60s in the task loop for testing convenience. Plus a one-shot
// fire on boot via `fw_first` so a staged update that landed during
// an offline overnight gets picked up at next power-on without
// waiting out the full poll interval.
#ifndef FW_POLL_INTERVAL_DEFAULT
#define FW_POLL_INTERVAL_DEFAULT 86400
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

// Compute SHA256 of the currently-running partition. Used by the
// `fw_sha=<8 hex>` heartbeat field for at-a-glance "what build is
// timmy actually running" without parsing build dates. Must match
// `tools/publish_fw.py`'s `sha256(open(firmware.bin).read())` exactly
// — fw_poll uses the equality check to decide skip-vs-fetch, so any
// mismatch turns into a crashloop (observed 2026-04-30: applied
// sha=81f977db, post-reboot esp_partition_get_sha256=bde6d32d → loop).
//
// `esp_partition_get_sha256` returns the SHA stored in the image
// header, hashed by the linker over the image's logical structure
// — *not* the bytes of the .bin file. So we hash the raw partition
// bytes ourselves: read `image_len` bytes (the .bin's true length on
// flash, including appended SHA + padding) and feed them through
// sha256_update. One-shot in setup(); the running image doesn't
// change without a reboot.
static void compute_running_fw_sha(char out_hex_64[65]) {
    out_hex_64[0] = '\0';
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (!running) {
        Serial.println("[ota] running partition unknown — fw_sha empty");
        return;
    }
    esp_partition_pos_t pos = { running->address, running->size };
    esp_image_metadata_t meta = {};
    esp_err_t err = esp_image_get_metadata(&pos, &meta);
    if (err != ESP_OK) {
        Serial.printf("[ota] esp_image_get_metadata failed: %s\n",
                      esp_err_to_name(err));
        return;
    }
    SHA256_CTX ctx;
    sha256_init(&ctx);
    uint8_t chunk[1024];
    uint32_t off = 0;
    while (off < meta.image_len) {
        uint32_t n = meta.image_len - off;
        if (n > sizeof(chunk)) n = sizeof(chunk);
        err = esp_partition_read(running, off, chunk, n);
        if (err != ESP_OK) {
            Serial.printf("[ota] esp_partition_read failed at %u: %s\n",
                          (unsigned)off, esp_err_to_name(err));
            return;
        }
        sha256_update(&ctx, chunk, n);
        off += n;
    }
    uint8_t digest[32];
    sha256_final(&ctx, digest);
    static const char hex_chars[] = "0123456789abcdef";
    for (int i = 0; i < 32; ++i) {
        out_hex_64[2*i]   = hex_chars[(digest[i] >> 4) & 0xf];
        out_hex_64[2*i+1] = hex_chars[digest[i] & 0xf];
    }
    out_hex_64[64] = '\0';
    Serial.printf("[ota] running image sha=%.8s... (image_len=%u)\n",
                  out_hex_64, (unsigned)meta.image_len);
}

// ---------- Procyon rescue WiFi (ESP32 mirror) ----------
// Mirror of hal/particle/src/critterchron_particle.cpp. Same fleet-
// shared rescue credential (`procyon` / `horology`) and KV-driven
// target-cred install via `wifi_ssid` / `wifi_password`. Differences
// from the Particle path:
//   * No DCT — credential storage is rolled here using NVS via the
//     Preferences library. Two slots: target (NVS-persisted, default
//     to compiled-in WIFI_SSID/WIFI_PASSWORD) and procyon (compiled-
//     in fallback).
//   * No DeviceOS-managed cred cycle — manual cycler swaps between
//     target and procyon when WiFi.status() stays unconnected past a
//     budget. RSSI-prefer behavior matches Particle in practice
//     because once a strong AP (procyon hotspot) is found, WiFi.begin
//     stays on it.
//   * No selective-remove issue — we control storage; full nuke-and-
//     restore semantics not needed.
#define PROCYON_SSID       "procyon"
#define PROCYON_PASSPHRASE "horology"

// Per-slot SSID/password buffers. 802.11 SSID max is 32 bytes; WPA2
// password max is 63. Round up + NUL.
static char g_target_ssid_[40] = {0};
static char g_target_pw_  [72] = {0};

// Cycler state. 15s budget per slot before we swap. Once WL_CONNECTED
// is hit, the cycler idles until the next disconnect.
enum WifiPhase { WIFI_TRY_TARGET, WIFI_TRY_PROCYON };
static WifiPhase     g_wifi_phase             = WIFI_TRY_TARGET;
static unsigned long g_wifi_phase_started_ms  = 0;
static const unsigned long WIFI_PHASE_BUDGET_MS = 15000;

// NVS keys (namespace "critterchron"). Loaded at boot, written when
// telemetry_cycle's KV-apply path commits a new target cred.
#define NVS_NAMESPACE      "critterchron"
#define NVS_KEY_WIFI_SSID  "wifi_ssid"
#define NVS_KEY_WIFI_PW    "wifi_pw"

static void load_target_creds_from_nvs_() {
    Preferences prefs;
    if (!prefs.begin(NVS_NAMESPACE, /*read-only*/ true)) {
        // First-boot or NVS-unavailable: fall back to compiled-in.
        strncpy(g_target_ssid_, WIFI_SSID,     sizeof(g_target_ssid_) - 1);
        strncpy(g_target_pw_,   WIFI_PASSWORD, sizeof(g_target_pw_)   - 1);
        Serial.println("[procyon] NVS empty; using compiled-in target");
        return;
    }
    String ssid = prefs.getString(NVS_KEY_WIFI_SSID, WIFI_SSID);
    String pw   = prefs.getString(NVS_KEY_WIFI_PW,   WIFI_PASSWORD);
    prefs.end();
    strncpy(g_target_ssid_, ssid.c_str(), sizeof(g_target_ssid_) - 1);
    strncpy(g_target_pw_,   pw.c_str(),   sizeof(g_target_pw_)   - 1);
    g_target_ssid_[sizeof(g_target_ssid_) - 1] = '\0';
    g_target_pw_  [sizeof(g_target_pw_)   - 1] = '\0';
    Serial.printf("[procyon] loaded target ssid=\"%s\" from NVS\n",
                  g_target_ssid_);
}

static bool save_target_creds_to_nvs_(const char* ssid, const char* pw) {
    Preferences prefs;
    if (!prefs.begin(NVS_NAMESPACE, /*read-only*/ false)) return false;
    bool ok = prefs.putString(NVS_KEY_WIFI_SSID, ssid) > 0
           && prefs.putString(NVS_KEY_WIFI_PW,   pw)   > 0;
    prefs.end();
    return ok;
}

// Manual cycler step. Called from the main loop on each iteration.
// No-op when connected; otherwise advances to the next slot when the
// current attempt's budget elapses. The first call of the boot session
// is responsible for kicking off WIFI_TRY_TARGET (setup() does that).
static void wifi_cycle_step_(unsigned long now) {
    if (WiFi.status() == WL_CONNECTED) return;
    if ((unsigned long)(now - g_wifi_phase_started_ms) < WIFI_PHASE_BUDGET_MS) {
        return;
    }
    if (g_wifi_phase == WIFI_TRY_TARGET) {
        Serial.printf("[procyon] target \"%s\" budget elapsed; trying procyon\n",
                      g_target_ssid_);
        WiFi.disconnect(true, true);
        WiFi.begin(PROCYON_SSID, PROCYON_PASSPHRASE);
        g_wifi_phase = WIFI_TRY_PROCYON;
    } else {
        Serial.printf("[procyon] procyon budget elapsed; trying target \"%s\"\n",
                      g_target_ssid_);
        WiFi.disconnect(true, true);
        WiFi.begin(g_target_ssid_, g_target_pw_);
        g_wifi_phase = WIFI_TRY_TARGET;
    }
    g_wifi_phase_started_ms = now;
}

// Force-restart the cycler from the target slot. Called after a
// successful KV-apply that may have changed g_target_ssid_/pw_, so the
// device tries the new creds immediately rather than waiting out the
// budget on whatever it's currently associated with.
static void wifi_cycle_restart_target_(unsigned long now) {
    WiFi.disconnect(true, true);
    WiFi.begin(g_target_ssid_, g_target_pw_);
    g_wifi_phase             = WIFI_TRY_TARGET;
    g_wifi_phase_started_ms  = now;
}

// Wifi-creds-applied visual signal — animated blue chase along the
// bottom row, fired only when we're currently on procyon AND the apply
// just landed. Mirror of the Particle path; same 10s window. The
// chase pattern lives in g_wifi_apply_chase_row_, refreshed each
// render frame from a deadline state.
#define WIFI_APPLY_SIGNAL_MS 10000
static unsigned long g_wifi_signal_until_ms = 0;
static uint8_t       g_wifi_apply_chase_row_[GRID_WIDTH * 3] = {0};

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

// ---------- Latency sparkline ----------
// Rolling row of GRID_WIDTH RGB pixels showing the last N heartbeats'
// network latency. Owned by the tel task: shifted-and-extended once per
// heartbeat after publish + poll_all + ir/fw poll have all contributed
// their samples to Stra2usClient::consume_latency_stats. Pushed into
// FastLEDSink as a destructive overlay row at y = GRID_HEIGHT - 1.
//
// Color mapping (Arduino-style linear `map`):
//   ms <= 20   → (0, 255, 0)         bright green
//   20 < ms ≤ 200 → (0, G, 0) where G = map(ms, 20, 200, 255, 1)
//   ms > 200   → (255, 0, 0)         red — saturated "this is bad"
// Single-channel-1 is plenty visible on this fleet (see memory note
// `feedback_low_rgb_visible.md`); the floor at 1 keeps a slow-but-not-
// terrible reading distinguishable from "off".
//
// Scale rationale: 20-200ms covers a small POST + response over LAN
// WiFi to a local Stra2us server. Below 20ms is unrealistic over WiFi
// (treat as max-good); above 200ms means the network path is degraded
// enough to notice. Hardcoded for now — operator regulates the time
// axis via heartbeep cadence (longer cadence = wider visible history).
static uint8_t g_latency_row[GRID_WIDTH * 3] = {0};

static void latency_color_(uint32_t ms,
                           uint8_t& r, uint8_t& g, uint8_t& b) {
    if (ms > 200) { r = 255; g = 0;   b = 0; return; }
    if (ms <= 20) { r = 0;   g = 255; b = 0; return; }
    // Linear interp on the green channel: 20→255, 200→1.
    long span  = 255 - 1;          // 254
    long range = 200 - 20;         // 180
    long val   = (long)ms - 20;    // 0..180
    long gv    = 255 - (val * span) / range;
    if (gv < 1) gv = 1;
    if (gv > 255) gv = 255;
    r = 0; g = (uint8_t)gv; b = 0;
}

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
// Schedule observability flags — written by the brightness loop, read by
// the heartbeat formatter. Mirror of the Particle port; see the parser
// site below for the wire format.
static volatile bool    g_sched_in_use    = false;
static volatile bool    g_sched_parse_err = false;
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

// ---------- Time-of-day brightness schedule ----------
// Mirror of hal/particle/src/critterchron_particle.cpp; see that file's
// detailed comment for the wire format and design rationale. Quick recap:
//   "23:00-07:00:1, 07:00-09:00:16, 09:00-23:00:64"
// Comma-separated `HH:MM-HH:MM:max_bri` segments, first match wins,
// wraparound by start > end. All-or-nothing parse. Empty = no schedule.
// Schedule should follow REAL wall-clock time — on this port that means
// reading off `g_real_clock` (un-wobbled), not `g_clock` (which the
// engine reads for the panel's display clock).

struct ScheduleSeg {
    uint16_t start_min;
    uint16_t end_min;
    uint8_t  max_bri;
    uint8_t  min_bri;     // 0 = "no min override" (single-value form);
                          // 1..255 = pin segment's min_brightness too
                          // (two-value form `HH:MM-HH:MM:min-max`).
};
static constexpr int  SCHEDULE_MAX_SEGS = 8;
static ScheduleSeg    s_sched_segs[SCHEDULE_MAX_SEGS];
static uint8_t        s_sched_count = 0;
static char           s_sched_last[160] = {0};
static bool           s_sched_parse_ok = false;

// Two segment shapes accepted (mirror of Particle):
//   `HH:MM-HH:MM:max`         → override max only
//   `HH:MM-HH:MM:min-max`     → override BOTH ends (hard-pin segment)
static bool parse_brightness_schedule(const char* s) {
    s_sched_count = 0;
    if (!s || !*s) return true;
    int seg_idx = 0;
    const char* p = s;
    while (*p && seg_idx < SCHEDULE_MAX_SEGS) {
        while (*p == ' ' || *p == ',' || *p == '\t') ++p;
        if (!*p) break;
        int sh, sm, eh, em, low, high, n = 0;
        bool two_value = false;
        if (sscanf(p, "%d:%d-%d:%d:%d-%d%n",
                   &sh, &sm, &eh, &em, &low, &high, &n) == 6) {
            two_value = true;
        } else {
            n = 0;
            if (sscanf(p, "%d:%d-%d:%d:%d%n",
                       &sh, &sm, &eh, &em, &high, &n) != 5) {
                const char* end = p;
                while (*end && *end != ',') ++end;
                int len = (int)(end - p);
                if (len > 28) len = 28;
                critterchron::g_errlog.record(critterchron::ErrCat::Other,
                    "sched: bad seg %d: %.*s", seg_idx, len, p);
                s_sched_count = 0;
                return false;
            }
            low = 0;  // sentinel: no min override
        }
        if (sh < 0 || sh > 23 || sm < 0 || sm > 59 ||
            eh < 0 || eh > 23 || em < 0 || em > 59) {
            critterchron::g_errlog.record(critterchron::ErrCat::Other,
                "sched: bad time seg %d", seg_idx);
            s_sched_count = 0;
            return false;
        }
        if (high < 0 || high > 255) {
            critterchron::g_errlog.record(critterchron::ErrCat::Other,
                "sched: max_bri %d out of range seg %d", high, seg_idx);
            s_sched_count = 0;
            return false;
        }
        if (two_value) {
            if (low < 1 || low > 255 || low > high) {
                critterchron::g_errlog.record(critterchron::ErrCat::Other,
                    "sched: min_bri %d out of range seg %d", low, seg_idx);
                s_sched_count = 0;
                return false;
            }
        }
        s_sched_segs[seg_idx].start_min = (uint16_t)(sh * 60 + sm);
        s_sched_segs[seg_idx].end_min   = (uint16_t)(eh * 60 + em);
        s_sched_segs[seg_idx].max_bri   = (uint8_t)high;
        s_sched_segs[seg_idx].min_bri   = (uint8_t)low;
        ++seg_idx;
        p += n;
    }
    s_sched_count = (uint8_t)seg_idx;
    return true;
}

// -1 = no segment matches; caller falls through to device default max_b.
// `out_min_bri` (optional) receives the segment's min override: 0 if
// single-value form, 1..255 if two-value.
static int schedule_match_max_bri(uint16_t now_min, int* out_min_bri = nullptr) {
    for (uint8_t i = 0; i < s_sched_count; ++i) {
        const auto& seg = s_sched_segs[i];
        bool in_seg;
        if (seg.start_min <= seg.end_min) {
            in_seg = (now_min >= seg.start_min && now_min < seg.end_min);
        } else {
            in_seg = (now_min >= seg.start_min || now_min < seg.end_min);
        }
        if (in_seg) {
            if (out_min_bri) *out_min_bri = (int)seg.min_bri;
            return (int)seg.max_bri;
        }
    }
    return -1;
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

    // Schedule marker — see Particle shim for rationale. `sched` =
    // schedule active and matched. `sched-err` = string set but parse
    // failed. Empty otherwise.
    const char* sched_tag = g_sched_parse_err ? " sched-err"
                          : g_sched_in_use     ? " sched"
                          : "";

    // Running firmware identity — first 8 hex of the partition SHA256,
    // computed once at boot. Lets an operator match the deployed device
    // against the sha printed by `tools/publish_fw.py` without needing
    // to parse build dates. Empty string sentinel if compute_running_fw_sha
    // failed at boot (rare; means the partition lookup failed).
    const char* fw_sha = g_cfg.running_fw_sha();
    char fw_sha_field[16];
    if (fw_sha && fw_sha[0]) {
        snprintf(fw_sha_field, sizeof(fw_sha_field), " fw_sha=%.8s", fw_sha);
    } else {
        fw_sha_field[0] = '\0';
    }
    // `net=<ssid>` makes the rescue case loud: steady state shows the
    // primary network's name; `net=procyon` is the obvious "this device
    // is on the rescue hotspot" signal in any heartbeat tail. Mirror of
    // the Particle path. Empty if WiFi.SSID() returns null/empty.
    String cur_ssid_str = WiFi.SSID();
    const char* cur_ssid = cur_ssid_str.c_str();
    if (!cur_ssid) cur_ssid = "";
#if CRIT_HAVE_LIGHT
    // `lux=X.X` is the BH1750 raw reading; bri=(min<cur<max) is the
    // mapped-and-smoothed output the sink is actually using. Together
    // they answer "is the room light what we think it is" (lux) and "is
    // the curve+clamp producing sensible brightness" (bri triplet).
    int rlen = snprintf(report, sizeof(report),
        "up=%lu rssi=%d heap=%lu rst=%d fw=%s%s script=%s net=%s bri=(%u<%u<%u%s) lux=%.1f "
        "phys=(%lu<%lu<%lu)us rend=(%lu<%lu<%lu)us "
        "interp=(%lu<%lu)us astar=(%lu<%lu)us "
        "agents=%u seeks_fail=%lu chip=%s",
        (unsigned long)(millis() / 1000),
        rssi,
        (unsigned long)ESP.getFreeHeap(),
        (int)esp_reset_reason(),
        APP_VERSION,
        fw_sha_field,
        script_tag,
        cur_ssid,
        (unsigned)g_bri_min, (unsigned)g_bri, (unsigned)g_bri_max, sched_tag,
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
        "up=%lu rssi=%d heap=%lu rst=%d fw=%s%s script=%s net=%s bri=(%u<%u<%u%s) "
        "phys=(%lu<%lu<%lu)us rend=(%lu<%lu<%lu)us "
        "interp=(%lu<%lu)us astar=(%lu<%lu)us "
        "agents=%u seeks_fail=%lu chip=%s",
        (unsigned long)(millis() / 1000),
        rssi,
        (unsigned long)ESP.getFreeHeap(),
        (int)esp_reset_reason(),
        APP_VERSION,
        fw_sha_field,
        script_tag,
        cur_ssid,
        (unsigned)g_bri_min, (unsigned)g_bri, (unsigned)g_bri_max, sched_tag,
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
    int pub_status = g_cfg.publish(STRA2US_TELEMETRY_TOPIC, report);
    Serial.printf("[tel] publish=%d %s\n", pub_status, report);
    if (have_err && pub_status == 200) {
        critterchron::g_errlog.mark_sent(pending_err.seq);
    }
    g_cfg.poll_all();
    g_cfg.close();

    // Latency sparkline: shift the rolling row left and append a new
    // pixel colored from this cycle's mean. The publish + poll_all
    // above (plus any ir_poll / fw_poll between heartbeats) have all
    // contributed samples to Stra2usClient's accumulator — drain it
    // here. When the knob is off, clear any latent overlay so flipping
    // the display off mid-session doesn't leave a stale row painted.
    int show_lat = g_cfg.get_int("latency_display", 0);
    if (show_lat) {
        uint32_t lmin = 0, lmean = 0, lmax = 0;
        if (g_cfg.consume_latency_stats(&lmin, &lmean, &lmax)) {
            memmove(g_latency_row, g_latency_row + 3,
                    (GRID_WIDTH - 1) * 3);
            uint8_t r, g, b;
            latency_color_(lmean, r, g, b);
            uint8_t* p = &g_latency_row[(GRID_WIDTH - 1) * 3];
            p[0] = r; p[1] = g; p[2] = b;
            g_sink.set_overlay_row(GRID_HEIGHT - 1, g_latency_row);
            Serial.printf("[lat] min=%lums mean=%lums max=%lums\n",
                          (unsigned long)lmin, (unsigned long)lmean,
                          (unsigned long)lmax);
        }
        // consume_latency_stats=false (no samples this window) leaves
        // the row untouched — better than a blank insert that suggests
        // a 0ms reading.
    } else {
        g_sink.clear_overlay();
    }

    // Procyon-rescue WiFi credential apply. Mirror of the Particle
    // path. After poll_all has refreshed wifi_ssid/wifi_password
    // (device-then-app fallback), if BOTH are non-empty AND the
    // hashed pair differs from the last successful apply, persist to
    // NVS and force-restart the WiFi cycler at the new target so the
    // device tries the new creds immediately rather than waiting for
    // the existing slot's budget to expire. Hash dedup makes the
    // steady state a no-op.
    {
        const char* w_ssid = g_cfg.wifi_ssid();
        const char* w_pw   = g_cfg.wifi_password();
        if (w_ssid && w_pw && w_ssid[0] && w_pw[0]) {
            // FNV-1a 32-bit over (ssid + NUL + password). Same hash
            // shape as the Particle side.
            uint32_t hh = 2166136261u;
            for (const char* p = w_ssid; *p; ++p) { hh ^= (uint8_t)*p; hh *= 16777619u; }
            hh *= 16777619u;
            for (const char* p = w_pw;   *p; ++p) { hh ^= (uint8_t)*p; hh *= 16777619u; }
            static uint32_t last_applied_hash = 0;
            if (hh != last_applied_hash) {
                bool ok = save_target_creds_to_nvs_(w_ssid, w_pw);
                if (ok) {
                    strncpy(g_target_ssid_, w_ssid, sizeof(g_target_ssid_) - 1);
                    strncpy(g_target_pw_,   w_pw,   sizeof(g_target_pw_)   - 1);
                    g_target_ssid_[sizeof(g_target_ssid_) - 1] = '\0';
                    g_target_pw_  [sizeof(g_target_pw_)   - 1] = '\0';
                    last_applied_hash = hh;
                    g_wifi_signal_until_ms = millis() + WIFI_APPLY_SIGNAL_MS;
                    Serial.printf("[procyon] wifi_creds: applied ssid=\"%s\"\n",
                                  w_ssid);
                    // Force a fresh attempt with the new creds. If
                    // we're currently on procyon, this disassociates
                    // and tries the new target immediately. If we're
                    // already on the (old) target, this is a no-op
                    // reassociate; minor blip, then either back on
                    // the same network or onto the new one.
                    wifi_cycle_restart_target_(millis());
                } else {
                    critterchron::g_errlog.record(
                        critterchron::ErrCat::Net,
                        "wifi_creds NVS save failed ssid=%s", w_ssid);
                }
            }
        }
    }

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

    // OTA Firmware poll timer. Same first-on-boot pattern as IR poll, much
    // longer cadence (1 day vs 20 min) since firmware updates are rare.
    // The fw_poll() call internally short-circuits when no `fw_target`
    // pointer is set, so this loop is harmless on devices nobody has
    // pointed at a firmware target.
    uint32_t last_fw_poll_ms = 0;
    bool     fw_first        = true;

    // Periodic WiFi reconnect kick. Mirror of the Particle landing
    // (2026-04-28). When WiFi drops, arduino-esp32's auto-reconnect
    // machinery normally brings it back, but if the radio gets wedged
    // (rico-equivalent on Gen 2 has been observed on Photon — same
    // failure mode plausibly hits ESP32) we want a periodic
    // `WiFi.reconnect()` to force a fresh attempt. Cadence borrowed
    // from `ir_poll_interval` (default 1200s) — already user-tunable,
    // already the right "wake up periodically and try things" rhythm.
    //
    // Note: arduino-esp32 has no `WiFi.connecting()` equivalent, so we
    // can't guard against racing the auto-reconnect like Particle does.
    // The cadence gate is conservative enough (≥20 min between kicks)
    // that any reasonable in-flight attempt has resolved by then;
    // `WiFi.reconnect()` is documented as idempotent in practice.
    uint32_t last_reconnect_kick_ms = millis();

    // Pre-register heartbeep + ir_poll_interval so cycle 1's poll_all
    // pulls them live — otherwise a live override would only kick in
    // starting with cycle 3. Same rationale as the Particle shim.
    (void)g_cfg.get_int("heartbeep",        HEARTBEEP_DEFAULT);
    (void)g_cfg.get_int("ir_poll_interval", IR_POLL_INTERVAL_DEFAULT);
    (void)g_cfg.get_int("fw_poll_interval", FW_POLL_INTERVAL_DEFAULT);
    (void)g_cfg.get_int("latency_display",  0);
    // wifi_ssid / wifi_password are string-typed, fetched via the
    // dedicated Stra2usClient::wifi_ssid()/wifi_password() accessors
    // (driven by poll_all's string-fallback block). No get_int
    // pre-register needed.
    (void)g_cfg.get_int("max_brightness",   MAX_BRIGHTNESS);
    // Snapshot-buffer KV pre-registers (FAILURE_TRIAGE.md §1). Mirror of
    // the heartbeep/ir_poll/etc. block above — primes the cache so
    // cycle-1's poll_all picks up live overrides from the first iteration.
    (void)g_cfg.get_int("snapshot_buffer_frames",  SNAPSHOT_BUFFER_FRAMES_DEFAULT);
    (void)g_cfg.get_int("dump_now",                0);
    (void)g_cfg.get_int("snapshot_seeks_spike",    5);
    (void)g_cfg.get_int("snapshot_heap_low",       5000);
    (void)g_cfg.get_int("snapshot_agent_drop_pct", 50);
    (void)g_cfg.get_int("trace_mode",              0);

    for (;;) {
        uint32_t now = millis();

        // Reconnect kick. Runs before any network attempt in this
        // iteration so a wedged radio gets a nudge before the rest of
        // the loop wastes cycles on no-op publishes. Heartbeat-visible
        // as `err=net:reconnect_kick offline=Nms` for fleet-wide
        // diagnostics. When WiFi *is* connected, bump the timestamp so
        // a brief future drop gets a full ir_poll_interval grace
        // period before the next kick — avoids hammering on flaky
        // networks.
        if (WiFi.status() != WL_CONNECTED) {
            int recon_int_s = g_cfg.get_int("ir_poll_interval", IR_POLL_INTERVAL_DEFAULT);
            if (recon_int_s < 60) recon_int_s = 60;
            uint32_t recon_int_ms = (uint32_t)recon_int_s * 1000UL;
            if (now - last_reconnect_kick_ms >= recon_int_ms) {
                Serial.printf("[net] reconnect kick (offline %lums, status=%d)\n",
                              (unsigned long)(now - last_reconnect_kick_ms),
                              (int)WiFi.status());
                critterchron::g_errlog.record(critterchron::ErrCat::Net,
                                              "reconnect_kick offline=%lums",
                                              (unsigned long)(now - last_reconnect_kick_ms));
                WiFi.reconnect();
                last_reconnect_kick_ms = now;
            }
        } else {
            last_reconnect_kick_ms = now;
        }

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
            int s = g_cfg.publish(STRA2US_TELEMETRY_TOPIC, msg);
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
            int s = g_cfg.publish(STRA2US_TELEMETRY_TOPIC, msg);
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
            int s = g_cfg.publish(STRA2US_TELEMETRY_TOPIC, msg);
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

        // FAILURE_TRIAGE.md §1: snapshot config + dump path. Runs after
        // telemetry_cycle so the freshly-poll_all'd KV cache reflects any
        // server-side changes (`dump_now`, `snapshot_buffer_frames`, etc.)
        // immediately. No-ops when the feature is dormant (frames=0).
        critterchron::snapshot::configure(
            (uint16_t)g_cfg.get_int("snapshot_buffer_frames",
                                    SNAPSHOT_BUFFER_FRAMES_DEFAULT));
        critterchron::snapshot::set_thresholds(
            g_cfg.get_int("snapshot_seeks_spike",     5),
            g_cfg.get_int("snapshot_heap_low",        5000),
            g_cfg.get_int("snapshot_agent_drop_pct",  50));
        critterchron::snapshot::check_dump_now_kv(
            g_cfg.get_int("dump_now", 0));

        // FAILURE_TRIAGE.md §2: continuous trace mode. trace_mode KV is
        // an int = "publish every N seconds" (0 = off). Reuses §1's
        // ring + encode + publish; differentiated by the `trigger=trace`
        // label, which routes the publish to the per-device trace topic
        // below. Operator must set this with TTL via `tools/trace_on.py`
        // — server-side TTL is the safety net against forgotten traces.
        {
            int trace_period_s = g_cfg.get_int("trace_mode", 0);
            static uint32_t s_last_trace_pub_ms = 0;
            if (trace_period_s > 0) {
                uint32_t period_ms = (uint32_t)trace_period_s * 1000UL;
                if (s_last_trace_pub_ms == 0 ||
                    now - s_last_trace_pub_ms >= period_ms) {
                    critterchron::snapshot::force_dump(
                        critterchron::snapshot::trigger_name::TRACE, "");
                    s_last_trace_pub_ms = now;
                }
            } else {
                s_last_trace_pub_ms = 0;  // reset cadence on disable
            }
        }

        if (critterchron::snapshot::dump_pending()) {
            // Static so the 4KB doesn't sit on the tel-task stack — see
            // debug_ota_hardfault_stack.md re: tel-thread stack pressure.
            static char snap_buf[SNAPSHOT_PUBLISH_BUF_BYTES];
            uint32_t now_unix = (uint32_t)time(nullptr);
            size_t n = critterchron::snapshot::encode(
                snap_buf, sizeof(snap_buf), DEVICE_NAME, now_unix);
            // Route by trigger label: §2 trace dumps go to the per-device
            // trace topic; everything else (manual / heap_low / spikes)
            // goes to the shared snapshot topic.
            const char* trig = critterchron::snapshot::pending_trigger();
            const bool is_trace =
                (strcmp(trig, critterchron::snapshot::trigger_name::TRACE) == 0);
            const char* topic = is_trace
                ? STRA2US_TRACE_TOPIC
                : STRA2US_SNAPSHOT_TOPIC;
            if (n > 0) {
                g_cfg.connect();
                int pub = g_cfg.publish(topic, snap_buf);
                Serial.printf("[snap] publish=%d bytes=%u trigger=%s\n",
                              pub, (unsigned)n, trig);
                if (pub >= 200 && pub < 300) {
                    critterchron::snapshot::clear_pending();
                } else {
                    critterchron::g_errlog.record(
                        critterchron::ErrCat::Net,
                        "snap publish=%d", pub);
                }
            }
        }

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

        // OTA Firmware poll on a separate, slower cadence (default 1 day).
        // Same connect/close bracket as ir_poll so the sidecar + blob GETs
        // don't interleave with concurrent publishes. fw_poll itself
        // short-circuits on missing `fw_target` pointer, so this is a
        // cheap no-op when no firmware target is staged. On a successful
        // OTA, fw_poll calls ESP.restart() and never returns from this
        // line.
        int fw_int_s = g_cfg.get_int("fw_poll_interval", FW_POLL_INTERVAL_DEFAULT);
        if (fw_int_s < 60) fw_int_s = 60;
        uint32_t fw_int_ms = (uint32_t)fw_int_s * 1000UL;
        if (fw_first || (millis() - last_fw_poll_ms >= fw_int_ms)) {
            g_cfg.connect();
            g_cfg.fw_poll();
            g_cfg.close();
            last_fw_poll_ms = millis();
            fw_first = false;
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

    // Compute the running image's SHA256 once and stash it in Stra2usClient
    // for use in the heartbeat (`fw_sha=...`) and by fw_poll's "are we
    // already running this firmware" short-circuit. One-shot: the running
    // image doesn't change without a reboot, and reboot reruns setup().
#if CRIT_HAVE_STRA2US
    {
        char fw_sha[65];
        compute_running_fw_sha(fw_sha);
        if (fw_sha[0]) g_cfg.set_running_fw_sha(fw_sha);
    }
#endif
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

    // Procyon rescue WiFi: load NVS-persisted target cred (or fall
    // back to compiled-in WIFI_SSID/WIFI_PASSWORD if NVS is empty),
    // then kick the cycler at the target slot. wifi_cycle_step_ in
    // the main loop will swap to procyon if the target doesn't
    // associate within WIFI_PHASE_BUDGET_MS.
    load_target_creds_from_nvs_();
    WiFi.begin(g_target_ssid_, g_target_pw_);
    g_wifi_phase            = WIFI_TRY_TARGET;
    g_wifi_phase_started_ms = millis();

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

    // Procyon WiFi cycler: swap target↔procyon when the current slot
    // fails to associate within WIFI_PHASE_BUDGET_MS. No-op when
    // WL_CONNECTED. Cheap to run every iteration.
    wifi_cycle_step_(now);

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

            // Time-of-day schedule override on max_b. See
            // parse_brightness_schedule above for wire format and parsing.
            // Re-parse on change (single strncmp/tick steady-state); apply
            // when current local time falls inside a defined segment.
            // g_real_clock is the un-wobbled time source — we want real
            // wall time, not the engine's wobbled display clock.
#if CRIT_HAVE_STRA2US
            const char* sched_str = g_cfg.brightness_schedule();
            if (strncmp(sched_str, s_sched_last, sizeof(s_sched_last)) != 0) {
                strncpy(s_sched_last, sched_str, sizeof(s_sched_last) - 1);
                s_sched_last[sizeof(s_sched_last) - 1] = '\0';
                s_sched_parse_ok = parse_brightness_schedule(s_sched_last);
            }
            bool sched_in_use = false;
            if (s_sched_parse_ok && s_sched_count > 0 && g_real_clock.valid()) {
                time_t local = g_real_clock.wall_now()
                             + (time_t)(g_real_clock.zone_offset_hours() * 3600.0f);
                struct tm tm;
                gmtime_r(&local, &tm);
                uint16_t now_min = (uint16_t)(tm.tm_hour * 60 + tm.tm_min);
                int sched_min = 0;
                int sched_max = schedule_match_max_bri(now_min, &sched_min);
                if (sched_max >= 0) {
                    max_b = sched_max;
                    // Two-value form (`HH:MM-HH:MM:min-max`) also pins
                    // min. sched_min == 0 means single-value form —
                    // keep the device's `min_brightness` as already
                    // pulled above.
                    if (sched_min > 0) min_b = sched_min;
                    sched_in_use = true;
                }
            }
            g_sched_in_use    = sched_in_use;
            g_sched_parse_err = (s_sched_last[0] != '\0' && !s_sched_parse_ok);
#endif

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

        // FAILURE_TRIAGE.md §1: record one ring-buffer frame per physics
        // tick. No-op when `snapshot_buffer_frames` KV is 0 (default).
        // `rend_max_us` arg is 0 in v1 — the per-tick render dt isn't
        // lifted out of the render block; the heartbeat-window aggregate
        // is in `g_rend_max_us` if we want it later.
        static uint32_t s_phys_tick_counter = 0;
        ++s_phys_tick_counter;
        critterchron::snapshot::append(
            s_phys_tick_counter, now,
            (uint16_t)g_engine.liveAgentCount(),
            (uint32_t)ESP.getFreeHeap(),
            (uint32_t)g_engine.metrics().failed_seeks,
            /*phys_us=*/ dt,
            /*rend_us=*/ 0);
    }

    // Render tick
    if (now - last_render_tick >= RENDER_TICK_MS) {
        last_render_tick = now;

#if CRIT_HAVE_WIFI
        // Wifi-creds-applied signal. Mirror of the Particle render-side
        // block. Animated blue chase on the bottom row, gated on
        // (within signal window) AND (currently joined to procyon).
        // Mutually-exclusive with the latency sparkline overlay (same
        // row); when the signal ends, falling-edge clear_overlay()
        // wipes the chase, and the next telemetry_cycle reasserts
        // latency if that's enabled.
        {
            String cur_ssid_str = WiFi.SSID();
            const char* cur_ssid_r = cur_ssid_str.c_str();
            bool on_procyon = cur_ssid_r &&
                              strcmp(cur_ssid_r, PROCYON_SSID) == 0;
            bool sig_active = on_procyon && now < g_wifi_signal_until_ms;
            static bool was_signaling = false;
            if (sig_active) {
                memset(g_wifi_apply_chase_row_, 0,
                       sizeof(g_wifi_apply_chase_row_));
                int span = GRID_WIDTH + 4;             // small over-roll
                int pos  = (int)((now / 80) % (uint32_t)span);
                for (int t = 0; t < 4; ++t) {          // head + 3-px tail
                    int x = pos - t;
                    if (x < 0 || x >= GRID_WIDTH) continue;
                    uint8_t bri = (uint8_t)(255 - (t * 64));
                    g_wifi_apply_chase_row_[x * 3 + 2] = bri;
                }
                g_sink.set_overlay_row(GRID_HEIGHT - 1,
                                       g_wifi_apply_chase_row_);
            } else if (was_signaling) {
                g_sink.clear_overlay();
            }
            was_signaling = sig_active;
        }
#endif

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
        diag_next_ms = now + 10000;   // 10s rollup; was 1s, too chatty
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

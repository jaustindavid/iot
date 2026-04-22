//
// critterchron_particle.cpp — Particle DeviceOS entry point.
//
// Shared across all Particle targets (Photon, Photon 2, Argon, …) — the
// code touches only DeviceOS APIs (TCPClient, WiFi, Time, analogRead,
// System.*), so one translation unit serves every WiFi-capable Particle
// device. Per-device differences live entirely in hal/devices/<device>.h:
// MATRIX_PIN names a GPIO or SPI, LIGHT_SENSOR_TYPE opts into CDS, etc.
//
// Per-platform Makefiles (hal/photon2/, hal/argon/, …) copy this file and
// the rest of hal/particle/src/ into their own src/ at build time.
//
// Components wired here:
//   - NeoPixelSink           (LED geometry + brightness)
//   - ParticleTimeSource     (DeviceOS RTC)
//   - WobblyTimeSource       (decorator; drifting virtual clock)
//   - LightSensor            (ambient brightness, CDS photoresistor)
//   - Stra2usClient          (live-tunable KV + heartbeat on a worker thread)
//

#if defined(PLATFORM_ID)

#include "Particle.h"
#include "creds.h"
#include "CritterEngine.h"
#include "interface/Config.h"
#include "LightSensor.h"
#include "NeoPixelSink.h"
#include "ParticleTimeSource.h"
#include "WobblyTimeSource.h"
#if defined(STRA2US_HOST)
#include "Stra2usClient.h"
#endif

SYSTEM_THREAD(ENABLED);

#define APP_VERSION __DATE__ " " __TIME__

#if !defined(GRID_WIDTH) || !defined(GRID_HEIGHT)
#error "creds.h must define GRID_WIDTH and GRID_HEIGHT"
#endif

// Physics tick rate is a property of the .crit script. Read live from
// critter_ir::RUNTIME_TICK_MS so an OTA IR swap picks up the new script's
// TICK line — a constexpr read of the compile-time default would pin us
// to whatever the baked-in .crit specified. Device header must not
// override it; the sim and the hardware share the same contract.
static inline uint32_t physics_tick_ms() { return critter_ir::RUNTIME_TICK_MS; }

// Render tick: 20ms = 50Hz on capable hardware (Photon 2 / Argon — DMA-based
// WS2812 driver, no IRQ impact). OG Photon / P1 uses a bit-banged driver
// that disables interrupts for ~7ms per show(); at 50Hz that's ~35% IRQs-off
// and the WICED Wi-Fi stack gets starved out of associating. Override per
// device in hal/devices/<name>.h — rico (P1) sets this to 50ms/20Hz.
#ifndef RENDER_TICK_MS
#define RENDER_TICK_MS 20
#endif

// Telemetry worker stack. The worker does HTTPS-ish TCP + msgpack parse +
// HMAC-SHA256 signing — deepest call chains measured run ~3KB, so 5KB is
// 1.5x headroom. Was 10KB historically; dropped for OG Photon's ~80KB
// user-RAM budget. Bump in a device header if a future consumer (deeper
// IR-poll, bigger SHA contexts, etc.) pushes the stack harder.
#ifndef TELEMETRY_STACK_BYTES
#define TELEMETRY_STACK_BYTES 5120
#endif

// Brightness is a 0..255 scale factor applied per channel at the sink.
// Device headers override for their hardware / ambient tuning.
#ifndef MAX_BRIGHTNESS
#define MAX_BRIGHTNESS 102       // ~40% of 255
#endif
#ifndef MIN_BRIGHTNESS
#define MIN_BRIGHTNESS 3         // floor; see scale_ch nonzero-preserve in NeoPixelSink
#endif
#ifndef TIMEZONE_OFFSET_HOURS
#define TIMEZONE_OFFSET_HOURS -5.0f
#endif

// Night-mode Schmitt thresholds. Mirror the defaults in devices/device_name.h
// so a device header that forgot to supply them still gets reasonable
// behavior instead of a compile error. Anchored at MIN_BRIGHTNESS — see
// device_name.h for the design rationale (floor-clamp hue-distortion regime).
#ifndef NIGHT_ENTER_BRIGHTNESS
#define NIGHT_ENTER_BRIGHTNESS (MIN_BRIGHTNESS)
#endif
#ifndef NIGHT_EXIT_BRIGHTNESS
#define NIGHT_EXIT_BRIGHTNESS  (MIN_BRIGHTNESS + 4)
#endif

// Heartbeat cadence in seconds. Compiled-in default when Stra2us has no
// live override yet. Overridable per device (drop to ~15 for debugging);
// floored at 10s in the worker loop.
#ifndef HEARTBEEP_DEFAULT
#define HEARTBEEP_DEFAULT 300
#endif

// Particle-cloud failsafe heartbeat. Fires from the same worker, topic
// "stra2us". When the Stra2us server is down we still need to know the
// device is alive and roughly why — Particle cloud keeps working even when
// our TCP path to Stra2us is broken, and a device without cloud visibility
// is a device we can't even OTA-flash. Floored at 60s to stay well under
// Particle's 1-event/sec rate limit.
#ifndef CLOUD_HEARTBEEP_DEFAULT
#define CLOUD_HEARTBEEP_DEFAULT 300
#endif

// OTA IR poll cadence in seconds. Decoupled from the heartbeat: pointer
// changes are human-scale events, and running ir_poll() every heartbeat
// added 1-2 HTTP round trips to the hot path. Floored at 60s in the worker
// loop. Default 1200s = 20 min — fresh pointers propagate within that
// window.
#ifndef IR_POLL_INTERVAL_DEFAULT
#define IR_POLL_INTERVAL_DEFAULT 1200
#endif

// Light sensor pin defaults match the coaticlock hardware layout: a CDS
// photoresistor in a voltage divider between a powered pin (HIGH) and a
// grounded pin (LOW), with the midpoint read on an analog pin. Device
// headers override when wiring differs.
#ifndef LIGHT_SENSOR_PWR_PIN
#define LIGHT_SENSOR_PWR_PIN A0
#endif
#ifndef LIGHT_SENSOR_SIG_PIN
#define LIGHT_SENSOR_SIG_PIN A1
#endif
#ifndef LIGHT_SENSOR_GND_PIN
#define LIGHT_SENSOR_GND_PIN A2
#endif

// Config surface: live Stra2us KV when creds are defined, else a no-op
// StaticConfig so consumers always get their compiled-in defaults. Both
// derive from Config, so `g_cfg` is passed to consumers as `const Config&`.
#if defined(STRA2US_HOST)
static Stra2usClient g_cfg(STRA2US_HOST, STRA2US_PORT,
                           STRA2US_CLIENT_ID, STRA2US_SECRET_HEX,
                           STRA2US_APP, DEVICE_NAME);
#else
static StaticConfig  g_cfg;
#endif

static NeoPixelSink         g_sink;
static ParticleTimeSource   g_real_clock(TIMEZONE_OFFSET_HOURS);
static WobblyTimeSource     g_clock(g_real_clock, g_cfg);
static critterchron::CritterEngine g_engine(g_sink, g_clock);

#if defined(LIGHT_SENSOR_TYPE)
static LightSensor          g_light(g_cfg);

// Boot-time sensor diagnostic. The 5Hz light block in loop() stashes the
// first BOOT_LIGHT_SAMPLES raw reads into this buffer; the telemetry
// worker fires a one-shot OOB publish with the samples once the buffer
// fills. Goal is to see directly whether the drain+settle put us in a
// good place, whether there's still a slow exponential rise (cap theory)
// or a stuck value (something else). ~60 bytes of RAM, ~200-byte payload
// on the wire, fires once per boot.
//
// Ordering: main loop writes the array + increments the count, sets
// g_boot_light_ready after the final write. The telemetry thread reads
// ready, then the array. On single-core Cortex-M under FreeRTOS the
// `volatile` + write-order-before-flag is the idiomatic fence; strict
// C++ memory model wants std::atomic but the risk here is cosmetic
// (a partially-formed report) and the fix would have to travel to
// g_bri et al. above first.
constexpr int        BOOT_LIGHT_SAMPLES = 30;   // 6s at 5Hz
static uint16_t      g_boot_light_raws[BOOT_LIGHT_SAMPLES];
static int           g_boot_light_count = 0;
static volatile bool g_boot_light_ready = false;
static bool          g_boot_light_sent  = false;  // tel-thread-only
#endif

// Telemetry-visible state. The worker thread reads these as snapshots;
// ARM aligned 32-bit writes are atomic enough for a "current value"
// heartbeat. Don't fold these into something that needs locking.
//
// The phys/rend timing snapshots are written by the main loop at each 1s
// rollup boundary — they reflect the last closed second, never an in-
// progress accumulator, so a telemetry read always sees a coherent window.
static volatile uint8_t  g_bri             = MAX_BRIGHTNESS;
static volatile uint8_t  g_bri_min         = MIN_BRIGHTNESS;
static volatile uint8_t  g_bri_max         = MAX_BRIGHTNESS;
static volatile uint32_t g_phys_avg_us     = 0;
static volatile uint32_t g_phys_max_us     = 0;
static volatile uint32_t g_rend_avg_us     = 0;
static volatile uint32_t g_rend_max_us     = 0;
// phys = whole tick; interp = opcode dispatch in processAgent; astar = the
// pathfinding cost inside processAgent. interp + astar ≤ phys (the gap is
// spawn rules + compaction + convergence). Baseline knobs for OTA IR — if a
// parsed-from-text blob drives interpretation cost up, these are where it
// shows up before `phys` breaches its budget.
static volatile uint32_t g_interp_avg_us   = 0;
static volatile uint32_t g_interp_max_us   = 0;
static volatile uint32_t g_astar_avg_us    = 0;
static volatile uint32_t g_astar_max_us    = 0;
static Thread*           g_tel_thread      = nullptr;

static unsigned long last_physics_tick = 0;
static unsigned long last_render_tick  = 0;
static int           last_sync_minute  = -1;

// If the previous boot was a crash (panic, watchdog, update-error), hold the
// engine off for RESCUE_HOLD_MS so a replacement firmware can be OTA-flashed
// without physically resetting the device. Clean reasons (power, pin, user,
// firmware update) start immediately.
static constexpr uint32_t RESCUE_HOLD_MS = 60000;
static bool          g_rescue_mode       = false;
static unsigned long g_rescue_start_ms   = 0;

// P1/Photon has a VBAT-backed RTC, so Time.isValid() latches true from the
// previous boot's time before Wi-Fi has even associated. Without a gate the
// device would skip the spinner and run the simulation on stale RTC time
// while still disconnected (white breathe). Hold rendering until we've seen
// the cloud once and forced a fresh syncTime; if cloud never comes, fall
// back to RTC after CLOUD_WAIT_FALLBACK_MS so an offline device still shows
// *something* rather than spinning forever.
static constexpr uint32_t CLOUD_WAIT_FALLBACK_MS = 60000;
static bool          g_cloud_seen          = false;
static unsigned long g_cloud_wait_start_ms = 0;

static SerialLogHandler logHandler(LOG_LEVEL_INFO);

// Local wall-clock minute from a TimeSource. The shim uses this to detect
// minute rollovers — must read the same (wobbled) clock the engine writes
// from, or the display lags behind the virtual time by up to a minute.
static int local_minute(const TimeSource& c) {
    time_t local = c.wall_now() + (time_t)(c.zone_offset_hours() * 3600.0f);
    struct tm tm;
    gmtime_r(&local, &tm);
    return tm.tm_min;
}

static bool is_crash_reset(int reason) {
    switch (reason) {
        case RESET_REASON_PANIC:
        case RESET_REASON_WATCHDOG:
        case RESET_REASON_UPDATE_ERROR:
        case RESET_REASON_UPDATE_TIMEOUT:
            return true;
        default:
            return false;
    }
}

#if defined(STRA2US_HOST)
// Heartbeat + KV poll, called from the telemetry worker. Heartbeat payload
// is diagnostic — grep-friendly k=v tokens, no JSON overhead. Policy:
// fresh connect on every cycle, hard close at the end — a stuck socket
// never poisons the next heartbeat. Poll cadence is the heartbeat cadence:
// configs don't change fast, and two thread tasks is one too many.
//
// Returns the HTTP status of the publish (or <=0 on TCP/network failure,
// 0 if we punted because time isn't synced yet). The worker inspects the
// status to decide the next attempt cadence.
static int telemetry_cycle() {
    if (!Time.isValid()) return 0;

    // 256 bytes was snug before the light-sensor diagnostic fragment below
    // widened the payload. Bump to 320 to leave headroom; truncation is still
    // handled by the final NUL-terminate at the bottom, but we'd lose the new
    // `light=(...)` fragment on every heartbeat without the extra bytes.
    char report[320];
    int  rssi = -127;
    if (WiFi.ready()) {
        WiFiSignal sig = WiFi.RSSI();
        rssi = sig.getStrength();
    }
    const auto& m = g_engine.metrics();
    // Timing fields use (avg<max<budget)us — ordering implies "avg is within
    // max is within budget", so a glance at the numbers tells you both
    // current cost and headroom. Budgets are the nominal tick periods.
    const uint32_t PHYS_BUDGET_US = physics_tick_ms() * 1000UL;
    constexpr uint32_t REND_BUDGET_US = (uint32_t)RENDER_TICK_MS  * 1000UL;
    const char* script = g_cfg.ir_loaded_script();
    const char* sha    = g_cfg.ir_loaded_sha();
    char script_tag[IR_SCRIPT_NAME_MAX + 16];
    if (script && *script && sha && *sha) {
        // name + first 8 hex of sha (e.g. `script=thyme@ab12cd34`) so the
        // server can distinguish re-publishes under the same name without
        // paying for a full 64-char digest in every heartbeat.
        snprintf(script_tag, sizeof(script_tag), "%.*s@%.8s",
                 (int)IR_SCRIPT_NAME_MAX, script, sha);
    } else {
        snprintf(script_tag, sizeof(script_tag), "default");
    }
    int rlen = snprintf(report, sizeof(report),
        "up=%lu rssi=%d mem=%lu rst=%d fw=%s script=%s "
        "bri=(%u<%u<%u) "
        "phys=(%lu<%lu<%lu)us rend=(%lu<%lu<%lu)us "
        "interp=(%lu<%lu)us astar=(%lu<%lu)us "
        "agents=%u seeks_fail=%lu",
        (unsigned long)System.uptime(),
        rssi,
        (unsigned long)System.freeMemory(),
        (int)System.resetReason(),
        APP_VERSION,
        script_tag,
        (unsigned)g_bri_min, (unsigned)g_bri, (unsigned)g_bri_max,
        (unsigned long)g_phys_avg_us,   (unsigned long)g_phys_max_us,   (unsigned long)PHYS_BUDGET_US,
        (unsigned long)g_rend_avg_us,   (unsigned long)g_rend_max_us,   (unsigned long)REND_BUDGET_US,
        (unsigned long)g_interp_avg_us, (unsigned long)g_interp_max_us,
        (unsigned long)g_astar_avg_us,  (unsigned long)g_astar_max_us,
        (unsigned)g_engine.liveAgentCount(),
        (unsigned long)m.failed_seeks);
    if (rlen >= (int)sizeof(report)) report[sizeof(report)-1] = '\0';

#if defined(LIGHT_SENSOR_TYPE)
    // Calibration diagnostic. `bri=(min<cur<max)` above only shows the
    // *output* of the sensor-mapping pipeline; when bri goes pathological
    // (TODO.md "Light-sensor calibration poisons itself") we need the
    // inputs — the live raw ADC read and the learned [cal_bright, cal_dark]
    // pair — to tell "raw tracking fine, mapping is wrong" apart from
    // "raw is stuck between a collapsed pair" apart from a scenario I
    // haven't thought of. Format: `light=(raw<cb<cd)` — same ordering
    // convention as bri (low-to-high in the mapping's sense: cal_bright
    // is the *low* raw value, cal_dark is the *high* one), so a glance
    // tells you whether raw is outside the learned range (good — instant
    // widen incoming) or inside it (mapping is doing what it was told).
    if (rlen > 0 && rlen < (int)sizeof(report) - 1) {
        int extra = snprintf(report + rlen, sizeof(report) - rlen,
                             " light=(%d<%d<%d)",
                             g_light.last_raw,
                             g_light.cal_bright,
                             g_light.cal_dark);
        if (extra > 0 && rlen + extra < (int)sizeof(report)) {
            rlen += extra;
        } else {
            report[sizeof(report) - 1] = '\0';
        }
    }
#endif

    g_cfg.connect();
    int pub_status = g_cfg.publish(STRA2US_APP, report);
    Log.info("telemetry: publish=%d %s", pub_status, report);
    g_cfg.poll_all();
    g_cfg.close();

    // Brightness ceiling: pull unconditionally so remote `max_brightness`
    // tuning reaches sensor-less devices too (they never call LightSensor::
    // update). On sensor devices the 200ms light block already handles this
    // via its own pull — the redundancy is cheap (cache read) and keeps
    // g_bri_max coherent for the heartbeat payload.
    int max_b = g_cfg.get_int("max_brightness", MAX_BRIGHTNESS);
    if (max_b < 0)   max_b = 0;
    if (max_b > 255) max_b = 255;
    g_bri_max = (uint8_t)max_b;
#if !defined(LIGHT_SENSOR_TYPE)
    g_sink.set_brightness(g_bri_max);
    g_bri = g_bri_max;
#endif
    return pub_status;
}

// Retry policy, driven by the publish status:
//   2xx                   — success. Next attempt at full hb cadence.
//   4xx                   — server rejected the client (bad ACL, unknown
//                           app, bad signature). Also full hb cadence —
//                           hammering a definite "no" is impolite, and
//                           the admin fixes the server side on their own
//                           schedule.
//   5xx / <=0 / no-time   — transient: server error, TCP/network failure,
//                           or we punted before sending. Exponential
//                           backoff starting at retry_floor_ms, doubling
//                           up to hb. Resets on any 2xx/4xx.
//   WiFi edge (up)        — fire immediately, regardless of timers.
static void telemetry_worker() {
    const unsigned long startup_delay_ms = 15000;
    const unsigned long retry_floor_ms   = 10000;
    unsigned long next_interval_ms = 0;           // 0 = fire asap
    unsigned long last_attempt_ms  = 0;
    unsigned long backoff_ms       = retry_floor_ms;
    unsigned long last_cloud_ms    = 0;           // 0 = never fired
    bool wifi_prev = false;
    bool first     = true;

    // Pre-register heartbeep + cloud_heartbeep + ir_poll_interval so cycle 1's
    // poll_all pulls them live. Otherwise they'd only be registered by the
    // get_int calls below (which run AFTER telemetry_cycle) and the live
    // overrides would take five minutes to kick in. Night thresholds same
    // story — registered here so the very first 200ms light-block poll
    // sees the live value on a freshly-flashed device.
    (void)g_cfg.get_int("heartbeep",               HEARTBEEP_DEFAULT);
    (void)g_cfg.get_int("cloud_heartbeep",         CLOUD_HEARTBEEP_DEFAULT);
    (void)g_cfg.get_int("ir_poll_interval",        IR_POLL_INTERVAL_DEFAULT);
    (void)g_cfg.get_int("night_enter_brightness",  NIGHT_ENTER_BRIGHTNESS);
    (void)g_cfg.get_int("night_exit_brightness",   NIGHT_EXIT_BRIGHTNESS);

    // OTA IR poll timer. Separate from the heartbeat cadence because pointer
    // changes propagate at human scale, not telemetry scale. Initialized so
    // the first poll fires on the first iteration after startup_delay_ms,
    // giving the network stack time to settle but still picking up pending
    // OTA targets promptly on reboot.
    unsigned long last_ir_poll_ms = 0;
    bool          ir_first        = true;

    while (true) {
        if (millis() < startup_delay_ms) { delay(100); continue; }
        unsigned long now = millis();

        // WiFi state flag is free to read — if we're offline we skip the
        // expensive TCP path entirely. Rising edge forces an immediate
        // heartbeat so a reconnect is visible to the server promptly.
        bool wifi_now = WiFi.ready();
        bool edge     = wifi_now && !wifi_prev;
        wifi_prev     = wifi_now;
        if (!wifi_now) { delay(100); continue; }

        bool due = first || edge ||
                   (now - last_attempt_ms >= next_interval_ms);
        if (!due) { delay(100); continue; }

        last_attempt_ms = now;
        first = false;
        int status = telemetry_cycle();

#if defined(LIGHT_SENSOR_TYPE)
        // OOB boot-light diagnostic. One-shot per boot, fires once the main
        // loop has captured its first BOOT_LIGHT_SAMPLES raw readings.
        // Purpose is distributed-console observability of the post-drain
        // settling curve — "is the cap-discharge hypothesis right?" becomes
        // answerable from telemetry alone. Format is free-form, matches the
        // heartbeat's grep-friendly k=v style. Not gated on telemetry_cycle
        // success: if the regular heartbeat failed, this cycle will retry
        // both next time — connect/close are per-block.
        if (g_boot_light_ready && !g_boot_light_sent) {
            // Static, not stack: the tel worker has a modest stack and 512B
            // of locals plus the snprintf frames was enough to SOS on the
            // Photon 2. Safe to reuse as static since this block is
            // one-shot per boot (gated by g_boot_light_sent).
            static char msg[512];
            int n = snprintf(msg, sizeof(msg),
                             "boot_light up=%lu n=%d raws=",
                             (unsigned long)System.uptime(),
                             BOOT_LIGHT_SAMPLES);
            for (int i = 0; i < BOOT_LIGHT_SAMPLES && n < (int)sizeof(msg) - 8; ++i) {
                n += snprintf(msg + n, sizeof(msg) - n,
                              "%s%u", i ? "," : "",
                              (unsigned)g_boot_light_raws[i]);
            }
            msg[sizeof(msg) - 1] = '\0';
            g_cfg.connect();
            int bs = g_cfg.publish(STRA2US_APP, msg);
            g_cfg.close();
            Log.info("boot_light publish=%d %s", bs, msg);
            g_boot_light_sent = true;
        }
#endif

        // Read heartbeep AFTER the cycle so we benefit from any override
        // pulled down in this cycle's poll_all. Otherwise a fresh boot with
        // an empty cache uses HEARTBEEP_DEFAULT to schedule the next fire,
        // and the live override only takes effect starting with cycle 3.
        int hb = g_cfg.get_int("heartbeep", HEARTBEEP_DEFAULT);
        if (hb < 10) hb = 10;
        unsigned long hb_ms = (unsigned long)hb * 1000UL;

        if (status >= 200 && status < 500) {
            // Success or client-rejected — settle into full cadence.
            next_interval_ms = hb_ms;
            backoff_ms       = retry_floor_ms;
        } else {
            // Transient failure — exponential backoff, capped at hb.
            next_interval_ms = backoff_ms;
            backoff_ms = (backoff_ms * 2 < hb_ms) ? backoff_ms * 2 : hb_ms;
        }

        // OTA IR poll, on its own slow cadence. Runs after telemetry_cycle
        // rather than inside it so a stalled ir_poll can't delay the next
        // heartbeat — worst case we skip an OTA check, not a heartbeat.
        // First pass fires immediately on startup so a pointer set while the
        // device was offline still gets picked up on boot without a 20-min
        // wait.
        int ir_int_s = g_cfg.get_int("ir_poll_interval", IR_POLL_INTERVAL_DEFAULT);
        if (ir_int_s < 60) ir_int_s = 60;
        unsigned long ir_int_ms = (unsigned long)ir_int_s * 1000UL;
        if (ir_first || (now - last_ir_poll_ms >= ir_int_ms)) {
            g_cfg.connect();
            g_cfg.ir_poll();
            g_cfg.close();
            last_ir_poll_ms = now;
            ir_first = false;
        }

        // Failsafe heartbeat to Particle cloud. Fires at most once per
        // cloud_heartbeep seconds, independent of the Stra2us status —
        // specifically so a broken Stra2us doesn't silence device
        // observability. Payload is grep-friendly k=v with the last Stra2us
        // status and the URL we're trying, so "is the server down, or is the
        // device misconfigured" is answerable from the event stream alone.
        int cloud_hb = g_cfg.get_int("cloud_heartbeep", CLOUD_HEARTBEEP_DEFAULT);
        if (cloud_hb < 60) cloud_hb = 60;
        unsigned long cloud_hb_ms = (unsigned long)cloud_hb * 1000UL;
        if (Particle.connected() &&
            (last_cloud_ms == 0 || now - last_cloud_ms >= cloud_hb_ms)) {
            char msg[128];
            snprintf(msg, sizeof(msg),
                     "up=%lu s2s=%d url=%s:%d fw=%s",
                     (unsigned long)System.uptime(),
                     status, STRA2US_HOST, STRA2US_PORT, APP_VERSION);
            Particle.publish("stra2us", msg, PRIVATE);
            last_cloud_ms = now;
        }

        delay(100);
    }
}
#endif  // STRA2US_HOST

// Rescue-mode indicator: amber chase across the strip. Same palette as the
// spinner (amber not blue) so it's obviously a different state at a glance.
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

void setup() {
    g_sink.begin();
    g_sink.set_brightness(MAX_BRIGHTNESS);

#if defined(LIGHT_SENSOR_TYPE)
    // CDS voltage divider: power HIGH, ground LOW, signal on an analog pin.
    //
    // Drain-then-power sequence. The previous "just set the pins" form
    // was vulnerable to a persistent-cap failure: a bypass cap on the
    // sensor breakout (between SIG↔GND, or a decoupling cap between
    // PWR↔GND) left partially charged by the bootloader would bias the
    // first samples, and the auto-calibrator would latch onto that
    // wrong value and hold it for minutes (dark-CDS RC is slow). Across
    // a reflash we saw raw stuck at 505 in a dark room that reads ~4030
    // on a clean boot. The fix is to collapse every cap in the divider
    // to a known 0V before we power it up: drive all three pins LOW for
    // a millisecond, then release SIG to high-Z input, bring PWR up,
    // and wait long enough for the cap to recharge through R_series
    // (tens of ms for reasonable cap values with a ~10k series resistor).
    pinMode(LIGHT_SENSOR_PWR_PIN, OUTPUT);
    pinMode(LIGHT_SENSOR_SIG_PIN, OUTPUT);
    pinMode(LIGHT_SENSOR_GND_PIN, OUTPUT);
    digitalWrite(LIGHT_SENSOR_PWR_PIN, LOW);
    digitalWrite(LIGHT_SENSOR_SIG_PIN, LOW);
    digitalWrite(LIGHT_SENSOR_GND_PIN, LOW);
    delay(1);                                  // collapse caps
    pinMode(LIGHT_SENSOR_SIG_PIN, INPUT);      // release to high-Z
    digitalWrite(LIGHT_SENSOR_PWR_PIN, HIGH);  // sensor powered
    delay(200);                                // divider settles
#endif

    int reason = System.resetReason();
    if (is_crash_reset(reason)) {
        g_rescue_mode     = true;
        g_rescue_start_ms = millis();
        Log.warn("Rescue hold: reset_reason=%d, waiting %lums for OTA flash",
                 reason, (unsigned long)RESCUE_HOLD_MS);
    }

    Time.zone(TIMEZONE_OFFSET_HOURS);
    Particle.syncTime();

    // Explicit Wi-Fi kick. AUTOMATIC + SYSTEM_THREAD(ENABLED) is supposed
    // to do this for us, but calling it explicitly is documented-safe and
    // cheap — on OG Photon the implicit path has been flaky.
    WiFi.on();
    Particle.connect();

    if (!g_engine.begin()) {
        Log.error("CritterEngine::begin() failed");
    }
    // RNG seed: bake in something device-ish so two units diverge.
    g_engine.seedRng((uint32_t)HAL_RNG_GetRandomNumber());

    // Telemetry thread moved out of setup(): on RAM-tight devices (OG
    // Photon with ~16KB free at boot) allocating the 10KB stack here
    // starved WICED of buffers and Wi-Fi never associated. Lazy-started
    // in loop() once the cloud has actually been seen.

    Log.info("CritterChron Initialized. %dx%d rot=%d fw=%s reset=%d",
             GRID_WIDTH, GRID_HEIGHT, GRID_ROTATION, APP_VERSION, reason);
}

// Pre-sync loading spinner — drawn directly into the sink, engine idle.
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

void loop() {
    unsigned long now = millis();

#if defined(LIGHT_SENSOR_TYPE)
    // Ambient light sample @ 5Hz. The sensor gives us a target brightness;
    // an EMA (α ≈ 1/50) smooths it against the current value so a flipped
    // light switch fades over ~10 seconds instead of popping. Q8 fixed-point
    // so the slow drift doesn't lose to integer truncation.
    static unsigned long last_light_ms     = 0;
    static unsigned long last_light_log_ms = 0;
    static uint32_t      bri_q8            = (uint32_t)MAX_BRIGHTNESS << 8;
    if (now - last_light_ms >= 200) {
        last_light_ms = now;
        int raw = analogRead(LIGHT_SENSOR_SIG_PIN);

        // Boot-time capture: stash the first BOOT_LIGHT_SAMPLES raws for the
        // one-shot OOB publish. Keep the writes before the ready-flag set so
        // the tel-thread never reads a half-populated array.
        if (g_boot_light_count < BOOT_LIGHT_SAMPLES) {
            g_boot_light_raws[g_boot_light_count] = (uint16_t)raw;
            g_boot_light_count++;
            if (g_boot_light_count == BOOT_LIGHT_SAMPLES) {
                g_boot_light_ready = true;
            }
        }

        int min_b = g_cfg.get_int("min_brightness", MIN_BRIGHTNESS);
        int max_b = g_cfg.get_int("max_brightness", MAX_BRIGHTNESS);
        if (min_b < 0)   min_b = 0;
        if (max_b > 255) max_b = 255;
        if (min_b > max_b) min_b = max_b;
        uint8_t target = g_light.update(raw, (uint8_t)min_b, (uint8_t)max_b);
        uint32_t target_q8 = (uint32_t)target << 8;
        bri_q8 = bri_q8 + ((int32_t)(target_q8 - bri_q8) / 50);
        uint8_t bri = (uint8_t)(bri_q8 >> 8);
        g_sink.set_brightness(bri);
        g_bri = bri;
        g_bri_min = (uint8_t)min_b;
        g_bri_max = (uint8_t)max_b;

        // Schmitt trigger into night mode. Drive off the smoothed `bri`
        // (the value that actually reaches the panel) rather than the
        // instantaneous sensor read, so a single dark sample doesn't
        // flip the palette. Enter at the sink's floor — the brightness
        // regime where the NeoPixel floor-clamp starts rounding colors
        // toward white — and exit a few units above so a candle flicker
        // doesn't oscillate. Night palette only exists for blobs that
        // declared one; engine.setNightMode on a bare blob is a no-op
        // at render time (resolveColor falls through to day palette).
        //
        // Both thresholds are Stra2us-tunable (`night_enter_brightness`,
        // `night_exit_brightness`). Compile-time defaults from the device
        // header remain the seed. Clamp exit >= enter + 1 on read so a
        // misconfigured KV (exit <= enter) can't deadlock the trigger
        // into a single state — minimum 1 LSB of hysteresis is preserved
        // even under adversarial input.
        int ne = g_cfg.get_int("night_enter_brightness", NIGHT_ENTER_BRIGHTNESS);
        int nx = g_cfg.get_int("night_exit_brightness",  NIGHT_EXIT_BRIGHTNESS);
        if (ne < 0)   ne = 0;
        if (ne > 255) ne = 255;
        if (nx < ne + 1) nx = ne + 1;
        if (nx > 255)    nx = 255;
        if (!g_engine.nightMode() && bri <= (uint8_t)ne) {
            g_engine.setNightMode(true);
            Log.info("night: ON (bri=%u <= %d)", (unsigned)bri, ne);
        } else if (g_engine.nightMode() && bri >= (uint8_t)nx) {
            g_engine.setNightMode(false);
            Log.info("night: OFF (bri=%u >= %d)", (unsigned)bri, nx);
        }

        if (now - last_light_log_ms >= 5000) {
            last_light_log_ms = now;
            Log.info("light raw=%d cal=[%d,%d] target=%u bri=%u range=[%d,%d]",
                     raw, g_light.cal_bright, g_light.cal_dark,
                     (unsigned)target, (unsigned)bri, min_b, max_b);
        }
    }
#endif

    // Rescue hold: previous boot crashed. Keep SYSTEM_THREAD running (so the
    // cloud can OTA-flash a replacement) but don't touch the engine. Amber
    // chase signals the state visually.
    if (g_rescue_mode) {
        if (now - g_rescue_start_ms < RESCUE_HOLD_MS) {
            if (now - last_render_tick >= RENDER_TICK_MS) {
                last_render_tick = now;
                draw_rescue(now);
            }
            return;
        }
        g_rescue_mode = false;
        Log.info("Rescue hold elapsed, starting engine");
    }

    // Cloud-first gate — see CLOUD_WAIT_FALLBACK_MS commentary above.
    // Define CLOUD_WAIT_DEBUG to re-enable the per-5s progress logs and
    // spinner-draw counter that were used to diagnose the OG Photon
    // WICED/RAM-starvation bug; off by default to keep serial quiet.
    if (!g_cloud_seen) {
        if (g_cloud_wait_start_ms == 0) {
            g_cloud_wait_start_ms = now;
#if defined(CLOUD_WAIT_DEBUG)
            Log.info("cloud-wait gate entered at t=%lums: wifi.ready=%d "
                     "particle.connected=%d time.valid=%d",
                     now, (int)WiFi.ready(), (int)Particle.connected(),
                     (int)Time.isValid());
#endif
        }
        if (Particle.connected()) {
            g_cloud_seen = true;
            Particle.syncTime();
            last_sync_minute = -1;
            Log.info("cloud first-contact after %lums, forcing syncTime",
                     now - g_cloud_wait_start_ms);
#if defined(STRA2US_HOST)
            if (g_tel_thread == nullptr) {
                g_tel_thread = new Thread("telemetry", telemetry_worker,
                                          OS_THREAD_PRIORITY_DEFAULT,
                                          TELEMETRY_STACK_BYTES);
                Log.info("telemetry thread started (stack=%u)",
                         (unsigned)TELEMETRY_STACK_BYTES);
            }
#endif
        } else if (now - g_cloud_wait_start_ms >= CLOUD_WAIT_FALLBACK_MS) {
            g_cloud_seen = true;
            Log.warn("cloud wait timeout (%lums); falling back to RTC. "
                     "final state: wifi.ready=%d time.valid=%d",
                     (unsigned long)CLOUD_WAIT_FALLBACK_MS,
                     (int)WiFi.ready(), (int)Time.isValid());
        } else {
#if defined(CLOUD_WAIT_DEBUG)
            static unsigned long last_wait_log_ms = 0;
            static uint32_t      spinner_draws    = 0;
            if (now - last_wait_log_ms >= 5000) {
                last_wait_log_ms = now;
                Log.info("cloud-wait: t=%lums wifi.ready=%d particle.connected=%d "
                         "time.valid=%d spinner_draws=%lu",
                         now, (int)WiFi.ready(), (int)Particle.connected(),
                         (int)Time.isValid(), (unsigned long)spinner_draws);
            }
#endif
            // Deliberately slower than RENDER_TICK_MS so OG Photon's
            // bit-banged WS2812 show() doesn't starve WICED Wi-Fi of IRQs
            // during the initial association window. 100ms = 10Hz = ~7%
            // IRQ-off duty cycle.
            if (now - last_render_tick >= 100) {
                last_render_tick = now;
                draw_spinner(now);
#if defined(CLOUD_WAIT_DEBUG)
                ++spinner_draws;
#endif
            }
            return;
        }
    }

    // Diagnostic rollup, emitted once per second. Tracks the extremes so a
    // slow outlier tick isn't averaged into invisibility.
    static unsigned long diag_next_ms      = 0;
    static uint32_t      diag_phys_count   = 0;
    static uint32_t      diag_phys_total   = 0;
    static uint32_t      diag_phys_max     = 0;
    static uint32_t      diag_rend_count   = 0;
    static uint32_t      diag_rend_total   = 0;
    static uint32_t      diag_rend_max     = 0;
    static uint32_t      diag_interp_total = 0;
    static uint32_t      diag_interp_max   = 0;
    static uint32_t      diag_astar_total  = 0;
    static uint32_t      diag_astar_max    = 0;

#if defined(STRA2US_HOST)
    // OTA IR swap. Consume a pending blob (if one has been fetched by the
    // telemetry thread) and re-seat the engine against the new tables.
    // Runs *before* physics so we never swap mid-tick — processAgent reads
    // BEHAVIORS[beh_idx].insns[pc] and those pointers must not shift under
    // it. Parse + reinit cost runs <10ms in practice.
    if (g_cfg.ir_apply_if_ready()) {
        if (!g_engine.reinit()) {
            Log.error("engine.reinit() failed after OTA IR swap");
        } else {
            // Fresh RNG entropy for the new script so two simultaneously-
            // swapped devices diverge rather than move in lockstep.
            g_engine.seedRng((uint32_t)HAL_RNG_GetRandomNumber());
            last_sync_minute = -1;   // force a syncTime on next physics tick
        }
    }
#endif

    // Physics tick
    if (now - last_physics_tick >= physics_tick_ms()) {
        last_physics_tick = now;

        if (g_clock.valid()) {
            // Re-sync the intended/clock layer once per virtual minute.
            int m = local_minute(g_clock);
            if (m != last_sync_minute) {
                int prev = last_sync_minute;
                last_sync_minute = m;
                g_engine.syncTime();
                Log.info("syncTime: minute %d -> %d", prev, m);
            }
            uint32_t t0 = micros();
            g_engine.tick();
            uint32_t dt = micros() - t0;
            uint32_t interp_dt = g_engine.interpUsLastTick();
            uint32_t astar_dt  = g_engine.astarUsLastTick();
            diag_phys_total += dt;
            diag_phys_count += 1;
            if (dt > diag_phys_max) diag_phys_max = dt;
            diag_interp_total += interp_dt;
            if (interp_dt > diag_interp_max) diag_interp_max = interp_dt;
            diag_astar_total  += astar_dt;
            if (astar_dt > diag_astar_max) diag_astar_max = astar_dt;
        }
    }

    // Render tick
    if (now - last_render_tick >= RENDER_TICK_MS) {
        last_render_tick = now;

        if (!g_clock.valid()) {
            draw_spinner(now);
            return;
        }

        float blend = (float)(now - last_physics_tick) / (float)physics_tick_ms();
        if (blend > 1.0f) blend = 1.0f;
        uint32_t t0 = micros();
        g_engine.render(blend);
        uint32_t dt = micros() - t0;
        diag_rend_total += dt;
        diag_rend_count += 1;
        if (dt > diag_rend_max) diag_rend_max = dt;
    }

    // Emit rollup. System.freeMemory() reflects heap headroom — if it keeps
    // falling monotonically we're leaking; if it oscillates we're churning.
    if ((long)(now - diag_next_ms) >= 0) {
        diag_next_ms = now + 1000;
        uint32_t free_mem = System.freeMemory();
        uint32_t phys_avg   = diag_phys_count ? (diag_phys_total   / diag_phys_count) : 0;
        uint32_t rend_avg   = diag_rend_count ? (diag_rend_total   / diag_rend_count) : 0;
        uint32_t interp_avg = diag_phys_count ? (diag_interp_total / diag_phys_count) : 0;
        uint32_t astar_avg  = diag_phys_count ? (diag_astar_total  / diag_phys_count) : 0;
        const auto& m = g_engine.metrics();
        Log.info("diag t=%lus phys=%lu(avg=%luus max=%luus) rend=%lu(avg=%luus max=%luus) "
                 "interp=(avg=%luus max=%luus) astar=(avg=%luus max=%luus) "
                 "agents=%u seeks_fail=%lu free=%lu",
                 (unsigned long)(now / 1000),
                 (unsigned long)diag_phys_count,
                 (unsigned long)phys_avg,
                 (unsigned long)diag_phys_max,
                 (unsigned long)diag_rend_count,
                 (unsigned long)rend_avg,
                 (unsigned long)diag_rend_max,
                 (unsigned long)interp_avg,
                 (unsigned long)diag_interp_max,
                 (unsigned long)astar_avg,
                 (unsigned long)diag_astar_max,
                 (unsigned)g_engine.liveAgentCount(),
                 (unsigned long)m.failed_seeks,
                 (unsigned long)free_mem);
        // Publish the last closed window to the telemetry-visible snapshots.
        g_phys_avg_us     = phys_avg;
        g_phys_max_us     = diag_phys_max;
        g_rend_avg_us     = rend_avg;
        g_rend_max_us     = diag_rend_max;
        g_interp_avg_us   = interp_avg;
        g_interp_max_us   = diag_interp_max;
        g_astar_avg_us    = astar_avg;
        g_astar_max_us    = diag_astar_max;
        diag_phys_count   = 0;
        diag_phys_total   = 0;
        diag_phys_max     = 0;
        diag_rend_count   = 0;
        diag_rend_total   = 0;
        diag_rend_max     = 0;
        diag_interp_total = 0;
        diag_interp_max   = 0;
        diag_astar_total  = 0;
        diag_astar_max    = 0;
    }
}

#endif  // PLATFORM_ID

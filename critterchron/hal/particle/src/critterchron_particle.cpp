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

// Physics tick rate is a property of the .crit script and comes from the
// generated IR header (critter_ir::TICK_RATE_MS). The device header must
// not override it — the sim and the hardware share the same contract.
static constexpr uint32_t PHYSICS_TICK_MS = critter_ir::TICK_RATE_MS;

// Render tick is firmware-level, not device-level. 20ms = 50Hz; worst-case
// render ~9ms leaves comfortable headroom. Bump down further if you need
// smoother smear at the cost of CPU.
static constexpr uint32_t RENDER_TICK_MS = 20;

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

    char report[256];
    int  rssi = -127;
    if (WiFi.ready()) {
        WiFiSignal sig = WiFi.RSSI();
        rssi = sig.getStrength();
    }
    const auto& m = g_engine.metrics();
    // Timing fields use (avg<max<budget)us — ordering implies "avg is within
    // max is within budget", so a glance at the numbers tells you both
    // current cost and headroom. Budgets are the nominal tick periods.
    constexpr uint32_t PHYS_BUDGET_US = (uint32_t)PHYSICS_TICK_MS * 1000UL;
    constexpr uint32_t REND_BUDGET_US = (uint32_t)RENDER_TICK_MS  * 1000UL;
    int rlen = snprintf(report, sizeof(report),
        "up=%lu rssi=%d mem=%lu rst=%d fw=%s "
        "bri=(%u<%u<%u) "
        "phys=(%lu<%lu<%lu)us rend=(%lu<%lu<%lu)us "
        "interp=(%lu<%lu)us astar=(%lu<%lu)us "
        "agents=%u seeks_fail=%lu",
        (unsigned long)System.uptime(),
        rssi,
        (unsigned long)System.freeMemory(),
        (int)System.resetReason(),
        APP_VERSION,
        (unsigned)g_bri_min, (unsigned)g_bri, (unsigned)g_bri_max,
        (unsigned long)g_phys_avg_us,   (unsigned long)g_phys_max_us,   (unsigned long)PHYS_BUDGET_US,
        (unsigned long)g_rend_avg_us,   (unsigned long)g_rend_max_us,   (unsigned long)REND_BUDGET_US,
        (unsigned long)g_interp_avg_us, (unsigned long)g_interp_max_us,
        (unsigned long)g_astar_avg_us,  (unsigned long)g_astar_max_us,
        (unsigned)g_engine.liveAgentCount(),
        (unsigned long)m.failed_seeks);
    if (rlen >= (int)sizeof(report)) report[sizeof(report)-1] = '\0';

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

    // Pre-register heartbeep + cloud_heartbeep so cycle 1's poll_all pulls
    // them live. Otherwise they'd only be registered by the get_int calls
    // below (which run AFTER telemetry_cycle) and the live overrides would
    // take five minutes to kick in.
    (void)g_cfg.get_int("heartbeep",       HEARTBEEP_DEFAULT);
    (void)g_cfg.get_int("cloud_heartbeep", CLOUD_HEARTBEEP_DEFAULT);

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
    pinMode(LIGHT_SENSOR_PWR_PIN, OUTPUT);
    digitalWrite(LIGHT_SENSOR_PWR_PIN, HIGH);
    pinMode(LIGHT_SENSOR_GND_PIN, OUTPUT);
    digitalWrite(LIGHT_SENSOR_GND_PIN, LOW);
    pinMode(LIGHT_SENSOR_SIG_PIN, INPUT);
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

    if (!g_engine.begin()) {
        Log.error("CritterEngine::begin() failed");
    }
    // RNG seed: bake in something device-ish so two units diverge.
    g_engine.seedRng((uint32_t)HAL_RNG_GetRandomNumber());

#if defined(STRA2US_HOST)
    g_tel_thread = new Thread("telemetry", telemetry_worker,
                              OS_THREAD_PRIORITY_DEFAULT, 10240);
#endif

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

    // Physics tick
    if (now - last_physics_tick >= PHYSICS_TICK_MS) {
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

        float blend = (float)(now - last_physics_tick) / (float)PHYSICS_TICK_MS;
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

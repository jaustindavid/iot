#include "Particle.h"
#include "neopixel.h"
#include "CoatiEngine.h"
#include "WobblyTime.h"
#include "Stra2usClient.h"
#include "LightSensor.h"
#include "creds.h"

// Application Firmware Revision
#define APP_VERSION __DATE__ " " __TIME__

SYSTEM_THREAD(ENABLED);
void telemetry_worker();
Thread* telemetryThread = nullptr;

// Hardware settings
#define PIXEL_PIN D2        // SPI MOSI
#define PIXEL_COUNT (GRID_W * GRID_H)
#define PIXEL_TYPE WS2812B

#ifndef MATRIX_PIN
#define MATRIX_PIN SPI
#endif

Adafruit_NeoPixel strip(PIXEL_COUNT, MATRIX_PIN, PIXEL_TYPE);

// Engine and Time logic
CoatiEngine engine;
WobblyTime wobbly(120, 300, 1.3f, 0.7f);
Stra2usClient client(STRA2US_HOST, STRA2US_PORT, STRA2US_CLIENT_ID, STRA2US_SECRET_HEX);

// Light Sensor class
LightSensor light_sensor;

// Global state
volatile float global_brightness = 0.7f;
volatile float param_min_brightness = 0.1f;
volatile float param_max_brightness = 1.0f;
volatile int global_cds = 0;
volatile int param_heartbeep = 300;

unsigned long last_physics_tick = 0;
unsigned long last_render_tick = 0;
unsigned long last_telemetry_tick = 0;
int last_display_minute = -1;

// Logging
SerialLogHandler logHandler(LOG_LEVEL_INFO);

void setup() {
	// Hardware setup
	pinMode(A0, OUTPUT);
	digitalWrite(A0, HIGH); // Power CDS divider
	pinMode(A2, OUTPUT);
	digitalWrite(A2, LOW);  // Ground CDS divider

	strip.begin();
	strip.show();

	// Time setup
	Time.zone(-5); // Default to EST, can be updated via telemetry
	Particle.syncTime();

	// Defer first telemetry for 15s to allow network stack to stabilize
	last_telemetry_tick = millis() - (param_heartbeep * 1000) + 15000; 

	telemetryThread = new Thread("IoT", telemetry_worker, OS_THREAD_PRIORITY_DEFAULT, 10240);
	Log.info("Coaticlock Initialized. Version: " APP_VERSION);
}

void send_telemetry() {
	if (!Time.isValid()) return;
	Log.info("Telemetry: Sending Heartbeat...");

	// 1. Send Heartbeat
	char report[256];
	uint32_t uptime = System.uptime();
	Log.info("Telemetry: Getting RSSI...");
	int rssi = -127;
	if (WiFi.ready()) {
		WiFiSignal sig = WiFi.RSSI();
		rssi = sig.getStrength();
	}

	Log.info("Telemetry: Formatting report...");
	int rlen = snprintf(report, sizeof(report), 
			"up=%lu rssi=%d mem=%lu rst=%d fw=%s lux(%d, %d, %d)->bri=(%.2f < %.2f < %.2f) p_hb=%ds",
             uptime, rssi, (unsigned long)System.freeMemory(), (int)System.resetReason(), APP_VERSION,
             global_cds, light_sensor.cal_dark, light_sensor.cal_bright, 
             param_min_brightness, global_brightness, param_max_brightness,
             param_heartbeep);
    if (rlen >= (int)sizeof(report)) report[sizeof(report)-1] = '\0';
    
    Log.info("Telemetry: Publishing to topic %s...", STRATUS_APP);
    int status = client.publish(STRATUS_APP, report);
    Log.info("Telemetry: Publish status = %d", status);
}

void poll_config() {
    if (!WiFi.ready()) return;
    Log.info("Telemetry: Polling Config KV...");
    
    char val[32];
    char key[128];
    
    // wobble_min_seconds
    snprintf(key, sizeof(key), "%s/%s/wobble_min_seconds", STRATUS_APP, STRA2US_CLIENT_ID);
    if (client.kv_get(key, val, sizeof(val)) || client.kv_get(STRATUS_APP "/wobble_min_seconds", val, sizeof(val))) {
        wobbly.wobble_min_seconds = atoi(val);
    }
    
    // wobble_max_seconds
    snprintf(key, sizeof(key), "%s/%s/wobble_max_seconds", STRATUS_APP, STRA2US_CLIENT_ID);
    if (client.kv_get(key, val, sizeof(val)) || client.kv_get(STRATUS_APP "/wobble_max_seconds", val, sizeof(val))) {
        wobbly.wobble_max_seconds = atoi(val);
    }
    
    // wobble_fast_rate
    snprintf(key, sizeof(key), "%s/%s/wobble_fast_rate", STRATUS_APP, STRA2US_CLIENT_ID);
    if (client.kv_get(key, val, sizeof(val)) || client.kv_get(STRATUS_APP "/wobble_fast_rate", val, sizeof(val))) {
        wobbly.fast_rate = atof(val);
    }

    // wobble_slow_rate
    snprintf(key, sizeof(key), "%s/%s/wobble_slow_rate", STRATUS_APP, STRA2US_CLIENT_ID);
    if (client.kv_get(key, val, sizeof(val)) || client.kv_get(STRATUS_APP "/wobble_slow_rate", val, sizeof(val))) {
        wobbly.slow_rate = atof(val);
    }
    
    // timezone_offset
    snprintf(key, sizeof(key), "%s/%s/timezone_offset", STRATUS_APP, STRA2US_CLIENT_ID);
    if (client.kv_get(key, val, sizeof(val)) || client.kv_get(STRATUS_APP "/timezone_offset", val, sizeof(val))) {
        Time.zone(atof(val));
    }

    // min_brightness
    snprintf(key, sizeof(key), "%s/%s/min_brightness", STRATUS_APP, STRA2US_CLIENT_ID);
    if (client.kv_get(key, val, sizeof(val)) || client.kv_get(STRATUS_APP "/min_brightness", val, sizeof(val))) {
        param_min_brightness = atof(val);
        Log.info("updating min brightness: %s => %f", val, param_min_brightness);
    }

    // max_brightness
    snprintf(key, sizeof(key), "%s/%s/max_brightness", STRATUS_APP, STRA2US_CLIENT_ID);
    if (client.kv_get(key, val, sizeof(val)) || client.kv_get(STRATUS_APP "/max_brightness", val, sizeof(val))) {
        param_max_brightness = atof(val);
    }

    // lux_exponent
    snprintf(key, sizeof(key), "%s/%s/lux_exponent", STRATUS_APP, STRA2US_CLIENT_ID);
    if (client.kv_get(key, val, sizeof(val)) || client.kv_get(STRATUS_APP "/lux_exponent", val, sizeof(val))) {
        light_sensor.set_exponent(atof(val));
    }
    // heartbeep
    snprintf(key, sizeof(key), "%s/%s/heartbeep", STRATUS_APP, STRA2US_CLIENT_ID);
    if (client.kv_get(key, val, sizeof(val)) || client.kv_get(STRATUS_APP "/heartbeep", val, sizeof(val))) {
        param_heartbeep = atoi(val);
        if (param_heartbeep < 10) param_heartbeep = 10; // Floor at 10 seconds to prevent DDoS
    }
}
void telemetry_worker() {
    while(true) {
        unsigned long now = millis();
        if (WiFi.ready() && (now - last_telemetry_tick >= ((unsigned long)param_heartbeep * 1000))) {
            last_telemetry_tick = now;
            client.connect();      // fresh connect, always
            send_telemetry();
            poll_config();
            client.close();        // hard close, always
        }
        delay(100);
    }
}

void loop() {
    unsigned long now = millis();

    // 1. Ambient Light Sensing (200ms)
    static unsigned long last_light_read = 0;
    if (now - last_light_read >= 200) {
        last_light_read = now;
        int raw = analogRead(A1);
	global_cds = raw;
        float target = light_sensor.update(raw);
        target = constrain(target, param_min_brightness, param_max_brightness);
        global_brightness = (target * 0.02f) + (global_brightness * 0.98f);

        
        static unsigned long last_light_log = 0;
        if (now - last_light_log >= 2000) {
            last_light_log = now;
            Log.info("Light Sensor: Raw=%d TargetBri=%.2f, GlobalBri=%.2f", 
                     raw, target, global_brightness);
        }
    }

    // 2. Physics Tick (10Hz / 100ms)
    if (now - last_physics_tick >= 100) {
        last_physics_tick = now;
        
        if (Time.isValid()) {
            // Sync time with WobblyTime
            time_t real_now = Time.now();
            time_t virtual_now = wobbly.advance(real_now);
            
            int v_min = Time.minute(virtual_now);
            if (v_min != last_display_minute) {
                last_display_minute = v_min;
                Log.info("Minute change detected: %d. Updating engine target.", v_min);
                engine.update_target(virtual_now);
            }

            engine.apply_pending_target();
        } else {
            static bool time_warned = false;
            if (!time_warned) { Log.info("Waiting for cloud time sync..."); time_warned = true; }
        }
        
        engine.tick();
    }

    // 3. Render Tick (60Hz / 16ms)
    if (now - last_render_tick >= 16) {
        last_render_tick = now;

        float blend = (float)(now - last_physics_tick) / 100.0f;
        if (blend > 1.0f) blend = 1.0f;

        if (!Time.isValid()) {
            // Loading spinner — system state, not script-driven.
            strip.clear();
            int cx = GRID_W / 2;
            int cy = GRID_H / 2 - 1;
            int phase = (now / 150) % 8;
            static const int DX[8] = { 0,  1, 1, 1, 0, -1, -1, -1};
            static const int DY[8] = {-1, -1, 0, 1, 1,  1,  0, -1};
            auto pix = [&](int x, int y, uint8_t r, uint8_t g, uint8_t b) {
                if (x < 0 || x >= GRID_W || y < 0 || y >= GRID_H) return;
                float s = global_brightness;
                strip.setPixelColor(x * GRID_H + ((x % 2) ? (GRID_H - 1 - y) : y),
                                    strip.Color((uint8_t)(r*s), (uint8_t)(g*s), (uint8_t)(b*s)));
            };
            pix(cx + DX[phase], cy + DY[phase], 0, 32, 64);
            pix(cx, cy, 0, 16, 32);
            strip.show();
        } else {
            engine.render(strip, global_brightness, now, blend, GRID_ROTATION);
        }
    }
}


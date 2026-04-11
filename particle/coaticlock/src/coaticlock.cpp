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
#define PIXEL_COUNT (GRID_WIDTH * GRID_HEIGHT)
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

uint32_t get_pixel_color(uint8_t r, uint8_t g, uint8_t b, float bri) {
    if (bri == 0) return 0;
    return strip.Color((uint8_t)(r?max(r*bri,1):0), 
                       (uint8_t)(g?max(g*bri,1):0),
                       (uint8_t)(b?max(b*bri,1):0));
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

        strip.clear();
        float bri = global_brightness;

        // Draw static endpoints (Dumpster & Pool)
        uint32_t red = get_pixel_color(64, 0, 0, bri);
        uint32_t blue = get_pixel_color(0, 0, 64, bri);
        
        auto map_pixel = [](int x, int y) {
            if (x < 0 || x >= GRID_WIDTH || y < 0 || y >= GRID_HEIGHT) return 999; // Return invalid index
            int rx = x, ry = y;
            int max_x = GRID_WIDTH - 1;
            int max_y = GRID_HEIGHT - 1;
            
            if (GRID_ROTATION == 90) {
                rx = max_y - y;
                ry = x;
            } else if (GRID_ROTATION == 180) {
                rx = max_x - x;
                ry = max_y - y;
            } else if (GRID_ROTATION == 270) {
                rx = y;
                ry = max_x - x;
            }

            int phys_h = (GRID_ROTATION == 90 || GRID_ROTATION == 270) ? GRID_WIDTH : GRID_HEIGHT;
            
            if (rx % 2 == 0) return (rx * phys_h) + ry;
            return (rx * phys_h) + ((phys_h - 1) - ry);
        };
        
        auto set_safe_pixel = [&](int x, int y, uint32_t color) {
            int idx = map_pixel(x, y);
            if (idx >= 0 && idx < PIXEL_COUNT) strip.setPixelColor(idx, color);
        };

        if (!Time.isValid()) {
            int cx = GRID_WIDTH / 2;
            int cy = GRID_HEIGHT / 2 - 1;
            int phase = (now / 150) % 8;
            int dx = 0, dy = 0;
            switch (phase) {
                case 0: dx = 0; dy = -1; break;
                case 1: dx = 1; dy = -1; break;
                case 2: dx = 1; dy = 0; break;
                case 3: dx = 1; dy = 1; break;
                case 4: dx = 0; dy = 1; break;
                case 5: dx = -1; dy = 1; break;
                case 6: dx = -1; dy = 0; break;
                case 7: dx = -1; dy = -1; break;
            }
            uint32_t spinner_color = get_pixel_color(0, 32, 64, bri);
            set_safe_pixel(cx + dx, cy + dy, spinner_color);
            set_safe_pixel(cx, cy, get_pixel_color(0, 16, 32, bri));
        }

        set_safe_pixel(0, GRID_HEIGHT - 1, red);
        set_safe_pixel(1, GRID_HEIGHT - 1, red);
        set_safe_pixel(GRID_WIDTH - 2, GRID_HEIGHT - 1, blue);
        set_safe_pixel(GRID_WIDTH - 1, GRID_HEIGHT - 1, blue);

        // Draw physical pixels
        for (int x = 0; x < GRID_WIDTH; x++) {
            for (int y = 0; y < GRID_HEIGHT; y++) {
                if (engine.current_board[x][y]) {
                    if (y == GRID_HEIGHT - 1 && (x <= 1 || x >= GRID_WIDTH - 2)) continue;
                    set_safe_pixel(x, y, get_pixel_color(0, 64, 0, engine.fade_board[x][y] * bri));
                }
            }
        }

        // Draw Agents
        uint32_t white = get_pixel_color(64, 64, 64, bri);
        uint32_t cyan = get_pixel_color(0, 64, 64, bri);
        uint32_t shimmer_red = get_pixel_color(64, 25, 25, bri);

        for (size_t i = 0; i < engine.agents.size(); i++) {
            CoatiAgent& a = engine.agents[i];
            uint32_t agent_color = 0;
            if (a.carrying) {
                float pulse = 0.7f + 0.3f * sin(now / 150.0f);
                agent_color = get_pixel_color(64, 64, 0, pulse * bri);
            } else {
                agent_color = (i == 0) ? white : cyan;
            }

            if (a.is_bored) {
                agent_color = (i == 0) ? get_pixel_color(32, 32, 32, bri) : get_pixel_color(0, 32, 32, bri);
            }

            if (a.wait_ticks > 1) {
                if ((sin(now / 30.0f) > 0)) agent_color = shimmer_red;
            }

            if (a.pos == a.last_pos) {
                set_safe_pixel(a.pos.x, a.pos.y, agent_color);
            } else {
                // Motion Blur
                uint32_t c1 = agent_color;
                uint8_t r = (c1 >> 16) & 0xFF;
                uint8_t g = (c1 >> 8) & 0xFF;
                uint8_t b = c1 & 0xFF;
                
                set_safe_pixel(a.last_pos.x, a.last_pos.y, get_pixel_color(r, g, b, (1.0f - blend) * bri));
                set_safe_pixel(a.pos.x, a.pos.y, get_pixel_color(r, g, b, blend * bri));
            }
        }

        strip.show();
    }
}


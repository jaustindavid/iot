#pragma once

// Stra2usClient — ESP32 port. Mirror of hal/particle/src/Stra2usClient.h with
// three mechanical swaps:
//
//   * gate flipped from PLATFORM_ID to ARDUINO_ARCH_ESP32
//   * #include "Particle.h"  →  <Arduino.h> + <WiFi.h>
//   * TCPClient tcp_          →  WiFiClient tcp_
//
// Everything else — cache model, OTA IR state, thread-safety notes — is
// identical to the Particle version. The HMAC signing + msgpack parse +
// HTTP framing code in the .cpp is already portable C++. Read the Particle
// header for the full design rationale; this one stays terse so the two
// stay diff-able.
//
// Thread safety: the main thread appends entries from Config getters; a
// FreeRTOS telemetry task updates values on existing entries. Same
// has_live-last / count-last ordering as Particle — aligned 32-bit writes
// on ESP32-C3 (RISC-V) and ESP32-S3 (Xtensa) match the ARM contract the
// ordering relies on.

#if defined(ARDUINO_ARCH_ESP32)

#include <Arduino.h>
#include <WiFi.h>
// creds.h first so device headers can override IR_OTA_BUFFER_BYTES before
// this class's members are instantiated — same ODR concern as the Particle
// header notes.
#include "creds.h"
#include "interface/Config.h"
#include "hmac_sha256.h"
// Pulled in for critter_ir::SCRIPT_NAME / SCRIPT_SHA — the heartbeat
// accessors below fall back to them so a flash-only or pre-first-OTA
// device still reports its real compiled-in script identity instead of
// `default@00000000` (observed 2026-04-27 on timmy publishing
// `ota_detected from=default@00000000` despite running compiled-in
// fraggle). Mirrors hal/particle/src/Stra2usClient.h.
#include "ir/IrRuntime.h"

class Stra2usClient : public Config {
public:
    Stra2usClient(const char* host, int port,
                  const char* client_id, const char* secret_hex,
                  const char* app, const char* device);

    // Config — hot path, no I/O, never fails. First call registers the key.
    bool  has       (const char* key) const override;
    int   get_int   (const char* key, int   def) const override;
    float get_float (const char* key, float def) const override;

    // Telemetry side — call from a dedicated FreeRTOS task. Connects on
    // demand, keeps the socket alive across calls, reconnects on failure.
    bool connect();
    void close();

    int publish(const char* topic, const char* message);

    void poll_key(size_t idx);
    void poll_all();

    size_t cache_size() const { return cache_count_; }

    // ---------- OTA Firmware (ESP32 only — Phase 2 of pull-OTA) ----------
    // fw_poll: read target pointer (device-then-app), fetch sidecar,
    // compare to running-image sha, stream new firmware through Update,
    // reboot on success. Call from the tel task on `fw_poll_interval`
    // cadence + once on boot. Idempotent — does nothing if no target
    // pointer is set, or if running sha already matches.
    void fw_poll();

    // Set the running image's SHA256 (64-hex string + NUL). Called once
    // at boot from setup() after `esp_partition_get_sha256()` returns.
    // Empty string (default) means "never set" — fw_poll always proceeds
    // to fetch on first call. Step 4 of Phase 2 wires this up; until
    // then the field is harmlessly empty and every poll re-fetches.
    void set_running_fw_sha(const char* sha_hex_64);
    const char* running_fw_sha() const { return fw_running_sha_; }

    // ---------- OTA IR ----------
    // Present here for source parity with the Particle port; M3 wires up the
    // KV/heartbeat side only, so nothing in the .ino calls these yet. They
    // go live in M4 when the OTA IR lifecycle lands on ESP32.
    void ir_poll();
    bool ir_apply_if_ready();
    bool ir_pending_ready() const { return ir_pending_len_ != 0; }
    const char* ir_pending_script() const { return ir_pending_ptr_; }
    const char* ir_pending_sha()    const { return ir_pending_sha_; }

    bool ir_detected_ready() const { return ir_detected_flag_; }
    void ir_clear_detected() { ir_detected_flag_ = false; }
    const char* ir_detected_from_name() const { return ir_detected_from_name_; }
    const char* ir_detected_from_sha()  const { return ir_detected_from_sha_;  }
    const char* ir_detected_to_name()   const { return ir_detected_to_name_;   }
    const char* ir_detected_to_sha()    const { return ir_detected_to_sha_;    }
    size_t      ir_detected_size()      const { return ir_detected_size_;      }

    // Identity of the currently-loaded script. Falls back to the
    // compiled-in blob's metadata (critter_ir::SCRIPT_NAME, populated
    // by loadDefault() at boot) when no OTA has applied yet, so a
    // pre-first-OTA device reports its real script in the heartbeat
    // (`script=fraggle@d0920aba`) instead of `default@00000000`.
    // Mirrors hal/particle/src/Stra2usClient.h.
    const char* ir_loaded_script() const {
        return ir_loaded_ptr_[0] ? ir_loaded_ptr_ : critter_ir::SCRIPT_NAME;
    }
    const char* ir_loaded_sha() const {
        return ir_loaded_sha_[0] ? ir_loaded_sha_ : critter_ir::SCRIPT_SHA;
    }

    // String tunable: time-of-day brightness schedule. Wire format
    // documented at the parser site in critterchron_esp32.ino's brightness
    // loop. Refreshed during poll_all() with the standard
    // <app>/<device>/<key> → <app>/<key> fallback. NUL-terminated; empty
    // string means the key is not set / not yet fetched. Mirrors
    // hal/particle/src/Stra2usClient.h.
    const char* brightness_schedule() const { return brightness_schedule_; }

    // ---------- Procyon rescue: target WiFi credentials ----------
    // Mirror of hal/particle/src/Stra2usClient.h. Returns the live
    // values fetched in poll_all() with the standard <app>/<device> →
    // <app> fallback. Empty string when the key is unset or never
    // fetched. Heartbeat-cycle code reads these, hashes the pair, and
    // calls into the WiFi-cred apply path when the hash changes — see
    // procyon TODO step 6 (ESP32 mirror).
    const char* wifi_ssid()     const { return wifi_ssid_; }
    const char* wifi_password() const { return wifi_password_; }

    // ---------- Network latency stats ----------
    // Per-call elapsed-time samples accumulate across publish() and the
    // small kv_fetch_ / kv_fetch_str_ paths (single TCP segment, response
    // < ~1KB). The streaming kv_fetch_stream_ is intentionally excluded —
    // a 1MB OTA fetch would dominate the metric and the user reading the
    // sparkline cares about steady-state heartbeat latency, not OTA
    // throughput. Heartbeat code calls consume_latency_stats() once per
    // cycle: returns the triple over the window since the last call and
    // resets the accumulator. Returns false (stats untouched) when the
    // window had no samples.
    bool consume_latency_stats(uint32_t* out_min_ms,
                               uint32_t* out_mean_ms,
                               uint32_t* out_max_ms);

private:
    static constexpr size_t CACHE_CAP = 32;
    static constexpr size_t KEY_MAX   = 40;

    struct Entry {
        char  key[KEY_MAX];
        bool  is_float;
        bool  has_live;
        int   def_i;
        float def_f;
        int   live_i;
        float live_f;
    };

    size_t register_key_(const char* key, bool is_float,
                         int def_i, float def_f) const;

    bool kv_fetch_(const char* full_key,
                   bool& is_float, int& out_i, float& out_f);
    bool kv_fetch_str_(const char* full_key,
                       char* buf, size_t buf_cap, size_t& out_len);

    // Streaming-fetch chunk callback. Called repeatedly with payload
    // bytes (msgpack bin header already stripped) as they arrive from
    // the server. Returning false aborts the fetch. `userdata` is
    // opaque forward — pass whatever the caller needs as context.
    using ChunkCallback = bool (*)(void* userdata,
                                   const uint8_t* chunk, size_t len);

    // Stream a large KV value through a callback rather than buffering
    // it. Used for firmware OTA fetches where the value is ~1MB and
    // the device can't afford a buffer that size. Returns true on a
    // clean, HMAC-verified, full-length fetch where every callback
    // invocation returned true. Sets *out_payload_size to the parsed
    // msgpack payload length (i.e., the actual binary size, not the
    // wrapped response length). Mirror of `kv_fetch_str_` for the
    // streaming case.
    bool kv_fetch_stream_(const char* full_key,
                          ChunkCallback cb, void* userdata,
                          size_t* out_payload_size);

    bool ensure_connected_();
    bool send_all_(const char* data, int len);
    int  read_response_(const char* uri,
                        char* body_out, size_t body_out_len);
    void sign_(const char* uri, const char* body, size_t body_len,
               uint32_t ts, char* out_hex);
    static void hex_to_bytes_(const char* hex, uint8_t* out, size_t n);
    static bool hex_equal_(const char* a, const char* b);

    const char* host_;
    int         port_;
    const char* client_id_;
    const char* secret_hex_;
    const char* app_;
    const char* device_;

    // WiFiClient API surface used here — connected(), connect(host,port),
    // stop(), write(buf,n), read(buf,n), available() — is identical to
    // Particle's TCPClient, which is why the .cpp body needs no further
    // transport changes. Confirmed against Arduino-ESP32 3.x NetworkClient.
    WiFiClient  tcp_;

    mutable Entry  cache_[CACHE_CAP];
    mutable size_t cache_count_ = 0;

    // ---------- OTA IR state ----------
#ifndef IR_OTA_BUFFER_BYTES
#define IR_OTA_BUFFER_BYTES 8192
#endif
#ifndef IR_SCRIPT_NAME_MAX
#define IR_SCRIPT_NAME_MAX 48
#endif

    char           ir_ota_buf_[IR_OTA_BUFFER_BYTES];
    volatile size_t ir_pending_len_ = 0;
    char           ir_pending_ptr_[IR_SCRIPT_NAME_MAX] = {0};
    char           ir_pending_sha_[65] = {0};

    char           ir_loaded_ptr_[IR_SCRIPT_NAME_MAX] = {0};
    char           ir_loaded_sha_[65] = {0};

    volatile bool  ir_detected_flag_ = false;
    char           ir_detected_from_name_[IR_SCRIPT_NAME_MAX] = {0};
    char           ir_detected_from_sha_ [65] = {0};
    char           ir_detected_to_name_  [IR_SCRIPT_NAME_MAX] = {0};
    char           ir_detected_to_sha_   [65] = {0};
    size_t         ir_detected_size_      = 0;

    // Brightness schedule string buffer. Refreshed in poll_all() with
    // device-then-app fallback; left untouched if both KV fetches fail
    // so a transient network blip doesn't drop a known-good schedule.
    // Mirrors hal/particle/src/Stra2usClient.h.
    char           brightness_schedule_[160] = {0};

    // Procyon-rescue target WiFi credential buffers. Same shape as the
    // brightness_schedule buffer above. SSID max per 802.11 is 32; pad
    // to 40. WPA2 password max is 63; pad to 72. Mirror of
    // hal/particle/src/Stra2usClient.h.
    char           wifi_ssid_     [40] = {0};
    char           wifi_password_ [72] = {0};

    // Running firmware SHA256 (64 lowercase hex chars + NUL). Populated
    // once at boot from `esp_partition_get_sha256()` (Phase 2 step 4);
    // empty string until then. fw_poll() compares this against the
    // sidecar's sha to decide whether to fetch. Empty = always fetch.
    char           fw_running_sha_[65] = {0};

    // ---------- Network latency accumulator ----------
    // record_latency_(ms) is called at the exit of publish/kv_fetch_/
    // kv_fetch_str_, regardless of success. Failures (timeouts) ARE
    // recorded — a 5s timeout sample showing red on the sparkline is
    // exactly the signal an operator wants. consume_latency_stats()
    // reads + resets atomically with respect to the tel task (only the
    // tel task touches these — main thread reads via the consumer).
    // Cap latency_count_ at UINT32_MAX/2 so the sum never wraps even
    // if no consumer ever runs.
    void record_latency_(uint32_t ms);
    uint32_t latency_min_ms_   = UINT32_MAX;
    uint32_t latency_max_ms_   = 0;
    uint32_t latency_sum_ms_   = 0;
    uint32_t latency_count_    = 0;

    // RAII helper — `LatencyScope _t(*this);` at function top records
    // `millis() - t0` on scope exit (every return path). Cleaner than
    // hand-pairing record_latency_ calls at each exit site.
    class LatencyScope {
    public:
        explicit LatencyScope(Stra2usClient& c) : c_(c), t0_(millis()) {}
        ~LatencyScope() { c_.record_latency_(millis() - t0_); }
    private:
        Stra2usClient& c_;
        uint32_t       t0_;
    };
};

#endif  // ARDUINO_ARCH_ESP32

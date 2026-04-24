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

    const char* ir_loaded_script() const { return ir_loaded_ptr_; }
    const char* ir_loaded_sha()    const { return ir_loaded_sha_; }

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
};

#endif  // ARDUINO_ARCH_ESP32

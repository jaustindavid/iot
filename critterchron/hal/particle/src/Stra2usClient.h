#pragma once

// Stra2usClient — Config-backed live-tunable KV surface, plus a heartbeat
// publisher. Consumers (WobblyTimeSource, LightSensor, …) never see this
// class; they hold a `const Config&` and call `get_int` / `get_float`.
//
// Cache model: on first `get_*("key", def)` call from a consumer (main
// thread), we append a cache entry with the compiled-in default. The
// telemetry thread periodically polls every cached key through the Stra2us
// API, trying `<app>/<device>/<key>` first, then `<app>/<key>`. If a live
// value is found, it's parsed from msgpack and written to the entry; the
// entry's `has_live` flag flips true. Subsequent `get_*` reads return the
// live value in place of the default.
//
// The Config.h hot-path contract forbids I/O and failure in `get_*` — this
// impl reads from RAM only. The network traffic is entirely on the poll
// side.
//
// Thread safety: the main (engine) thread appends entries; the telemetry
// thread updates values on existing entries only. Count is bumped last on
// append, and `has_live` is flipped last on update — readers that see
// has_live==true are guaranteed to see the value already committed.
// Adequate for Photon 2's aligned 32-bit writes; revisit if we port to a
// platform with weaker ordering.

#if defined(PLATFORM_ID)

#include "Particle.h"
#include "interface/Config.h"
#include "hmac_sha256.h"

class Stra2usClient : public Config {
public:
    // `app` is the namespace for KV and heartbeat topic (e.g. "coaticlock").
    // `device` is the per-unit override namespace (e.g. "ricky_raccoon").
    Stra2usClient(const char* host, int port,
                  const char* client_id, const char* secret_hex,
                  const char* app, const char* device);

    // Config — hot path, no I/O, never fails. First call registers the key.
    bool  has       (const char* key) const override;
    int   get_int   (const char* key, int   def) const override;
    float get_float (const char* key, float def) const override;

    // Telemetry side — call from a dedicated thread. Connects on demand,
    // keeps the socket alive across calls, reconnects on failure.
    bool connect();
    void close();

    // Publish a text message to queue `<topic>`.
    int publish(const char* topic, const char* message);

    // Poll one cached key through the app/device fallback chain.
    // Updates its live value if the server has one.
    void poll_key(size_t idx);

    // Poll every cached key in sequence. Convenience wrapper for the
    // telemetry thread's main loop.
    void poll_all();

    size_t cache_size() const { return cache_count_; }

private:
    static constexpr size_t CACHE_CAP = 32;
    static constexpr size_t KEY_MAX   = 40;

    struct Entry {
        char  key[KEY_MAX];
        bool  is_float;      // static once registered
        bool  has_live;      // flipped true after a successful poll
        int   def_i;
        float def_f;
        int   live_i;
        float live_f;
    };

    // Find-or-append. Called on the main thread from get_*. `is_float`
    // distinguishes the two default types; a key registered as int stays
    // int for its lifetime (consumers shouldn't mix types per key).
    size_t register_key_(const char* key, bool is_float,
                         int def_i, float def_f) const;

    // Fetch a fully-qualified key (e.g. "coaticlock/ricky_raccoon/foo").
    // Returns true on 200 + parsable msgpack; sets `is_float` and writes
    // either `out_i` or `out_f`.
    bool kv_fetch_(const char* full_key,
                   bool& is_float, int& out_i, float& out_f);

    bool ensure_connected_();
    bool send_all_(const char* data, int len);
    // Reads status + headers + body, stream-verifies X-Response-Signature
    // against URI + body + X-Response-Timestamp under this client's secret.
    // Returns the HTTP status on success, -1 on any read / verify failure.
    // 2xx responses without signature headers are treated as failures;
    // non-2xx responses skip verification (error bodies are not signed by
    // the server, and the caller already treats them as fetch failure).
    int  read_response_(const char* uri,
                        char* body_out, size_t body_out_len);
    void sign_(const char* uri, const char* body, size_t body_len,
               uint32_t ts, char* out_hex);
    static void hex_to_bytes_(const char* hex, uint8_t* out, size_t n);
    // Constant-time compare of two 64-char hex strings (HMAC-SHA256 digest).
    static bool hex_equal_(const char* a, const char* b);

    const char* host_;
    int         port_;
    const char* client_id_;
    const char* secret_hex_;
    const char* app_;
    const char* device_;

    TCPClient   tcp_;

    // Cache. `cache_count_` is written last on append and read first on
    // lookup — a racing telemetry thread never walks past a half-
    // initialized entry.
    mutable Entry  cache_[CACHE_CAP];
    mutable size_t cache_count_ = 0;
};

#endif  // PLATFORM_ID

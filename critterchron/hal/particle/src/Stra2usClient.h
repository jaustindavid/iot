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
// creds.h first so device headers can override IR_OTA_BUFFER_BYTES and
// friends; the override must be visible to every TU that includes this
// header or ir_ota_buf_'s sizeof diverges between TUs (ODR / class layout
// mismatch). OG Photon overrides down to reclaim .bss for WICED.
#include "creds.h"
#include "interface/Config.h"
#include "hmac_sha256.h"
// Pulled in for critter_ir::SCRIPT_NAME / SCRIPT_SHA — the heartbeat
// accessors below fall back to them so a flash-only (NO_IR_OTA) or
// pre-first-OTA device still reports its real compiled-in script identity
// instead of a generic "default".
#include "ir/IrRuntime.h"

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

    // ---------- OTA IR ----------
    //
    // Two-step fetch: `<app>/<device>/ir` holds a script name (msgpack str);
    // `<app>/scripts/<name>` holds the text-format IR blob (also msgpack str,
    // multi-KB). ir_poll() runs on the telemetry thread: it fetches the
    // pointer and, if it differs from what's currently loaded, fetches the
    // blob into an internal buffer and marks a pending slot. Does not touch
    // critter_ir:: tables — that's the main thread's job.
    //
    // ir_apply_if_ready() runs on the main thread between ticks: if the
    // pending slot is populated, it calls critter_ir::load() on the buffer.
    // Returns true iff a fresh blob was successfully parsed and installed,
    // so the caller can run engine.reinit() and log the swap.
    //
    // Single pending slot: the telemetry thread won't overwrite a blob the
    // main thread hasn't consumed yet. If consumption is slow, we just miss
    // a poll cycle — next heartbeat will retry.
    void ir_poll();
    bool ir_apply_if_ready();

    // True iff the telemetry thread has staged a fetched blob that the
    // main thread hasn't yet applied. Lets the main thread choose a
    // delay between "OTA arrived" and "swap tables" — e.g. to paint a
    // loading indicator on the grid so the operator sees their publish
    // land before the new script takes over. Read of a volatile size_t,
    // safe without a lock.
    bool ir_pending_ready() const { return ir_pending_len_ != 0; }

    // Identity of the *pending* blob (set alongside ir_pending_len_ by
    // ir_poll() before it flips length-last). Safe to read iff
    // ir_pending_ready() returned true in the same thread-order. Used by
    // the main thread to snapshot the name+sha into the OTA lifecycle
    // publish payload without having to reach into private members.
    const char* ir_pending_script() const { return ir_pending_ptr_; }
    const char* ir_pending_sha()    const { return ir_pending_sha_; }

    // ---------- OTA detected (lifecycle publish #1) ----------
    //
    // ir_poll() snapshots identity + flips `ir_detected_flag_` when it
    // discovers a new sidecar pointing at a candidate it's about to
    // fetch. The tel worker main loop reads the flag on its next
    // iteration, publishes `ota_detected`, and clears the flag via
    // ir_clear_detected(). Publishing from the main loop rather than
    // from inside ir_poll keeps the POST out of the sidecar-GET/blob-GET
    // window on the keep-alive socket — a wedge there hard-freezes the
    // tel thread (TODO.md completed entry 2026-04-22).
    //
    // Snapshot is frozen at detect time because the "from" identity
    // (ir_loaded_*) may be overwritten by a concurrent main-thread
    // ir_apply_if_ready() before the publish fires, which would turn the
    // message into "from=NEW to=NEW" — useless. Snapshot holds from/to
    // name+sha plus the sidecar-declared size (5 fields).
    bool ir_detected_ready() const { return ir_detected_flag_; }
    void ir_clear_detected() { ir_detected_flag_ = false; }
    const char* ir_detected_from_name() const { return ir_detected_from_name_; }
    const char* ir_detected_from_sha()  const { return ir_detected_from_sha_;  }
    const char* ir_detected_to_name()   const { return ir_detected_to_name_;   }
    const char* ir_detected_to_sha()    const { return ir_detected_to_sha_;    }
    size_t      ir_detected_size()      const { return ir_detected_size_;      }

    // Name of the script currently loaded on this device. Falls back to the
    // compiled-in blob's metadata (critter_ir::SCRIPT_NAME, populated by
    // loadDefault()) when no OTA has applied yet — so a flash-only
    // (NO_IR_OTA) or pre-first-OTA device still reports the real script it's
    // running instead of a generic placeholder. Used in the heartbeat
    // payload so the server can tell "running older thyme" from "running
    // default" at a glance across the fleet.
    const char* ir_loaded_script() const {
        return ir_loaded_ptr_[0] ? ir_loaded_ptr_ : critter_ir::SCRIPT_NAME;
    }
    // 64-char hex src_sha256 of the loaded blob. Same fallback rule as
    // ir_loaded_script(): returns the compiled-in blob's sha when no OTA
    // has overridden it. Content-addressed identity: lets us detect
    // re-publishes under the same name without any server-side version
    // bookkeeping.
    const char* ir_loaded_sha() const {
        return ir_loaded_sha_[0] ? ir_loaded_sha_ : critter_ir::SCRIPT_SHA;
    }

    // String tunable: time-of-day brightness schedule. Wire format is
    // documented at the parser site in critterchron_particle.cpp's
    // brightness loop. Refreshed during poll_all() with the standard
    // <app>/<device>/<key> → <app>/<key> fallback. NUL-terminated;
    // empty string means the key is not set / not yet fetched (treat
    // as "no schedule, use device default max_brightness"). Currently
    // the only string-valued tunable — generalize via a Config
    // interface extension when a second string key shows up.
    const char* brightness_schedule() const { return brightness_schedule_; }

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

    // Fetch a fully-qualified key whose msgpack value is a string. The raw
    // response body is read into `buf`, then the msgpack header is stripped
    // in place (memmove of payload to buf[0]) and a null terminator is
    // appended at buf[out_len]. `buf_cap` must include room for that NUL.
    // Handles fixstr (0xa0-0xbf), str8 (0xd9), str16 (0xda), str32 (0xdb).
    // Returns false on HTTP error, signature-verify failure, overflow, or
    // a non-string msgpack prefix.
    bool kv_fetch_str_(const char* full_key,
                       char* buf, size_t buf_cap, size_t& out_len);

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

    // ---------- OTA IR state ----------
    //
    // IR_OTA_BUFFER_BYTES sized for ~2x the largest blob we've seen (~4KB
    // with source trailer). Sits in .bss on the particle build. Override
    // per-device in hal/devices/<name>.h — rico (OG Photon) drops it to
    // 6KB to reclaim WICED's heap headroom; Photon 2 / Argon can bump it
    // back up if scripts grow past 4KB.
    //
    // NO_IR_OTA opt-out: device headers can `#define NO_IR_OTA` to drop
    // the 8KB ir_ota_buf_ entirely — reclaims the buffer for WICED /
    // cloud-stack heap on memory-tight devices (P1-class) that run a
    // compiled-in script and don't need IR-OTA. Heartbeat + config pull
    // still work; only the ir_poll/ir_apply path is stubbed (see the
    // matching guards in Stra2usClient.cpp and critterchron_particle.cpp).
    // The handoff fields (ir_pending_len_ / ir_detected_flag_ / etc.)
    // stay in the class so the public API is uniform; they simply never
    // get written, so ir_pending_ready() / ir_detected_ready() always
    // return false and every call site short-circuits naturally.
#ifndef IR_OTA_BUFFER_BYTES
#define IR_OTA_BUFFER_BYTES 8192
#endif
#ifndef IR_SCRIPT_NAME_MAX
#define IR_SCRIPT_NAME_MAX 48
#endif

    // Pending slot. pending_len_ == 0 means "empty, telemetry may fill".
    // Non-zero means "full, main thread must consume". Telemetry writes
    // buffer + name + sha, then length (length last, after memory barriers
    // implicit in the blocking fetch path); main reads length then the rest
    // then zeros length. One-slot handoff — no mutex.
#ifndef NO_IR_OTA
    char           ir_ota_buf_[IR_OTA_BUFFER_BYTES];
#endif
    volatile size_t ir_pending_len_ = 0;
    char           ir_pending_ptr_[IR_SCRIPT_NAME_MAX] = {0};
    // 64-char hex + NUL. Extracted from the fetched blob's header *before*
    // load() mangles the buffer — load flips whitespace to NUL and the sha
    // line becomes hard to re-find after that.
    char           ir_pending_sha_[65] = {0};

    // Identity of the currently-loaded script: name for humans, sha for
    // change detection. Name alone was the old check; it misses re-publishes
    // under the same name, so the sha is the real source of truth.
    char           ir_loaded_ptr_[IR_SCRIPT_NAME_MAX] = {0};
    char           ir_loaded_sha_[65] = {0};

    // Detected-OTA snapshot + flag. See the public-API block above for
    // why this snapshot lives on the class rather than as a stack
    // capture in ir_poll. Volatile flag for the tel-loop/main-loop
    // reader on the flip side; the buffers are touched by the same
    // thread that flips the flag (ir_poll runs on tel), so only the
    // flag itself is ever racing with a foreign-thread read.
    volatile bool  ir_detected_flag_ = false;
    char           ir_detected_from_name_[IR_SCRIPT_NAME_MAX] = {0};
    char           ir_detected_from_sha_ [65] = {0};
    char           ir_detected_to_name_  [IR_SCRIPT_NAME_MAX] = {0};
    char           ir_detected_to_sha_   [65] = {0};
    size_t         ir_detected_size_      = 0;

    // Brightness schedule string buffer. 160 bytes is enough for ~6 segments
    // at typical lengths ("HH:MM-HH:MM:NNN, " ≈ 20 chars). Refreshed in
    // poll_all() with device-then-app fallback; left untouched if both KV
    // fetches fail so a transient network blip doesn't drop a known-good
    // schedule.
    char           brightness_schedule_[160] = {0};
};

#endif  // PLATFORM_ID

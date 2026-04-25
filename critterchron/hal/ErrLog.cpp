#include "ErrLog.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

// Platform-specific lock + millis source + serial echo. Particle has
// `Mutex` and `Log.error`; ESP32-Arduino has FreeRTOS semaphores and
// a similar Log namespace; host build uses std::mutex + stderr.
#if defined(PLATFORM_ID)
  #include "Particle.h"
  static Mutex s_mu;
#elif defined(ARDUINO_ARCH_ESP32) || defined(ESP_PLATFORM)
  // Arduino-ESP32 ships <mutex> in libc++; std::mutex backs onto a
  // FreeRTOS recursive mutex internally. Cheap, correct, no header
  // surface beyond what's already in scope from Arduino.h.
  #include <Arduino.h>
  #include <mutex>
  static std::mutex s_mu;
#else
  #include <chrono>
  #include <mutex>
  static std::mutex s_mu;
  static auto s_t0 = std::chrono::steady_clock::now();
#endif

namespace critterchron {

const char* err_cat_tag(ErrCat cat) {
    switch (cat) {
        case ErrCat::OtaFetch: return "ota_fetch";
        case ErrCat::OtaApply: return "ota_apply";
        case ErrCat::Boot:     return "boot";
        case ErrCat::Sensor:   return "sensor";
        case ErrCat::Other:    return "other";
    }
    return "other";
}

ErrLog::Lock::Lock()  { s_mu.lock(); }
ErrLog::Lock::~Lock() { s_mu.unlock(); }

static uint32_t now_millis() {
#if defined(PLATFORM_ID)
    return millis();
#elif defined(ARDUINO_ARCH_ESP32) || defined(ESP_PLATFORM)
    return (uint32_t)millis();
#else
    auto d = std::chrono::steady_clock::now() - s_t0;
    return (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(d).count();
#endif
}

ErrLog::ErrLog() {
    for (int i = 0; i < N; ++i) {
        ring_[i].seq    = 0;
        ring_[i].millis = 0;
        ring_[i].cat    = ErrCat::Other;
        ring_[i].sent   = true;   // empty slots are "nothing to send"
        ring_[i].msg[0] = '\0';
    }
}

void ErrLog::record(ErrCat cat, const char* fmt, ...) {
    char buf[sizeof(ErrEntry::msg)];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) buf[0] = '\0';
    // vsnprintf guarantees NUL-termination on success; on truncation
    // (n >= sizeof(buf)) the last byte is already NUL. Either way, safe.

    {
        Lock lk;
        ErrEntry& slot = ring_[head_];
        slot.seq    = ++total_;          // 1-based; 0 reserved as "no entry"
        slot.millis = now_millis();
        slot.cat    = cat;
        slot.sent   = false;
        std::memcpy(slot.msg, buf, sizeof(slot.msg));
        slot.msg[sizeof(slot.msg) - 1] = '\0';  // belt-and-suspenders
        head_ = (head_ + 1) % N;
        if (count_ < N) ++count_;
    }

    // Serial echo so existing log-tail dev workflow keeps working. Doing
    // this OUTSIDE the lock — Log.error can be slow (USB serial drain)
    // and we don't want to stall a producer thread on it.
#if defined(PLATFORM_ID)
    Log.error("err[%s] %s", err_cat_tag(cat), buf);
#elif defined(ARDUINO_ARCH_ESP32) || defined(ESP_PLATFORM)
    // Arduino-ESP32 doesn't have Particle's Log; fall back to Serial.
    // Serial may not be initialized at boot — printing during very
    // early init is best-effort.
    Serial.print("err[");
    Serial.print(err_cat_tag(cat));
    Serial.print("] ");
    Serial.println(buf);
#else
    fprintf(stderr, "err[%s] %s\n", err_cat_tag(cat), buf);
#endif
}

bool ErrLog::peek_oldest_unsent(ErrEntry& out) {
    Lock lk;
    if (count_ == 0) return false;
    // Walk ring from oldest to newest. `head_` points at the next write
    // slot; oldest valid entry is at (head_ - count_) mod N.
    int start = (head_ + N - count_) % N;
    for (int i = 0; i < count_; ++i) {
        int idx = (start + i) % N;
        if (!ring_[idx].sent) {
            out = ring_[idx];
            return true;
        }
    }
    return false;
}

void ErrLog::mark_sent(uint32_t seq) {
    Lock lk;
    for (int i = 0; i < N; ++i) {
        if (ring_[i].seq == seq && !ring_[i].sent) {
            ring_[i].sent = true;
            return;
        }
    }
    // Silent no-op if not found — entry got overwritten between peek
    // and mark_sent (extremely unlikely with N=4 and 10s heartbeat
    // cadence, but possible during a flood). The producer's intent
    // ("publish this error") is satisfied either way.
}

ErrLog g_errlog;

}  // namespace critterchron

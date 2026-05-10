#pragma once

// Abstract knob store. Consumers (WobblyTimeSource, LightSensor, the Photon
// shim) ask by key and always provide a compiled-in default; the store
// decides whether a live override exists and returns it if so. This is the
// seam between tunable classes and whatever backs the KV surface (Stra2us
// today, possibly a different store tomorrow).
//
// First call to get_* implicitly registers the key + default with the store
// so its poll loop knows what to fetch. Consumers don't have to enumerate
// their own keys anywhere else.
//
// Contract: get_* MUST NEVER block on I/O and MUST NEVER fail. If no live
// value is available, return `def`. Callers depend on this for hot-path
// reads.

struct Config {
    virtual bool  has       (const char* key) const              = 0;
    virtual int   get_int   (const char* key, int   def) const   = 0;
    virtual float get_float (const char* key, float def) const   = 0;
    virtual ~Config() = default;
};

// No-op impl for bring-up and for devices without a live config backend.
struct StaticConfig : public Config {
    bool  has       (const char*)               const override { return false; }
    int   get_int   (const char*, int   def)    const override { return def;   }
    float get_float (const char*, float def)    const override { return def;   }
};

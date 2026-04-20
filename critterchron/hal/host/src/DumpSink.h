#pragma once
#include "interface/LedSink.h"

// Host-side no-op LedSink. The engine still goes through its render()
// path, but pixels aren't written anywhere — the useful observable is
// CritterEngine::dumpStateJsonl() from main.cpp. Keeps the render path
// exercised (so a bad sink.set(x,y,...) call still segfaults/ASAN-fails
// in CI) without dragging in a GUI dep.

class DumpSink : public LedSink {
public:
    void clear() override {}
    void set(int /*x*/, int /*y*/,
             uint8_t /*r*/, uint8_t /*g*/, uint8_t /*b*/) override {}
    void show() override {}
};

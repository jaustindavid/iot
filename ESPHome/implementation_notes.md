# Engineering & Implementation Notes: Coati Clock

This document covers the structural choices made while developing the underlying `coati_engine.h` physics engine driving the clock simulation.

---

## The C++ Physics Engine

The core of the logic is decoupled from ESPHome's internal node graph. Instead of registering an overriding `Component` with rigid setup/loop lifecycle bindings, the entire engine is instantiated as a static pointer natively inside the isolated display lambda in `bb32.yaml`:

```cpp
static CoatiEngine* engine = nullptr;
if (engine == nullptr) {
    engine = new CoatiEngine();
    id(engine_ptr) = (void*)engine;
}
```

This isolates the engine context, prevents ESPHome template instantiation mismatch errors during `gcc` compilation (specifically relating to the `GlobalsComponent` trying to marshal complex custom structs), and gives the display lambda exclusive polling access via `engine->tick()`.

The engine pointer is shared to the `interval:` component via the `engine_ptr` global (a `void*` cast), allowing asynchronous font rasterization outside the render lambda.

---

## The `DummyDisplay` Intercept

**Challenge:** To calculate pathfinding routes, the engine needs to know the exact grid coordinates of the incoming time text string. However, extracting rasterized pixels from an existing `font::Font` file programmatically is exceptionally difficult in barebones C++ without heavyweight graphics libraries.

**Solution:** `coati_engine.h` defines a custom `DummyDisplay` class that inherits from `esphome::display::DisplayBuffer`.
- By mocking `get_width_internal` and `get_height_internal`, we trick ESPHome into treating this object as a valid screen.
- When `engine->prepare_target()` is called, it triggers `dummy->strftime(...)`.
- We override `draw_absolute_pixel_internal()` to intercept every pixel drawn by the BDF font rendering engine, saving them as `true` inside a local boolean array instead of driving hardware pins.
- The result is copied into `pending_target[][]` (a staging buffer), which is then atomically swapped into `target_board[][]` by `apply_pending_target()` inside the render lambda context. This prevents torn reads during the swap.

---

## Double-Buffered Target Rendering

Font rasterization runs in the `interval:` timer context (a FreeRTOS timer task at higher priority than the main loop). It writes to `pending_target[][]` and sets `target_pending = true`. The render lambda, running in the main task context, calls `apply_pending_target()` at the top of each frame, which atomically swaps in the new target and resets agent state (paths, wait/bored ticks). This prevents race conditions between font rasterization and the display pipeline.

---

## Wobbly Time

The device stays synced to real SNTP time but tracks and displays a "wobbly" virtual time advanced 2–5 minutes ahead (range configurable in `bb32.yaml`). The algorithm:

- At startup (or when the current wobbly offset drifts outside the configured range), a new random target offset is chosen uniformly in `[wobble_min_sec, wobble_max_sec]`.
- If wobbly time is **behind** its target: virtual seconds tick 30% faster than wall-clock seconds.
- If wobbly time is **ahead** of its target: virtual seconds tick 30% slower.
- The 30% rate is configurable (`wobble_catch_up_rate`).

This allows the clock to display a plausible but slightly drifted time, giving the coatis slack to catch up or slow down without jarring jumps.

---

## Multi-Agent Decision Tree

The `CoatiEngine` holds an active vector of `CoatiAgent` structs. Decision execution during `tick()` is split into logical phases:

### 1. Goal Claiming (Economy)
Before any pathfinding routes are drawn, the engine extracts the delta between `current_board` and `target_board` into `extras` (pixels to remove) and `missing` (pixels to add). Each agent claims exactly one destination, preventing duplicate work.

### 2. Budgeted A* Pathfinding
The `find_path()` function implements time-budgeted A* with a configurable node expansion limit (`max_nodes = 40` by default):

- If the destination is reached within the budget, the full optimal path is returned.
- If the budget is exhausted before reaching the destination, the path is returned to the **best frontier node** — the settled cell geometrically closest to the destination. This is the "downhill" fallback.
- The agent walks the partial path, then re-plans on the next tick from its new (closer) position.
- This produces naturally organic, meandering movement on long journeys and keeps per-tick CPU time bounded.

Steps are costed: orthogonal = 1.0, diagonal = 1.414. Other agents apply a soft penalty (+20) rather than a hard block, preventing permanent path failures in narrow corridors.

### 3. Placement Guard (Claimed Target Only)
Agents carrying a pixel may only **place** it at `a.pos == a.claimed_target`. They do NOT place at intermediate missing positions they happen to step through en route. This is critical with budgeted partial paths: the intermediate waypoint endpoint can coincidentally land on another clock pixel that needs filling, causing a misplaced pixel. See **Issue #4** in the issues log.

### 4. Deadlock Recovery & Escape
In narrow passages, two agents can collide head-on:
- Wait timeout is asymmetric by agent index (`3 + i * 2` ticks), so they don't break standoffs synchronously.
- On timeout, the agent wipes its path and takes a single valid retreat step into an adjacent clear space.

### 5. Endpoint Asymmetry
Agents use `dumpster[i%2]` and `pool[i%2]` as their respective fetch/drop endpoints. This ensures each agent has its own dedicated slot on each end, preventing deadlocks at the pickup/dropoff points.

### 6. Hardware-Safe Contexts (Interrupt Isolation)
- **Pathing Stagger:** A `path_calculated_this_tick` flag limits `find_path()` to ONE agent per 50ms tick, flattening the CPU spike.
- **Font Rasterization:** The BDF text rasterizer runs asynchronously via the `interval:` component, not inside the render lambda.

### 7. Idle Throttling
When no work remains, agents use a `bored_ticks` counter and only take a random step every 20 ticks (~1,000ms), producing a docile crawling behavior instead of jittery noise.

---

## Render Pipeline

```
interval (1s)        render lambda (33ms)
─────────────        ────────────────────────────────────────────────
prepare_target()  →  apply_pending_target()   (atomic target swap)
  write pending_      tick()                   (physics: 50ms budget)
  target[][]          fade_board advancement   (active pixels only)
                      memcpy snapshot          (current_board, fade_board)
                      draw board pixels        (green, fade-weighted)
                      draw agent sprites       (motion-interpolated)
```

The render lambda runs at 33ms (≈30fps). Physics ticks fire every 50ms from within the lambda, giving ~1.5 render frames per physics step. Agent positions between ticks are motion-interpolated using `blend = elapsed / 50ms`.

Pixel fade: when a pixel is placed, `fade_board[x][y]` is reset to 0.0 and advances by ~0.032 per frame (reaching 1.0 in ~500ms). The fade loop only advances pixels where `current_board[x][y]` is true, preventing stale accumulation on dark positions.

---
---

# Known Issues & Engineering Log

> This section documents bugs, root causes, and resolutions encountered during development. Future engineers: these **will** bite you again as complexity grows.

---

## Issue #1 — Stack Overflow Corrupting `current_board` (PRIMARY SPARKLE SOURCE)

**Symptom:** Intermittent "sparkle" — random green pixels briefly illuminating near the drawn clock digits. More frequent during active coati movement. At startup, sparkle density tracked with how many clock pixels had been placed.

**Root Cause:** `find_path()` allocated its scratch arrays on the call stack:
```cpp
bool visited[32][8];    // 256 bytes
Point parent[32][8];    // 2048 bytes  ← Point is 8 bytes
float cost[32][8];      // 1024 bytes
// Total: ~3.3KB on stack per find_path() call
```
The ESP32-C3's main task stack (default ~8KB in ESPHome) was being overflowed when the render lambda's frame + `tick()` frame + `find_path()` frame all accumulated. On RISC-V, the stack grows downward and can corrupt adjacent heap memory. The `CoatiEngine` object is heap-allocated (`new CoatiEngine()`), so a stack underflow corrupted `current_board[][]` — causing random positions to flip `true` and briefly illuminate.

**Resolution:** Moved `visited`, `parent`, and `cost` from stack-local to pre-allocated member variables of `CoatiEngine`:
```cpp
bool pf_visited[32][8];
Point pf_parent[32][8];
float pf_cost[32][8];
```
These live in the heap with the rest of the engine object. Stack usage per `find_path()` call dropped by ~3.3KB. Sparkle frequency dropped dramatically.

**Lesson:** On embedded targets with small default task stacks, any function allocating >1KB of stack locals inside a deep call chain is high risk. Always budget the complete call-stack depth: render_lambda + tick() + find_path() + priority_queue operations.

---

## Issue #2 — Priority Queue Heap Churn

**Symptom:** Residual sparkle after Issue #1 fix. Correlated with active coati pathfinding.

**Root Cause:** `std::priority_queue` backed by `std::vector` performs heap reallocations as it grows. Each `find_path()` call created a new queue object at capacity 0, growing via repeated `new`/`copy`/`delete` cycles as nodes were pushed. On embedded targets, frequent small heap allocations cause fragmentation and occasionally trigger reallocation at inconvenient moments relative to the WS2812B RMT DMA window.

**Resolution:** Pre-reserve a `pf_q_storage` vector as a member variable of `CoatiEngine` and pass it as the backing container to the priority queue:
```cpp
pf_q_storage.reserve(448);  // 40 nodes * 8 neighbors + headroom
// In find_path():
pf_q_storage.clear();  // clears without freeing capacity
std::priority_queue<..., CompareNode> q(CompareNode{}, pf_q_storage);
```
After first warm-up, no further heap reallocation occurs.

**Lesson:** `std::priority_queue` (and any STL container) will allocate by default. On embedded, pre-reserve or use static storage. `clear()` on a vector preserves capacity; the priority_queue will reuse it.

---

## Issue #3 — Attempted O(N²) Dijkstra Replacement

**Symptom:** Replacing A* with a simpler allocation-free O(N²) Dijkstra (linear min-scan instead of priority queue) caused sparkle to **return** at original intensity.

**Root Cause:** Not fully diagnosed. The O(N²) scan (256 outer iterations × 256 inner cell reads) was expected to complete in < 1ms via early-exit when the destination was found. However, in practice the sparkle returned as severely as before the Issue #1 fix. Suspected causes: (a) pathological worst-case inputs hitting full O(N²) and displacing RMT DMA callbacks, or (b) a subtle interaction between the linear scan's memory access pattern and cache behavior that we weren't able to isolate. The reversion to priority_queue A* immediately restored the near-zero-sparkle behavior.

**Resolution:** Reverted to A* with priority_queue. Kept all the memory-layout improvements from Issue #1 and #2.

**Lesson:** On a single-core microcontroller, algorithm complexity matters less than execution time predictability and memory access locality. An "obviously simpler" algorithm can introduce unexpected timing problems if it changes allocation patterns, cache behavior, or interrupt-responsiveness of the main loop.

---

## Issue #4 — Budgeted Partial Paths Cause Misplaced Pixels ("Slow Sparkle")

**Symptom:** After implementing time-budgeted A* (stops after 40 node expansions, returns partial path to best frontier), a new artifact appeared: several LEDs lit up near the clock digits and stayed lit for multiple seconds before fading out. Distinctly different from the fast single-frame sparkle of Issues #1/#2.

**Root Cause:** The budgeted A* returns a partial path whose endpoint is the geometrically closest settled node to the destination. This intermediate waypoint can **coincidentally be another clock pixel** that also happens to need placing (i.e., it's in the `missing` set).

The original placement guard was:
```cpp
if (standing_on_missing && !current_board[a.pos.x][a.pos.y]) {
    // place pixel HERE
```

`standing_on_missing` is `true` for **any** missing position, not just the agent's claimed target. So an agent carrying a pixel, traveling toward missing position A via partial path, would stop at intermediate missing position B, place its pixel there, and continue without a pixel. Position B would then glow with the biological fade-in for several seconds until another agent picked it up as an extra.

**Resolution:** Added explicit claimed-target guard to placement:
```cpp
bool at_claimed_target = (a.claimed_target.x != -1 && a.pos == a.claimed_target);
if (standing_on_missing && !current_board[a.pos.x][a.pos.y] && at_claimed_target) {
```

Agents now only place at their exact claimed destination. Passing through other missing positions triggers a re-plan, not a premature placement.

**Lesson:** Any change to **how agents move** (partial paths, alternate routing, A* tuning) requires re-auditing all the **action trigger conditions** that assume agents arrive at positions in predictable ways. The claimed_target guard should have been in place from the start — it's the invariant that makes the economy model correct.

---

## Timing & Resource Budget Reference

| Resource | Value | Notes |
|---|---|---|
| ESP32-C3 CPU | 160 MHz | Single core RISC-V |
| Main task stack | ~8KB | ESPHome default; deep call chains risky |
| WS2812B data time | ~7.7ms | 256 LEDs × 24 bits × 1.25µs/bit |
| Render interval | 33ms | ~30fps |
| Physics tick | 50ms | Called from within render lambda |
| A* node budget | 40 nodes | ~50µs typical, ~400µs worst-case |
| pf_q_storage | 448 slots × 12B = ~5.4KB heap | Pre-reserved; no reallocation after warmup |
| pf_visited/parent/cost | 3.3KB heap (member vars) | Was stack — caused Issue #1 |
| board_snap / fade_snap | 1.3KB stack | In render lambda — acceptable |
| Fade-in rate | ~0.032/frame | 0→1 in ~500ms at 33ms frame |
| Bored wander rate | 1 step / 1000ms | 20 bored_ticks × 50ms/tick |

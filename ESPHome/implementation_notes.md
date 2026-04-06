# Coati Clock — Engineering & Implementation Reference

This document describes the architectural decisions, subsystem design, and known failure modes of the Coati Clock firmware. It is intended as a reference for engineers maintaining or extending this system. Sections are written to explain not only *what* was built, but *why* specific choices were made, so future engineers can reason about trade-offs when modifying the system.

---

## 1. System Overview

The Coati Clock is an ESPHome-based firmware for an ESP32-C3 microcontroller driving a 32×8 matrix of addressable WS2812B LEDs. The display shows the current time using a multi-agent simulation: two virtual "coati" sprites move individual illuminated pixels across the matrix to construct and maintain clock digit glyphs. The simulation runs in real time and is continuously re-animated as the displayed time changes.

The device synchronizes to wall-clock time via SNTP but displays a "wobbly" virtual time — a configurable offset from real time, with a drift-correction algorithm that keeps the virtual clock self-consistent.

---

## 2. Architecture

### 2.1 Physics Engine Isolation

The core simulation logic resides entirely in `coati_engine.h` and is decoupled from ESPHome's internal component graph. Rather than subclassing `esphome::Component` (which would impose rigid setup/loop lifecycle bindings and trigger template instantiation conflicts in `GlobalsComponent` when marshalling complex custom structs), the engine is instantiated once as a heap-allocated object via a raw pointer inside the display lambda:

```cpp
static CoatiEngine* engine = nullptr;
if (engine == nullptr) {
    engine = new CoatiEngine();
    id(engine_ptr) = (void*)engine;
}
```

The pointer is shared to the `interval:` component via an ESPHome `global:` variable typed as `void*`. This indirection allows the asynchronous font rasterization task to access the same engine instance without introducing a statically initialized global object, which can cause initialization order problems in embedded build systems.

### 2.2 The DummyDisplay Font Intercept

The pathfinding system requires knowledge of the exact pixel coordinates of each clock digit glyph. Extracting rasterized pixel data directly from an ESPHome `font::Font` object at runtime is impractical without access to the underlying BDF parser internals.

The solution is a mock display class, `DummyDisplay`, which subclasses `esphome::display::DisplayBuffer`. It satisfies the virtual interface with stub implementations of `get_width_internal()` and `get_height_internal()`, making ESPHome's font rendering pipeline treat it as a valid output device. The `draw_absolute_pixel_internal()` method is overridden to write pixel coordinates into a local boolean array instead of driving hardware.

When `prepare_target()` is called, it invokes `dummy->strftime()` with the target time string. The resulting pixel map is captured in `pending_target[][]`.

### 2.3 Double-Buffered Target Rendering

Font rasterization runs in the FreeRTOS timer task context (used by the ESPHome `interval:` component), which executes at higher priority than the main task. The display render lambda runs in the main task context. These two contexts share access to the engine's `target_board[][]` state.

To prevent torn reads, target updates use a double-buffer pattern:

1. The interval task writes the newly rasterized glyph layout to `pending_target[][]` and sets the `volatile bool target_pending` flag.
2. At the top of each render frame, the render lambda calls `apply_pending_target()`, which atomically swaps `pending_target` into `target_board` and resets agent state (clearing paths, wait counters, and bored counters).

This ensures the render loop always sees a consistent, non-torn board state. Agent resets occur within the main task, preventing race conditions between target updates and in-flight pathfinding operations.

### 2.4 Wobbly Time

The device synchronizes to real SNTP time but tracks and displays a virtual time offset by a configurable number of minutes ahead of wall-clock time. The algorithm operates as follows:

- At initialization (and whenever the accumulated offset drifts outside the configured range), a new target offset is sampled uniformly from the interval `[wobble_min_sec, wobble_max_sec]` (configured in `bb32.yaml`).
- If the current virtual time is **behind** its target offset: each virtual second elapses at `1 + wobble_catch_up_rate` times the real rate (default: 1.30×).
- If the current virtual time is **ahead** of its target offset: each virtual second elapses at `1 - wobble_catch_up_rate` times the real rate (default: 0.70×).

The result is a clock that displays a plausible but drifted time, self-correcting without visible jumps. Because the displayed time is always in the future relative to real time, visual latency introduced by pathfinding delays is perceptually acceptable.

---

## 3. Multi-Agent Simulation

### 3.1 Board State Model

Three parallel arrays define the state of every cell in the 32×8 grid:

| Array | Type | Meaning |
|---|---|---|
| `current_board[32][8]` | `bool` | Whether a pixel is physically placed and lit |
| `target_board[32][8]` | `bool` | Whether a pixel should be present in the current glyph |
| `fade_board[32][8]` | `float` | Interpolated brightness [0.0, 1.0] for biological fade-in |

The delta between `current_board` and `target_board` produces two task lists per tick:
- **extras**: positions in `current_board` but not in `target_board` (pixels to remove)
- **missing**: positions in `target_board` but not in `current_board` (pixels to add)

### 3.2 Agent Decision Phases

Each call to `tick()` processes agents in two sequential phases.

**Phase 1 — Movement:** Agents with a non-empty `current_path` take their next step. A collision system detects head-on conflicts and applies asymmetric timeout logic (patience = `3 + agent_index * 2` ticks) so agents do not break deadlocks synchronously. On timeout, the blocked agent executes a one-tile retreat into the nearest unoccupied adjacent cell.

**Phase 2 — Decision:** Agents with an empty path and no pending wait evaluate their situation and issue new orders:
- A **carrying** agent heads toward its claimed missing position, the pool (disposal), or waits for new work.
- A **non-carrying** agent heads toward an unclaimed extra (to pick up), the dumpster (to fetch from supply), or enters idle wander.

To prevent duplicate work, each agent filters the global `extras` and `missing` lists against the `claimed_target` values of all other agents before selecting a destination.

### 3.3 Budgeted A\* Pathfinding

The `find_path()` function implements A\* with a configurable node-expansion budget (`max_nodes`, default 40). The function maintains three pre-allocated member arrays (`pf_visited`, `pf_parent`, `pf_cost`) and a pre-reserved priority queue backing store (`pf_q_storage`, capacity 448 entries). These are allocated once at construction and reused on every call, avoiding heap allocation during normal operation.

**Partial path behavior:** If the destination is not reached within the node budget, the function returns the path to the **best frontier node** — the settled cell with the smallest Euclidean distance to the destination. The agent walks this partial path and re-plans on the next tick from its new (closer) position. Each successive re-plan covers a shorter distance, completing within budget. This iterative approach produces naturally meandering movement on long journeys, as each re-plan reflects the current obstacle state (other agents may have moved).

**Soft agent avoidance:** Agent positions are treated as traversable with a +20 cost penalty rather than hard obstacles. This prevents permanent path-planning failure in narrow corridors where a hard block would leave no valid route.

**Path stagger:** A per-tick boolean flag (`path_calculated_this_tick`) limits pathfinding to one agent per tick, distributing the computational cost across frames and preventing CPU spikes at the moment of a minute rollover (when all agent paths are simultaneously invalidated).

### 3.4 Placement Guard

Agents carrying a pixel may only place it when `a.pos == a.claimed_target`. This guard is critical with budgeted partial paths: the intermediate waypoint that terminates a partial path may coincidentally overlap with another position in the `missing` set. Without the guard, the agent would prematurely place its pixel at the wrong position. See **Issue #4** in the issues log.

### 3.5 Endpoint Asymmetry

Each agent uses `dumpster[i % 2]` and `pool[i % 2]` as its assigned fetch and disposal endpoints respectively. This prevents both agents from simultaneously targeting the same single-cell endpoint, which would cause a deadlock at the node with no valid resolution.

### 3.6 Biological Fade-In

When a pixel is placed, `fade_board[x][y]` is reset to 0.0 and advances by approximately 0.032 per render frame (reaching 1.0 in approximately 500ms at 33ms frame intervals). The fade advancement loop is gated on `current_board[x][y] == true`, ensuring that unoccupied positions never accumulate stale brightness values regardless of prior state.

### 3.7 Idle Throttling

When no extras or missing positions exist, agents enter idle mode. Rather than stepping every tick, agents accumulate a `bored_ticks` counter and take a single random step only when `bored_ticks >= 20` (approximately one step per 1,000ms). This produces docile, slow wandering behavior.

---

## 4. Render Pipeline

```
interval: (1s)            display lambda (33ms)
──────────────────        ─────────────────────────────────────────────
prepare_target()      →   apply_pending_target()   atomic target swap
  rasterize glyph          tick()                  physics step (50ms gate)
  write pending_target      fade_board advance      active pixels only
  set target_pending        memcpy snapshot         board + fade arrays
                            draw board pixels        green, fade-weighted
                            draw agent sprites       motion-interpolated
```

The render lambda executes at 33ms intervals (≈30 fps). The physics tick fires from within the render lambda whenever at least 50ms have elapsed since the last tick, giving approximately 1.5 render frames per physics step. Agent positions are motion-interpolated between ticks using `blend = elapsed_ms / 50.0`.

---

## 5. Known Issues & Engineering Log

> This section documents bugs discovered during development, their root causes, and their resolutions. Engineers extending this system should review these entries before modifying pathfinding, rendering, or memory layout, as the underlying constraints that produced each issue remain present.

---

### Issue #1 — Stack Overflow Corrupting `current_board` *(Primary Sparkle Source)*

**Symptom:** Intermittent "sparkle" — random green pixels briefly illuminating near the drawn clock digits. Frequency correlated with active agent movement. At startup, sparkle density tracked proportionally with how many clock pixels had been placed.

**Root Cause:** `find_path()` allocated its scratch arrays as call-stack locals:

```cpp
bool visited[32][8];    // 256 bytes
Point parent[32][8];    // 2048 bytes  (Point = two int32 fields)
float cost[32][8];      // 1024 bytes
// Total: ~3.3 KB of stack per find_path() call
```

The cumulative call-stack depth of the render lambda → `tick()` → `find_path()` chain exceeded the ESP32-C3 main task stack (ESPHome default: approximately 8 KB). On RISC-V, the stack grows downward into the heap. The `CoatiEngine` object is heap-allocated (`new CoatiEngine()`), and the stack underflow corrupted its `current_board[][]` member array — causing arbitrary positions to read as `true` and briefly illuminate.

**Resolution:** The three scratch arrays were moved from stack-local scope to pre-allocated member variables of `CoatiEngine` (`pf_visited`, `pf_parent`, `pf_cost`). These reside in the heap alongside the rest of the engine object. Per-call stack usage of `find_path()` dropped by approximately 3.3 KB. Sparkle frequency dropped dramatically.

**Standing Constraint:** The main task stack remains a finite shared resource. Any function introduced into the render lambda → tick() call chain with significant stack allocation should be profiled. The current headroom is approximately 3–4 KB. Recursive functions, deep STL iterator chains, and large VLAs are high-risk additions.

---

### Issue #2 — Priority Queue Heap Churn

**Symptom:** Residual sparkle following the Issue #1 fix. Frequency still correlated with active pathfinding.

**Root Cause:** `std::priority_queue`, backed by `std::vector`, performs heap reallocations as it grows. Each call to `find_path()` constructed a new queue object at zero capacity, which then grew via repeated allocate/copy/free cycles as nodes were pushed. On embedded targets with limited heap, frequent small allocations cause fragmentation and occasionally interleave with time-critical subsystems (specifically, ESPHome's RMT DMA callback infrastructure for WS2812B output).

**Resolution:** A `std::vector<std::pair<float, Point>>` member variable (`pf_q_storage`) is reserved to 448 entries at construction time and passed as the backing container to the priority queue. Before each call to `find_path()`, the vector is cleared with `pf_q_storage.clear()`, which resets its size to zero without releasing capacity. After the initial warm-up call, no further heap reallocation occurs during normal operation.

**Standing Constraint:** The 448-entry reservation is sized for a node budget of 40 (`max_nodes * 8 neighbors + headroom`). If `max_nodes` is significantly increased, `pf_q_storage.reserve()` in the constructor should be updated proportionally.

---

### Issue #3 — O(N²) Dijkstra Replacement Regression

**Symptom:** An attempt to replace A\* with a simpler, allocation-free O(N²) Dijkstra implementation (linear minimum scan over `pf_cost[][]` instead of a priority queue) caused sparkle to return at intensity comparable to the pre-fix baseline.

**Root Cause:** Not definitively isolated. The O(N²) minimum scan was expected to complete in under 1ms per path via early exit when the destination was settled. However, the regression was immediate and severe. The leading hypotheses are: (a) in pathological cases (long paths through dense glyph obstacles), the full 256-iteration worst case was reached, producing CPU execution bursts that displaced FreeRTOS timer callbacks and/or RMT DMA service; or (b) the linear scan's sequential memory access pattern over a 256-element pf_cost array produced cache pressure that interacted poorly with the addressable LED DMA buffer at a critical timing boundary. The exact mechanism was not confirmed before the decision was made to revert.

**Resolution:** The O(N²) Dijkstra was abandoned. The A\* implementation with priority queue and pre-reserved backing store (see Issue #2) was restored. All memory-layout improvements from Issue #1 were retained.

**Lesson:** On a single-core microcontroller, algorithmic simplicity does not guarantee timing predictability. A change that eliminates dynamic allocation may introduce different timing pathologies through execution duration, memory access patterns, or interrupt latency. Any proposed replacement to the pathfinding core should be validated against a deployed device, not benchmarked in isolation.

---

### Issue #4 — Budgeted Partial Paths Cause Misplaced Pixels *(Slow Sparkle)*

**Symptom:** Following the introduction of time-budgeted A\* with best-frontier fallback, a new artifact was observed: clusters of LEDs near the clock digits illuminated gradually and remained lit for several seconds before fading out. This "slow sparkle" was visually distinct from the fast single-frame artifacts of Issues #1 and #2.

**Root Cause:** The budgeted A\* returns a partial path whose terminal node is the geometrically closest settled node to the destination — an intermediate waypoint, not the intended clock pixel. This intermediate waypoint may coincidentally occupy a position that is also present in the `missing` set (i.e., another clock pixel that has not yet been placed by any agent).

The original placement condition was:

```cpp
if (standing_on_missing && !current_board[a.pos.x][a.pos.y]) {
    // place pixel at a.pos
}
```

`standing_on_missing` evaluates to `true` for **any** missing position, not exclusively the agent's assigned target. A carrying agent would therefore place its pixel at a random intermediate missing position, producing a spurious illuminated pixel. This pixel would remain until a subsequent tick identified it as an `extra` and dispatched an agent to remove it — a process that could take several seconds depending on agent workload.

**Resolution:** A claimed-target guard was added to the placement condition:

```cpp
bool at_claimed_target = (a.claimed_target.x != -1 && a.pos == a.claimed_target);
if (standing_on_missing && !current_board[a.pos.x][a.pos.y] && at_claimed_target) {
    // place pixel at a.pos
}
```

Agents may only place at their exact claimed destination. An agent standing at any other missing position falls through to the re-plan path and continues toward its actual target.

**Standing Constraint:** The claimed-target invariant must be preserved by any future modification to agent movement or path planning. Any mechanism that causes an agent to arrive at a position other than its `claimed_target` before the path is exhausted — including priority overrides, emergency reroutes, or inter-agent handoff schemes — must account for this placement guard. Opportunistic **pickup** of extras (which has no target-check guard) is intentional and correct; only *placement* requires the strict match.

---

## 6. Timing & Resource Budget Reference

| Resource | Value | Notes |
|---|---|---|
| Processor | ESP32-C3, 160 MHz | Single-core RISC-V RV32IMC |
| Main task stack | ~8 KB | ESPHome default; see Issue #1 |
| WS2812B frame time | ~7.7 ms | 256 LEDs × 24 bits × 1.25 µs/bit |
| Render interval | 33 ms (≈30 fps) | `update_interval` in `bb32.yaml` |
| Physics tick gate | 50 ms | Evaluated per render frame |
| A\* node budget | 40 nodes | `max_nodes` in `find_path()` |
| `pf_q_storage` | 448 slots × 12 B ≈ 5.4 KB | Heap; pre-reserved at construction |
| `pf_visited/parent/cost` | 3.3 KB | Heap (member vars); see Issue #1 |
| `board_snap` / `fade_snap` | 1.3 KB | Stack in render lambda; acceptable |
| Fade-in rate | ~0.032 / frame | 500 ms to full brightness at 33 ms/frame |
| Bored wander rate | 1 step / 1,000 ms | 20 `bored_ticks` × 50 ms/tick |
| Wobbly time range | 120–300 s | Configurable in `bb32.yaml` |
| Catch-up rate | ±30% | Configurable in `bb32.yaml` |

---

## 7. Engineering Rules

### Rule #1 — DO NOT BREAK OTA

Over-the-air update capability is the project's most critical operational primitive. Without it, every firmware change requires physical access to the device and a USB cable.

**If OTA appears to be broken, STOP. Verify it with certainty before making any structural changes.** Specifically:

1. Attempt the OTA flash directly by IP: `esphome run bb32.yaml --device 192.168.69.164 --no-logs`
2. Confirm the exact error message. "Connection reset by peer" and "Authentication failed" are different problems with different fixes.
3. Check whether the issue is OTA at all — a failure in a separate subsystem (e.g., a telemetry client failing to reach its server) will produce a similar-looking error in the logs but has nothing to do with OTA.
4. Do not change the SDK version, LED driver, logging level, framerate, or any other structural parameter until OTA failure is confirmed with a direct flash attempt.

---

### Issue #5 — OTA Spiral: Misread Telemetry Error Triggers Unnecessary Structural Changes

**Date:** 2026-04-06

**Symptom:** OTA flashing appeared broken. A cascade of SDK version changes (5.1.6 → 4.4.6 → 5.1.6), LED driver swaps, framerate reductions, and logging changes were made in an attempt to "fix" OTA, breaking the device into a boot loop at one point.

**Root Cause:** The original error was not an OTA failure at all. The Stra2us telemetry client was failing to reach its server (`192.168.153.x`) from a device on a different subnet (`192.168.69.x`) — a simple network routing issue. The "Failed to create socket" message was logged at the same severity level as OTA errors and was misread as evidence that OTA was broken.

The actual OTA failure, when it was later tested directly, was a **protocol version mismatch**: the device had been originally flashed with OTA v1 firmware, but ESPHome 2025.5.2 defaults to the newer OTA v2 handshake. The fix was a single line: `version: 1` in the `ota:` config block to sync protocols (followed by a migration to v2 via one intentional USB flash).

**Resolution:**
- Added `version: 1` to OTA config to restore the handshake.
- Performed one USB flash to migrate to OTA v2 (the modern default).
- Verified OTA v2 works wirelessly. USB cable is no longer required for updates.

**Lesson:** See **Rule #1** above. Verify OTA failure with a direct flash attempt before touching anything structural. A connection error in an unrelated subsystem is not evidence that OTA is broken.

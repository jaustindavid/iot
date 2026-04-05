# Engineering & Implementation Notes: Coati Clock

This document covers the structural choices made while developing the underlying `coati_engine.h` physics engine driving the clock simulation. 

## The C++ Physics Engine
The core of the logic is decoupled from ESPHome's internal node graph. Instead of registering an overriding `Component` with rigid setup/loop lifecycle bindings, the entire engine is instantiated as a static pointer natively inside the isolated display lambda in `bb32.yaml`:
```cpp
static auto engine = new CoatiEngine();
```
This isolates the engine context, prevents ESPHome template instantiation mismatch errors during `gcc` compilation (specifically relating to the `GlobalsComponent` trying to marshal complex custom structs), and gives the 50ms display lambda exclusive polling access via `engine->tick()`.

## The `DummyDisplay` Intercept
**Challenge:** To calculate pathfinding routes, the engine needs to know the exact grid coordinates of the incoming time text string. However, extracting rasterized pixels from an existing `font::Font` file programmatically is exceptionally difficult in barebones C++ without heavyweight graphics libraries.
**Solution:** `coati_engine.h` defines a custom `DummyDisplay` class that inherits from `esphome::display::DisplayBuffer`. 
- By mocking the `get_width_internal` and `get_height_internal` functions, we trick ESPHome into treating this object as a valid screen.
- When `engine->update_target()` is called, it triggers `dummy->strftime(...)`.
- We override `draw_absolute_pixel_internal()` to intercept every pixel drawn by the BDF font rendering engine, blindly saving them as `true` inside a local boolean array instead of driving hardware pins.

## Multi-Agent Decision Tree
The `CoatiEngine` holds an active vector of `CoatiAgent` structs. Decision execution during `tick()` is split into logical phases:

### 1. Goal Claiming (Economy)
The most severe issue in multi-agent arrays is duplicated work. Before any pathfinding routes are drawn, the engine extracts the delta between the `current_board` and `target_board` representing available `extras` and `missing` task destinations.
- Each `CoatiAgent` stores a `claimed_target`. 
- As the loop evaluates agents sequentially, it strips any coordinate that has already been claimed by Agent[N] from the availability list before handing it to Agent[N+1]. 

### 2. Physical Collision & Pathing
An A* pathfinding algorithm is utilized for evaluating `start` to `dest`.
- Steps orthogonal (cost 1.0) and diagonal (cost 1.4).
- The `current_board` (the currently illuminated green pixels on the clock face) acts as hard grid constraints. 
- *Soft Agent Avoidance:* In A*, other agents are treated with a soft algorithm penalty (+20 cost) instead of a hard block. This permits an agent to selectively route straight into an alleyway occupied by its partner rather than returning an empty path array and permanently panicking.

### 3. Deadlock Recovery & Escape
In narrow passages, two agents can collide head-on, continually pausing for the other to move. 
- Timeout patience scales asymmetrically based on Agent Index (`3 + i * 2`). This offsets their impatience so they don't break symmetrical standoffs synchronously.
- If a timeout is reached, the panicking agent wipes its path and mathematically traces a 1-tile valid retreat step into a physically clear adjacent space, unblocking the alleyway for the more patient agent to clear.

### 4. Target Acquisition & Ghost Assignments
Endpoints (Dumpster & Pool) evaluate against an `available_missing` delta rather than a raw `missing` array. If an agent arrives at an endpoint after its partner has already locked onto the final destination coordinate, it is mathematically blocked from spawning a payload, preventing it from freezing dead while holding a "Ghost" assignment.

### 5. Hardware Safe Contexts (Interrupt Isolation)
To protect strictly timed serial data protocols (WS2812B handled via the hardware RMT buffer) from CPU starvation underruns, heavy mathematical actions are staggered or offloaded:
- **Pathing Stagger:** A blocking boolean limits `find_path()` execution to ONE agent per 50ms engine frame, flattening the CPU spike when tasks are wiped during a minute rollover.
- **Font Rasterization:** The intense BDF text rasterizer `DummyDisplay::strftime()` is entirely removed from the ESPHome `addressable_light` render cycle. Instead, it executes asynchronously via an isolated memory pointer mapped to an ESPHome `interval:` component running on the primary background Arduino `loop()`.

### 6. Idle Throttling
When no work remains, agents trigger a random step. However, jumping wildly at 50ms intervals is visually chaotic. Agents increment a `bored_ticks` counter against the 50ms main loop, and only process a random step when `bored_ticks >= 20` (equivalent to one step every 1,000ms), achieving a docile, crawling behavior.

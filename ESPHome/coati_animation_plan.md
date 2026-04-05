# Coati Animation Engine (Revised)

This document outlines the proposed architecture for replacing the standard
digital clock with a custom, animated C++ engine where a 1x1 grey "coati" pixel
scampers around the 8x32 LED matrix to physically transport and place each
pixel.

## Core Rules & Logic 
Based on your feedback, the Coati adheres to strict game-like physics:
1. **No Teleporting:** She must take each step physically.
2. **Pathfinding Obstacles:** She cannot phase through or overlap existing
   active pixels. She must maneuver around them using Dijkstra/A* pathfinding,
   taking advantage of the font's designed "holes".
3. **Movement:** She can step 1 space horizontally, vertically, or diagonally
   per frame. 
4. **Conservation of Pixels (Economy):**
   - She must physically carry pixels between locations.
   - **Matching Pixels:** If the current time shares pixels or simply has pixels
     available, she must pick up an existing pixel from its current position and
     MOVE it to the new time's target location.
   - **Extra Pixels (Discards):** Any leftover pixels must be picked up and
     carried to a **Blue Pool** (bottom-right corner, `[31][7]`) and "dunked" to
     be discarded.
   - **Missing Pixels (Fetches):** If she needs more pixels than are currently
     displayed, she must path to a **Red Dumpster** (bottom-left corner,
     `[0][7]`) and fetch them one by one.

## Proposed Architecture

### 1. `coati_engine.h` (Custom C++ Logic)
We cannot achieve this level of complexity inside a simple YAML lambda. We will
create a standalone `coati_engine.h` file containing a `CoatiClock` class.
The class will maintain:
- `bool current_board[32][8]`: The actual pixels currently illuminated on the
  board (what the Coati has to navigate around).
- `bool target_board[32][8]`: The layout of the incoming timestamp we are
  trying to build.
- **The Coati State Machine:**
  - Coordinates: `(int x, int y)`
  - Status: `IDLE`, `PATHING_TO_PICKUP`, `PATHING_TO_DEPOSIT`,
    `PATHING_TO_DUMPSTER`, `PATHING_TO_POOL`.
  - Inventory: `bool carrying_pixel`.
- **A* Pathfinding Algorithm:** A function that receives `start(x,y)`,
  `end(x,y)`, and checks against `current_board` grids to build a list of
  stepping coordinates. Diagonals are weighted slightly higher to prefer
  straight lines but allowed for squeezing through gaps.

### 2. Time Synchronization
When the minute rolls over:
1. ESPHome yields the timestamp string (e.g. `1234`).
2. We internally render the string using a dummy buffer to identify which
   `[x][y]` coordinates make up the *target* board.
3. We compare `current_board` and `target_board` to calculate how many pixels
   are moving vs. being added/removed.
4. The Coati state machine kicks in and locks out any new time updates until
   she finishes her chores.

### 3. Rendering Intercept
The standard ESPHome lambda running at a high refresh rate (e.g., 50ms) will
simply call `my_coati_clock->tick()` and then `my_coati_clock->draw(it)`.
The `draw()` command will iterates over `current_board` to draw green pixels,
conditionally draws the red dumpster/blue pool, and draws the 1x1 grey pixel
representing the Coati.

## Open Questions
1. **Render Colors:** I assume the actual time pixels will be Green. The
   Dumpster will be Red and Pool will be Blue. Does the Dumpster/Pool only
   render when she is actively using them, or do you want them visible on the
   matrix permanently?
2. **Pathing Order:** Does she tear down all unnecessary old pixels (dumping
   them) FIRST, and then build the new pixels, or just handle all available
   moves sequentially from left to right? (Clearing out unused pixels first
   usually guarantees better pathfinding routes).

## Verification Plan
1. Stand up `coati_engine.h` and implement an A* pathfinding class that honors
   `current_board` obstacles.
2. Build unit tests/simulations to verify the algorithm doesn't get trapped by
   the custom numeral layouts.
3. Integrate testing buttons into ESPHome that force a time transition to test
   the pickup/carry/deposit payload logic.

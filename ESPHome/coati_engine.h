#pragma once

#include "esphome.h"
#include <queue>
#include <vector>
#include <cmath>
#include <algorithm>
#include <cstdlib> // for rand()

namespace esphome {

struct Point {
  int x, y;
  bool operator==(const Point& o) const { return x == o.x && y == o.y; }
  bool operator!=(const Point& o) const { return x != o.x || y != o.y; }
};

struct CompareNode {
    bool operator()(const std::pair<float, Point>& a, const std::pair<float, Point>& b) {
        return a.first > b.first;
    }
};

class DummyDisplay : public display::DisplayBuffer {
public:
  bool buffer[32][8] = {false};
  
  void clear_buffer() {
    for (int x = 0; x < 32; x++)
      for (int y = 0; y < 8; y++)
        buffer[x][y] = false;
  }
  
  void draw_absolute_pixel_internal(int x, int y, Color color) override {
    if (x >= 0 && x < 32 && y >= 0 && y < 8) {
      if (color.w > 0 || color.r > 0 || color.g > 0 || color.b > 0) {
        buffer[x][y] = true;
      }
    }
  }
  
  display::DisplayType get_display_type() override { return display::DisplayType::DISPLAY_TYPE_COLOR; }
  void update() override {}
protected:
  int get_width_internal() override { return 32; }
  int get_height_internal() override { return 8; }
};

struct CoatiAgent {
  Point pos;
  Point last_pos;
  bool carrying = false;
  std::vector<Point> current_path;
  Point claimed_target = {-1, -1};
  int wait_ticks = 0;
  int bored_ticks = 0;
  int wash_ticks = 0;  // > 0: currently dunking at pool (counts down each tick)
  bool washed = false; // true: this carry has been washed, ready to place
  int pause_ticks = 0; // > 0: frozen after pickup/placement (counts down each tick)
  int stuck_ticks = 0; // watchdog: increments when agent makes no progress

  CoatiAgent(Point p, Point lp, bool c, std::vector<Point> path, Point target, int w, int b)
      : pos(p), last_pos(lp), carrying(c) , current_path(path), claimed_target(target), wait_ticks(w), bored_ticks(b) {}
  CoatiAgent() = default;
};

// ---- Wobbly Time ----
// Tracks a virtual "display" time that stays ahead of real time by a random
// amount in [wobble_min_seconds, wobble_max_seconds].
// It ticks faster when behind its target offset, slower when ahead.
// Parameters are injected from YAML globals.
struct WobblyTime {
  int wobble_min_seconds;   // Min advance in seconds (e.g. 120)
  int wobble_max_seconds;   // Max advance in seconds (e.g. 300)
  float fast_rate;          // Rate multiplier when catching up (e.g. 1.3)
  float slow_rate;          // Rate multiplier when ahead (e.g. 0.7)

  double target_offset_secs; // Current target advance in seconds
  double virtual_epoch_secs; // Virtual display time as unix epoch
  long last_wall_ms;         // millis() at last update
  bool initialized;

  WobblyTime(int mn, int mx, float fast, float slow)
    : wobble_min_seconds(mn), wobble_max_seconds(mx),
      fast_rate(fast), slow_rate(slow),
      target_offset_secs(0), virtual_epoch_secs(0),
      last_wall_ms(0), initialized(false) {}

  void seed(ESPTime real_now) {
    // Pick initial random target in [min, max] seconds ahead
    int range_secs = wobble_max_seconds - wobble_min_seconds;
    target_offset_secs = wobble_min_seconds + (rand() % (range_secs + 1));
    virtual_epoch_secs = real_now.timestamp + target_offset_secs;
    last_wall_ms = millis();
    initialized = true;
  }

  // Call every second (from interval: block). Returns updated virtual time.
  ESPTime advance(ESPTime real_now) {
    if (!initialized) {
      seed(real_now);
      return make_time(virtual_epoch_secs);
    }

    long now_ms = millis();
    double wall_elapsed = (now_ms - last_wall_ms) / 1000.0;
    last_wall_ms = now_ms;

    // Current offset of virtual time vs real time
    double current_offset = virtual_epoch_secs - real_now.timestamp;

    // Pick tick rate based on whether we're ahead or behind target
    float rate = (current_offset < target_offset_secs) ? fast_rate : slow_rate;
    virtual_epoch_secs += wall_elapsed * rate;

    // If we've reached the target offset, pick a new target
    if (std::abs(current_offset - target_offset_secs) < 5.0) {
      int range_secs = wobble_max_seconds - wobble_min_seconds;
      target_offset_secs = wobble_min_seconds + (rand() % (range_secs + 1));
    }

    return make_time(virtual_epoch_secs);
  }

  ESPTime make_time(double epoch) {
    time_t t = (time_t)epoch;
    return ESPTime::from_epoch_local(t);
  }
};

class CoatiEngine {
public:
  DummyDisplay* dummy;
  bool current_board[32][8] = {false};
  bool target_board[32][8] = {false};
  float fade_board[32][8];
  
  // Double-buffer for target: timer context writes to pending_target,
  // render context atomically swaps via apply_pending_target().
  bool pending_target[32][8] = {false};
  volatile bool target_pending = false;
  
  // Pre-allocated pathfinding scratch space.
  // Keeping these on the HEAP (as member vars) instead of the stack prevents
  // ~3.3KB peak stack usage per find_path() call, which was overflowing the
  // ESP32-C3's main task stack and causing random current_board corruption.
  bool pf_visited[32][8];
  Point pf_parent[32][8];
  float pf_cost[32][8];
  // Pre-reserved priority_queue backing store. After first expansion it never
  // reallocates, eliminating heap churn on every pathfinding call.
  std::vector<std::pair<float, Point>> pf_q_storage;
  
  std::vector<CoatiAgent> agents;
  int pool_user = -1;  // index of agent currently using the pool (-1 = free)

  // Pre-allocated work vectors for tick() — promoted from locals to members
  // so they are never heap-allocated/freed on the hot 20 Hz path.
  // Always call .clear() before use; capacity is never shrunk.
  std::vector<Point> tk_extras, tk_missing;
  std::vector<Point> tk_avail_e, tk_avail_m;
  std::vector<Point> tk_wander;
  
  std::vector<Point> dumpster = {{0, 7}, {1, 7}};
  std::vector<Point> pool = {{30, 7}, {31, 7}};
  
  CoatiEngine() {
    dummy = new DummyDisplay();
    agents.push_back(CoatiAgent{{8, 4}, {8, 4}, false, {}, {-1, -1}, 0, 0});
    agents.push_back(CoatiAgent{{24, 4}, {24, 4}, false, {}, {-1, -1}, 0, 0});
    
    for(int x=0; x<32; x++) {
        for(int y=0; y<8; y++) {
            // Start at 0: dark positions shouldn't carry stale 1.0 values.
            // Placed pixels will fade in from 0 naturally.
            fade_board[x][y] = 0.0f;
        }
    }
    // Reserve queue backing once so pathfinding never reallocates on the heap.
    // 40 nodes * 8 neighbors = 320 max pushes; 448 gives comfortable headroom.
    pf_q_storage.reserve(448);
    // Pre-reserve tick() work vectors to prevent heap fragmentation from the
    // repeated alloc/free cycle that occurs at 20 Hz over many minutes.
    tk_extras.reserve(64);  tk_missing.reserve(64);
    tk_avail_e.reserve(64); tk_avail_m.reserve(64);
    tk_wander.reserve(16);
  }

  // Called from the FreeRTOS timer/interval context.
  // Renders the font into a staging buffer WITHOUT touching any live state.
  void prepare_target(ESPTime t, display::BaseFont *clock_font) {
    if (!t.is_valid()) return;
    dummy->clear_buffer();
    Color c = Color(0, 255, 0);
    dummy->strftime(3,  -1, clock_font, c, "%H", t);
    dummy->strftime(17, -1, clock_font, c, "%M", t);
    for (int x = 0; x < 32; x++)
      for (int y = 0; y < 8; y++)
        pending_target[x][y] = dummy->buffer[x][y];
    target_pending = true; // Signal that a new target is ready
  }

  // Called from the render lambda (main task context) BEFORE tick().
  // Atomically swaps the staged target into the live target_board.
  void apply_pending_target() {
    if (!target_pending) return;
    target_pending = false;
    int added = 0, removed = 0;
    for (int x = 0; x < 32; x++)
      for (int y = 0; y < 8; y++) {
        if (!target_board[x][y] && pending_target[x][y]) added++;
        if (target_board[x][y] && !pending_target[x][y]) removed++;
        target_board[x][y] = pending_target[x][y];
      }
    // Reset agents fully — clear frustration state so shimmer doesn't misfire
    pool_user = -1;
    for (auto &agent : agents) {
      agent.claimed_target = {-1, -1};
      agent.current_path.clear();
      agent.wait_ticks = 0;
      agent.bored_ticks = 0;
      agent.wash_ticks = 0;
      agent.washed = false;
      agent.pause_ticks = 0;
    }
    ESP_LOGW("coati", "TARGET APPLIED: +%d -%d pixels, agents reset", added, removed);
  }

  float dist(Point a, Point b) {
    return std::sqrt((a.x - b.x)*(a.x - b.x) + (a.y - b.y)*(a.y - b.y));
  }

  // Time-budgeted A*: expands at most max_nodes nodes then returns the best
  // partial path found so far. If dest was reached within budget, returns the
  // full optimal path. Otherwise, returns the path to the settled node that is
  // geometrically closest to dest ("best downhill frontier").
  //
  // Benefit: on long journeys the agent starts walking immediately rather than
  // blocking a full-grid search. Each re-plan tick starts from the new (closer)
  // position, so subsequent budgets complete faster. The meandering produced by
  // mid-journey re-plans gives an organic, animal-like movement quality.
  std::vector<Point> find_path(Point start, Point dest, int self_index, int max_nodes = 40) {
    // Scratch arrays are pre-allocated member vars (not stack) to avoid
    // the ~3.3KB stack spike that was corrupting adjacent heap memory.
    memset(pf_visited, 0, sizeof(pf_visited));
    for (int x = 0; x < 32; x++)
      for (int y = 0; y < 8; y++)
        pf_cost[x][y] = 1e9f;

    // Clear the pre-reserved backing store without freeing its capacity.
    pf_q_storage.clear();
    std::priority_queue<
      std::pair<float, Point>,
      std::vector<std::pair<float, Point>>,
      CompareNode> q(CompareNode{}, pf_q_storage);

    pf_cost[start.x][start.y] = 0;
    q.push({0, start});

    int nodes_expanded = 0;
    Point best_frontier = {-1, -1};
    float best_h = 1e9f;

    while (!q.empty() && nodes_expanded < max_nodes) {
      Point curr = q.top().second;
      q.pop();

      if (curr == dest) { best_frontier = dest; break; }
      if (pf_visited[curr.x][curr.y]) continue;
      pf_visited[curr.x][curr.y] = true;
      nodes_expanded++;

      // Track the settled node geometrically closest to dest as partial fallback.
      float h = dist(curr, dest);
      if (h < best_h) { best_h = h; best_frontier = curr; }

      for (int dx = -1; dx <= 1; dx++) {
        for (int dy = -1; dy <= 1; dy++) {
          if (dx == 0 && dy == 0) continue;
          int nx = curr.x + dx, ny = curr.y + dy;
          if (nx < 0 || nx >= 32 || ny < 0 || ny >= 8) continue;

          // Lit pixels are passable but expensive — cost 50 ensures unlit
          // paths are always preferred, while guaranteeing an escape route
          // exists when an agent is surrounded by placed pixels.
          float pixel_cost = (current_board[nx][ny] &&
                              !(nx == dest.x && ny == dest.y)) ? 50.0f : 0.0f;

          float penalty = 0.0f;
          for (size_t j = 0; j < agents.size(); j++) {
            if ((int)j != self_index &&
                agents[j].pos.x == nx && agents[j].pos.y == ny &&
                !(nx == dest.x && ny == dest.y))
              penalty = 20.0f;
          }

          float step = (dx == 0 || dy == 0) ? 1.0f : 1.414f;
          float nc = pf_cost[curr.x][curr.y] + step + penalty + pixel_cost;
          if (nc < pf_cost[nx][ny]) {
            pf_cost[nx][ny] = nc;
            pf_parent[nx][ny] = curr;
            q.push({nc + dist({nx, ny}, dest), {nx, ny}});
          }
        }
      }
    }

    // Use full dest if reached, else best partial frontier.
    Point target = (pf_cost[dest.x][dest.y] < 1e8f) ? dest : best_frontier;
    std::vector<Point> path;
    if (target.x == -1 || target == start) return path;

    Point cur = target;
    while (cur != start) {
      path.push_back(cur);
      cur = pf_parent[cur.x][cur.y];
    }
    std::reverse(path.begin(), path.end());
    return path;
  }
  
  Point find_closest(Point start, const std::vector<Point>& candidates) {
    Point best = {-1, -1};
    float min_d = 1e9;
    for (auto c : candidates) {
      float d = dist(start, c);
      if (d < min_d) {
        min_d = d;
        best = c;
      }
    }
    return best;
  }

  void tick() {
    // Periodic status summary — one line every ~30 s (600 ticks at 20 Hz).
    // Tells you exactly where each agent is and what it is doing without
    // producing noise during normal operation.
    static int tick_count = 0;
    if (++tick_count >= 600) {
        tick_count = 0;
        int extras = 0, missing = 0;
        for (int x = 0; x < 32; x++) for (int y = 0; y < 8; y++) {
            if (current_board[x][y] && !target_board[x][y]) extras++;
            if (!current_board[x][y] && target_board[x][y]) missing++;
        }
        for (size_t i = 0; i < agents.size(); i++) {
            auto& a = agents[i];
            const char* phase = "idle";
            if (a.carrying && !a.washed)  phase = "washing";
            else if (a.carrying)           phase = "placing";
            else if (!a.current_path.empty()) phase = "moving";
            else if (a.pause_ticks > 0)    phase = "paused";
            ESP_LOGI("coati", "[%d] (%d,%d) %s carry=%d washed=%d path=%d wait=%d stuck=%d | ext=%d mis=%d pool=%d",
                (int)i, a.pos.x, a.pos.y, phase,
                (int)a.carrying, (int)a.washed,
                (int)a.current_path.size(), a.wait_ticks, a.stuck_ticks,
                extras, missing, pool_user);
        }
    }

    tk_extras.clear();
    tk_missing.clear();
    
    // Recalculate available tasks
    for (int x = 0; x < 32; x++) {
      for (int y = 0; y < 8; y++) {
        bool is_dumpster = (y == 7 && (x == 0 || x == 1));
        bool is_pool = (y == 7 && (x == 30 || x == 31));
        if (is_dumpster || is_pool) continue;

        if (current_board[x][y] && !target_board[x][y]) {
          tk_extras.push_back({x, y});
        }
        if (!current_board[x][y] && target_board[x][y]) {
          tk_missing.push_back({x, y});
        }
      }
    }

    // Pool-user watchdog: if the pool owner is no longer at the pool and has
    // no wash ticks active, the lock is permanently stale — release it.
    if (pool_user != -1) {
        CoatiAgent& owner = agents[pool_user];
        bool at_pool = false;
        for (auto& p : pool) if (owner.pos == p) at_pool = true;
        if (!at_pool && owner.wash_ticks == 0) {
            ESP_LOGW("coati", "POOL WATCHDOG: releasing stale pool_user=%d", pool_user);
            pool_user = -1;
        }
    }

    // Process agents sequentially
    for (size_t i = 0; i < agents.size(); i++) {
        CoatiAgent& a = agents[i];
        
        // Sync historical position for temporal motion blurring interpolation
        a.last_pos = a.pos;

        if (!a.current_path.empty()) {
            Point next_step = a.current_path.front();
            
            // Check for real-time collision with another agent
            bool collision = false;
            int blocker = -1;
            for (size_t j = 0; j < agents.size(); j++) {
                if (i != j && agents[j].pos == next_step) { collision = true; blocker = j; }
            }

            // Head-on swap: if both agents want each other's cell, let them
            // pass through each other atomically instead of deadlocking.
            if (collision && blocker != -1 && !agents[blocker].current_path.empty() &&
                agents[blocker].current_path.front() == a.pos) {
                agents[blocker].pos = a.pos;
                agents[blocker].current_path.erase(agents[blocker].current_path.begin());
                agents[blocker].wait_ticks = 0;
                agents[blocker].stuck_ticks = 0;
                a.pos = next_step;
                a.current_path.erase(a.current_path.begin());
                a.wait_ticks = 0;
                a.stuck_ticks = 0;
                ESP_LOGI("coati", "SWAP [%d]<->[%d]", (int)i, blocker);
                continue;
            }

            if (collision) {
                a.wait_ticks++;
                // Asymmetrical timeout based on agent Index ensures they don't break deadlock symmetrically
                if (a.wait_ticks > (3 + i * 2)) {
                    // Deadlock! Drop the path and force a random retreat step!
                    a.current_path.clear();
                    
                    std::vector<Point> valid_escape;
                    for (int dx = -1; dx <= 1; dx++) {
                        for (int dy = -1; dy <= 1; dy++) {
                            if (dx == 0 && dy == 0) continue;
                            int nx = a.pos.x + dx;
                            int ny = a.pos.y + dy;
                            if (nx >= 0 && nx < 32 && ny >= 0 && ny < 8) {
                                if (!current_board[nx][ny]) {
                                    bool clash = false;
                                    for (size_t j = 0; j < agents.size(); j++) {
                                        if (i != j && agents[j].pos.x == nx && agents[j].pos.y == ny) clash = true;
                                    }
                                    if (!clash) valid_escape.push_back({nx, ny});
                                }
                            }
                        }
                    }
                    // Fallback: if every clear neighbour is agent-blocked, allow
                    // stepping onto a lit pixel to break the deadlock.
                    if (valid_escape.empty()) {
                        for (int dx = -1; dx <= 1; dx++) {
                            for (int dy = -1; dy <= 1; dy++) {
                                if (dx == 0 && dy == 0) continue;
                                int nx = a.pos.x + dx, ny = a.pos.y + dy;
                                if (nx >= 0 && nx < 32 && ny >= 0 && ny < 8) {
                                    bool clash = false;
                                    for (size_t j = 0; j < agents.size(); j++)
                                        if (i != j && agents[j].pos.x == nx && agents[j].pos.y == ny) clash = true;
                                    if (!clash) valid_escape.push_back({nx, ny});
                                }
                            }
                        }
                    }
                    if (!valid_escape.empty()) {
                        int r = std::rand() % valid_escape.size();
                        Point esc = valid_escape[r];
                        a.current_path.push_back(esc);
                        ESP_LOGW("coati", "DEADLOCK RETREAT [%d] (%d,%d)->(%d,%d) carry=%d",
                            (int)i, a.pos.x, a.pos.y, esc.x, esc.y, (int)a.carrying);
                    } else {
                        ESP_LOGW("coati", "DEADLOCK RETREAT [%d] (%d,%d) CORNERED, no escape",
                            (int)i, a.pos.x, a.pos.y);
                    }
                    
                    a.claimed_target = {-1, -1};
                    a.wait_ticks = 0;
                }
                continue; // Skip moving this frame
            }
            
            // Move!
            a.pos = next_step;
            a.current_path.erase(a.current_path.begin());
            a.wait_ticks = 0;
            a.stuck_ticks = 0;  // made progress
            continue; // We made our physical step, allow next agent to move
        }
    }

    bool path_calculated_this_tick = false;

    // Phase 2 of Tick: For agents that didn't move because their path was empty, make decisions.
    for (size_t i = 0; i < agents.size(); i++) {
        CoatiAgent& a = agents[i];
        if (!a.current_path.empty() || a.wait_ticks > 0) continue; // Already moving or blocked
        if (a.pause_ticks > 0) { a.pause_ticks--; continue; }      // Dainty pause after pickup/place

        // Re-filter available arrays for this specific agent's decision processing
        tk_avail_e.clear();
        for (auto e : tk_extras) {
            bool claimed = false;
            for (size_t j = 0; j < agents.size(); j++) {
                if (i != j && agents[j].claimed_target == e) claimed = true;
            }
            if (!claimed) tk_avail_e.push_back(e);
        }
        tk_avail_m.clear();
        for (auto m : tk_missing) {
            bool claimed = false;
            for (size_t j = 0; j < agents.size(); j++) {
                if (i != j && agents[j].claimed_target == m) claimed = true;
            }
            if (!claimed) tk_avail_m.push_back(m);
        }

        if (a.carrying) {
            // =================================================================
            // PHASE A — WASHING: visit pool and dunk before placing
            // =================================================================
            if (!a.washed) {

                // --- Actively dunking: bob between pool level and y-1 ---
                if (a.wash_ticks > 0) {
                    a.wash_ticks--;
                    if (a.wash_ticks == 0) {
                        // Dunking complete — release pool and move to placement phase
                        a.washed = true;
                        if (pool_user == (int)i) pool_user = -1;
                        a.claimed_target = {-1, -1};
                        ESP_LOGI("coati", "WASHED [%d] done at (%d,%d)", (int)i, a.pos.x, a.pos.y);
                    } else {
                        // Queue next bob step: emerge (y-1) or dunk (pool.y)
                        Point spot = pool[i % pool.size()];
                        if (a.pos.y == spot.y)
                            a.current_path = {{spot.x, spot.y - 1}};  // emerge
                        else
                            a.current_path = {spot};                   // dunk
                    }
                    continue;
                }

                // --- At pool: claim and start dunking ---
                bool at_my_pool = (a.pos == pool[i % pool.size()]);
                if (at_my_pool) {
                    if (pool_user == -1 || pool_user == (int)i) {
                        pool_user = (int)i;
                        a.wash_ticks = 5;  // 2.5 bobs (5 half-steps)
                        Point spot = pool[i % pool.size()];
                        a.current_path = {{spot.x, spot.y - 1}};  // first emerge
                        ESP_LOGI("coati", "WASHING [%d] starting at (%d,%d)", (int)i, a.pos.x, a.pos.y);
                    }
                    // else pool busy — wait in place this tick
                    continue;
                }

                // --- Not at pool yet ---
                if (path_calculated_this_tick) continue;

                if (pool_user == -1 || pool_user == (int)i) {
                    // Pool is free (or ours): head there — continue to skip escape logic
                    Point pool_dest = pool[i % pool.size()];
                    a.current_path = find_path(a.pos, pool_dest, i);
                    a.claimed_target = pool_dest;
                    path_calculated_this_tick = true;
                    continue;  // has a path, no need for escape logic
                } else {
                    // Pool is busy: wander nearby rather than crowding the approach.
                    // Clear stale claimed_target so the panic escape check below can fire.
                    a.claimed_target = {-1, -1};
                    a.bored_ticks++;
                    if (a.bored_ticks >= 8) {
                        a.bored_ticks = 0;
                        if (std::rand() % 2 == 0) {
                            tk_wander.clear();
                            for (int dx = -1; dx <= 1; dx++) for (int dy = -1; dy <= 1; dy++) {
                                if (dx == 0 && dy == 0) continue;
                                int nx = a.pos.x + dx, ny = a.pos.y + dy;
                                bool is_pool_cell = (ny == 7 && (nx == 30 || nx == 31));
                                if (nx >= 0 && nx < 32 && ny >= 0 && ny < 8 &&
                                    !current_board[nx][ny] && !is_pool_cell) {
                                    bool clash = false;
                                    for (size_t j = 0; j < agents.size(); j++)
                                        if (i != j && agents[j].pos.x == nx && agents[j].pos.y == ny) clash = true;
                                    if (!clash) tk_wander.push_back({nx, ny});
                                }
                            }
                            if (!tk_wander.empty())
                                a.current_path.push_back(tk_wander[std::rand() % tk_wander.size()]);
                        }
                    }
                }
                // Fall through: panic escape / deadlock-break logic runs below
            }

            // =================================================================
            // PHASE B — PLACING: washed pixel, find a missing slot or dispose
            // =================================================================
            bool standing_on_missing = false;
            for (auto m : tk_missing) if (a.pos == m) standing_on_missing = true;
            bool standing_on_pool = false;
            for (auto p : pool) if (a.pos == p) standing_on_pool = true;

            bool at_claimed_target = (a.claimed_target.x != -1 && a.pos == a.claimed_target);
            if (standing_on_missing && !current_board[a.pos.x][a.pos.y] && at_claimed_target) {
                if (fade_board[a.pos.x][a.pos.y] > 0.05f)
                    ESP_LOGW("coati", "STALE FADE at (%d,%d): %.2f before placement", a.pos.x, a.pos.y, fade_board[a.pos.x][a.pos.y]);
                current_board[a.pos.x][a.pos.y] = true;
                fade_board[a.pos.x][a.pos.y] = 0.0f;
                a.carrying = false;
                a.washed = false;
                a.claimed_target = {-1, -1};
                a.pause_ticks = 4;  // Pause to "place" daintily
                ESP_LOGI("coati", "PLACE [%d] pixel at (%d,%d)", (int)i, a.pos.x, a.pos.y);
                continue;
            } else if (standing_on_pool) {
                // No reachable missing slots — dispose
                bool has_reachable = !tk_avail_m.empty() &&
                                     find_closest(a.pos, tk_avail_m).x != -1;
                if (!has_reachable) {
                    a.carrying = false;
                    a.washed = false;
                    a.claimed_target = {-1, -1};
                    ESP_LOGI("coati", "DISPOSE [%d] at pool (%d,%d)", (int)i, a.pos.x, a.pos.y);
                    continue;
                }
            }

            if (path_calculated_this_tick) continue;

            if (!tk_avail_m.empty()) {
                Point dest = find_closest(a.pos, tk_avail_m);
                if (dest.x != -1) {
                    a.current_path = find_path(a.pos, dest, i);
                    if (!a.current_path.empty()) {
                        a.claimed_target = dest;
                    } else {
                        for (auto m : tk_avail_m) {
                            a.current_path = find_path(a.pos, m, i);
                            if (!a.current_path.empty()) { a.claimed_target = m; break; }
                        }
                    }
                    path_calculated_this_tick = true;
                }
            } else {
                // No missing slots after washing — head to pool to dispose
                Point pool_dest = pool[i % pool.size()];
                a.current_path = find_path(a.pos, pool_dest, i);
                a.claimed_target = pool_dest;
                path_calculated_this_tick = true;
            }
        } else {
            // Not carrying
            bool standing_on_extra = false;
            for (auto e : tk_extras) if (a.pos == e) standing_on_extra = true;
            bool standing_on_dumpster = false;
            for (auto d : dumpster) if (a.pos == d) standing_on_dumpster = true;

            if (standing_on_extra && current_board[a.pos.x][a.pos.y]) {
                current_board[a.pos.x][a.pos.y] = false;
                fade_board[a.pos.x][a.pos.y] = 0.0f;
                a.carrying = true;
                a.claimed_target = {-1, -1};
                a.pause_ticks = 4;  // Pause to "pick up" daintily
                ESP_LOGI("coati", "PICKUP [%d] pixel at (%d,%d)", (int)i, a.pos.x, a.pos.y);
                continue;
            } else if (standing_on_dumpster && !tk_avail_m.empty()) {
                a.carrying = true;
                a.claimed_target = {-1, -1};
                a.pause_ticks = 4;  // Pause to "fetch" daintily
                ESP_LOGI("coati", "FETCH [%d] from dumpster at (%d,%d)", (int)i, a.pos.x, a.pos.y);
                continue;
            }
            
            if (path_calculated_this_tick) continue; // STAGGER PATHFINDING

            if (!tk_avail_e.empty()) {
                Point dest = find_closest(a.pos, tk_avail_e);
                a.current_path = find_path(a.pos, dest, i);
                if (!a.current_path.empty()) {
                    a.claimed_target = dest;
                } else {
                    for(auto e : tk_avail_e) {
                        a.current_path = find_path(a.pos, e, i);
                        if (!a.current_path.empty()) {
                            a.claimed_target = e; break;
                        }
                    }
                }
                path_calculated_this_tick = true;
            } else if (!tk_avail_m.empty()) {
                // Spread agents across separate dumpster cells to avoid exit deadlock
                Point fetch_dest = dumpster[i % dumpster.size()];
                a.current_path = find_path(a.pos, fetch_dest, i);
                a.claimed_target = fetch_dest;
                path_calculated_this_tick = true;
            } else {
                // Bored!
                a.claimed_target = {-1, -1};
                a.bored_ticks++;
            }
        }

        // --- Panic Escape Logic ---
        if (a.current_path.empty() && a.claimed_target.x == -1 &&
           (a.carrying || !tk_avail_m.empty() || !tk_avail_e.empty()))
        {
            a.wait_ticks++;
            // Wait briefly to see if the blocking agent moves out of our way naturally
            if (a.wait_ticks > 5) {
                a.wait_ticks = 0;
                std::vector<Point> valid_escape;
                for (int dx = -1; dx <= 1; dx++) {
                    for (int dy = -1; dy <= 1; dy++) {
                        if (dx == 0 && dy == 0) continue;
                        int nx = a.pos.x + dx;
                        int ny = a.pos.y + dy;
                        if (nx >= 0 && nx < 32 && ny >= 0 && ny < 8) {
                            if (!current_board[nx][ny]) {
                                bool collision = false;
                                for (size_t j = 0; j < agents.size(); j++) {
                                    if (i != j && agents[j].pos.x == nx && agents[j].pos.y == ny) collision = true;
                                }
                                if (!collision) valid_escape.push_back({nx, ny});
                            }
                        }
                    }
                }
                // Fallback: allow stepping onto a lit pixel if entirely cornered.
                if (valid_escape.empty()) {
                    for (int dx = -1; dx <= 1; dx++) {
                        for (int dy = -1; dy <= 1; dy++) {
                            if (dx == 0 && dy == 0) continue;
                            int nx = a.pos.x + dx, ny = a.pos.y + dy;
                            if (nx >= 0 && nx < 32 && ny >= 0 && ny < 8) {
                                bool collision = false;
                                for (size_t j = 0; j < agents.size(); j++)
                                    if (i != j && agents[j].pos.x == nx && agents[j].pos.y == ny) collision = true;
                                if (!collision) valid_escape.push_back({nx, ny});
                            }
                        }
                    }
                }
                if (!valid_escape.empty()) {
                    int r = std::rand() % valid_escape.size();
                    Point esc = valid_escape[r];
                    a.current_path.push_back(esc);
                    ESP_LOGW("coati", "PANIC ESCAPE [%d] (%d,%d)->(%d,%d) carry=%d wait was %d",
                        (int)i, a.pos.x, a.pos.y, esc.x, esc.y, (int)a.carrying, a.wait_ticks);
                } else {
                    ESP_LOGW("coati", "PANIC ESCAPE [%d] (%d,%d) CORNERED, no escape carry=%d",
                        (int)i, a.pos.x, a.pos.y, (int)a.carrying);
                }
            }
        }

        // --- Stuck Watchdog ---
        // If an agent reaches the end of Phase 2 without having taken a step
        // or queued a path this tick, increment stuck_ticks. After ~10 seconds
        // (200 ticks at 20 Hz) of this, perform a full state reset on the agent
        // as a last resort to break any unforeseen deadlock.
        if (a.current_path.empty() && a.pos == a.last_pos) {
            a.stuck_ticks++;
            // Early warning at ~2.5 s — lets you see something is wrong well
            // before the 10 s hard reset fires.
            if (a.stuck_ticks == 50) {
                const char* phase = "idle";
                if (a.carrying && !a.washed)  phase = "washing";
                else if (a.carrying)           phase = "placing";
                ESP_LOGW("coati", "STUCK EARLY [%d] (%d,%d) %s carry=%d washed=%d claimed=(%d,%d) pool=%d",
                    (int)i, a.pos.x, a.pos.y, phase,
                    (int)a.carrying, (int)a.washed,
                    a.claimed_target.x, a.claimed_target.y, pool_user);
            }
            if (a.stuck_ticks > 200) {
                ESP_LOGW("coati", "STUCK WATCHDOG [%d]: hard reset after %d ticks", (int)i, a.stuck_ticks);
                a.stuck_ticks = 0;
                a.claimed_target = {-1, -1};
                a.wait_ticks = 0;
                a.bored_ticks = 0;
                a.wash_ticks = 0;
                a.washed = false;
                a.pause_ticks = 0;
                a.carrying = false;
                if (pool_user == (int)i) pool_user = -1;
            }
        } else {
            a.stuck_ticks = 0;
        }

        // --- "Floor is Lava" Escape ---
        // When an idle non-carrying agent sits on a target_board cell, it risks
        // being physically surrounded as surrounding clock pixels are placed. The
        // agent cannot place a pixel itself (needs to carry one) and cannot pass
        // through placed pixels, so it will become permanently trapped.
        // Escape immediately to the nearest cell that is neither placed nor targeted.
        if (a.current_path.empty() && a.claimed_target.x == -1 && !a.carrying &&
            target_board[a.pos.x][a.pos.y] && !path_calculated_this_tick) {
            tk_wander.clear();
            for (int x = 0; x < 32; x++)
                for (int y = 0; y < 8; y++)
                    if (!target_board[x][y] && !current_board[x][y])
                        tk_wander.push_back({x, y});
            if (!tk_wander.empty()) {
                Point closest = find_closest(a.pos, tk_wander);
                a.current_path = find_path(a.pos, closest, i);
                path_calculated_this_tick = true;
                ESP_LOGI("coati", "LAVA ESCAPE [%d] (%d,%d)->(%d,%d)",
                    (int)i, a.pos.x, a.pos.y, closest.x, closest.y);
            } else {
                ESP_LOGW("coati", "LAVA ESCAPE [%d] (%d,%d) TRAPPED, no safe cell", (int)i, a.pos.x, a.pos.y);
            }
            continue;
        }

        // --- Bored Wandering Logic ---
        // One step, ~50% chance per trigger interval. Motion blur is handled by
        // Phase 1's last_pos update, making the single step visually smooth.
        if (a.current_path.empty() && a.claimed_target.x == -1) {
            if (a.bored_ticks >= 20) {
                a.bored_ticks = 0;
                if (std::rand() % 2 == 0) {
                    tk_wander.clear();
                    for (int dx = -1; dx <= 1; dx++) {
                        for (int dy = -1; dy <= 1; dy++) {
                            if (dx == 0 && dy == 0) continue;
                            int nx = a.pos.x + dx;
                            int ny = a.pos.y + dy;
                            if (nx >= 0 && nx < 32 && ny >= 0 && ny < 8) {
                                if (!current_board[nx][ny] && !target_board[nx][ny]) {
                                    bool collision = false;
                                    for (size_t j = 0; j < agents.size(); j++) {
                                        if (i != j && agents[j].pos.x == nx && agents[j].pos.y == ny)
                                            collision = true;
                                    }
                                    if (!collision) tk_wander.push_back({nx, ny});
                                }
                            }
                        }
                    }
                    if (!tk_wander.empty()) {
                        a.current_path.push_back(tk_wander[std::rand() % tk_wander.size()]);
                    }
                }
            }
        }

    } // end agent loop
  }
};

} // namespace esphome

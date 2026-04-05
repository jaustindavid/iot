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
};

class CoatiEngine {
public:
  DummyDisplay* dummy;
  bool current_board[32][8] = {false};
  bool target_board[32][8] = {false};
  float fade_board[32][8];
  
  std::vector<CoatiAgent> agents;
  
  std::vector<Point> dumpster = {{0, 7}, {1, 7}};
  std::vector<Point> pool = {{30, 7}, {31, 7}};
  
  CoatiEngine() {
    dummy = new DummyDisplay();
    // Initialize two coatis at somewhat random distinct locations
    agents.push_back({{8, 4}, {8, 4}, false, {}, {-1, -1}, 0, 0});
    agents.push_back({{24, 4}, {24, 4}, false, {}, {-1, -1}, 0, 0});
    
    for(int x=0; x<32; x++) {
        for(int y=0; y<8; y++) {
            fade_board[x][y] = 1.0f;
        }
    }
  }

  void update_target(ESPTime current_time, display::BaseFont *clock_font) {
    if (!current_time.is_valid()) return;
    dummy->clear_buffer();
    
    Color c = Color(0, 255, 0);
    dummy->strftime(4, -1, clock_font, c, "%H%M", current_time);
    
    for (int x = 0; x < 32; x++) {
      for (int y = 0; y < 8; y++) {
        target_board[x][y] = dummy->buffer[x][y];
      }
    }
    
    // Changing target breaks current claims if they are invalidated,
    // so we can just wipe all active claims and let them recalculate!
    for (auto &agent : agents) {
        agent.claimed_target = {-1, -1};
        agent.current_path.clear();
    }
  }

  float dist(Point a, Point b) {
    return std::sqrt((a.x - b.x)*(a.x - b.x) + (a.y - b.y)*(a.y - b.y));
  }

  std::vector<Point> find_path(Point start, Point dest, int self_index) {
    bool visited[32][8] = {false};
    Point parent[32][8];
    float cost[32][8];
    for (int x=0; x<32; x++) for (int y=0; y<8; y++) cost[x][y] = 1e9;
    
    std::priority_queue<std::pair<float, Point>, std::vector<std::pair<float, Point>>, CompareNode> q;
    
    cost[start.x][start.y] = 0;
    q.push({0, start});
    
    while (!q.empty()) {
      Point curr = q.top().second;
      q.pop();
      
      if (curr == dest) break;
      if (visited[curr.x][curr.y]) continue;
      visited[curr.x][curr.y] = true;
      
      for (int dx = -1; dx <= 1; dx++) {
        for (int dy = -1; dy <= 1; dy++) {
          if (dx == 0 && dy == 0) continue;
          int nx = curr.x + dx;
          int ny = curr.y + dy;
          if (nx >= 0 && nx < 32 && ny >= 0 && ny < 8) {
            bool is_obstacle = current_board[nx][ny];
            if (nx == dest.x && ny == dest.y) is_obstacle = false;
            
            float agent_penalty = 0.0f;
            // Introduce a soft penalty rather than a hard block for other agents,
            // so if an agent is trapped in an alley, it will still find a valid path
            // instead of permanently failing and entering the panic state.
            for (size_t j = 0; j < agents.size(); j++) {
                if (j != self_index && agents[j].pos.x == nx && agents[j].pos.y == ny) {
                    if (nx != dest.x || ny != dest.y) {
                        agent_penalty = 20.0f; // Strongly discourage, but allow pathing
                    }
                }
            }

            if (!is_obstacle) {
              float step_cost = (dx == 0 || dy == 0) ? 1.0f : 1.414f;
              if (cost[curr.x][curr.y] + step_cost + agent_penalty < cost[nx][ny]) {
                cost[nx][ny] = cost[curr.x][curr.y] + step_cost + agent_penalty;
                parent[nx][ny] = curr;
                float priority = cost[nx][ny] + dist({nx, ny}, dest);
                q.push({priority, {nx, ny}});
              }
            }
          }
        }
      }
    }
    
    std::vector<Point> path;
    if (cost[dest.x][dest.y] >= 1e8) return path; 
    
    Point curr = dest;
    while (curr != start) {
      path.push_back(curr);
      curr = parent[curr.x][curr.y];
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
    std::vector<Point> extras;
    std::vector<Point> missing;
    
    // Recalculate available tasks
    for (int x = 0; x < 32; x++) {
      for (int y = 0; y < 8; y++) {
        bool is_dumpster = (y == 7 && (x == 0 || x == 1));
        bool is_pool = (y == 7 && (x == 30 || x == 31));
        if (is_dumpster || is_pool) continue;

        if (current_board[x][y] && !target_board[x][y]) {
          extras.push_back({x, y});
        }
        if (!current_board[x][y] && target_board[x][y]) {
          missing.push_back({x, y});
        }
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
            for (size_t j = 0; j < agents.size(); j++) {
                if (i != j && agents[j].pos == next_step) collision = true;
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
                    if (!valid_escape.empty()) {
                        int r = std::rand() % valid_escape.size();
                        a.current_path.push_back(valid_escape[r]);
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
            continue; // We made our physical step, allow next agent to move
        }
    }

    bool path_calculated_this_tick = false;

    // Phase 2 of Tick: For agents that didn't move because their path was empty, make decisions.
    for (size_t i = 0; i < agents.size(); i++) {
        CoatiAgent& a = agents[i];
        if (!a.current_path.empty() || a.wait_ticks > 0) continue; // Already moving or blocked

        // Re-filter available arrays for this specific agent's decision processing
        std::vector<Point> available_extras;
        for (auto e : extras) {
            bool claimed = false;
            for (size_t j = 0; j < agents.size(); j++) {
                if (i != j && agents[j].claimed_target == e) claimed = true;
            }
            if (!claimed) available_extras.push_back(e);
        }
        std::vector<Point> available_missing;
        for (auto m : missing) {
            bool claimed = false;
            for (size_t j = 0; j < agents.size(); j++) {
                if (i != j && agents[j].claimed_target == m) claimed = true;
            }
            if (!claimed) available_missing.push_back(m);
        }

        if (a.carrying) {
            bool standing_on_missing = false;
            for (auto m : missing) if (a.pos == m) standing_on_missing = true;
            bool standing_on_pool = false;
            for (auto p : pool) if (a.pos == p) standing_on_pool = true;

            if (standing_on_missing && !current_board[a.pos.x][a.pos.y]) {
                current_board[a.pos.x][a.pos.y] = true;
                fade_board[a.pos.x][a.pos.y] = 0.0f; // Start biological fade-in from 0% brightness!
                a.carrying = false;
                a.claimed_target = {-1, -1};
                continue;
            } else if (standing_on_pool && available_missing.empty()) { // Dunk if no other missing tasks available for us!
                a.carrying = false;
                a.claimed_target = {-1, -1};
                continue;
            }
            
            if (path_calculated_this_tick) continue; // STAGGER PATHFINDING

            if (!available_missing.empty()) {
                Point dest = find_closest(a.pos, available_missing);
                if (dest.x != -1) {
                    a.current_path = find_path(a.pos, dest, i);
                    if (!a.current_path.empty()) {
                        a.claimed_target = dest;
                    } else {
                        // Fallback: try ALL available missing
                        for(auto m : available_missing) {
                            a.current_path = find_path(a.pos, m, i);
                            if (!a.current_path.empty()) {
                                a.claimed_target = m; break;
                            }
                        }
                    }
                    path_calculated_this_tick = true;
                }
            } else if (!available_extras.empty() || a.carrying) { // We MUST dispose of our payload at the Pool if there are no missing targets!
                a.current_path = find_path(a.pos, pool[0], i);
                a.claimed_target = pool[0];
                path_calculated_this_tick = true;
            } else {
                // Done! Wait for target to change.
                a.claimed_target = {-1, -1};
                a.bored_ticks++;
            }
        } else {
            // Not carrying
            bool standing_on_extra = false;
            for (auto e : extras) if (a.pos == e) standing_on_extra = true;
            bool standing_on_dumpster = false;
            for (auto d : dumpster) if (a.pos == d) standing_on_dumpster = true;

            if (standing_on_extra && current_board[a.pos.x][a.pos.y]) {
                current_board[a.pos.x][a.pos.y] = false;
                a.carrying = true;
                a.claimed_target = {-1, -1};
                continue;
            } else if (standing_on_dumpster && !available_missing.empty()) { // DO NOT pickup if there are zero available missing slots for us (others claimed them)!
                a.carrying = true;
                a.claimed_target = {-1, -1};
                continue;
            }
            
            if (path_calculated_this_tick) continue; // STAGGER PATHFINDING

            if (!available_extras.empty()) {
                Point dest = find_closest(a.pos, available_extras);
                a.current_path = find_path(a.pos, dest, i);
                if (!a.current_path.empty()) {
                    a.claimed_target = dest;
                } else {
                    for(auto e : available_extras) {
                        a.current_path = find_path(a.pos, e, i);
                        if (!a.current_path.empty()) {
                            a.claimed_target = e; break;
                        }
                    }
                }
                path_calculated_this_tick = true;
            } else if (!available_missing.empty()) {
                a.current_path = find_path(a.pos, dumpster[1], i); // Fetch
                a.claimed_target = dumpster[1];
                path_calculated_this_tick = true;
            } else {
                // Bored!
                a.claimed_target = {-1, -1};
                a.bored_ticks++;
            }
        }

        // --- Panic Escape Logic ---
        // If we have tasks or are carrying a pixel, but failed to find ANY valid path (likely trapped by another agent)
        if (a.current_path.empty() && a.claimed_target.x == -1 && 
           (a.carrying || !available_missing.empty() || !available_extras.empty())) 
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
                if (!valid_escape.empty()) {
                    int r = std::rand() % valid_escape.size();
                    a.current_path.push_back(valid_escape[r]); // Take one blind random step
                }
            }
        }

        // --- Bored Wandering Logic ---
        if (a.current_path.empty() && a.claimed_target.x == -1) {
            // Less than one step per second. Tick is 50ms, so 20 ticks = 1 second.
            if (a.bored_ticks >= 20) {
                a.bored_ticks = 0;
                // Move random legal direction
                std::vector<Point> valid_wander;
                for (int dx = -1; dx <= 1; dx++) {
                    for (int dy = -1; dy <= 1; dy++) {
                        if (dx == 0 && dy == 0) continue;
                        int nx = a.pos.x + dx;
                        int ny = a.pos.y + dy;
                        if (nx >= 0 && nx < 32 && ny >= 0 && ny < 8) {
                            if (!current_board[nx][ny]) { // Don't step on clock pixels
                                bool collision = false;
                                for (size_t j = 0; j < agents.size(); j++) {
                                    if (i != j && agents[j].pos.x == nx && agents[j].pos.y == ny) collision = true;
                                }
                                if (!collision) {
                                    valid_wander.push_back({nx, ny});
                                }
                            }
                        }
                    }
                }
                
                if (!valid_wander.empty()) {
                    int r = std::rand() % valid_wander.size();
                    a.current_path.push_back(valid_wander[r]);
                }
            }
        }

    } // end agent loop
  }
};

} // namespace esphome

#pragma once

#include "Particle.h"
#include "creds.h" // Injects device-specific GRID_WIDTH, GRID_HEIGHT, GRID_ROTATION
#include <vector>
#include <queue>
#include <cmath>
#include <algorithm>

struct Point {
    int x, y;
    bool operator==(const Point& o) const { return x == o.x && y == o.y; }
    bool operator!=(const Point& o) const { return x != o.x || y != o.y; }
};

struct CoatiAgent {
    Point pos;
    Point last_pos;
    bool carrying = false;
    std::vector<Point> current_path;
    Point claimed_target = {-1, -1};
    int wait_ticks = 0;
    int bored_ticks = 0;
    bool is_bored = false;
    int wash_ticks = 0;  // > 0: currently dunking at pool
    bool washed = false; // true: this carry has been washed, ready to place
    int pause_ticks = 0; // > 0: frozen after pickup/placement (counts down each tick)
    int stuck_ticks = 0; // watchdog

    CoatiAgent(Point p) : pos(p), last_pos(p) {}
    CoatiAgent() = default;
};

class CoatiEngine {
public:
    bool current_board[GRID_WIDTH][GRID_HEIGHT] = {false};
    bool target_board[GRID_WIDTH][GRID_HEIGHT] = {false};
    float fade_board[GRID_WIDTH][GRID_HEIGHT] = {0.0f};

    // Staging buffer for target updates (font rendering)
    bool pending_target[GRID_WIDTH][GRID_HEIGHT] = {false};
    volatile bool target_pending = false;

    std::vector<CoatiAgent> agents;
    int pool_user = -1;
    time_t active_target = 0;
    time_t pending_time = 0;

    // Const locations
    Point dumpster[2] = {{0, GRID_HEIGHT - 1}, {1, GRID_HEIGHT - 1}};
    Point pool[2] = {{GRID_WIDTH - 2, GRID_HEIGHT - 1}, {GRID_WIDTH - 1, GRID_HEIGHT - 1}};

    CoatiEngine();

    void update_target(time_t virtual_now);
    void apply_pending_target();
    void tick();

    // Pathfinding
    std::vector<Point> find_path(Point start, Point dest, int self_index, int max_nodes = 256);

private:
    // Pathfinding scratch space (member vars to avoid stack overflow)
    bool pf_visited[GRID_WIDTH][GRID_HEIGHT];
    Point pf_parent[GRID_WIDTH][GRID_HEIGHT];
    float pf_cost[GRID_WIDTH][GRID_HEIGHT];

    Point find_closest(Point start, const std::vector<Point>& candidates);
    float dist(Point a, Point b) {
        return std::sqrt((a.x - b.x)*(a.x - b.x) + (a.y - b.y)*(a.y - b.y));
    }

    void draw_digit(int x, int y_offset, char c);
};

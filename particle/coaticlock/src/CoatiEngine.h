#pragma once

#include "Particle.h"
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
    int wash_ticks = 0;  // > 0: currently dunking at pool
    bool washed = false; // true: this carry has been washed, ready to place
    int pause_ticks = 0; // > 0: frozen after pickup/placement (counts down each tick)
    int stuck_ticks = 0; // watchdog

    CoatiAgent(Point p) : pos(p), last_pos(p) {}
    CoatiAgent() = default;
};

class CoatiEngine {
public:
    bool current_board[32][8] = {false};
    bool target_board[32][8] = {false};
    float fade_board[32][8] = {0.0f};

    // Staging buffer for target updates (font rendering)
    bool pending_target[32][8] = {false};
    volatile bool target_pending = false;

    std::vector<CoatiAgent> agents;
    int pool_user = -1;
    time_t active_target = 0;
    time_t pending_time = 0;

    // Const locations
    Point dumpster[2] = {{0, 7}, {1, 7}};
    Point pool[2] = {{30, 7}, {31, 7}};

    CoatiEngine();

    void update_target(time_t virtual_now);
    void apply_pending_target();
    void tick();

    // Pathfinding
    std::vector<Point> find_path(Point start, Point dest, int self_index, int max_nodes = 256);

private:
    // Pathfinding scratch space (member vars to avoid stack overflow)
    bool pf_visited[32][8];
    Point pf_parent[32][8];
    float pf_cost[32][8];

    Point find_closest(Point start, const std::vector<Point>& candidates);
    float dist(Point a, Point b) {
        return std::sqrt((a.x - b.x)*(a.x - b.x) + (a.y - b.y)*(a.y - b.y));
    }

    void draw_digit(int x, char c);
};

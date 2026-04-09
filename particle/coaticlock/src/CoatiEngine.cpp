#include "CoatiEngine.h"
#include <cstring>

static const uint8_t megafont_bitmaps[][6] = {
    {0x70, 0x98, 0x98, 0x98, 0x98, 0x70}, // 0
    {0x30, 0x70, 0x30, 0x30, 0x30, 0x78}, // 1
    {0xF0, 0x18, 0x70, 0xC0, 0xC0, 0xF8}, // 2
    {0xF8, 0x18, 0x70, 0x18, 0x98, 0x70}, // 3
    {0x80, 0x98, 0xF8, 0x18, 0x18, 0x18}, // 4
    {0xF8, 0xC0, 0xF0, 0x18, 0x98, 0x70}, // 5
    {0x70, 0xC0, 0xF0, 0xC8, 0xC8, 0x70}, // 6
    {0xF8, 0x18, 0x30, 0x60, 0x60, 0x60}, // 7
    {0x70, 0x98, 0x70, 0x98, 0x98, 0x70}, // 8
    {0x70, 0x98, 0x78, 0x18, 0x98, 0x70}, // 9
    {0x00, 0x18, 0x00, 0x00, 0x18, 0x00}  // :
};

struct CompareNode {
    bool operator()(const std::pair<float, Point>& a, const std::pair<float, Point>& b) {
        return a.first > b.first;
    }
};

CoatiEngine::CoatiEngine() {
    agents.push_back(CoatiAgent{{GRID_WIDTH / 4, 4}});
    agents.push_back(CoatiAgent{{(GRID_WIDTH * 3) / 4, 4}});
}

void CoatiEngine::draw_digit(int x, int y_offset, char char_val) {
    int idx = -1;
    if (char_val >= '0' && char_val <= '9') idx = char_val - '0';
    else if (char_val == ':') idx = 10;
    if (idx == -1) return;

    for (int row = 0; row < 6; row++) {
        uint8_t bits = megafont_bitmaps[idx][row];
        for (int col = 0; col < 5; col++) {
            if (bits & (0x80 >> col)) {
                int px = x + col;
                int py = row + y_offset; // Allow custom vertical placement
                if (px >= 0 && px < GRID_WIDTH && py >= 0 && py < GRID_HEIGHT) {
                    pending_target[px][py] = true;
                }
            }
        }
    }
}

void CoatiEngine::update_target(time_t virtual_now) {
    if (!Time.isValid()) return;
    int h = Time.hour(virtual_now);
    int m = Time.minute(virtual_now);
    
    char h_str[3], m_str[3];
    snprintf(h_str, sizeof(h_str), "%02d", h);
    snprintf(m_str, sizeof(m_str), "%02d", m);

    memset(pending_target, 0, sizeof(pending_target));
    
    if (GRID_HEIGHT >= 16) {
        // Square layout (16x16)
        draw_digit(2, 1, h_str[0]);    // Top row
        draw_digit(8, 1, h_str[1]);
        
        draw_digit(2, 8, m_str[0]); // Bottom row
        draw_digit(8, 8, m_str[1]);
    } else {
        // Wide layout (e.g. 32x8)
        draw_digit(2, 1, h_str[0]);
        draw_digit(8, 1, h_str[1]);
        draw_digit(14, 1, ':');
        draw_digit(18, 1, m_str[0]);
        draw_digit(24, 1, m_str[1]);
    }
    
    pending_time = virtual_now;
    target_pending = true;
}

void CoatiEngine::apply_pending_target() {
    if (!target_pending) return;
    target_pending = false;
    active_target = pending_time;
    int target_count = 0;
    for (int x = 0; x < GRID_WIDTH; x++) {
        for (int y = 0; y < GRID_HEIGHT; y++) {
            target_board[x][y] = pending_target[x][y];
            if (target_board[x][y]) target_count++;
        }
    }
    Log.info("Engine: Applied target %lu, pixels=%d, cur_addr=%p, tgt_addr=%p", 
             (unsigned long)active_target, target_count, (void*)current_board, (void*)target_board);

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
}

std::vector<Point> CoatiEngine::find_path(Point start, Point dest, int self_index, int max_nodes) {
    memset(pf_visited, 0, sizeof(pf_visited));
    memset(pf_parent, 0, sizeof(pf_parent));
    for (int x = 0; x < GRID_WIDTH; x++)
        for (int y = 0; y < GRID_HEIGHT; y++)
            pf_cost[x][y] = 1e9f;

    std::priority_queue<
        std::pair<float, Point>,
        std::vector<std::pair<float, Point>>,
        CompareNode> q;

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

        float h = dist(curr, dest);
        if (h < best_h) { best_h = h; best_frontier = curr; }

        for (int dx = -1; dx <= 1; dx++) {
            for (int dy = -1; dy <= 1; dy++) {
                if (dx == 0 && dy == 0) continue;
                int nx = curr.x + dx, ny = curr.y + dy;
                if (nx < 0 || nx >= GRID_WIDTH || ny < 0 || ny >= GRID_HEIGHT) continue;

                float pixel_cost = (current_board[nx][ny] && !(nx == dest.x && ny == dest.y)) ? 50.0f : 0.0f;
                float penalty = 0.0f;
                for (size_t j = 0; j < agents.size(); j++) {
                    if ((int)j != self_index && agents[j].pos.x == nx && agents[j].pos.y == ny && !(nx == dest.x && ny == dest.y))
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

    Point target = (pf_cost[dest.x][dest.y] < 1e8f) ? dest : best_frontier;
    std::vector<Point> path;
    if (target.x == -1 || (target.x == start.x && target.y == start.y)) {
        Log.trace("find_path: No expansion or target=start");
        return path;
    }

    Point cur = target;
    int safety = 0;
    while (!(cur.x == start.x && cur.y == start.y) && safety < 256) {
        path.push_back(cur);
        cur = pf_parent[cur.x][cur.y];
        safety++;
    }
    std::reverse(path.begin(), path.end());
    Log.trace("find_path: Found path length %d", (int)path.size());
    return path;
}

Point CoatiEngine::find_closest(Point start, const std::vector<Point>& candidates) {
    Point best = {-1, -1};
    float min_d = 1e9;
    for (auto c : candidates) {
        float d = dist(start, c);
        if (d < min_d) { min_d = d; best = c; }
    }
    return best;
}

void CoatiEngine::tick() {
    static std::vector<Point> tk_extras, tk_missing, tk_avail_e, tk_avail_m, tk_wander;
    tk_extras.clear(); tk_missing.clear();
    
    if (active_target == 0) return;
    
    for (int x = 0; x < GRID_WIDTH; x++) {
        for (int y = 0; y < GRID_HEIGHT; y++) {
            bool is_dumpster = (y == GRID_HEIGHT - 1 && (x == 0 || x == 1));
            bool is_pool = (y == GRID_HEIGHT - 1 && (x == GRID_WIDTH - 2 || x == GRID_WIDTH - 1));
            if (is_dumpster || is_pool) continue;
            if (current_board[x][y] && !target_board[x][y]) tk_extras.push_back({x, y});
            if (!current_board[x][y] && target_board[x][y]) tk_missing.push_back({x, y});
        }
    }

    for (int x = 0; x < GRID_WIDTH; x++) {
        for (int y = 0; y < GRID_HEIGHT; y++) {
            bool is_dumpster = (y == GRID_HEIGHT - 1 && (x == 0 || x == 1));
            bool is_pool = (y == GRID_HEIGHT - 1 && (x == GRID_WIDTH - 2 || x == GRID_WIDTH - 1));
            if (is_dumpster || is_pool) continue;
            if (current_board[x][y] && !target_board[x][y]) tk_extras.push_back({x, y});
            if (!current_board[x][y] && target_board[x][y]) tk_missing.push_back({x, y});
        }
    }
    for (size_t i = 0; i < agents.size(); i++) {
        CoatiAgent& a = agents[i];
        a.last_pos = a.pos;

        if (!a.current_path.empty()) {
            Point next_step = a.current_path.front();
            bool collision = false;
            for (size_t j = 0; j < agents.size(); j++) {
                if (i != j && agents[j].pos == next_step) collision = true;
            }

            if (collision) {
                a.wait_ticks++;
                if (a.wait_ticks > (3 + (int)i * 2)) {
                    a.current_path.clear();
                    a.claimed_target = {-1, -1};
                    a.wait_ticks = 0;
                }
                continue;
            }
            a.pos = next_step;
            a.current_path.erase(a.current_path.begin());
            a.wait_ticks = 0;
            a.stuck_ticks = 0;
            continue;
        }
    }

    bool path_calculated = false;
    for (size_t i = 0; i < agents.size(); i++) {
        CoatiAgent& a = agents[i];
        if (!a.current_path.empty() || a.wait_ticks > 0) continue;
        if (a.pause_ticks > 0) { a.pause_ticks--; continue; }

        tk_avail_e.clear();
        for (auto e : tk_extras) {
            bool claimed = false;
            for (size_t j = 0; j < agents.size(); j++) if (i != j && agents[j].claimed_target == e) claimed = true;
            if (!claimed) tk_avail_e.push_back(e);
        }
        tk_avail_m.clear();
        for (auto m : tk_missing) {
            bool claimed = false;
            for (size_t j = 0; j < agents.size(); j++) if (i != j && agents[j].claimed_target == m) claimed = true;
            if (!claimed) tk_avail_m.push_back(m);
        }

        if (a.carrying) {
            if (!a.washed) {
                if (a.wash_ticks > 0) {
                    a.wash_ticks--;
                    if (a.wash_ticks == 0) {
                        Log.info("Agent %d: Washed", (int)i);
                        a.washed = true;
                        if (pool_user == (int)i) pool_user = -1;
                        a.claimed_target = {-1, -1};
                    } else {
                        Point spot = pool[i % 2];
                        if (a.pos.y == spot.y) a.current_path = {{spot.x, spot.y - 1}};
                        else a.current_path = {spot};
                    }
                    continue;
                }
                bool at_pool = (a.pos.x == pool[i % 2].x && a.pos.y == pool[i % 2].y);
                if (at_pool) {
                    if (pool_user == -1 || pool_user == (int)i) {
                        Log.info("Agent %d: At pool, starting wash", (int)i);
                        pool_user = (int)i;
                        a.wash_ticks = 5;
                        a.current_path = {{a.pos.x, a.pos.y - 1}};
                    }
                    continue;
                }
                if (path_calculated) continue;
                if (pool_user == -1 || pool_user == (int)i) {
                    Point pool_dest = pool[i % 2];
                    Log.info("Agent %d: Calculating path to pool {%d,%d}", (int)i, pool_dest.x, pool_dest.y);
                    a.current_path = find_path(a.pos, pool_dest, (int)i);
                    a.claimed_target = pool_dest;
                    path_calculated = true;
                    continue;
                }
            } else {
                // Placing logic
                bool standing_on_m = false;
                for (auto m : tk_missing) if (a.pos.x == m.x && a.pos.y == m.y) standing_on_m = true;
                bool standing_on_dumpster_dump = (a.pos.y == GRID_HEIGHT - 1 && (a.pos.x == 0 || a.pos.x == 1));

                if (standing_on_dumpster_dump && tk_missing.empty()) {
                    Log.info("Agent %d: Dumped excess pixel at Dumpster", (int)i);
                    a.carrying = false; a.washed = false; a.claimed_target = {-1, -1}; a.pause_ticks = 4;
                    continue;
                } else if (standing_on_m && !current_board[a.pos.x][a.pos.y] && a.pos.x == a.claimed_target.x && a.pos.y == a.claimed_target.y) {
                    Log.info("Agent %d: Placed pixel at {%d,%d}", (int)i, a.pos.x, a.pos.y);
                    current_board[a.pos.x][a.pos.y] = true;
                    fade_board[a.pos.x][a.pos.y] = 0.0f;
                    a.carrying = false; a.washed = false; a.claimed_target = {-1, -1}; a.pause_ticks = 4;
                    continue;
                }
                if (path_calculated) continue;
                if (!tk_avail_m.empty()) {
                    Point dest = find_closest(a.pos, tk_avail_m);
                    Log.info("Agent %d: Carrying, pathing to missing {%d,%d}", (int)i, dest.x, dest.y);
                    a.current_path = find_path(a.pos, dest, (int)i);
                    if (!a.current_path.empty()) a.claimed_target = dest;
                    path_calculated = true;
                } else {
                    Point dest = dumpster[i % 2];
                    Log.info("Agent %d: Carrying excess, pathing to dumpster {%d,%d}", (int)i, dest.x, dest.y);
                    a.current_path = find_path(a.pos, dest, (int)i);
                    a.claimed_target = dest;
                    path_calculated = true;
                }
            }
        } else {
            // Not carrying
            bool standing_on_e = false;
            for (auto e : tk_extras) if (a.pos.x == e.x && a.pos.y == e.y) standing_on_e = true;
            bool standing_on_dumpster = (a.pos.y == GRID_HEIGHT - 1 && (a.pos.x == 0 || a.pos.x == 1));

            if ((standing_on_e && current_board[a.pos.x][a.pos.y]) || (standing_on_dumpster && !tk_missing.empty())) {
                Log.info("Agent %d: Picked up pixel at {%d,%d} (Target: %s)", (int)i, a.pos.x, a.pos.y, standing_on_dumpster ? "Dumpster" : "Board");
                if (standing_on_e) {
                    current_board[a.pos.x][a.pos.y] = false;
                    fade_board[a.pos.x][a.pos.y] = 0.0f;
                }
                a.carrying = true; a.claimed_target = {-1, -1}; a.pause_ticks = 4;
                continue;
            }
            if (path_calculated) continue;
            if (!tk_avail_e.empty()) {
                Point dest = find_closest(a.pos, tk_avail_e);
                Log.info("Agent %d: Empty, pathing to extra {%d,%d}", (int)i, dest.x, dest.y);
                a.current_path = find_path(a.pos, dest, (int)i);
                if (!a.current_path.empty()) a.claimed_target = dest;
                path_calculated = true;
            } else if (!tk_avail_m.empty()) {
                Point dest = dumpster[i % 2];
                Log.info("Agent %d: Empty/Available, pathing to dumpster {%d,%d}", (int)i, dest.x, dest.y);
                a.current_path = find_path(a.pos, dest, (int)i);
                a.claimed_target = dest;
                path_calculated = true;
            } else {
                bool floor_is_lava = current_board[a.pos.x][a.pos.y] && (a.pos.y != GRID_HEIGHT - 1);
                if (floor_is_lava) {
                    Log.info("Agent %d: Floor is Lava! Escaping to ground.", (int)i);
                    Point dest = { a.pos.x, GRID_HEIGHT - 1 };
                    a.current_path = find_path(a.pos, dest, (int)i);
                    path_calculated = true;
                } else {
                    a.bored_ticks++;
                    if (a.bored_ticks > 30) {
                        if (rand() % 2 == 0) {
                            int nx = a.pos.x + (rand() % 3 - 1);
                            if (nx >= 0 && nx < GRID_WIDTH && a.pos.y == GRID_HEIGHT - 1) {
                                a.current_path = {{nx, GRID_HEIGHT - 1}};
                            }
                        }
                        a.bored_ticks = 0;
                    }
                }
            }
        }
    }

    // Fade updates
    for (int x = 0; x < GRID_WIDTH; x++) {
        for (int y = 0; y < GRID_HEIGHT; y++) {
            if (current_board[x][y] && fade_board[x][y] < 1.0f) {
                fade_board[x][y] += 0.032f;
                if (fade_board[x][y] > 1.0f) fade_board[x][y] = 1.0f;
            }
        }
    }
}

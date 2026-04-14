#include "CoatiEngine.h"

const int   CoatiEngine::nest_count = 1;
const Point CoatiEngine::nest[] = {{0, 7}};

// megafont 5×6 bitmaps (0–9, colon)
static const uint8_t _megafont[][6] = {
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

namespace {
struct _CmpNode {
    bool operator()(const std::pair<float,Point>& a,
                    const std::pair<float,Point>& b) const {
        return a.first > b.first;
    }
};
} // namespace

CoatiEngine::CoatiEngine() {
    agents.reserve(80);
    for (int i = 0; i < 80; i++) {
        AgentState a;
        a.index = i;
        a.pos = {0, 7};
        a.last_pos = a.pos;
        a.active = false;
        agents.push_back(a);
    }
}

void CoatiEngine::_draw_digit(int x, int y_off, char c) {
    int idx = -1;
    if (c >= '0' && c <= '9') idx = c - '0';
    else if (c == ':') idx = 10;
    if (idx < 0) return;
    for (int row = 0; row < 6; row++) {
        uint8_t bits = _megafont[idx][row];
        for (int col = 0; col < 5; col++) {
            if (bits & (0x80 >> col)) {
                int px = x + col, py = row + y_off;
                if (px >= 0 && px < GRID_W && py >= 0 && py < GRID_H)
                    pending_target[px][py] = true;
            }
        }
    }
}

void CoatiEngine::update_target(time_t virtual_now) {
    if (!Time.isValid()) return;
    int h = Time.hour(virtual_now);
    int m = Time.minute(virtual_now);
    char hs[3], ms_[3];
    snprintf(hs, sizeof(hs),  "%02d", h);
    snprintf(ms_, sizeof(ms_), "%02d", m);
    memset(pending_target, 0, sizeof(pending_target));
    if (GRID_H >= 16) {
        _draw_digit(2,  1, hs[0]);  _draw_digit(9,  1, hs[1]);
        _draw_digit(2,  8, ms_[0]); _draw_digit(9,  8, ms_[1]);
    } else {
        _draw_digit(4,  1, hs[0]);  _draw_digit(10, 1, hs[1]);
        _draw_digit(17, 1, ms_[0]); _draw_digit(23, 1, ms_[1]);
    }
    pending_time   = virtual_now;
    target_pending = true;
}

void CoatiEngine::apply_pending_target() {
    if (!target_pending) return;
    target_pending = false;
    active_target  = pending_time;
    for (int x = 0; x < GRID_W; x++)
        for (int y = 0; y < GRID_H; y++)
            target_board[x][y] = pending_target[x][y];
    // Reset landmark locks
    // Reset all agents
    for (auto& a : agents) {
        a.claimed      = {-1, -1};
        a.path.clear();
        a.wait_counter = 0;
        a.pause_counter = 0;
    }
    Log.info("Engine: applied new target");
}

std::vector<Point> CoatiEngine::find_path(Point start, Point dest, int self_idx) {
    if (start == dest) return {};
    memset(pf_visited, 0, sizeof(pf_visited));
    for (int x = 0; x < GRID_W; x++)
        for (int y = 0; y < GRID_H; y++)
            pf_cost[x][y] = 1e9f;

    using _PQNode = std::pair<float, Point>;
    std::priority_queue<_PQNode, std::vector<_PQNode>, _CmpNode> q;
    pf_cost[start.x][start.y] = 0.0f;
    q.push({0.0f, start});
    const int MAX_NODES = 256;
    int expanded = 0;
    Point best = {-1, -1};
    float best_h = 1e9f;

    while (!q.empty() && expanded < MAX_NODES) {
        Point cur = q.top().second; q.pop();
        if (cur == dest) { best = dest; break; }
        if (pf_visited[cur.x][cur.y]) continue;
        pf_visited[cur.x][cur.y] = true;
        expanded++;
        float h = sqrtf((float)((cur.x-dest.x)*(cur.x-dest.x)+(cur.y-dest.y)*(cur.y-dest.y)));
        if (h < best_h) { best_h = h; best = cur; }
        for (int dx = -1; dx <= 1; dx++) {
            for (int dy = -1; dy <= 1; dy++) {
                if (dx == 0 && dy == 0) continue;
                int nx = cur.x + dx, ny = cur.y + dy;
                if (nx < 0 || nx >= GRID_W || ny < 0 || ny >= GRID_H) continue;
                bool is_dest = (nx == dest.x && ny == dest.y);
                float lit_cost = (!is_dest && current_board[nx][ny]) ? 50.0f : 0.0f;
                float occ_cost = 0.0f;
                for (size_t j = 0; j < agents.size(); j++)
                    if ((int)j != self_idx && agents[j].active &&
                        agents[j].pos.x == nx && agents[j].pos.y == ny && !is_dest)
                        occ_cost = 20.0f;
                float step = (dx==0||dy==0) ? 1.000f : 1.414f;
                float nc = pf_cost[cur.x][cur.y] + step + lit_cost + occ_cost;
                if (nc < pf_cost[nx][ny]) {
                    pf_cost[nx][ny] = nc;
                    pf_parent[nx][ny] = cur;
                    float g = sqrtf((float)((nx-dest.x)*(nx-dest.x)+(ny-dest.y)*(ny-dest.y)));
                    q.push({nc + g, {nx, ny}});
                }
            }
        }
    }

    Point tgt = (pf_cost[dest.x][dest.y] < 1e8f) ? dest : best;
    std::vector<Point> path;
    if (tgt.x < 0 || tgt == start) return path;
    Point cur = tgt;
    for (int safety = 0; !(cur == start) && safety < 512; safety++) {
        path.push_back(cur);
        cur = pf_parent[cur.x][cur.y];
    }
    std::reverse(path.begin(), path.end());
    return path;
}

Point CoatiEngine::_find_closest(Point s, const std::vector<Point>& cands) {
    Point best = {-1, -1}; float md = 1e9f;
    for (auto& c : cands) {
        float d = sqrtf((float)((s.x-c.x)*(s.x-c.x)+(s.y-c.y)*(s.y-c.y)));
        if (d < md) { md = d; best = c; }
    }
    return best;
}

bool CoatiEngine::_in_vec(const std::vector<Point>& v, Point p) const {
    for (auto& c : v) if (c == p) return true;
    return false;
}

bool CoatiEngine::_near_lit(Point p) const {
    for (int dx = -1; dx <= 1; dx++)
        for (int dy = -1; dy <= 1; dy++) {
            if (dx == 0 && dy == 0) continue;
            int nx = p.x+dx, ny = p.y+dy;
            if (nx>=0 && nx<GRID_W && ny>=0 && ny<GRID_H && current_board[nx][ny])
                return true;
        }
    return false;
}

bool CoatiEngine::_is_landmark(Point p) const {
    for (int i = 0; i < nest_count; i++) if (nest[i] == p) return true;
    return false;
}

void CoatiEngine::tick() {
    tk_extras.clear();
    tk_missing.clear();

    for (int x = 0; x < GRID_W; x++) {
        for (int y = 0; y < GRID_H; y++) {
            if (_is_landmark({x, y})) continue;
            if ((current_board[x][y] && !target_board[x][y])) tk_extras.push_back({x, y});
            if ((!current_board[x][y] && target_board[x][y])) tk_missing.push_back({x, y});
        }
    }

    // Spawn phase
    {
        static int _spawn_timer = 0;
        if (++_spawn_timer >= 10) {
            _spawn_timer = 0;
            if ((!tk_missing.empty())) {
                for (auto& _sa : agents) {
                    if (!_sa.active) {
                        _sa.active    = true;
                        _sa.pos       = {0, 7};
                        _sa.last_pos  = _sa.pos;
                        _sa.path.clear();
                        _sa.claimed   = {-1, -1};
                        _sa.wait_counter = 0;
                        _sa.pause_counter = 0;
                        break;
                    }
                }
            }
        }
    }

    // Movement phase
    for (size_t i = 0; i < agents.size(); i++) {
        AgentState& a = agents[i];
        if (!a.active) continue;
        a.last_pos = a.pos;
        if (a.path.empty()) continue;
        Point nxt = a.path.front();
        bool col  = false;
        for (size_t j = 0; j < agents.size(); j++)
            if (i != j && agents[j].active && agents[j].pos == nxt) { col = true; break; }
        if (col) {
            a.wait_counter++;
            a.wait_counter++;
            if ((a.wait_counter > 3)) {
                a.path.clear();
                a.claimed = {-1, -1};
                a.wait_counter = 0;  // reset
            }
        } else {
            a.pos = nxt;
            a.path.erase(a.path.begin());
            a.wait_counter = 0;
        }
    }

    // Behavior phase
    int _pf_budget = 0;
    for (size_t i = 0; i < agents.size(); i++) {
        AgentState& a = agents[i];
        if (!a.active) continue;
        if (!a.path.empty() || a.wait_counter > 0) continue;
        if (_pf_budget >= 999) break;


        bool _used_pf = false;
        // Compute available (unclaimed-by-others) term subsets
        std::vector<Point> tk_avail_extras;
        for (auto& _p : tk_extras) {
            bool _cl = false;
            for (size_t j = 0; j < agents.size(); j++)
                if (j != i && agents[j].active && agents[j].claimed == _p) { _cl = true; break; }
            if (!_cl) tk_avail_extras.push_back(_p);
        }
        std::vector<Point> tk_avail_missing;
        for (auto& _p : tk_missing) {
            bool _cl = false;
            for (size_t j = 0; j < agents.size(); j++)
                if (j != i && agents[j].active && agents[j].claimed == _p) { _cl = true; break; }
            if (!_cl) tk_avail_missing.push_back(_p);
        }

        auto _eval = [&]() {
            if ((a.pause_counter > 0)) {
                a.pause_counter--;
                return;  // done
                return;  // end of when-block
            }
            if ((_in_vec(tk_extras, a.pos) && current_board[a.pos.x][a.pos.y])) {
                if (a.pos.x >= 0 && a.pos.x < GRID_W && a.pos.y >= 0 && a.pos.y < GRID_H) {
                    current_board[a.pos.x][a.pos.y] = false;
                    fade_board[a.pos.x][a.pos.y]    = 0.0f;
                }
                a.pause_counter = 5;
                return;  // done
                return;  // end of when-block
            }
            if (current_board[a.pos.x][a.pos.y]) {
                // set_display_color: simulator-only — omitted on device
                return;  // done
                return;  // end of when-block
            }
            if (_in_vec(tk_missing, a.pos)) {
                // set_display_color: simulator-only — omitted on device
                if (a.pos.x >= 0 && a.pos.x < GRID_W && a.pos.y >= 0 && a.pos.y < GRID_H) {
                    current_board[a.pos.x][a.pos.y] = true;
                    fade_board[a.pos.x][a.pos.y]    = 0.0f;
                }
                return;  // done
                return;  // end of when-block
            }
            if ((!tk_avail_missing.empty())) {
                {  // seek nearest available missing
                    if (!tk_avail_missing.empty()) {
                        Point _dest = _find_closest(a.pos, tk_avail_missing);
                        a.claimed = _dest;
                        if (a.pos == _dest) return;  // already here
                        if (!_used_pf) {
                            a.path = find_path(a.pos, _dest, (int)i);
                            _used_pf = true;
                        }
                    }
                }
                return;  // done
                return;  // end of when-block
            }
            if ((!tk_avail_extras.empty())) {
                {  // seek nearest available extras
                    if (!tk_avail_extras.empty()) {
                        Point _dest = _find_closest(a.pos, tk_avail_extras);
                        a.claimed = _dest;
                        if (a.pos == _dest) return;  // already here
                        if (!_used_pf) {
                            a.path = find_path(a.pos, _dest, (int)i);
                            _used_pf = true;
                        }
                    }
                }
                return;  // done
                return;  // end of when-block
            }
            if ((a.pos != nest[i % nest_count])) {
                // set_display_color: simulator-only — omitted on device
                {  // seek nest
                    Point _dest = nest[i % nest_count];
                    a.claimed = _dest;
                    if (a.pos == _dest) return;  // already here
                    if (!_used_pf) {
                        a.path = find_path(a.pos, _dest, (int)i);
                        _used_pf = true;
                    }
                }
                return;  // done
                return;  // end of when-block
            }
            if ((a.pos == nest[i % nest_count])) {
                a.active = false;
                a.path.clear();
                a.claimed = {-1, -1};
                return;  // despawn
                return;  // end of when-block
            }
        };
        _eval();
        if (_used_pf) _pf_budget++;
    }

    // Fade update
    for (int x = 0; x < GRID_W; x++)
        for (int y = 0; y < GRID_H; y++)
            if (current_board[x][y] && fade_board[x][y] < 1.0f) {
                fade_board[x][y] += 0.032f;
                if (fade_board[x][y] > 1.0f) fade_board[x][y] = 1.0f;
            }
}

void CoatiEngine::render(Adafruit_NeoPixel& strip, float bri, uint32_t now_ms, float blend, int rotation) {
    strip.clear();
    const int PIXEL_COUNT = GRID_W * GRID_H;

    auto _map = [&](int x, int y) -> int {
        if (x < 0 || x >= GRID_W || y < 0 || y >= GRID_H) return -1;
        int rx = x, ry = y;
        int max_x = GRID_W - 1;
        int max_y = GRID_H - 1;
        if (rotation == 90)       { rx = max_y - y; ry = x; }
        else if (rotation == 180) { rx = max_x - x; ry = max_y - y; }
        else if (rotation == 270) { rx = y; ry = max_x - x; }
        int phys_h = (rotation == 90 || rotation == 270) ? GRID_W : GRID_H;
        if (rx % 2 == 0) return (rx * phys_h) + ry;
        return (rx * phys_h) + ((phys_h - 1) - ry);
    };

    auto _set = [&](int x, int y, uint32_t c) {
        int idx = _map(x, y);
        if (idx >= 0 && idx < PIXEL_COUNT) strip.setPixelColor(idx, c);
    };

    auto _cc = [](uint8_t r, uint8_t g, uint8_t b, float s) -> uint32_t {
        if (s <= 0.0f) return 0;
        if (s > 1.0f) s = 1.0f;
        auto scale = [](uint8_t v, float s) -> uint8_t {
            if (v == 0) return 0;
            float f = (float)v * s;
            return (uint8_t)(f < 1.0f ? 1.0f : f);
        };
        return Adafruit_NeoPixel::Color(scale(r, s), scale(g, s), scale(b, s));
    };

    // board pixels (lit cells, excluding landmark cells)
    for (int x = 0; x < GRID_W; x++) {
        for (int y = 0; y < GRID_H; y++) {
            if (!current_board[x][y]) continue;
            if (_is_landmark({x, y})) continue;
            _set(x, y, _cc(0, 64, 0, fade_board[x][y] * bri));
        }
    }

    // landmark: nest
    for (int k = 0; k < nest_count; k++) {
        const Point& p = nest[k];
        _set(p.x, p.y, _cc(64, 0, 0, bri));
    }

    // agents
    for (size_t i = 0; i < agents.size(); i++) {
        AgentState& a = agents[i];
        if (!a.active) continue;
        uint32_t color = 0;
        bool matched = false;

        if (!matched && (current_board[a.pos.x][a.pos.y])) {
            uint32_t _c = _cc(0, 255, 0, bri);
            color = _c;
            matched = true;
        }
        if (!matched && (_in_vec(tk_missing, a.pos))) {
            uint32_t _c = _cc(0, 255, 0, bri);
            color = _c;
            matched = true;
        }
        if (!matched) {
            uint32_t _c = _cc(255, 0, 0, bri);
            color = _c;
            matched = true;
        }

        if (a.pos == a.last_pos) {
            _set(a.pos.x, a.pos.y, color);
        } else {
            uint8_t r = (color >> 16) & 0xFF;
            uint8_t g = (color >>  8) & 0xFF;
            uint8_t b =  color        & 0xFF;
            _set(a.last_pos.x, a.last_pos.y, _cc(r, g, b, (1.0f - blend)));
            _set(a.pos.x,      a.pos.y,      _cc(r, g, b, blend));
        }
    }

    strip.show();
}
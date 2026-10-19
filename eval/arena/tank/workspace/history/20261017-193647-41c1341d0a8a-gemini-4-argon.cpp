#include "tank.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

using namespace tank;

namespace {

constexpr int INF = 1000000;

struct MapCache {
    // 3 attack lanes per enemy base: 0=Left flank, 1=Center, 2=Right flank
    int dist_lane[6][3][CELLS];
    // Minimum across all 3 lanes
    int dist_atk[6][CELLS];
    // Distance from tank's current pos (or spawn) to cell c
    int dist_from_tank[6][CELLS];
    // Distance from supply k to cell c
    int dist_supply[MAX_SUPPLY][CELLS];
    // Is cell part of seat's base protective ring/wall?
    bool is_base_shield[2][CELLS];
    int base_ray_dist[2][CELLS];
    int base_ray_dir[2][CELLS]; // direction from c toward base[seat]
    int base_ray_lane[2][CELLS]; // 0=left, 1=center, 2=right, -1=none

    // Role assignments per tank 0..5
    int assigned_lane[6];
    int assigned_supply[6];
    int assigned_def_enemy[6];
    int assigned_def_urgency[6];

    // Snapshot of root grid to detect broken walls during lookahead
    uint8_t root_grid[CELLS];
    bool is_atk_wall[2][CELLS];
};

static MapCache G;
static int last_pos_hist[6][6];
static int hist_turn = -1;

inline int manhattan(int c1, int c2) {
    return std::abs(cell_x(c1) - cell_x(c2)) + std::abs(cell_y(c1) - cell_y(c2));
}

inline bool is_enemy_spawn_soon(const State& s, int seat, int cell) {
    int op = 1 - seat;
    for (int t = 0; t < 3; ++t) {
        int i = tank_id(op, t);
        if (s.spawn[i] == cell && !s.tank[i].alive && respawn_in(s, i) <= 1) {
            return true;
        }
    }
    return false;
}

inline bool is_ally_spawn_soon(const State& s, int seat, int self_id, int cell) {
    for (int t = 0; t < 3; ++t) {
        int i = tank_id(seat, t);
        if (i != self_id && s.spawn[i] == cell && !s.tank[i].alive && respawn_in(s, i) <= 1) {
            return true;
        }
    }
    return false;
}

inline void count_ray_obstacles(const State& s, int seat, int c, int& bricks, int& steels) {
    bricks = 0;
    steels = 0;
    int dist = G.base_ray_dist[seat][c];
    if (dist <= 0) return;
    int dir = G.base_ray_dir[seat][c];
    for (int st = 1; st < dist; ++st) {
        uint8_t g = s.grid[c + st * DIR_DELTA[dir]];
        if (g == BRICK) bricks++;
        else if (g == STEEL) steels++;
    }
}

int enemy_base_threat_level(const State& s, int seat, int e) {
    if (!s.tank[e].alive) return 0;
    int epos = s.tank[e].pos;
    if (G.base_ray_dist[seat][epos] > 0) {
        int bricks = 0, steels = 0;
        count_ray_obstacles(s, seat, epos, bricks, steels);
        bool can_st = (s.tank[e].level >= 2 && s.P.level2_steel);
        if (steels == 0 || can_st) {
            if (bricks == 0 && G.base_ray_dist[seat][epos] <= 7) return 3;
            if (bricks <= 1 && G.base_ray_dist[seat][epos] <= 6) return 3;
            if (bricks <= 2 && G.base_ray_dist[seat][epos] <= 6) return 2;
        }
    }
    int eatk = G.dist_atk[e][epos];
    int ey = cell_y(epos);
    bool in_my_half = (seat == 0) ? (ey >= 7) : (ey <= 7);
    bool deep_in_my_half = (seat == 0) ? (ey >= 10) : (ey <= 4);
    if (eatk <= 7 || deep_in_my_half || manhattan(epos, s.base_pos[seat]) <= 5) return 3;
    if (eatk <= 14 || in_my_half) return 2;
    return 0;
}

void spfa_run(int* dist, int* q, bool* in_q, int& head, int& tail,
              const State& s, int seat, int self_id, int move_cost, int wall_cost, bool can_steel, bool add_congestion) {
    int op = 1 - seat;
    while (head != tail) {
        int u = q[head++];
        if (head == CELLS) head = 0;
        in_q[u] = false;
        int du = dist[u];
        for (int dir = 0; dir < 4; ++dir) {
            int v = u + DIR_DELTA[dir];
            uint8_t g = s.grid[v];
            if (g == OUTSIDE || g == WATER || g == BASE0 || g == BASE1) continue;
            if (g == STEEL && !can_steel) continue;
            if (G.is_base_shield[seat][v] && (g == BRICK || g == STEEL)) continue;
            int step_c = (g == EMPTY) ? move_cost : wall_cost;

            if (add_congestion && v != s.tank[self_id].pos) {
                int occ = tank_at(s, v);
                if (occ >= 0 && occ != self_id) {
                    step_c += (seat_of(occ) == seat) ? 5 : 4;
                }
                for (int et = 0; et < 3; ++et) {
                    int eid = tank_id(op, et);
                    if (s.tank[eid].alive && can_fire(s, eid) && manhattan(v, s.tank[eid].pos) == 1) {
                        if (s.tank[self_id].hp <= s.tank[eid].hp) step_c += 3;
                    }
                }
            }

            if (du + step_c < dist[v]) {
                dist[v] = du + step_c;
                if (!in_q[v]) {
                    in_q[v] = true;
                    q[tail++] = v;
                    if (tail == CELLS) tail = 0;
                }
            }
        }
    }
}

void precompute(const State& s) {
    std::memset(&G, 0, sizeof(G));
    std::memcpy(G.root_grid, s.grid, sizeof(s.grid));
    for (int seat = 0; seat < 2; ++seat) {
        for (int c = 0; c < CELLS; ++c) G.base_ray_lane[seat][c] = -1;
    }

    const int n = s.P.size;
    const int R = s.P.ring;

    for (int seat = 0; seat < 2; ++seat) {
        int bpos = s.base_pos[seat];
        int bx = cell_x(bpos), by = cell_y(bpos);
        for (int y = std::max(0, by - R); y <= std::min(n - 1, by + R); ++y) {
            for (int x = std::max(0, bx - R); x <= std::min(n - 1, bx + R); ++x) {
                int c = cell_of(x, y);
                if (c != bpos) G.is_base_shield[seat][c] = true;
            }
        }
        for (int d = 0; d < 4; ++d) {
            int to_base_dir = d ^ 1;
            int lane = (d == LEFT) ? 0 : (d == RIGHT ? 2 : 1);
            for (int k = 1;; ++k) {
                int c = bpos + k * DIR_DELTA[d];
                uint8_t g = s.grid[c];
                if (g == OUTSIDE || g == BASE0 || g == BASE1) break;
                if (k <= 3 && (g == BRICK || g == STEEL)) {
                    G.is_base_shield[seat][c] = true;
                }
                G.base_ray_dist[seat][c] = k;
                G.base_ray_dir[seat][c] = to_base_dir;
                G.base_ray_lane[seat][c] = lane;
            }
        }
    }

    int q[CELLS];
    bool in_q[CELLS] = {};

    // Supply distances
    for (int k = 0; k < s.n_supply; ++k) {
        for (int c = 0; c < CELLS; ++c) G.dist_supply[k][c] = INF;
        int src = s.supply[k];
        int head = 0, tail = 0;
        G.dist_supply[k][src] = 0;
        q[tail++] = src;
        in_q[src] = true;
        while (head != tail) {
            int u = q[head++];
            if (head == CELLS) head = 0;
            in_q[u] = false;
            int du = G.dist_supply[k][u];
            for (int d = 0; d < 4; ++d) {
                int v = u + DIR_DELTA[d];
                uint8_t g = s.grid[v];
                if (G.is_base_shield[0][v] || G.is_base_shield[1][v]) {
                    if (g == BRICK || g == STEEL) continue;
                }
                int w = (g == EMPTY) ? 1 : (g == BRICK ? 3 : INF);
                if (w < INF && du + w < G.dist_supply[k][v]) {
                    G.dist_supply[k][v] = du + w;
                    if (!in_q[v]) {
                        in_q[v] = true;
                        q[tail++] = v;
                        if (tail == CELLS) tail = 0;
                    }
                }
            }
        }
    }

    // Per-tank Dijkstra for the 3 lanes of enemy base and distance from tank
    for (int i = 0; i < 6; ++i) {
        int seat = seat_of(i);
        int op = 1 - seat;
        int spd = s.P.types[type_of(i)].speed;
        int rl = reload_turns(s, i);
        bool can_steel = (s.tank[i].level >= 2 && s.P.level2_steel);
        int move_cost = (spd >= 2) ? 1 : 2;
        int wall_cost = move_cost + 2 + 2 * rl;

        for (int c = 0; c < CELLS; ++c) {
            G.dist_atk[i][c] = INF;
            G.dist_from_tank[i][c] = INF;
            for (int l = 0; l < 3; ++l) G.dist_lane[i][l][c] = INF;
        }

        int bpos = s.base_pos[op];

        for (int d = 0; d < 4; ++d) {
            int check_c = bpos + DIR_DELTA[d];
            if (s.grid[check_c] == OUTSIDE) continue;
            int lane = (d == LEFT) ? 0 : (d == RIGHT ? 2 : 1);
            int head = 0, tail = 0;

            int bricks_b = 0, steels_b = 0;
            for (int k = 1;; ++k) {
                int c = bpos + k * DIR_DELTA[d];
                uint8_t g = s.grid[c];
                if (g == OUTSIDE || g == BASE0 || g == BASE1) break;
                if (steels_b > 0 && !can_steel) break;

                if (g != WATER && (g != STEEL || can_steel)) {
                    int walls_b = bricks_b + steels_b;
                    int shot_interval = 2 + 2 * rl + (k > 2 ? (k - 1) : 0);
                    int post_cost = walls_b * shot_interval;
                    if (k == 1) post_cost += 0;
                    else if (k == 2) post_cost += 1;
                    else post_cost += 2 + (k - 2) * 3;

                    for (int t = 0; t < 3; ++t) {
                        int sp = s.spawn[tank_id(op, t)];
                        if (sp == c) post_cost += 5;
                        else if (c == bpos + DIR_DELTA[d] && sp == bpos + 2 * DIR_DELTA[d]) {
                            post_cost += 3;
                        }
                    }
                    if (g == BRICK || g == STEEL) {
                        post_cost += 2 + 2 * rl;
                    }
                    if (post_cost < G.dist_lane[i][lane][c]) {
                        G.dist_lane[i][lane][c] = post_cost;
                        if (!in_q[c]) {
                            in_q[c] = true;
                            q[tail++] = c;
                            if (tail == CELLS) tail = 0;
                        }
                    }
                }
                if (g == BRICK) bricks_b++;
                else if (g == STEEL) steels_b++;
            }

            spfa_run(G.dist_lane[i][lane], q, in_q, head, tail, s, seat, i, move_cost, wall_cost, can_steel, true);
        }

        for (int c = 0; c < CELLS; ++c) {
            G.dist_atk[i][c] = std::min({G.dist_lane[i][0][c], G.dist_lane[i][1][c], G.dist_lane[i][2][c]});
        }

        int start_c = s.tank[i].alive ? s.tank[i].pos : s.spawn[i];
        int head = 0, tail = 0;
        G.dist_from_tank[i][start_c] = 0;
        q[tail++] = start_c;
        in_q[start_c] = true;
        spfa_run(G.dist_from_tank[i], q, in_q, head, tail, s, seat, i, move_cost, wall_cost, can_steel, false);
    }

    // Role Assignment for each seat
    int next_turn = s.turn + 1;
    int turns_to_spawn = INF;
    if (next_turn <= s.P.supply_first) {
        turns_to_spawn = s.P.supply_first - next_turn;
    } else {
        int rem = (next_turn - s.P.supply_first) % s.P.supply_period;
        turns_to_spawn = (rem == 0) ? 0 : (s.P.supply_period - rem);
    }

    for (int seat = 0; seat < 2; ++seat) {
        int op = 1 - seat;
        for (int tp = 0; tp < 3; ++tp) {
            int i = tank_id(seat, tp);
            G.assigned_lane[i] = 1;
            G.assigned_supply[i] = -1;
            G.assigned_def_enemy[i] = -1;
            G.assigned_def_urgency[i] = 0;
        }

        // 1. Ironclad Defense Assignment
        for (int et = 0; et < 3; ++et) {
            int e = tank_id(op, et);
            int th = enemy_base_threat_level(s, seat, e);
            if (th >= 2) {
                int best_t = -1, second_t = -1;
                int best_d = INF, second_d = INF;
                for (int tp = 0; tp < 3; ++tp) {
                    int i = tank_id(seat, tp);
                    int pos = s.tank[i].alive ? s.tank[i].pos : s.spawn[i];
                    if (s.tank[i].alive && G.dist_atk[i][pos] <= 1 && s.base_hp[op] <= 4 && s.base_hp[seat] >= s.base_hp[op] + 3) {
                        continue;
                    }
                    int d = G.dist_from_tank[e][pos] + (s.tank[i].alive ? 0 : respawn_in(s, i) * 3);
                    if (s.tank[i].alive && G.dist_atk[i][pos] <= 2) d += 10;
                    if (d < best_d) {
                        second_d = best_d; second_t = best_t;
                        best_d = d; best_t = i;
                    } else if (d < second_d) {
                        second_d = d; second_t = i;
                    }
                }
                if (best_t >= 0 && (th == 3 || best_d <= 24)) {
                    if (th > G.assigned_def_urgency[best_t]) {
                        G.assigned_def_enemy[best_t] = e;
                        G.assigned_def_urgency[best_t] = th;
                    }
                }
                if (th == 3 && (s.tank[e].level >= 1 || s.tank[e].hp >= 2) && second_t >= 0 && second_d <= 14) {
                    if (th > G.assigned_def_urgency[second_t]) {
                        G.assigned_def_enemy[second_t] = e;
                        G.assigned_def_urgency[second_t] = th;
                    }
                }
            }
        }

        // 2. Supply / Star assignment
        bool sup_taken[MAX_SUPPLY] = {};
        for (int step_match = 0; step_match < 3; ++step_match) {
            int best_i = -1, best_k = -1, best_d = INF;
            for (int tp = 0; tp < 3; ++tp) {
                int i = tank_id(seat, tp);
                if (!s.tank[i].alive || s.tank[i].level >= s.P.max_level) continue;
                if (G.assigned_supply[i] >= 0) continue;
                if (G.assigned_def_urgency[i] >= 3) continue;
                if (G.dist_atk[i][s.tank[i].pos] <= 2) continue;

                for (int k = 0; k < s.n_supply; ++k) {
                    if (sup_taken[k]) continue;
                    bool has_star = ((s.star_mask >> k) & 1) != 0;
                    if (!has_star && turns_to_spawn > 9) continue;
                    int ds = G.dist_supply[k][s.tank[i].pos];
                    int max_reach = has_star ? 10 : (turns_to_spawn + 3);
                    if (ds <= max_reach && ds < best_d) {
                        best_d = ds;
                        best_i = i;
                        best_k = k;
                    }
                }
            }
            if (best_i >= 0 && best_k >= 0) {
                G.assigned_supply[best_i] = best_k;
                sup_taken[best_k] = true;
            }
        }

        // 3. Multi-Lane Attack Assignment
        int best_l[3] = {0, 1, 2};
        int best_total_cost = INF;
        for (int l0 = 0; l0 < 3; ++l0) {
            for (int l1 = 0; l1 < 3; ++l1) {
                for (int l2 = 0; l2 < 3; ++l2) {
                    int ls[3] = {l0, l1, l2};
                    int cost = 0;
                    for (int tp = 0; tp < 3; ++tp) {
                        int i = tank_id(seat, tp);
                        int pos = s.tank[i].alive ? s.tank[i].pos : s.spawn[i];
                        int d = G.dist_lane[i][ls[tp]][pos];
                        if (d >= INF / 2) d = 200;
                        if (ls[tp] != 1) d -= 2;
                        cost += d;
                    }
                    if (l0 == l1) cost += 16;
                    if (l0 == l2) cost += 16;
                    if (l1 == l2) cost += 16;
                    if (cost < best_total_cost) {
                        best_total_cost = cost;
                        best_l[0] = l0; best_l[1] = l1; best_l[2] = l2;
                    }
                }
            }
        }
        for (int tp = 0; tp < 3; ++tp) {
            G.assigned_lane[tank_id(seat, tp)] = best_l[tp];
        }

        for (int c = 0; c < CELLS; ++c) {
            if (s.grid[c] != BRICK && s.grid[c] != STEEL) continue;
            if (G.is_base_shield[seat][c]) continue;
            if (G.is_base_shield[op][c] || G.base_ray_dist[op][c] > 0) {
                G.is_atk_wall[seat][c] = true;
                continue;
            }
            for (int tp = 0; tp < 3; ++tp) {
                int i = tank_id(seat, tp);
                if (!s.tank[i].alive) continue;
                int pos = s.tank[i].pos;
                if (manhattan(pos, c) <= 2) {
                    int lane = G.assigned_lane[i];
                    if (G.dist_lane[i][lane][c] < G.dist_lane[i][lane][pos] ||
                        G.dist_atk[i][c] < G.dist_atk[i][pos]) {
                        G.is_atk_wall[seat][c] = true;
                    }
                    int sup = G.assigned_supply[i];
                    if (sup >= 0 && G.dist_supply[sup][c] < G.dist_supply[sup][pos]) {
                        G.is_atk_wall[seat][c] = true;
                    }
                    int def_e = G.assigned_def_enemy[i];
                    if (def_e >= 0 && G.dist_from_tank[def_e][c] < G.dist_from_tank[def_e][pos]) {
                        G.is_atk_wall[seat][c] = true;
                    }
                }
            }
        }
    }
}

struct RayHit {
    int kind; // 0=none/outside, 1=brick, 2=steel, 3=my_base, 4=op_base, 5=ally_tank, 6=enemy_tank, 7=enemy_bullet
    int dist;
    int target_id;
    int cell;
};

RayHit trace_shot(const State& s, int seat, int self_id, int pos, int dir) {
    int opp_dir = dir ^ 1;
    for (int step = 1;; ++step) {
        int c = pos + step * DIR_DELTA[dir];
        for (int k = 0; k < s.n_bullets; ++k) {
            if (s.bullet[k].pos == c && s.bullet[k].dir == opp_dir) {
                return RayHit{7, step, k, c};
            }
        }
        uint8_t g = s.grid[c];
        if (g == OUTSIDE) return RayHit{0, step, -1, c};
        if (g == BASE0 + seat) return RayHit{3, step, seat, c};
        if (g == BASE0 + (1 - seat)) return RayHit{4, step, 1 - seat, c};
        if (g == BRICK) return RayHit{1, step, -1, c};
        if (g == STEEL) return RayHit{2, step, -1, c};
        if (g == EMPTY) {
            int tid = tank_at(s, c);
            if (tid >= 0 && tid != self_id) {
                if (seat_of(tid) == seat) return RayHit{5, step, tid, c};
                else return RayHit{6, step, tid, c};
            }
        }
    }
}

struct MoveSafety {
    int p1, p2;
    int existing_bullet_hits;
    int direct_enemy_threat;
    int cross_enemy_threat;
    int cancels_enemy_bullet;
    int cancels_base_bullet;
    int intercepts_enemy_fire;
};

MoveSafety check_cmd_safety(const State& s, int i, Cmd c) {
    MoveSafety ms{};
    const Tank& t = s.tank[i];
    int seat = seat_of(i);
    int op = 1 - seat;
    int pos = t.pos;
    ms.p1 = pos;
    ms.p2 = pos;

    bool fired = false;
    int fire_dir = -1;
    int f1 = NO_CELL, f2 = NO_CELL;
    if (is_move(c)) {
        int d1 = c - 1;
        int n1 = pos + DIR_DELTA[d1];
        if (s.grid[n1] == EMPTY && tank_at(s, n1) < 0) ms.p1 = n1;
        ms.p2 = ms.p1;
    } else if (is_move2(c)) {
        int d1 = (c - 9) >> 2, d2 = (c - 9) & 3;
        int n1 = pos + DIR_DELTA[d1];
        if (s.grid[n1] == EMPTY && tank_at(s, n1) < 0) ms.p1 = n1;
        int n2 = ms.p1 + DIR_DELTA[d2];
        if (s.grid[n2] == EMPTY && (n2 == pos || tank_at(s, n2) < 0)) ms.p2 = n2;
    } else if (is_fire(c) && can_fire(s, i)) {
        fired = true;
        fire_dir = c - 5;
        f1 = pos + DIR_DELTA[fire_dir];
        if ((s.grid[f1] == EMPTY || s.grid[f1] == WATER) && tank_at(s, f1) < 0) {
            f2 = f1 + DIR_DELTA[fire_dir];
        }
    }

    // 1. Existing bullets on board
    for (int k = 0; k < s.n_bullets; ++k) {
        const Bullet& b = s.bullet[k];
        int b1 = b.pos + DIR_DELTA[b.dir];
        if (fired) {
            bool cancel1 = (b.dir == (fire_dir ^ 1) && b.pos == f1) || (b1 == f1 && (s.grid[f1] == EMPTY || s.grid[f1] == WATER));
            if (cancel1) {
                if (seat_of(b.owner) != seat) {
                    ms.cancels_enemy_bullet++;
                    if (G.base_ray_dist[seat][b.pos] > 0 && G.base_ray_dir[seat][b.pos] == b.dir) {
                        ms.cancels_base_bullet++;
                    }
                }
                continue;
            }
        }
        if (ms.p1 != pos && b1 == pos && ms.p1 == b.pos) {
            ms.existing_bullet_hits++;
            continue;
        }
        if (b1 == ms.p1 && s.grid[b1] == EMPTY) {
            ms.existing_bullet_hits++;
            continue;
        }
        uint8_t g1 = s.grid[b1];
        if (g1 != EMPTY && g1 != WATER) continue;
        int v1 = tank_at(s, b1);
        if (v1 >= 0 && v1 != i) continue;

        int b2 = b1 + DIR_DELTA[b.dir];
        if (fired && f2 != NO_CELL) {
            bool cancel2 = (b.dir == (fire_dir ^ 1) && b1 == f2) || (b2 == f2 && (s.grid[f2] == EMPTY || s.grid[f2] == WATER));
            if (cancel2) {
                if (seat_of(b.owner) != seat) {
                    ms.cancels_enemy_bullet++;
                    if (G.base_ray_dist[seat][b.pos] > 0 && G.base_ray_dir[seat][b.pos] == b.dir) {
                        ms.cancels_base_bullet++;
                    }
                }
                continue;
            }
        }
        if (ms.p2 != ms.p1 && b2 == ms.p1 && ms.p2 == b1) {
            ms.existing_bullet_hits++;
            continue;
        }
        if (b2 == ms.p2 && s.grid[b2] == EMPTY) {
            ms.existing_bullet_hits++;
            continue;
        }
    }

    // 2. Potential shots from armed enemy tanks (lazy trace_shot only when cancel or hit occurs!)
    for (int t_idx = 0; t_idx < 3; ++t_idx) {
        int e = tank_id(op, t_idx);
        if (!s.tank[e].alive || !can_fire(s, e)) continue;
        int epos = s.tank[e].pos;
        for (int ed = 0; ed < 4; ++ed) {
            int eb1 = epos + DIR_DELTA[ed];
            if (fired) {
                bool cancel1 = (ed == (fire_dir ^ 1) && epos == f1) || (eb1 == f1 && (s.grid[f1] == EMPTY || s.grid[f1] == WATER));
                if (cancel1) {
                    RayHit erh = trace_shot(s, op, e, epos, ed);
                    if (erh.kind == 6 || erh.kind == 4 || (erh.kind == 1 && erh.dist <= 2)) {
                        ms.intercepts_enemy_fire++;
                    }
                    continue;
                }
            }
            bool hit = false;
            if (eb1 == ms.p1 && s.grid[eb1] == EMPTY) {
                hit = true;
            } else {
                uint8_t g1 = s.grid[eb1];
                int v1 = tank_at(s, eb1);
                if ((g1 == EMPTY || g1 == WATER) && (v1 < 0 || v1 == i)) {
                    int eb2 = eb1 + DIR_DELTA[ed];
                    bool cancelled2 = false;
                    if (fired && f2 != NO_CELL) {
                        if ((ed == (fire_dir ^ 1) && eb1 == f2) || (eb2 == f2 && (s.grid[f2] == EMPTY || s.grid[f2] == WATER))) {
                            cancelled2 = true;
                            RayHit erh = trace_shot(s, op, e, epos, ed);
                            if (erh.kind == 6 || erh.kind == 4 || (erh.kind == 1 && erh.dist <= 2)) {
                                ms.intercepts_enemy_fire++;
                            }
                        }
                    }
                    if (!cancelled2 && eb2 == ms.p2 && s.grid[eb2] == EMPTY) {
                        hit = true;
                    }
                }
            }
            if (hit) {
                RayHit erh = trace_shot(s, op, e, epos, ed);
                if (erh.kind == 6 || erh.kind == 4 || (erh.kind == 1 && erh.dist <= 2)) {
                    ms.direct_enemy_threat++;
                } else {
                    ms.cross_enemy_threat++;
                }
            }
        }
    }

    return ms;
}

double score_single_cmd(const State& s, int i, Cmd c, bool use_global_maps = true) {
    const Tank& t = s.tank[i];
    if (!t.alive) return 0.0;
    int seat = seat_of(i);
    int op = 1 - seat;
    int pos = t.pos;

    MoveSafety ms = check_cmd_safety(s, i, c);

    if (is_move2(c)) {
        if (ms.p1 == pos && ms.p2 == pos) return -50000.0;
        if (ms.p2 == pos && ms.p1 != pos) return -400.0;
    }

    double score = 0.0;

    // 1. Survival against existing bullets and telefrags
    if (ms.existing_bullet_hits > 0) {
        if (ms.existing_bullet_hits >= t.hp) {
            score -= 6500.0 + 1800.0 * t.level;
        } else {
            score -= 2200.0 * ms.existing_bullet_hits;
        }
    }
    if (ms.direct_enemy_threat > 0) {
        if (ms.direct_enemy_threat >= t.hp) {
            score -= 1350.0 + 500.0 * t.level;
        } else {
            score -= 380.0 * ms.direct_enemy_threat;
        }
    }
    if (ms.cross_enemy_threat > 0) {
        if (t.hp == 1) score -= 180.0 * ms.cross_enemy_threat;
        else score -= 45.0 * ms.cross_enemy_threat;
    }
    if (ms.cancels_enemy_bullet > 0) {
        score += 240.0 * ms.cancels_enemy_bullet;
    }
    if (ms.cancels_base_bullet > 0) {
        score += 1350.0 * ms.cancels_base_bullet;
    }
    if (ms.intercepts_enemy_fire > 0) {
        score += 320.0 * ms.intercepts_enemy_fire;
    }

    if (is_enemy_spawn_soon(s, seat, ms.p2)) {
        score -= 9000.0;
    }
    if (is_ally_spawn_soon(s, seat, i, ms.p2)) {
        score -= 1200.0;
    }

    bool is_stuck = (last_pos_hist[i][0] == pos && last_pos_hist[i][1] == pos && last_pos_hist[i][2] == pos);

    // 2. Firing evaluation
    if (is_fire(c)) {
        if (!can_fire(s, i)) return -1e9;
        int d = c - 5;
        RayHit rh = trace_shot(s, seat, i, pos, d);

        if (rh.kind == 3) {
            return -100000.0;
        }
        if (rh.kind == 5) {
            return -25000.0;
        }
        if (rh.kind == 1) {
            if (G.is_base_shield[seat][rh.cell]) {
                return -25000.0;
            } else if (G.is_base_shield[op][rh.cell] || (G.base_ray_dist[op][pos] > 0 && G.base_ray_dir[op][pos] == d)) {
                if (rh.dist <= 2) score += 620.0;
                else if (rh.dist <= 4) score += 350.0;
                else score += 80.0;
            } else if (rh.dist <= 2) {
                int lane = G.assigned_lane[i];
                if (!use_global_maps || G.is_atk_wall[seat][rh.cell] ||
                    G.dist_lane[i][lane][rh.cell] < G.dist_lane[i][lane][pos] ||
                    G.dist_atk[i][rh.cell] < G.dist_atk[i][pos]) {
                    score += 360.0;
                } else {
                    score += 50.0;
                }
            } else {
                score -= (ms.intercepts_enemy_fire > 0 ? 0.0 : 160.0);
            }
        } else if (rh.kind == 2) {
            if (t.level >= 2 && s.P.level2_steel) {
                if (G.is_base_shield[seat][rh.cell]) {
                    return -25000.0;
                }
                int lane = G.assigned_lane[i];
                if (rh.dist <= 3 && (!use_global_maps || G.is_atk_wall[seat][rh.cell] ||
                    G.dist_lane[i][lane][rh.cell] < G.dist_lane[i][lane][pos] ||
                    G.dist_atk[i][rh.cell] < G.dist_atk[i][pos] || G.base_ray_dist[op][pos] > 0)) {
                    score += 420.0;
                } else {
                    score -= (ms.intercepts_enemy_fire > 0 ? 0.0 : 100.0);
                }
            } else {
                score -= (ms.intercepts_enemy_fire > 0 ? 0.0 : 250.0);
            }
        } else if (rh.kind == 4) {
            int next_c = pos + DIR_DELTA[d];
            bool can_walk_closer = (s.grid[next_c] == EMPTY && tank_at(s, next_c) < 0);
            if (rh.dist <= 2) {
                score += 1300.0;
            } else if (rh.dist <= 4) {
                score += can_walk_closer ? 620.0 : 980.0;
            } else {
                score += can_walk_closer ? 60.0 : 620.0;
            }
        } else if (rh.kind == 6) {
            int enemy_id = rh.target_id;
            const Tank& et = s.tank[enemy_id];
            bool enemy_can_fire = can_fire(s, enemy_id);
            int next_c = pos + DIR_DELTA[d];
            bool can_advance = (s.grid[next_c] == EMPTY && tank_at(s, next_c) < 0);
            int ethreat = enemy_base_threat_level(s, seat, enemy_id);
            double priority_bonus = ethreat * 320.0 + et.level * 240.0 + (type_of(enemy_id) == FIRE ? 120.0 : 0.0);
            if (G.base_ray_dist[op][pos] > 0 && G.base_ray_dir[op][pos] == d) {
                priority_bonus += 450.0;
            }

            if (rh.dist == 1) {
                score += (enemy_can_fire ? 750.0 : 1200.0) + priority_bonus;
                if (et.hp == 1) score += 450.0;
            } else if (rh.dist == 2) {
                score += (enemy_can_fire ? 600.0 : 1000.0) + priority_bonus;
                if (et.hp == 1) score += 350.0;
            } else if (rh.dist == 3) {
                if (is_stuck && can_advance) score -= 150.0;
                else score += (can_advance ? 30.0 : 180.0) + priority_bonus * 0.25;
            } else {
                if (can_advance) score -= 180.0;
                else if (bullet_cap(s, i) > 1) score += 60.0;
                else score -= 100.0;
            }
        } else if (rh.kind == 7) {
            if (rh.dist <= 3) score += 260.0;
            else score -= 60.0;
        } else {
            score -= (ms.intercepts_enemy_fire > 0 ? 0.0 : 240.0);
        }
    }

    // 3. Role-based positional evaluation of `ms.p2`
    if (use_global_maps) {
        int def_e = G.assigned_def_enemy[i];
        int def_urg = G.assigned_def_urgency[i];
        int sup_k = G.assigned_supply[i];
        int lane = G.assigned_lane[i];

        // A. Defense role
        if (def_e >= 0 && s.tank[def_e].alive) {
            int de_now = G.dist_from_tank[def_e][pos];
            int de_next = G.dist_from_tank[def_e][ms.p2];
            double w = (def_urg == 3) ? 125.0 : 75.0;
            if (de_next < INF) {
                score += (de_now - de_next) * w - de_next * 4.0;
            }
        }

        // B. Star collection role
        if (sup_k >= 0 && t.level < s.P.max_level) {
            bool has_star = ((s.star_mask >> sup_k) & 1) != 0;
            int sup_cell = s.supply[sup_k];
            int ds_now = G.dist_supply[sup_k][pos];
            int ds_next = G.dist_supply[sup_k][ms.p2];
            if (has_star && ms.p2 == sup_cell) {
                score += 1050.0;
            } else {
                double w = has_star ? 68.0 : 48.0;
                score += (ds_now - ds_next) * w - ds_next * 4.0;
                if (ms.p2 == sup_cell) score += 180.0;
            }
        }

        // C. Assigned Attack Lane progress
        int dl_now = G.dist_lane[i][lane][pos];
        int dl_next = G.dist_lane[i][lane][ms.p2];
        int da_now = G.dist_atk[i][pos];
        int da_next = G.dist_atk[i][ms.p2];

        double lane_w = (def_e >= 0 && def_urg == 3) ? 0.0 : (def_e >= 0 ? 12.0 : (sup_k >= 0 ? 22.0 : 42.0));
        double atk_w  = (def_e >= 0 && def_urg == 3) ? 0.0 : (def_e >= 0 ?  8.0 : (sup_k >= 0 ? 12.0 : 20.0));

        if (dl_next < INF) {
            score += (dl_now - dl_next) * lane_w - dl_next * (lane_w > 0 ? 1.5 : 0.0);
        }
        if (da_next < INF) {
            score += (da_now - da_next) * atk_w - da_next * (atk_w > 0 ? 1.0 : 0.0);
        }

        if (is_stuck && ms.p2 == pos && !is_fire(c)) {
            score -= 120.0;
        }
        for (int h = 0; h < 4; ++h) {
            if (ms.p2 == last_pos_hist[i][h] && ms.p2 != pos) {
                score -= 18.0;
            }
        }
    }

    return score;
}

Orders fast_rollout_orders(const State& s, int seat) {
    Orders ord;
    int chosen_p1[3] = {-1, -1, -1};
    int chosen_p2[3] = {-1, -1, -1};
    bool base_shot_taken = false;
    int op = 1 - seat;

    for (int type = 0; type < 3; ++type) {
        int i = tank_id(seat, type);
        if (!s.tank[i].alive) {
            ord.cmd[type] = STAY;
            continue;
        }
        Cmd list[N_CMDS];
        int n = legal_commands(s, i, list);
        double best_sc = -1e18;
        Cmd best_c = STAY;

        for (int k = 0; k < n; ++k) {
            Cmd c = list[k];
            double sc = score_single_cmd(s, i, c, true);
            MoveSafety ms = check_cmd_safety(s, i, c);

            for (int prev = 0; prev < type; ++prev) {
                if (ms.p1 == chosen_p1[prev] || ms.p2 == chosen_p2[prev]) {
                    sc -= 600.0;
                }
            }
            if (is_fire(c) && base_shot_taken) {
                int d = c - 5;
                int pos = s.tank[i].pos;
                if (G.base_ray_dist[op][pos] > 0 && G.base_ray_dir[op][pos] == d) {
                    int b_cnt = 0, s_cnt = 0;
                    count_ray_obstacles(s, op, pos, b_cnt, s_cnt);
                    if (b_cnt == 0 && s_cnt == 0) sc -= 950.0;
                }
            }

            if (sc > best_sc) {
                best_sc = sc;
                best_c = c;
            }
        }
        ord.cmd[type] = best_c;
        MoveSafety ms = check_cmd_safety(s, i, best_c);
        chosen_p1[type] = ms.p1;
        chosen_p2[type] = ms.p2;
        if (is_fire(best_c)) {
            int d = best_c - 5;
            int pos = s.tank[i].pos;
            if (G.base_ray_dist[op][pos] > 0 && G.base_ray_dir[op][pos] == d && G.base_ray_dist[op][pos] <= 2) {
                int b_cnt = 0, s_cnt = 0;
                count_ray_obstacles(s, op, pos, b_cnt, s_cnt);
                if (b_cnt == 0 && s_cnt == 0) base_shot_taken = true;
            }
        }
    }
    return ord;
}

double evaluate_state(const State& s, int me) {
    int op = 1 - me;
    if (s.done) {
        if (s.winner == me) {
            return 1000000.0 + (300 - s.turn) * 500.0 + (s.base_hp[me] - s.base_hp[op]) * 2000.0 + (s.kills[me] - s.kills[op]) * 200.0;
        } else if (s.winner == op) {
            return -1000000.0 - (300 - s.turn) * 500.0 + (s.base_hp[me] - s.base_hp[op]) * 2000.0 + (s.kills[me] - s.kills[op]) * 200.0;
        } else {
            return 0.0;
        }
    }

    double val = 0.0;

    // 1. Base HP
    int my_bhp = s.base_hp[me];
    int op_bhp = s.base_hp[op];
    val += (my_bhp - op_bhp) * 3800.0;
    if (my_bhp <= 6) val -= (7 - my_bhp) * 1400.0;
    if (op_bhp <= 6) val += (7 - op_bhp) * 1400.0;

    // 2. Kills and Stars Eaten
    val += (s.kills[me] - s.kills[op]) * 950.0;
    val += (s.stars_eaten[me] - s.stars_eaten[op]) * 850.0;

    // 3. Base shield walls & attack walls destroyed since root
    for (int seat = 0; seat < 2; ++seat) {
        double sgn = (seat == me) ? 1.0 : -1.0;
        int bpos = s.base_pos[seat];
        for (int d = 0; d < 4; ++d) {
            int c1 = bpos + DIR_DELTA[d];
            if (s.grid[c1] == BRICK) val += sgn * 360.0;
        }
    }
    if (s.wall_hash != 0) {
        for (int c = 0; c < CELLS; ++c) {
            if (G.root_grid[c] != EMPTY && s.grid[c] == EMPTY) {
                if (G.is_base_shield[me][c]) val -= 500.0;
                else if (G.is_base_shield[op][c]) val += 420.0;
                else {
                    if (G.is_atk_wall[me][c]) val += 260.0;
                    if (G.is_atk_wall[op][c]) val -= 220.0;
                }
            }
        }
    }

    // 4. Bullets currently flying toward either base
    for (int k = 0; k < s.n_bullets; ++k) {
        const Bullet& b = s.bullet[k];
        for (int seat = 0; seat < 2; ++seat) {
            if (G.base_ray_dist[seat][b.pos] > 0 && G.base_ray_dir[seat][b.pos] == b.dir) {
                int dist = G.base_ray_dist[seat][b.pos];
                int bricks_in_way = 0, steels_in_way = 0;
                count_ray_obstacles(s, seat, b.pos, bricks_in_way, steels_in_way);
                if (steels_in_way == 0 || b.steel) {
                    double sgn = (seat == me) ? -1.0 : 1.0;
                    if (bricks_in_way == 0 && dist <= 6) {
                        val += sgn * (1650.0 - dist * 180.0);
                    } else if (bricks_in_way == 1 && dist <= 4) {
                        val += sgn * 340.0;
                    }
                }
            }
        }
    }

    // 5. Tank alive status, HP, level, reload/ammo readiness, and bullet danger
    int best_supply_dist[2][MAX_SUPPLY];
    for (int k = 0; k < s.n_supply; ++k) {
        best_supply_dist[0][k] = INF;
        best_supply_dist[1][k] = INF;
    }

    int best_atk_dist[2] = {INF, INF};
    int second_atk_dist[2] = {INF, INF};
    int lanes_occupied[2][3] = {};

    for (int i = 0; i < 6; ++i) {
        int seat = seat_of(i);
        int op_seat = 1 - seat;
        double sgn = (seat == me) ? 1.0 : -1.0;
        const Tank& t = s.tank[i];

        if (!t.alive) {
            int wait = respawn_in(s, i);
            int sp_dist = G.dist_atk[i][s.spawn[i]];
            if (sp_dist >= INF / 2) sp_dist = 30;
            val -= sgn * (900.0 + wait * 260.0 + sp_dist * 16.0);
            continue;
        }

        double tank_val = 1150.0 + t.hp * 580.0;
        if (t.level == 1) tank_val += 1350.0;
        else if (t.level >= 2) tank_val += 2700.0;

        if (can_fire(s, i)) {
            tank_val += 110.0;
        } else {
            for (int k = 0; k < s.n_bullets; ++k) {
                if (s.bullet[k].owner == i) {
                    int bp = s.bullet[k].pos;
                    int bd = s.bullet[k].dir;
                    int steps_left = 0;
                    for (int st = 1; st <= 12; ++st) {
                        int nc = bp + st * DIR_DELTA[bd];
                        uint8_t g = s.grid[nc];
                        if (g != EMPTY && g != WATER) break;
                        if (tank_at(s, nc) >= 0) break;
                        steps_left++;
                    }
                    if (steps_left >= 3 && bullet_cap(s, i) == 1) {
                        tank_val -= steps_left * 35.0;
                    }
                }
            }
        }

        int survival_options = 0;
        Cmd list[N_CMDS];
        int nc = legal_commands(s, i, list);
        for (int k = 0; k < nc; ++k) {
            MoveSafety ms = check_cmd_safety(s, i, list[k]);
            if (ms.existing_bullet_hits == 0 && !is_enemy_spawn_soon(s, seat, ms.p2)) {
                survival_options++;
            }
        }
        if (survival_options == 0) {
            if (t.hp <= 1) tank_val -= (1150.0 + t.hp * 580.0 + t.level * 1350.0 + 750.0);
            else tank_val -= 650.0;
        } else if (survival_options <= 2) {
            tank_val -= 120.0;
        }

        int th = enemy_base_threat_level(s, op_seat, i);
        if (th == 3) {
            tank_val += 550.0 + t.hp * 200.0 + (can_fire(s, i) ? 200.0 : 0.0);
        } else if (th == 2) {
            tank_val += 240.0 + t.hp * 100.0;
        }

        val += sgn * tank_val;

        int d_atk = G.dist_atk[i][t.pos];
        if (G.base_ray_dist[op_seat][t.pos] > 0) {
            int rdist = G.base_ray_dist[op_seat][t.pos];
            int rbricks = 0, rsteels = 0;
            count_ray_obstacles(s, op_seat, t.pos, rbricks, rsteels);
            bool can_st = (t.level >= 2 && s.P.level2_steel);
            if (rsteels == 0 || can_st) {
                int cur_walls = rbricks + rsteels;
                int dynamic_d = cur_walls * 4 + (rdist <= 2 ? 0 : (rdist - 2) * 2);
                if (dynamic_d < d_atk) d_atk = dynamic_d;
                int rlane = G.base_ray_lane[op_seat][t.pos];
                if (rlane >= 0 && rdist <= 6 && cur_walls <= 1) {
                    lanes_occupied[seat][rlane]++;
                }
            }
        }
        if (d_atk < best_atk_dist[seat]) {
            second_atk_dist[seat] = best_atk_dist[seat];
            best_atk_dist[seat] = d_atk;
        } else if (d_atk < second_atk_dist[seat]) {
            second_atk_dist[seat] = d_atk;
        }

        int def_e = G.assigned_def_enemy[i];
        int def_urg = G.assigned_def_urgency[i];
        if (def_e >= 0 && def_urg >= 2 && s.tank[def_e].alive) {
            int dd = G.dist_from_tank[def_e][t.pos];
            if (dd < INF / 2) {
                val += sgn * (-dd * (def_urg == 3 ? 45.0 : 25.0));
            }
        } else {
            int lane = G.assigned_lane[i];
            int dl = G.dist_lane[i][lane][t.pos];
            if (dl < INF / 2) {
                val += sgn * (-dl * 14.0);
            } else if (d_atk < INF / 2) {
                val += sgn * (-d_atk * 14.0);
            }
        }

        if (t.level < s.P.max_level) {
            for (int k = 0; k < s.n_supply; ++k) {
                int ds = G.dist_supply[k][t.pos];
                if (ds < best_supply_dist[seat][k]) {
                    best_supply_dist[seat][k] = ds;
                }
            }
        }
    }

    // 6. Base Attack Progress & Multi-Angle Surround Bonus
    for (int seat = 0; seat < 2; ++seat) {
        double sgn = (seat == me) ? 1.0 : -1.0;
        if (best_atk_dist[seat] < INF / 2) {
            val += sgn * (1200.0 - best_atk_dist[seat] * 40.0);
        }
        if (second_atk_dist[seat] < INF / 2) {
            val += sgn * (650.0 - second_atk_dist[seat] * 24.0);
        }
        int active_lanes = (lanes_occupied[seat][0] > 0) + (lanes_occupied[seat][1] > 0) + (lanes_occupied[seat][2] > 0);
        if (active_lanes >= 2) {
            val += sgn * 380.0 * (active_lanes - 1);
        }
    }

    // 7. Star Control evaluation
    int turns_to_spawn = INF;
    if (s.turn < s.P.supply_first) {
        turns_to_spawn = s.P.supply_first - s.turn;
    } else {
        int rem = (s.turn - s.P.supply_first) % s.P.supply_period;
        turns_to_spawn = (rem == 0) ? s.P.supply_period : (s.P.supply_period - rem);
    }

    for (int k = 0; k < s.n_supply; ++k) {
        bool has_star = ((s.star_mask >> k) & 1) != 0;
        if (has_star || turns_to_spawn <= 8) {
            double weight = has_star ? 55.0 : (9 - turns_to_spawn) * 5.5;
            if (best_supply_dist[me][k] < INF) {
                val += std::max(0.0, (13.0 - best_supply_dist[me][k]) * weight);
            }
            if (best_supply_dist[op][k] < INF) {
                val -= std::max(0.0, (13.0 - best_supply_dist[op][k]) * weight);
            }
        }
    }

    // 8. Defensive Interception & Anti-Crowding
    for (int seat = 0; seat < 2; ++seat) {
        double sgn = (seat == me) ? 1.0 : -1.0;
        int enemy = 1 - seat;
        for (int et = 0; et < 3; ++et) {
            int eid = tank_id(enemy, et);
            if (!s.tank[eid].alive) continue;
            int th = enemy_base_threat_level(s, seat, eid);
            if (th >= 2) {
                int min_def_dist = INF;
                for (int mt = 0; mt < 3; ++mt) {
                    int mid = tank_id(seat, mt);
                    if (s.tank[mid].alive) {
                        int d = G.dist_from_tank[eid][s.tank[mid].pos];
                        if (d < min_def_dist) min_def_dist = d;
                    }
                }
                if (min_def_dist < INF / 2) {
                    double urgency = (th == 3) ? 75.0 : 40.0;
                    val -= sgn * min_def_dist * urgency;
                }
            }
        }

        for (int a = 0; a < 3; ++a) {
            int ia = tank_id(seat, a);
            if (!s.tank[ia].alive) continue;
            for (int b = a + 1; b < 3; ++b) {
                int ib = tank_id(seat, b);
                if (!s.tank[ib].alive) continue;
                int md = manhattan(s.tank[ia].pos, s.tank[ib].pos);
                if (md == 1) val -= sgn * 45.0;
                else if (md == 2 && (cell_x(s.tank[ia].pos) == cell_x(s.tank[ib].pos) ||
                                     cell_y(s.tank[ia].pos) == cell_y(s.tank[ib].pos))) {
                    val -= sgn * 25.0;
                }
            }
        }
    }

    return val;
}

struct ScoredCmd {
    Cmd c;
    double sc;
    MoveSafety ms;
};

struct ScoredOrders {
    Orders ord;
    double prior;
};

int generate_candidate_orders(const State& s, int seat, ScoredOrders* out, int max_out) {
    ScoredCmd top_cmds[3][8];
    int n_top[3] = {0, 0, 0};

    for (int type = 0; type < 3; ++type) {
        int i = tank_id(seat, type);
        if (!s.tank[i].alive) {
            top_cmds[type][0] = ScoredCmd{STAY, 0.0, MoveSafety{}};
            n_top[type] = 1;
            continue;
        }
        Cmd list[N_CMDS];
        int n = legal_commands(s, i, list);
        ScoredCmd all[N_CMDS];
        int valid_n = 0;
        for (int k = 0; k < n; ++k) {
            double sc = score_single_cmd(s, i, list[k], true);
            if (sc <= -40000.0) continue;
            all[valid_n].c = list[k];
            all[valid_n].sc = sc;
            all[valid_n].ms = check_cmd_safety(s, i, list[k]);
            valid_n++;
        }
        if (valid_n == 0) {
            top_cmds[type][0] = ScoredCmd{STAY, 0.0, check_cmd_safety(s, i, STAY)};
            n_top[type] = 1;
            continue;
        }
        std::sort(all, all + valid_n, [](const ScoredCmd& a, const ScoredCmd& b) {
            return a.sc > b.sc;
        });

        int keep = std::min(valid_n, (type == FAST ? 5 : 4));
        for (int k = 0; k < keep; ++k) {
            top_cmds[type][k] = all[k];
        }
        n_top[type] = keep;
    }

    ScoredOrders pool[120];
    int n_pool = 0;
    int op = 1 - seat;

    for (int a = 0; a < n_top[0]; ++a) {
        for (int b = 0; b < n_top[1]; ++b) {
            for (int c = 0; c < n_top[2]; ++c) {
                const ScoredCmd* scs[3] = {&top_cmds[0][a], &top_cmds[1][b], &top_cmds[2][c]};
                double total = scs[0]->sc + scs[1]->sc + scs[2]->sc;

                for (int u = 0; u < 3; ++u) {
                    int iu = tank_id(seat, u);
                    if (!s.tank[iu].alive) continue;
                    for (int v = u + 1; v < 3; ++v) {
                        int iv = tank_id(seat, v);
                        if (!s.tank[iv].alive) continue;
                        if (scs[u]->ms.p1 == scs[v]->ms.p1 || scs[u]->ms.p2 == scs[v]->ms.p2) {
                            total -= 750.0;
                        }
                        if (scs[u]->ms.p1 == s.tank[iv].pos && scs[v]->ms.p1 == s.tank[iu].pos) {
                            total -= 750.0;
                        }
                        if (is_fire(scs[u]->c)) {
                            int fd = scs[u]->c - 5;
                            int p = s.tank[iu].pos;
                            if (p + DIR_DELTA[fd] == scs[v]->ms.p1 || p + 2 * DIR_DELTA[fd] == scs[v]->ms.p2) {
                                total -= 5000.0;
                            }
                        }
                        if (is_fire(scs[v]->c)) {
                            int fd = scs[v]->c - 5;
                            int p = s.tank[iv].pos;
                            if (p + DIR_DELTA[fd] == scs[u]->ms.p1 || p + 2 * DIR_DELTA[fd] == scs[u]->ms.p2) {
                                total -= 5000.0;
                            }
                        }
                    }
                }

                int base_shots = 0;
                for (int u = 0; u < 3; ++u) {
                    int iu = tank_id(seat, u);
                    if (!s.tank[iu].alive || !is_fire(scs[u]->c)) continue;
                    int d = scs[u]->c - 5;
                    int pos = s.tank[iu].pos;
                    if (G.base_ray_dist[op][pos] > 0 && G.base_ray_dir[op][pos] == d && G.base_ray_dist[op][pos] <= 2) {
                        int b_cnt = 0, s_cnt = 0;
                        count_ray_obstacles(s, op, pos, b_cnt, s_cnt);
                        if (b_cnt == 0 && s_cnt == 0) base_shots++;
                    }
                }
                if (base_shots >= 2) total -= 850.0 * (base_shots - 1);

                Orders ord;
                ord.cmd[0] = scs[0]->c;
                ord.cmd[1] = scs[1]->c;
                ord.cmd[2] = scs[2]->c;
                pool[n_pool++] = ScoredOrders{ord, total};
            }
        }
    }

    std::sort(pool, pool + n_pool, [](const ScoredOrders& x, const ScoredOrders& y) {
        return x.prior > y.prior;
    });

    int res = std::min(n_pool, max_out);
    for (int k = 0; k < res; ++k) out[k] = pool[k];
    return res;
}

} // namespace

void init(const State& s, int me) {
    (void)me;
    std::memset(last_pos_hist, -1, sizeof(last_pos_hist));
    hist_turn = -1;
    precompute(s);
}

Orders decide(const State& s, int me) {
    int op = 1 - me;
    if (s.turn != hist_turn) {
        for (int i = 0; i < 6; ++i) {
            for (int h = 5; h > 0; --h) last_pos_hist[i][h] = last_pos_hist[i][h - 1];
            last_pos_hist[i][0] = s.tank[i].alive ? s.tank[i].pos : -1;
        }
        hist_turn = s.turn;
    }

    precompute(s);

    constexpr int MAX_MY = 32;
    constexpr int MAX_OP = 22;
    ScoredOrders my_cands[MAX_MY];
    ScoredOrders op_cands[MAX_OP];

    int n_my = generate_candidate_orders(s, me, my_cands, MAX_MY);
    int n_op = generate_candidate_orders(s, op, op_cands, MAX_OP);

    if (n_my <= 0) return Orders{};
    if (n_my == 1) return my_cands[0].ord;

    double op_weights[MAX_OP];
    double max_op_p = op_cands[0].prior;
    double sum_op_w = 0.0;
    for (int j = 0; j < n_op; ++j) {
        op_weights[j] = std::exp(std::max(-10.0, (op_cands[j].prior - max_op_p) / 420.0));
        sum_op_w += op_weights[j];
    }
    for (int j = 0; j < n_op; ++j) op_weights[j] /= sum_op_w;

    static double mat[MAX_MY][MAX_OP];
    static State s1_cache[MAX_MY][MAX_OP];

    for (int i = 0; i < n_my; ++i) {
        for (int j = 0; j < n_op; ++j) {
            State& s1 = s1_cache[i][j];
            s1 = s;
            step_as(s1, me, my_cands[i].ord, op_cands[j].ord);
            mat[i][j] = evaluate_state(s1, me) + my_cands[i].prior * 0.06;
        }
    }

    for (int i = 0; i < n_my; ++i) {
        if (budget_left() < 6.0) break;
        for (int j = 0; j < std::min(n_op, 14); ++j) {
            if (budget_left() < 5.0) break;
            const State& s1 = s1_cache[i][j];
            if (s1.done) continue;
            State s2 = s1;
            Orders r0 = fast_rollout_orders(s2, 0);
            Orders r1 = fast_rollout_orders(s2, 1);
            step(s2, r0, r1);
            double v2 = evaluate_state(s2, me);

            if (!s2.done && j < 8 && budget_left() > 10.0) {
                State s3 = s2;
                Orders r30 = fast_rollout_orders(s3, 0);
                Orders r31 = fast_rollout_orders(s3, 1);
                step(s3, r30, r31);
                double v3 = evaluate_state(s3, me);
                mat[i][j] = 0.40 * mat[i][j] + 0.35 * v2 + 0.25 * v3;
            } else {
                mat[i][j] = 0.50 * mat[i][j] + 0.50 * v2;
            }
        }
    }

    int best_idx = 0;
    double best_total = -1e18;
    int top_op_k = std::min(n_op, 8);

    for (int i = 0; i < n_my; ++i) {
        double worst_top = 1e18;
        double expected = 0.0;

        for (int j = 0; j < n_op; ++j) {
            double v = mat[i][j];
            if (j < top_op_k && v < worst_top) worst_top = v;
            expected += op_weights[j] * v;
        }

        double softmin_num = 0.0;
        double softmin_den = 0.0;
        for (int j = 0; j < n_op; ++j) {
            double v = mat[i][j];
            double w = op_weights[j] * std::exp(std::min(12.0, (worst_top - v) / 600.0));
            softmin_num += w * v;
            softmin_den += w;
        }
        double softmin = (softmin_den > 0.0) ? (softmin_num / softmin_den) : worst_top;

        double combined = 0.45 * expected + 0.35 * softmin + 0.20 * worst_top;
        if (combined > best_total) {
            best_total = combined;
            best_idx = i;
        }
    }

    return my_cands[best_idx].ord;
}

#include "tank.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

using namespace tank;

namespace {

constexpr int INF = 1000000;

struct MapCache {
    // For each tank 0..5, cost in sub-steps to attack the opponent's base from cell c
    int dist_atk[6][CELLS];
    // For each tank 0..5, cost in sub-steps from tank's current pos to cell c
    int dist_from_tank[6][CELLS];
    // For each supply 0..2, cost in steps (brick=3, empty=1) to cell c
    int dist_supply[MAX_SUPPLY][CELLS];
    // Is cell part of seat's base protective ring/wall?
    bool is_base_shield[2][CELLS];
    // Shooting post info for attacking base of seat (0 or 1):
    int base_ray_dist[2][CELLS];
    int base_ray_dir[2][CELLS]; // direction from c toward base[seat]
    int base_ray_bricks[2][CELLS];
    int base_ray_steels[2][CELLS];
};

static MapCache G;
static int last_pos_hist[6][6];
static int last_op_base_hp[2] = {24, 24};
static int no_dmg_turns[2] = {0, 0};
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

void precompute(const State& s) {
    std::memset(&G, 0, sizeof(G));
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
            int bricks = 0, steels = 0;
            int to_base_dir = d ^ 1;
            for (int k = 1;; ++k) {
                int c = bpos + k * DIR_DELTA[d];
                uint8_t g = s.grid[c];
                if (g == OUTSIDE || g == BASE0 || g == BASE1) break;
                if (k <= 3 && (g == BRICK || g == STEEL)) {
                    G.is_base_shield[seat][c] = true;
                }
                G.base_ray_dist[seat][c] = k;
                G.base_ray_dir[seat][c] = to_base_dir;
                G.base_ray_bricks[seat][c] = bricks;
                G.base_ray_steels[seat][c] = steels;
                if (g == BRICK) bricks++;
                else if (g == STEEL) steels++;
            }
        }
    }

    // Supply distances
    for (int k = 0; k < s.n_supply; ++k) {
        for (int c = 0; c < CELLS; ++c) G.dist_supply[k][c] = INF;
        int src = s.supply[k];
        int q[CELLS * 4];
        int head = 0, tail = 0;
        G.dist_supply[k][src] = 0;
        q[tail++] = src;
        while (head < tail) {
            int u = q[head++];
            int du = G.dist_supply[k][u];
            for (int d = 0; d < 4; ++d) {
                int v = u + DIR_DELTA[d];
                uint8_t g = s.grid[v];
                int w = (g == EMPTY) ? 1 : (g == BRICK ? 3 : INF);
                if (w < INF && du + w < G.dist_supply[k][v]) {
                    G.dist_supply[k][v] = du + w;
                    q[tail++] = v;
                }
            }
        }
    }

    // Per-tank Dijkstra for attacking enemy base and distance from tank
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
        }

        // 1. Seed shooting posts for enemy base `op`
        int bpos = s.base_pos[op];
        int q[CELLS * 8];
        int head = 0, tail = 0;

        for (int d = 0; d < 4; ++d) {
            for (int k = 1;; ++k) {
                int c = bpos + k * DIR_DELTA[d];
                uint8_t g = s.grid[c];
                if (g == OUTSIDE || g == BASE0 || g == BASE1) break;
                int steels_b = G.base_ray_steels[op][c];
                int bricks_b = G.base_ray_bricks[op][c];
                if (steels_b > 0 && !can_steel) break;
                if (g == WATER) continue;
                if (g == STEEL && !can_steel) continue;

                int walls_b = bricks_b + steels_b;
                int shot_interval = 2 + 2 * rl + (k > 2 ? (k - 1) : 0);
                int post_cost = walls_b * shot_interval;
                if (k == 1) post_cost += 0;
                else if (k == 2) post_cost += 1;
                else post_cost += 2 + (k - 2) * 3;

                // Penalty if c is on or right in front of an enemy spawn point
                for (int t = 0; t < 3; ++t) {
                    int sp = s.spawn[tank_id(op, t)];
                    if (sp == c) post_cost += 4;
                    else if (c == bpos + DIR_DELTA[d] && sp == bpos + 2 * DIR_DELTA[d]) {
                        post_cost += 2; // Standing at (7,1)/(7,13) right in front of center spawn
                    }
                }
                if (g == BRICK || g == STEEL) {
                    post_cost += 2 + 2 * rl;
                }
                if (post_cost < G.dist_atk[i][c]) {
                    G.dist_atk[i][c] = post_cost;
                    q[tail++] = c;
                }
            }
        }

        while (head < tail) {
            int u = q[head++];
            int du = G.dist_atk[i][u];
            for (int d = 0; d < 4; ++d) {
                int v = u + DIR_DELTA[d];
                uint8_t g = s.grid[v];
                if (g == OUTSIDE || g == WATER || g == BASE0 || g == BASE1) continue;
                if (g == STEEL && !can_steel) continue;
                if (G.is_base_shield[seat][v] && (g == BRICK || g == STEEL)) continue;
                int step_c = (g == EMPTY) ? move_cost : wall_cost;
                if (du + step_c < G.dist_atk[i][v]) {
                    G.dist_atk[i][v] = du + step_c;
                    q[tail++] = v;
                }
            }
        }

        // 2. Distance from tank's current position (or spawn if dead)
        int start_c = s.tank[i].alive ? s.tank[i].pos : s.spawn[i];
        head = tail = 0;
        G.dist_from_tank[i][start_c] = 0;
        q[tail++] = start_c;
        while (head < tail) {
            int u = q[head++];
            int du = G.dist_from_tank[i][u];
            for (int d = 0; d < 4; ++d) {
                int v = u + DIR_DELTA[d];
                uint8_t g = s.grid[v];
                if (g == OUTSIDE || g == WATER || g == BASE0 || g == BASE1) continue;
                if (g == STEEL && !can_steel) continue;
                if (G.is_base_shield[seat][v] && (g == BRICK || g == STEEL)) continue;
                int step_c = (g == EMPTY) ? move_cost : wall_cost;
                if (du + step_c < G.dist_from_tank[i][v]) {
                    G.dist_from_tank[i][v] = du + step_c;
                    q[tail++] = v;
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
    int potential_enemy_hits;
    int cancels_enemy_bullet;
    int cancels_base_bullet;
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
    }

    for (int k = 0; k < s.n_bullets; ++k) {
        const Bullet& b = s.bullet[k];
        int b1 = b.pos + DIR_DELTA[b.dir];
        if (fired && b.dir == (fire_dir ^ 1)) {
            int f1 = pos + DIR_DELTA[fire_dir];
            if (b.pos == f1 || b1 == f1) {
                ms.cancels_enemy_bullet++;
                if (G.base_ray_dist[seat][b.pos] > 0 && G.base_ray_dir[seat][b.pos] == b.dir) {
                    ms.cancels_base_bullet++;
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
        if (fired && b.dir == (fire_dir ^ 1)) {
            int f1 = pos + DIR_DELTA[fire_dir];
            if (s.grid[f1] == EMPTY || s.grid[f1] == WATER) {
                int f2 = f1 + DIR_DELTA[fire_dir];
                if (b1 == f2 || b2 == f2) {
                    ms.cancels_enemy_bullet++;
                    if (G.base_ray_dist[seat][b.pos] > 0 && G.base_ray_dir[seat][b.pos] == b.dir) {
                        ms.cancels_base_bullet++;
                    }
                    continue;
                }
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

    for (int t_idx = 0; t_idx < 3; ++t_idx) {
        int e = tank_id(op, t_idx);
        if (!s.tank[e].alive || !can_fire(s, e)) continue;
        int epos = s.tank[e].pos;
        for (int ed = 0; ed < 4; ++ed) {
            int eb1 = epos + DIR_DELTA[ed];
            if (fired && ed == (fire_dir ^ 1)) {
                int f1 = pos + DIR_DELTA[fire_dir];
                if (epos == f1 || eb1 == f1) continue;
            }
            if (eb1 == ms.p1 && s.grid[eb1] == EMPTY) {
                ms.potential_enemy_hits++;
                continue;
            }
            uint8_t g1 = s.grid[eb1];
            if (g1 != EMPTY && g1 != WATER) continue;
            int v1 = tank_at(s, eb1);
            if (v1 >= 0 && v1 != i) continue;

            int eb2 = eb1 + DIR_DELTA[ed];
            if (fired && ed == (fire_dir ^ 1)) {
                int f1 = pos + DIR_DELTA[fire_dir];
                if (s.grid[f1] == EMPTY || s.grid[f1] == WATER) {
                    int f2 = f1 + DIR_DELTA[fire_dir];
                    if (eb1 == f2 || eb2 == f2) continue;
                }
            }
            if (eb2 == ms.p2 && s.grid[eb2] == EMPTY) {
                ms.potential_enemy_hits++;
                continue;
            }
        }
    }

    return ms;
}

// Is enemy tank `e` currently a serious threat to `seat`'s base?
int enemy_base_threat_level(const State& s, int seat, int e) {
    if (!s.tank[e].alive) return 0;
    int epos = s.tank[e].pos;
    // Direct line of fire to our base with 0 or 1 brick!
    if (G.base_ray_dist[seat][epos] > 0) {
        int steels = G.base_ray_steels[seat][epos];
        int bricks = G.base_ray_bricks[seat][epos];
        bool can_st = (s.tank[e].level >= 2 && s.P.level2_steel);
        if (steels == 0 || can_st) {
            if (bricks == 0 && G.base_ray_dist[seat][epos] <= 5) return 3; // Actively hitting our base!
            if (bricks <= 1 && G.base_ray_dist[seat][epos] <= 4) return 2;
        }
    }
    int eatk = G.dist_atk[e][epos];
    if (eatk <= 4) return 3;
    if (eatk <= 8 || manhattan(epos, s.base_pos[seat]) <= 4) return 2;
    if (eatk <= 12) return 1;
    return 0;
}

double score_single_cmd(const State& s, int i, Cmd c, bool use_global_maps = true) {
    const Tank& t = s.tank[i];
    if (!t.alive) return 0.0;
    int seat = seat_of(i);
    int op = 1 - seat;
    int pos = t.pos;

    MoveSafety ms = check_cmd_safety(s, i, c);
    double score = 0.0;

    // 1. Survival against existing bullets and telefrags
    if (ms.existing_bullet_hits > 0) {
        if (ms.existing_bullet_hits >= t.hp) {
            score -= 5000.0 + 1500.0 * t.level;
        } else {
            score -= 1800.0 * ms.existing_bullet_hits;
        }
    }
    if (ms.potential_enemy_hits > 0) {
        if (ms.potential_enemy_hits >= t.hp) {
            score -= 1450.0 + 500.0 * t.level;
        } else {
            score -= 550.0 * ms.potential_enemy_hits;
        }
    }
    if (ms.cancels_enemy_bullet > 0) {
        score += 180.0 * ms.cancels_enemy_bullet;
    }
    if (ms.cancels_base_bullet > 0) {
        score += 1100.0 * ms.cancels_base_bullet;
    }

    if (is_enemy_spawn_soon(s, seat, ms.p2)) {
        score -= 8000.0;
    }
    if (is_ally_spawn_soon(s, seat, i, ms.p2)) {
        score -= 900.0;
    }

    // Check if tank `i` has been stuck in the same cell for >= 3 turns
    bool is_stuck = (last_pos_hist[i][0] == pos && last_pos_hist[i][1] == pos && last_pos_hist[i][2] == pos);

    // 2. Firing evaluation
    if (is_fire(c)) {
        if (!can_fire(s, i)) return -1e9;
        int d = c - 5;
        RayHit rh = trace_shot(s, seat, i, pos, d);

        if (rh.kind == 3) {
            return -100000.0; // Never shoot own base!
        }
        if (rh.kind == 5) {
            if (rh.dist <= 4) score -= 2500.0;
            else score -= 400.0;
        } else if (rh.kind == 1) {
            if (G.is_base_shield[seat][rh.cell]) {
                score -= 6000.0;
            } else if (G.is_base_shield[op][rh.cell] || (G.base_ray_dist[op][pos] > 0 && G.base_ray_dir[op][pos] == d)) {
                if (rh.dist <= 2) score += 550.0;
                else if (rh.dist <= 4) score += 300.0;
                else score += 60.0;
            } else if (rh.dist <= 2) {
                if (use_global_maps && G.dist_atk[i][rh.cell] < G.dist_atk[i][pos]) {
                    score += 320.0;
                } else {
                    score += 65.0;
                }
            } else {
                score -= 140.0;
            }
        } else if (rh.kind == 2) {
            if (t.level >= 2 && s.P.level2_steel) {
                if (G.is_base_shield[seat][rh.cell]) {
                    score -= 6000.0;
                } else if (rh.dist <= 3 && (!use_global_maps || G.dist_atk[i][rh.cell] < G.dist_atk[i][pos] || G.base_ray_dist[op][pos] > 0)) {
                    score += 360.0;
                } else {
                    score -= 80.0;
                }
            } else {
                score -= 200.0;
            }
        } else if (rh.kind == 4) {
            int next_c = pos + DIR_DELTA[d];
            bool can_walk_closer = (s.grid[next_c] == EMPTY && tank_at(s, next_c) < 0);
            if (rh.dist <= 2) {
                score += 1250.0;
            } else if (rh.dist <= 4) {
                score += can_walk_closer ? 620.0 : 950.0;
            } else {
                score += can_walk_closer ? 80.0 : 600.0;
            }
        } else if (rh.kind == 6) {
            int enemy_id = rh.target_id;
            const Tank& et = s.tank[enemy_id];
            bool enemy_can_fire = can_fire(s, enemy_id);
            int next_c = pos + DIR_DELTA[d];
            bool can_advance = (s.grid[next_c] == EMPTY && tank_at(s, next_c) < 0);

            if (rh.dist == 1) {
                score += enemy_can_fire ? 700.0 : 1150.0;
                if (et.hp == 1) score += 300.0;
            } else if (rh.dist == 2) {
                score += enemy_can_fire ? 550.0 : 950.0;
                if (et.hp == 1) score += 200.0;
            } else if (rh.dist == 3) {
                // At dist 3, bullet takes 2 turns. If we are stuck or chasing a base attacker, advance instead!
                if (is_stuck && can_advance) {
                    score -= 150.0;
                } else {
                    score += can_advance ? 40.0 : 180.0;
                }
            } else {
                // At dist >= 4, shooting an enemy tank locks our gun and rarely hits (especially vs FIRE tank!)
                if (can_advance) score -= 180.0;
                else if (bullet_cap(s, i) > 1) score += 50.0;
                else score -= 80.0;
            }
        } else if (rh.kind == 7) {
            if (rh.dist <= 3) score += 220.0;
            else score -= 60.0;
        } else {
            score -= 220.0;
        }
    }

    // 3. Positional evaluation of resulting cell `ms.p2`
    if (use_global_maps) {
        // Check if this tank should intercept a dangerous enemy attacking our base!
        int target_enemy = -1;
        int best_threat = 0;
        int best_edist = INF;

        // Only assign defense if our base isn't winning a 1-turn base race right now
        bool we_have_lethal_now = (s.base_hp[op] == 1 && G.dist_atk[i][pos] == 0);
        if (!we_have_lethal_now && G.dist_atk[i][pos] > 2) {
            for (int et_idx = 0; et_idx < 3; ++et_idx) {
                int e = tank_id(op, et_idx);
                int th = enemy_base_threat_level(s, seat, e);
                if (th >= 2) {
                    int d_to_e = G.dist_from_tank[e][pos];
                    if (th > best_threat || (th == best_threat && d_to_e < best_edist)) {
                        best_threat = th;
                        best_edist = d_to_e;
                        target_enemy = e;
                    }
                }
            }
        }

        if (target_enemy >= 0 && best_edist <= 18) {
            // Use exact BFS distance G.dist_from_tank[target_enemy] to close in on the attacker!
            int de_now = G.dist_from_tank[target_enemy][pos];
            int de_next = G.dist_from_tank[target_enemy][ms.p2];
            double w = (best_threat == 3) ? 95.0 : 55.0;
            if (de_next < INF) {
                score += (de_now - de_next) * w - de_next * 3.0;
            }
            // If adjacent brick blocks path to target_enemy, reward shooting that brick!
            if (is_fire(c)) {
                int fd = c - 5;
                int adj = pos + DIR_DELTA[fd];
                if (s.grid[adj] == BRICK && !G.is_base_shield[seat][adj] &&
                    G.dist_from_tank[target_enemy][adj] < de_now) {
                    score += 450.0;
                }
            }
        }

        int d_now = G.dist_atk[i][pos];
        int d_next = G.dist_atk[i][ms.p2];
        double atk_weight = (target_enemy >= 0 && best_threat == 3 && best_edist <= 14) ? 15.0 : 45.0;
        if (d_next < INF) {
            score += (d_now - d_next) * atk_weight - d_next * 2.0;
        }

        // Star pickup / approach for tanks with level < 2
        if (t.level < s.P.max_level && !(target_enemy >= 0 && best_threat == 3 && best_edist <= 10)) {
            int next_turn = s.turn + 1;
            int turns_to_spawn = INF;
            if (next_turn <= s.P.supply_first) {
                turns_to_spawn = s.P.supply_first - next_turn;
            } else {
                int rem = (next_turn - s.P.supply_first) % s.P.supply_period;
                turns_to_spawn = (rem == 0) ? 0 : (s.P.supply_period - rem);
            }

            for (int k = 0; k < s.n_supply; ++k) {
                bool has_star = ((s.star_mask >> k) & 1) != 0;
                int sup_cell = s.supply[k];
                int ds_now = G.dist_supply[k][pos];
                int ds_next = G.dist_supply[k][ms.p2];
                if (has_star) {
                    if (ms.p2 == sup_cell) {
                        score += 950.0;
                    } else if (ds_next <= 8) {
                        bool closer_ally = false;
                        for (int a = 0; a < 3; ++a) {
                            int aid = tank_id(seat, a);
                            if (aid != i && s.tank[aid].alive && s.tank[aid].level < s.P.max_level) {
                                if (G.dist_supply[k][s.tank[aid].pos] < ds_now) closer_ally = true;
                            }
                        }
                        if (!closer_ally) {
                            score += (ds_now - ds_next) * 55.0 + (10 - ds_next) * 8.0;
                        }
                    }
                } else if (turns_to_spawn <= 5 && ds_next <= 6) {
                    bool closer_ally = false;
                    for (int a = 0; a < 3; ++a) {
                        int aid = tank_id(seat, a);
                        if (aid != i && s.tank[aid].alive && s.tank[aid].level < s.P.max_level) {
                            if (G.dist_supply[k][s.tank[aid].pos] < ds_now) closer_ally = true;
                        }
                    }
                    if (!closer_ally) {
                        if (turns_to_spawn == 0 && ms.p2 == sup_cell) {
                            score += 220.0;
                        } else {
                            score += (ds_now - ds_next) * 35.0;
                        }
                    }
                }
            }
        }

        // Anti-oscillation & anti-stuck penalty
        if (is_stuck && ms.p2 == pos && !is_fire(c)) {
            score -= 80.0;
        }
        for (int h = 0; h < 4; ++h) {
            if (ms.p2 == last_pos_hist[i][h] && ms.p2 != pos) {
                score -= 14.0;
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
                    sc -= 500.0;
                }
            }
            if (is_fire(c) && base_shot_taken) {
                int d = c - 5;
                if (G.base_ray_dist[op][s.tank[i].pos] > 0 && G.base_ray_dir[op][s.tank[i].pos] == d &&
                    G.base_ray_bricks[op][s.tank[i].pos] == 0 && G.base_ray_steels[op][s.tank[i].pos] == 0) {
                    sc -= 900.0;
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
            if (G.base_ray_dist[op][pos] == 1 || (G.base_ray_dist[op][pos] == 2 && G.base_ray_bricks[op][pos] == 0 && G.base_ray_steels[op][pos] == 0)) {
                if (G.base_ray_dir[op][pos] == d) base_shot_taken = true;
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
    val += (my_bhp - op_bhp) * 4400.0;
    if (my_bhp <= 6) val -= (7 - my_bhp) * 1200.0;
    if (op_bhp <= 6) val += (7 - op_bhp) * 1200.0;

    // 2. Kills and Stars Eaten
    val += (s.kills[me] - s.kills[op]) * 850.0;
    val += (s.stars_eaten[me] - s.stars_eaten[op]) * 750.0;

    // 3. Base protective ring bricks
    for (int seat = 0; seat < 2; ++seat) {
        double sgn = (seat == me) ? 1.0 : -1.0;
        int bpos = s.base_pos[seat];
        for (int d = 0; d < 4; ++d) {
            int c1 = bpos + DIR_DELTA[d];
            if (s.grid[c1] == BRICK) val += sgn * 320.0;
        }
    }

    // 4. Bullets currently flying toward either base
    for (int k = 0; k < s.n_bullets; ++k) {
        const Bullet& b = s.bullet[k];
        for (int seat = 0; seat < 2; ++seat) {
            if (G.base_ray_dist[seat][b.pos] > 0 && G.base_ray_dir[seat][b.pos] == b.dir) {
                int dist = G.base_ray_dist[seat][b.pos];
                int bricks_in_way = G.base_ray_bricks[seat][b.pos];
                int steels_in_way = G.base_ray_steels[seat][b.pos];
                if (steels_in_way == 0 || b.steel) {
                    double sgn = (seat == me) ? -1.0 : 1.0;
                    if (bricks_in_way == 0 && dist <= 6) {
                        val += sgn * (1700.0 - dist * 180.0);
                    } else if (bricks_in_way == 1 && dist <= 4) {
                        val += sgn * 350.0;
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

    for (int i = 0; i < 6; ++i) {
        int seat = seat_of(i);
        double sgn = (seat == me) ? 1.0 : -1.0;
        const Tank& t = s.tank[i];

        if (!t.alive) {
            int wait = respawn_in(s, i);
            val -= sgn * (650.0 + wait * 220.0);
            continue;
        }

        double tank_val = 700.0 + t.hp * 480.0;
        if (t.level == 1) tank_val += 1100.0;
        else if (t.level >= 2) tank_val += 2300.0;

        if (can_fire(s, i)) {
            tank_val += 95.0;
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
                        tank_val -= steps_left * 30.0;
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
            if (t.hp <= 1) tank_val -= (700.0 + t.hp * 480.0 + t.level * 1100.0 + 600.0);
            else tank_val -= 550.0;
        } else if (survival_options <= 2) {
            tank_val -= 110.0;
        }

        val += sgn * tank_val;

        int d_atk = G.dist_atk[i][t.pos];
        int op_seat = 1 - seat;
        if (G.base_ray_dist[op_seat][t.pos] > 0) {
            int rdist = G.base_ray_dist[op_seat][t.pos];
            int rsteels = G.base_ray_steels[op_seat][t.pos];
            bool can_st = (t.level >= 2 && s.P.level2_steel);
            if (rsteels == 0 || can_st) {
                int cur_walls = 0;
                int rdir = G.base_ray_dir[op_seat][t.pos];
                for (int st = 1; st < rdist; ++st) {
                    uint8_t gg = s.grid[t.pos + st * DIR_DELTA[rdir]];
                    if (gg == BRICK || gg == STEEL) cur_walls++;
                }
                int dynamic_d = cur_walls * 4 + (rdist <= 2 ? 0 : (rdist - 2) * 2);
                if (dynamic_d < d_atk) d_atk = dynamic_d;
            }
        }
        if (d_atk < best_atk_dist[seat]) {
            second_atk_dist[seat] = best_atk_dist[seat];
            best_atk_dist[seat] = d_atk;
        } else if (d_atk < second_atk_dist[seat]) {
            second_atk_dist[seat] = d_atk;
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

    // 6. Base Attack Progress comparison
    for (int seat = 0; seat < 2; ++seat) {
        double sgn = (seat == me) ? 1.0 : -1.0;
        if (best_atk_dist[seat] < INF) {
            val += sgn * (1200.0 - best_atk_dist[seat] * 42.0);
        }
        if (second_atk_dist[seat] < INF) {
            val += sgn * (600.0 - second_atk_dist[seat] * 22.0);
        }
        for (int tp = 0; tp < 3; ++tp) {
            int i = tank_id(seat, tp);
            if (s.tank[i].alive) {
                int da = G.dist_atk[i][s.tank[i].pos];
                if (da < INF) val += sgn * (-da * 12.0);
            }
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
        if (has_star || turns_to_spawn <= 6) {
            double weight = has_star ? 45.0 : (7 - turns_to_spawn) * 5.5;
            if (best_supply_dist[me][k] < INF) {
                val += std::max(0.0, (12.0 - best_supply_dist[me][k]) * weight);
            }
            if (best_supply_dist[op][k] < INF) {
                val -= std::max(0.0, (12.0 - best_supply_dist[op][k]) * weight);
            }
        }
    }

    // 8. Defensive Interception using exact path distance G.dist_from_tank
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
                if (min_def_dist < INF) {
                    double urgency = (th == 3) ? 48.0 : 24.0;
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
                if (md == 1) val -= sgn * 25.0;
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
        for (int k = 0; k < n; ++k) {
            all[k].c = list[k];
            all[k].sc = score_single_cmd(s, i, list[k], true);
            all[k].ms = check_cmd_safety(s, i, list[k]);
        }
        std::sort(all, all + n, [](const ScoredCmd& a, const ScoredCmd& b) {
            return a.sc > b.sc;
        });

        int keep = std::min(n, (type == FAST ? 5 : 4));
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
                            total -= 600.0;
                        }
                        if (scs[u]->ms.p1 == s.tank[iv].pos && scs[v]->ms.p1 == s.tank[iu].pos) {
                            total -= 600.0;
                        }
                    }
                }

                int base_shots = 0;
                for (int u = 0; u < 3; ++u) {
                    int iu = tank_id(seat, u);
                    if (!s.tank[iu].alive || !is_fire(scs[u]->c)) continue;
                    int d = scs[u]->c - 5;
                    int pos = s.tank[iu].pos;
                    if (G.base_ray_dist[op][pos] > 0 && G.base_ray_dir[op][pos] == d &&
                        G.base_ray_bricks[op][pos] == 0 && G.base_ray_steels[op][pos] == 0 &&
                        G.base_ray_dist[op][pos] <= 2) {
                        base_shots++;
                    }
                }
                if (base_shots >= 2) total -= 750.0 * (base_shots - 1);

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
    last_op_base_hp[0] = s.base_hp[1];
    last_op_base_hp[1] = s.base_hp[0];
    no_dmg_turns[0] = no_dmg_turns[1] = 0;
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
        if (s.base_hp[op] < last_op_base_hp[me]) no_dmg_turns[me] = 0;
        else no_dmg_turns[me]++;
        last_op_base_hp[me] = s.base_hp[op];
        hist_turn = s.turn;
    }

    precompute(s);

    constexpr int MAX_MY = 28;
    constexpr int MAX_OP = 20;
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
        op_weights[j] = std::exp(std::max(-10.0, (op_cands[j].prior - max_op_p) / 400.0));
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
            mat[i][j] = evaluate_state(s1, me) + my_cands[i].prior * 0.08;
        }
    }

    for (int i = 0; i < n_my; ++i) {
        if (budget_left() < 6.0) break;
        for (int j = 0; j < std::min(n_op, 12); ++j) {
            if (budget_left() < 5.0) break;
            const State& s1 = s1_cache[i][j];
            if (s1.done) continue;
            State s2 = s1;
            Orders r0 = fast_rollout_orders(s2, 0);
            Orders r1 = fast_rollout_orders(s2, 1);
            step(s2, r0, r1);
            double v2 = evaluate_state(s2, me);

            if (!s2.done && j < 6 && budget_left() > 10.0) {
                State s3 = s2;
                Orders r30 = fast_rollout_orders(s3, 0);
                Orders r31 = fast_rollout_orders(s3, 1);
                step(s3, r30, r31);
                double v3 = evaluate_state(s3, me);
                mat[i][j] = 0.45 * mat[i][j] + 0.35 * v2 + 0.20 * v3;
            } else {
                mat[i][j] = 0.55 * mat[i][j] + 0.45 * v2;
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

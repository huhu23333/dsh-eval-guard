// bot.cpp —— v2：一层博弈搜索（我方联合命令 × 对方联合应答，tank::step 精确结算）
// + 启发式评估（基地血量、坦克价值、火力覆盖危险图、炮线攻击成本、吃星）。
#include "tank.hpp"
#include <algorithm>

using namespace tank;

namespace {

constexpr int BRICK_COST = 5;
constexpr int ATK_INF    = 500;

static int ME = 0;

static int16_t dist_me[CELLS];      // 我方到对方基地的 Dijkstra 代价
static int16_t dist_opp[CELLS];     // 敌方到我基地的 Dijkstra 代价
static int16_t atk_me[CELLS];       // 从该格沿炮线打到对方基地的近似回合数
static int16_t atk_opp[CELLS];      // 从该格沿炮线打到我基地的近似回合数
static int16_t star_dist[CELLS];   // 到最近补给点的 BFS 距离（只走空地）
static int16_t hot[CELLS];          // 敌方火力覆盖

static int  last_pos[3];
static int  quiet_turns;
static int  last_ehp;

bool aligned_dir(int a, int b, int& d) {
    int ax = a % STRIDE, ay = a / STRIDE, bx = b % STRIDE, by = b / STRIDE;
    if (ax == bx && ay != by) { d = by > ay ? DOWN : UP; return true; }
    if (ay == by && ax != bx) { d = bx > ax ? RIGHT : LEFT; return true; }
    return false;
}

// from 沿 d 到 target 之间（不含两端）全为空地/水且无坦克
bool line_clear(const State& s, int from, int d, int target) {
    int c = from + DIR_DELTA[d];
    while (c != target) {
        uint8_t g = s.grid[c];
        if (g != EMPTY && g != WATER) return false;
        if (tank_at(s, c) >= 0) return false;
        c += DIR_DELTA[d];
    }
    return true;
}

template <bool USE_HOT>
static void dijkstra_impl(const State& s, int target, int16_t out[CELLS]) {
    static int heap_cost[CELLS * 8]; static int heap_cell[CELLS * 8];
    int hn = 0;
    auto push = [&](int cost, int cell) {
        heap_cost[hn] = cost; heap_cell[hn] = cell;
        int i = hn++;
        while (i > 0) {
            int p = (i - 1) / 2;
            if (heap_cost[p] <= heap_cost[i]) break;
            std::swap(heap_cost[p], heap_cost[i]);
            std::swap(heap_cell[p], heap_cell[i]);
            i = p;
        }
    };
    auto pop = [&]() {
        int top = heap_cell[0];
        heap_cell[0] = heap_cell[--hn]; heap_cost[0] = heap_cost[hn];
        int i = 0;
        for (;;) {
            int l = 2 * i + 1, r = l + 1, m = i;
            if (l < hn && heap_cost[l] < heap_cost[m]) m = l;
            if (r < hn && heap_cost[r] < heap_cost[m]) m = r;
            if (m == i) break;
            std::swap(heap_cost[i], heap_cost[m]); std::swap(heap_cell[i], heap_cell[m]);
            i = m;
        }
        return top;
    };
    for (int c = 0; c < CELLS; ++c) out[c] = ATK_INF;
    out[target] = 0;
    push(0, target);
    while (hn) {
        int cell = pop();
        int base = out[cell];
        for (int k = 0; k < 4; ++k) {
            int n = cell + DIR_DELTA[k];
            uint8_t g = s.grid[n];
            int w = (g == EMPTY) ? 1 : (g == BRICK ? BRICK_COST : -1);
            if (w < 0) continue;
            if (USE_HOT) w += 4 * hot[n];          // 敌方火力覆盖当寻路代价
            if (base + w < out[n]) { out[n] = (int16_t)(base + w); push(out[n], n); }
        }
    }
}
static void dijkstra_to(const State& s, int target, int16_t out[CELLS]) {
    dijkstra_impl<false>(s, target, out);
}
static void dijkstra_to_hot(const State& s, int target, int16_t out[CELLS]) {
    dijkstra_impl<true>(s, target, out);
}

static void atk_map_to(const State& s, int tgt, const int16_t* d, int16_t out[CELLS]) {
    for (int y = 0; y < s.P.size; ++y)
        for (int x = 0; x < s.P.size; ++x) {
            int c = cell_of(x, y);
            uint8_t g = s.grid[c];
            if (g != EMPTY && g != BRICK) { out[c] = (int16_t)ATK_INF; continue; }
            int best = ATK_INF;
            for (int k = 0; k < 4; ++k) {
                int bricks = 0, steps = 0, cc = c;
                bool ok = false;
                for (;;) {
                    cc += DIR_DELTA[k]; ++steps;
                    uint8_t gg = s.grid[cc];
                    if (gg == OUTSIDE) break;
                    if (gg == WATER || gg == EMPTY) continue;
                    if (gg == BRICK) { ++bricks; continue; }
                    if (cc == tgt) ok = true;
                    break;
                }
                if (ok) {
                    int cost = 2 * bricks + (steps + 1) / 2;
                    if (cost < best) best = cost;
                }
            }
            if (best >= ATK_INF) best = 40 + (d[c] < ATK_INF ? d[c] : 400);
            out[c] = (int16_t)best;
        }
}

void compute_maps(const State& s) {
    const int opp = 1 - ME;

    // ---- hot：敌方火力覆盖（先算，寻路要用） ----
    for (int c = 0; c < CELLS; ++c) hot[c] = 0;
    for (int e = 3 * opp; e < 3 * opp + 3; ++e) {
        const Tank& te = s.tank[e];
        if (!te.alive) continue;
        int rl = reload_left(s, e);
        if (rl > 1) continue;                       // 一回合内开不了炮的不算
        int w = 1;
        if (can_fire(s, e)) w = 2;                  // 现在就能开炮
        if (s.tank[e].level >= 1 || s.P.types[type_of(e)].reload == 0) w += 1;   // 能连发更危险
        for (int k = 0; k < 4; ++k) {
            int c = te.pos;
            for (;;) {
                c += DIR_DELTA[k];
                uint8_t g = s.grid[c];
                if (g != EMPTY && g != WATER) break;            // 砖/钢/基地挡
                if (tank_at(s, c) >= 0) { hot[c] += w; break; } // 第一辆坦克挡（会被打）
                hot[c] += w;
            }
        }
    }

    // 我方的进攻寻路把敌方火力覆盖当额外代价：绕开架好的炮线
    dijkstra_to_hot(s, s.base_pos[opp], dist_me);
    dijkstra_to(s, s.base_pos[ME], dist_opp);
    atk_map_to(s, s.base_pos[opp], dist_me, atk_me);
    atk_map_to(s, s.base_pos[ME], dist_opp, atk_opp);

    // ---- star_dist ----
    static int q[CELLS];
    int head = 0, tail = 0;
    for (int c = 0; c < CELLS; ++c) star_dist[c] = (int16_t)ATK_INF;
    for (int k = 0; k < s.n_supply; ++k) {
        int sc = s.supply[k];
        if (s.grid[sc] == EMPTY && star_dist[sc] > 0) { star_dist[sc] = 0; q[tail++] = sc; }
    }
    while (head < tail) {
        int c = q[head++];
        for (int k = 0; k < 4; ++k) {
            int n = c + DIR_DELTA[k];
            if (s.grid[n] == EMPTY && star_dist[n] > star_dist[c] + 1) {
                star_dist[n] = (int16_t)(star_dist[c] + 1);
                q[tail++] = n;
            }
        }
    }
}

// ---------------- 评估 ----------------
double tank_value(const State& s, int i) {
    static const int basev[3] = {260, 750, 520};
    const Tank& t = s.tank[i];
    if (!t.alive) return 40;
    return basev[type_of(i)] + 130 * t.hp + 240 * t.level;
}

// pos 处的坦克 m 是否有敌方子弹正朝它飞来（对穿可抵消时 reduc 返回 true）
double bullet_threat(const State& s, int m, bool& cancelable) {
    const Tank& tm = s.tank[m];
    double worst = 0; cancelable = false;
    for (int b = 0; b < s.n_bullets; ++b) {
        const Bullet& bl = s.bullet[b];
        if (seat_of(bl.owner) == seat_of(m) && bl.owner != m) continue;   // 己方子弹也算（友伤），自己的不算
        int d;
        if (!aligned_dir(bl.pos, tm.pos, d) || d != bl.dir) continue;
        if (!line_clear(s, bl.pos, d, tm.pos)) continue;                   // 中间有坦克/墙挡着
        int dist = abs(cell_x(tm.pos) - cell_x(bl.pos)) + abs(cell_y(tm.pos) - cell_y(bl.pos));
        // 下一回合它走 2 格；能否在我逃掉之前命中我（我站着不动就一定中）
        if (dist <= 6) {
            double p = dist <= 2 ? 0.85 : (dist <= 4 ? 0.6 : 0.35);
            if (p > worst) { worst = p; cancelable = can_fire(s, m); }
        }
    }
    return worst;
}

// pos 处的坦克朝目标基地的"持续打击率"：炮线越清、越近，打基地越快。
// 返回 rate ∈ (0,1]（每回合能打掉几血）；打不到返回 0。线上的坦克会挡子弹。
double base_rate(const State& s, int i, int tgt_base) {
    const Tank& t = s.tank[i];
    if (!t.alive) return 0;
    for (int d = 0; d < 4; ++d) {
        int bricks = 0, steps = 0, cc = t.pos;
        bool ok = false;
        for (;;) {
            cc += DIR_DELTA[d]; ++steps;
            uint8_t g = s.grid[cc];
            if (g == OUTSIDE) break;
            if (g == WATER || g == EMPTY) {
                if (tank_at(s, cc) >= 0) break;     // 有坦克挡着
                continue;
            }
            if (g == BRICK) { ++bricks; continue; }
            if (cc == tgt_base) ok = true;
            break;
        }
        if (ok) {
            int delay = 2 * bricks + (steps + 1) / 2;
            return 1.0 / delay;
        }
    }
    return 0;
}

// 守家目标距离：优先追杀离我基地最近的入侵者（那才是正在打我基地的），否则回家守着。
int guard_dist_at(const State& s, int i, int pos) {
    const int16_t* home = (seat_of(i) == ME ? dist_opp : dist_me);
    int best_key = 99, best_dd = 99;
    for (int e = 0; e < 6; ++e) {
        if (seat_of(e) == seat_of(i) || !s.tank[e].alive) continue;
        int hb = home[s.tank[e].pos] >= ATK_INF ? 20 : home[s.tank[e].pos];
        if (hb > 8) continue;                  // 只追真正逼近我基地的
        int dd = abs(cell_x(pos) - cell_x(s.tank[e].pos)) + abs(cell_y(pos) - cell_y(s.tank[e].pos));
        if (hb < best_key || (hb == best_key && dd < best_dd)) { best_key = hb; best_dd = dd; }
    }
    if (best_key < 99) return best_dd;
    return home[pos] >= ATK_INF ? 20 : home[pos];
}
int guard_dist(const State& s, int i) {
    return s.tank[i].alive ? guard_dist_at(s, i, s.tank[i].pos) : 12;
}

double evaluate(const State& s) {
    const int opp = 1 - ME;
    if (s.done) return s.winner == ME ? 1e9 : (s.winner < 0 ? 0.0 : -1e9);
    static const double atkw[3] = {24, 55, 30};
    double sc = 3000.0 * (s.base_hp[ME] - s.base_hp[opp])
              + 550.0 * (s.kills[ME] - s.kills[opp])
              + 380.0 * (s.stars_eaten[ME] - s.stars_eaten[opp]);
    for (int i = 0; i < 6; ++i) {
        int seat = seat_of(i), tp = type_of(i);
        const Tank& t = s.tank[i];
        double v = tank_value(s, i);
        sc += (seat == ME ? v : -0.92 * v);
        double sign = (seat == ME ? -1.0 : +1.15);
        if (tp == ARMOR) {
            // 装甲型守家：追杀入侵者 / 守在基地旁
            int g = t.alive ? guard_dist(s, i) : 12;
            if (g > 14) g = 14;
            sc += sign * 62.0 * g;
        } else {
            const int16_t* am = (seat == ME ? atk_me : atk_opp);
            const int16_t* dm = (seat == ME ? dist_me : dist_opp);
            int a = t.alive ? (am[t.pos] >= ATK_INF ? 200 : am[t.pos]) : 21;
            if (a > 22) a = 22;
            int dist = t.alive ? (dm[t.pos] >= ATK_INF ? 30 : dm[t.pos]) : 26;
            if (dist > 28) dist = 28;
            sc += sign * (atkw[tp] * a + 7.0 * dist);
        }
        if (t.alive) {
            // 已进入基地炮位的持续伤害率（最能分出优劣的一项）
            double rate = base_rate(s, i, seat == ME ? s.base_pos[opp] : s.base_pos[ME]);
            sc += (seat == ME ? +2800.0 : -4000.0) * rate;
            if (tp == FAST && t.level < 2) {
                int sd = star_dist[t.pos] >= ATK_INF ? 60 : star_dist[t.pos];
                sc += (seat == ME ? -42.0 : +14.0) * sd;
            } else if (seat == ME && t.level < 2 && star_dist[t.pos] < 4) {
                sc -= 22.0 * star_dist[t.pos];
            }
        }
    }
    // 有敌车正在打我基地（rate ≥ 1/2）时：全员向它收拢（杀死它是最大的一笔分）
    for (int e = 3 * opp; e < 3 * opp + 3; ++e) {
        const Tank& te = s.tank[e];
        if (!te.alive) continue;
        double er = base_rate(s, e, s.base_pos[ME]);
        if (er >= 0.5) {
            for (int m = 3 * ME; m < 3 * ME + 3; ++m) {
                const Tank& tm = s.tank[m];
                if (!tm.alive) continue;
                int dd = abs(cell_x(tm.pos) - cell_x(te.pos)) + abs(cell_y(tm.pos) - cell_y(te.pos));
                sc -= 10.0 * (dd > 14 ? 14 : dd);
            }
        }
    }
    // 拥堵惩罚：我方两辆车贴在一起（会互相挡路、挡炮线）
    for (int m1 = 3 * ME; m1 < 3 * ME + 3; ++m1)
        for (int m2 = m1 + 1; m2 < 3 * ME + 3; ++m2) {
            if (!s.tank[m1].alive || !s.tank[m2].alive) continue;
            int dd = abs(cell_x(s.tank[m1].pos) - cell_x(s.tank[m2].pos)) + abs(cell_y(s.tank[m1].pos) - cell_y(s.tank[m2].pos));
            if (dd == 1) sc -= 45;
        }
    // 威胁：对方下一回合能沿清空炮线打到我（危险图 + 动态对齐都算）
    for (int m = 3 * ME; m < 3 * ME + 3; ++m) {
        const Tank& tm = s.tank[m];
        if (!tm.alive) continue;
        double vm = tank_value(s, m);
        double danger = 0;
        for (int e = 3 * opp; e < 3 * opp + 3; ++e) {
            const Tank& te = s.tank[e];
            if (!te.alive || !can_fire(s, e)) continue;
            int d;
            if (aligned_dir(te.pos, tm.pos, d) && line_clear(s, te.pos, d, tm.pos)) {
                int back = d ^ 1;   // 对向（U<->D, L<->R）
                bool mutual = can_fire(s, m) && line_clear(s, tm.pos, back, te.pos);
                danger += mutual ? 0.16 : 0.42;   // 对射抵消的算轻一点
            }
        }
        if (tm.pos >= 0 && tm.pos < CELLS) danger += 0.13 * hot[tm.pos];
        // 在飞的子弹
        bool canc;
        danger += bullet_threat(s, m, canc);
        if (danger > 0.75) danger = 0.75;
        sc -= danger * vm;
    }
    for (int e = 3 * opp; e < 3 * opp + 3; ++e) {
        const Tank& te = s.tank[e];
        if (!te.alive) continue;
        double ve = tank_value(s, e);
        double danger = 0;
        for (int m = 3 * ME; m < 3 * ME + 3; ++m) {
            const Tank& tm = s.tank[m];
            if (!tm.alive || !can_fire(s, m)) continue;
            int d;
            if (aligned_dir(tm.pos, te.pos, d) && line_clear(s, tm.pos, d, te.pos)) danger += 0.3;
        }
        bool canc;
        danger += 0.7 * bullet_threat(s, e, canc);
        if (danger > 0.75) danger = 0.75;
        sc += danger * ve * 0.85;
    }
    return sc;
}

// ---------------- 候选生成 ----------------
struct Cand { Cmd c; double pre; };

bool base_line(const State& s, int pos, int d, int base, int& bricks) {
    bricks = 0;
    int c = pos;
    for (;;) {
        c += DIR_DELTA[d];
        uint8_t g = s.grid[c];
        if (g == OUTSIDE) return false;
        if (g == WATER || g == EMPTY) continue;
        if (g == BRICK) { ++bricks; continue; }
        if (c == base) return true;
        return false;
    }
}

// 当前 pos 是否在某发子弹接下来的路径上（1..2 小步内）
bool in_bullet_path(const State& s, int pos) {
    for (int b = 0; b < s.n_bullets; ++b) {
        const Bullet& bl = s.bullet[b];
        int c = bl.pos;
        for (int k = 1; k <= 2; ++k) {
            c += DIR_DELTA[bl.dir];
            if (c == pos) return true;
        }
    }
    return false;
}

void gen_cand(const State& s, int i, int me, Cand out[26], int& n, bool enemy, int keep) {
    Cmd list[N_CMDS];
    int ncmd = legal_commands(s, i, list);
    const Tank& t = s.tank[i];
    const int tgt_base = enemy ? s.base_pos[me] : s.base_pos[1 - me];
    const int16_t* amap = enemy ? atk_opp : atk_me;
    double a1 = amap[t.pos] >= ATK_INF ? 200 : amap[t.pos];
    double h1 = t.alive ? hot[t.pos] : 0;
    bool in_path = t.alive && in_bullet_path(s, t.pos);
    n = 0;
    for (int k = 0; k < ncmd; ++k) {
        Cmd c = list[k];
        double sc = 0;
        if (is_fire(c)) {
            int d = c - 5;
            int bricks;
            if (base_line(s, t.pos, d, tgt_base, bricks)) sc += 240 - 26 * bricks;
            for (int b = 0; b < s.n_bullets; ++b) {
                const Bullet& bl = s.bullet[b];
                // 来弹正对着我飞（对向 = d^1），且在 1..6 格内：开炮可抵消
                int ad;
                if (bl.dir == (d ^ 1) && seat_of(bl.owner) != seat_of(i) &&
                    aligned_dir(bl.pos, t.pos, ad) && ad == d) {
                    int dist = abs(cell_x(bl.pos) - cell_x(t.pos)) + abs(cell_y(bl.pos) - cell_y(t.pos));
                    if (dist >= 1 && dist <= 6) sc += 150;
                }
            }
            for (int e = 0; e < 6; ++e) {
                if (e == i || !s.tank[e].alive) continue;
                int d2;
                if (aligned_dir(t.pos, s.tank[e].pos, d2) && d2 == d &&
                    line_clear(s, t.pos, d, s.tank[e].pos)) {
                    bool friendly = seat_of(e) == seat_of(i);
                    double bonus = friendly ? -180 : 110;
                    // 正在打我基地的敌车（ camper）：必须打
                    if (!friendly && base_rate(s, e, s.base_pos[seat_of(i)]) >= 0.5) bonus = 500;
                    sc += bonus;
                }
            }
            int cc = t.pos;
            for (;;) {
                cc += DIR_DELTA[d];
                uint8_t g = s.grid[cc];
                if (g == OUTSIDE) break;
                if (g == WATER || g == EMPTY) continue;
                if (g == BRICK || g == STEEL) break;
                if (cc == s.base_pos[seat_of(i)]) sc -= 260;
                break;
            }
        } else if (c != STAY) {
            int dest;
            if (is_move2(c)) {
                int d1 = (c - 9) >> 2, d2 = (c - 9) & 3;
                int p1 = t.pos + DIR_DELTA[d1];
                dest = (s.grid[p1 + DIR_DELTA[d2]] == EMPTY) ? p1 + DIR_DELTA[d2] : p1;
            } else dest = t.pos + DIR_DELTA[c - 1];
            if (dest != t.pos && s.grid[dest] == EMPTY) {
                if (type_of(i) == ARMOR) {
                    int g1 = guard_dist_at(s, i, t.pos), g2 = guard_dist_at(s, i, dest);
                    sc += 58 * (g1 - g2);          // 守家梯度
                } else {
                    double a2 = amap[dest] >= ATK_INF ? 200 : amap[dest];
                    double w = enemy ? 30 : (type_of(i) == FIRE ? 55 : 24);
                    sc += w * (a1 - a2);
                }
                // 全员：靠近正在打我基地的敌车
                for (int e = 0; e < 6; ++e) {
                    if (seat_of(e) == seat_of(i) || !s.tank[e].alive) continue;
                    if (base_rate(s, e, s.base_pos[seat_of(i)]) >= 0.5) {
                        int dd1 = abs(cell_x(t.pos) - cell_x(s.tank[e].pos)) + abs(cell_y(t.pos) - cell_y(s.tank[e].pos));
                        int dd2 = abs(cell_x(dest) - cell_x(s.tank[e].pos)) + abs(cell_y(dest) - cell_y(s.tank[e].pos));
                        sc += 20 * (dd1 - dd2);
                    }
                }
                sc -= 45 * hot[dest] * (enemy ? 0.3 : 1.0);
                sc += 30 * (h1 - hot[dest]);
                if (star_at(s, dest) && s.tank[i].level < 2) sc += 190;
                if (type_of(i) == FAST && s.tank[i].level < 2) {
                    int sd1 = star_dist[t.pos] >= ATK_INF ? 60 : star_dist[t.pos];
                    int sd2 = star_dist[dest] >= ATK_INF ? 60 : star_dist[dest];
                    sc += 26 * (sd1 - sd2);
                }
                if (in_path && !in_bullet_path(s, dest)) sc += 220;   // 逃出弹道
                for (int b = 0; b < s.n_bullets; ++b) {
                    const Bullet& bl = s.bullet[b];
                    int c2 = bl.pos;
                    for (int kk = 1; kk <= 2; ++kk) {
                        c2 += DIR_DELTA[bl.dir];
                        if (c2 == dest) sc -= 130;
                    }
                }
            } else if (dest == t.pos) {
                // 原地等一小步的两步命令
            }
        } else {
            if (in_path) sc -= 200;   // 站在弹道里别不动
        }
        out[n].c = c; out[n].pre = sc; ++n;
    }
    std::sort(out, out + n, [](const Cand& x, const Cand& y) {
        if (x.pre != y.pre) return x.pre > y.pre;
        return x.c < y.c;
    });
    if (n > keep) n = keep;
}

} // namespace

void init(const tank::State& s, int me) {
    ME = me;
    for (int k = 0; k < 3; ++k) last_pos[k] = -1;
    quiet_turns = 0;
    last_ehp = s.base_hp[1 - me];
}

Orders decide(const tank::State& s, int me) {
    ME = me;
    compute_maps(s);
    quiet_turns = s.base_hp[1 - me] < last_ehp ? 0 : quiet_turns + 1;
    last_ehp = s.base_hp[1 - me];

    Orders best;
    const int opp = 1 - me;

    Cand mc[3][26]; int mn[3];
    for (int tp = 0; tp < 3; ++tp) {
        int i = tank_id(me, tp);
        gen_cand(s, i, me, mc[tp], mn[tp], false, 6);
        if (mn[tp] < 1) { mc[tp][0].c = STAY; mn[tp] = 1; }
    }
    Cand ec[3][26]; int en[3];
    for (int tp = 0; tp < 3; ++tp) {
        int i = tank_id(opp, tp);
        gen_cand(s, i, me, ec[tp], en[tp], true, 4);
        if (en[tp] < 1) { ec[tp][0].c = STAY; en[tp] = 1; }
    }

    double best_score = -1e18;
    bool have = false;
    for (int a = 0; a < mn[0]; ++a)
    for (int b = 0; b < mn[1]; ++b)
    for (int c = 0; c < mn[2]; ++c) {
        Orders mine;
        mine.cmd[0] = mc[0][a].c; mine.cmd[1] = mc[1][b].c; mine.cmd[2] = mc[2][c].c;
        double mn_min = 1e18, mn_sum = 0; int cnt = 0;
        for (int p = 0; p < en[0]; ++p)
        for (int q = 0; q < en[1]; ++q)
        for (int r = 0; r < en[2]; ++r) {
            Orders theirs;
            theirs.cmd[0] = ec[0][p].c; theirs.cmd[1] = ec[1][q].c; theirs.cmd[2] = ec[2][r].c;
            State sim = s;
            step_as(sim, me, mine, theirs);
            double v = evaluate(sim);
            if (v < mn_min) mn_min = v;
            mn_sum += v; ++cnt;
        }
        double sc = 0.65 * mn_min + 0.35 * (mn_sum / cnt);
        if (quiet_turns >= 14)
            for (int tp = 0; tp < 3; ++tp)
                if (mine.cmd[tp] != STAY) sc += 0.6;
        if (!have || sc > best_score + 1e-9) { best_score = sc; best = mine; have = true; }
    }

    for (int tp = 0; tp < 3; ++tp) {
        int i = tank_id(me, tp);
        last_pos[tp] = s.tank[i].alive ? s.tank[i].pos : -1;
    }
    return best;
}

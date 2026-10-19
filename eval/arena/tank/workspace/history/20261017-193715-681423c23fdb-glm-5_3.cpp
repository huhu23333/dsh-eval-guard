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
static int16_t guard_bfs[CELLS];   // 从我方守家装甲出发的真实寻路距离（砖算 5）
static int invader_cell = -1;      // 本回合选定的入侵者格子（守家追杀目标）

double base_rate(const State& s, int i, int tgt_base);   // 前置声明

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
            if (USE_HOT) w += hot[n];              // 敌方火力覆盖当寻路代价（轻量，避免绕远）
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

    // 守家的追杀目标：离我基地最近（寻路距离）或正在打我基地的敌车；从它做真实寻路图
    invader_cell = -1;
    {
        int best_key = 99, best_cell = -1;
        for (int e = 3 * opp; e < 3 * opp + 3; ++e) {
            if (!s.tank[e].alive) continue;
            int hb = dist_opp[s.tank[e].pos] >= ATK_INF ? 20 : dist_opp[s.tank[e].pos];
            bool pounding = base_rate(s, e, s.base_pos[ME]) >= 0.3;
            if (hb > 8 && !pounding) continue;
            if (hb < best_key) { best_key = hb; best_cell = s.tank[e].pos; }
        }
        invader_cell = best_cell;
        if (invader_cell >= 0) dijkstra_to(s, invader_cell, guard_bfs);
        else for (int c = 0; c < CELLS; ++c) guard_bfs[c] = ATK_INF;
    }

    // ---- star_dist：从"没被敌方火力覆盖"的补给点出发 ----
    static int q[CELLS];
    int head = 0, tail = 0;
    for (int c = 0; c < CELLS; ++c) star_dist[c] = (int16_t)ATK_INF;
    int usable = 0;
    for (int k = 0; k < s.n_supply; ++k) {
        int sc = s.supply[k];
        if (s.grid[sc] == EMPTY && hot[sc] < 3) { star_dist[sc] = 0; q[tail++] = sc; ++usable; }
    }
    if (usable == 0)   // 全被占就退回全部补给点
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
        // 子弹每回合 2 格；近了才真致命，远的走开就行
        if (dist <= 6) {
            double p = dist <= 2 ? 0.7 : (dist <= 4 ? 0.25 : 0.08);
            if (p > worst) { worst = p; cancelable = can_fire(s, m); }
        }
    }
    return worst;
}

// pos 处的坦克朝目标基地的"持续打击率"：炮线越清、越近，打基地越快。
// 返回 rate ∈ (0,1]（每回合能打掉几血）；打不到返回 0。线上的坦克会挡子弹。
// occ：格子 -> 坦克编号（-1 无），传 nullptr 就现场算（慢，只在低频路径用）。
double base_rate_occ(const State& s, int i, int tgt_base, const int* occ) {
    static int local_occ[CELLS];
    if (!occ) {
        for (int c = 0; c < CELLS; ++c) local_occ[c] = -1;
        for (int k = 0; k < 6; ++k) if (s.tank[k].alive) local_occ[s.tank[k].pos] = k;
        occ = local_occ;
    }
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
                if (occ[cc] >= 0) break;     // 有坦克挡着
                continue;
            }
            if (g == BRICK) { ++bricks; continue; }
            if (cc == tgt_base) ok = true;
            break;
        }
        if (ok) {
            int delay = 2 * bricks + (steps + 1) / 2;
            double rate = 1.0 / delay;
            // 炮位被敌方直线火力盯着（能被抵消/击毁）就打折：另选侧翼炮位
            int threat = 0;
            for (int k = 0; k < 4; ++k) {
                int cc = t.pos;
                for (;;) {
                    cc += DIR_DELTA[k];
                    uint8_t gg = s.grid[cc];
                    if (gg != EMPTY && gg != WATER) break;
                    int ti = occ[cc];
                    if (ti >= 0) {
                        if (seat_of(ti) != seat_of(i) && can_fire(s, ti)) ++threat;
                        break;
                    }
                }
            }
            if (threat > 0) rate *= (threat >= 2 ? 0.25 : 0.45);
            return rate;
        }
    }
    return 0;
}
double base_rate(const State& s, int i, int tgt_base) {
    return base_rate_occ(s, i, tgt_base, nullptr);
}

// 守家目标距离：优先追杀离我基地最近的入侵者（那才是正在打我基地的），否则回家守着。
int guard_dist_at(const State& s, int i, int pos) {
    const int16_t* home = (seat_of(i) == ME ? dist_opp : dist_me);
    const int my_base = s.base_pos[seat_of(i)];
    int best_key = 99, best_dd = 99;
    for (int e = 0; e < 6; ++e) {
        if (seat_of(e) == seat_of(i) || !s.tank[e].alive) continue;
        int hb = home[s.tank[e].pos] >= ATK_INF ? 20 : home[s.tank[e].pos];
        // 寻路距离会被围墙砖骗（绕路远≠威胁小）：正在打我基地的一律算入侵者
        bool pounding = base_rate(s, e, my_base) >= 0.3;
        if (hb > 8 && !pounding) continue;     // 只追真正逼近我基地的
        // 我方守家用真实寻路距离（曼哈顿会被水/砖骗，绕不过去）
        int dd;
        if (seat_of(i) == ME && invader_cell >= 0 && guard_bfs[pos] < ATK_INF)
            dd = guard_bfs[pos];
        else
            dd = abs(cell_x(pos) - cell_x(s.tank[e].pos)) + abs(cell_y(pos) - cell_y(s.tank[e].pos));
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
    // ---- 一次性缓存：占格表、各车打击率、两边的守家目标 ----
    static int occ[CELLS];
    static double brate[6];
    for (int c = 0; c < CELLS; ++c) occ[c] = -1;
    for (int i = 0; i < 6; ++i) if (s.tank[i].alive) occ[s.tank[i].pos] = i;
    for (int i = 0; i < 6; ++i)
        brate[i] = base_rate_occ(s, i, s.base_pos[1 - seat_of(i)], occ);
    // 守家目标：离自家基地最近（寻路距离）或正在打自家基地的敌车
    int guard_cell[2] = {-1, -1};   // [seat]：该 seat 的守家坦克该追的格子
    {
        const int16_t* home2[2] = {dist_opp, dist_me};   // [seat] 距自家基地
        for (int seat = 0; seat < 2; ++seat) {
            int best_key = 99, best_cell = -1;
            for (int e = 3 * (1 - seat); e < 3 * (1 - seat) + 3; ++e) {
                if (!s.tank[e].alive) continue;
                int hb = home2[seat][s.tank[e].pos] >= ATK_INF ? 20 : home2[seat][s.tank[e].pos];
                if (hb > 8 && brate[e] < 0.3) continue;
                if (hb < best_key) { best_key = hb; best_cell = s.tank[e].pos; }
            }
            guard_cell[seat] = best_cell;
        }
    }
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
            const int16_t* home = (seat == ME ? dist_opp : dist_me);
            int g;
            if (!t.alive) g = 12;
            else if (guard_cell[seat] >= 0)
                g = abs(cell_x(t.pos) - cell_x(guard_cell[seat])) + abs(cell_y(t.pos) - cell_y(guard_cell[seat]));
            else g = home[t.pos] >= ATK_INF ? 14 : home[t.pos];
            if (g > 14) g = 14;
            sc += sign * 62.0 * g;
        } else {
            const int16_t* am = (seat == ME ? atk_me : atk_opp);
            const int16_t* dm = (seat == ME ? dist_me : dist_opp);
            int a = t.alive ? (am[t.pos] >= ATK_INF ? 200 : am[t.pos]) : 21;
            if (a > 22) a = 22;
            // 距离项不封顶：保持"向对方基地推进"的梯度；死了按最差算
            int dist = t.alive ? (dm[t.pos] >= ATK_INF ? 40 : dm[t.pos]) : 40;
            if (dist > 40) dist = 40;
            sc += sign * (atkw[tp] * a + 7.0 * dist);
        }
        if (t.alive) {
            // 已进入基地炮位的持续伤害率（最能分出优劣的一项）
            sc += (seat == ME ? +2800.0 : -4000.0) * brate[i];
            if (tp == FAST && t.level < 2) {
                int sd = star_dist[t.pos] >= ATK_INF ? 60 : star_dist[t.pos];
                sc += (seat == ME ? -52.0 : +16.0) * sd;
            } else if (seat == ME && t.level < 2 && star_dist[t.pos] < 4) {
                sc -= 22.0 * star_dist[t.pos];
            }
        }
    }
    // 有敌车正在打我基地（rate ≥ 1/2）时：全员向它收拢（杀死它是最大的一笔分）
    for (int e = 3 * opp; e < 3 * opp + 3; ++e) {
        const Tank& te = s.tank[e];
        if (!te.alive || brate[e] < 0.5) continue;
        for (int m = 3 * ME; m < 3 * ME + 3; ++m) {
            const Tank& tm = s.tank[m];
            if (!tm.alive) continue;
            int dd = abs(cell_x(tm.pos) - cell_x(te.pos)) + abs(cell_y(tm.pos) - cell_y(te.pos));
            sc -= 22.0 * (dd > 14 ? 14 : dd);
        }
    }
    // 拥堵惩罚：我方两辆车贴在一起（会互相挡路、挡炮线）
    for (int m1 = 3 * ME; m1 < 3 * ME + 3; ++m1)
        for (int m2 = m1 + 1; m2 < 3 * ME + 3; ++m2) {
            if (!s.tank[m1].alive || !s.tank[m2].alive) continue;
            int dd = abs(cell_x(s.tank[m1].pos) - cell_x(s.tank[m2].pos)) + abs(cell_y(s.tank[m1].pos) - cell_y(s.tank[m2].pos));
            if (dd == 1) sc -= 160;
        }
    // 站在敌方出生点上：对方复活时会被直接炸毁（telefrag），常驻打基地必须避开这些格
    for (int m = 3 * ME; m < 3 * ME + 3; ++m) {
        const Tank& tm = s.tank[m];
        if (!tm.alive) continue;
        for (int e = 3 * opp; e < 3 * opp + 3; ++e)
            if (tm.pos == s.spawn[e])
                sc -= (s.tank[e].alive ? 350.0 : 900.0);
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
                // 子弹每回合只飞 2 格：远处的炮线只要保持移动就能躲，贴脸才致命
                int D = abs(cell_x(tm.pos) - cell_x(te.pos)) + abs(cell_y(tm.pos) - cell_y(te.pos));
                double p = (D <= 1) ? 0.45 : (D == 2 ? 0.2 : 0.06);
                int back = d ^ 1;   // 对向（U<->D, L<->R）
                bool mutual = can_fire(s, m) && line_clear(s, tm.pos, back, te.pos);
                danger += mutual ? p * 0.45 : p;
            }
        }
        if (tm.pos >= 0 && tm.pos < CELLS) danger += 0.05 * hot[tm.pos];
        // 在飞的子弹
        bool canc;
        danger += bullet_threat(s, m, canc);
        if (danger > 0.8) danger = 0.8;
        // 守家装甲多怕 60%：活着就能拖住对方攻基地
        sc -= danger * vm * (type_of(m) == ARMOR ? 1.6 : 1.0);
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
            if (aligned_dir(tm.pos, te.pos, d) && line_clear(s, tm.pos, d, te.pos)) {
                int D = abs(cell_x(te.pos) - cell_x(tm.pos)) + abs(cell_y(te.pos) - cell_y(tm.pos));
                danger += (D <= 1) ? 0.4 : (D == 2 ? 0.18 : 0.06);
            }
        }
        bool canc;
        danger += 0.6 * bullet_threat(s, e, canc);
        if (danger > 0.8) danger = 0.8;
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
                sc -= 12 * hot[dest] * (enemy ? 0.3 : 1.0);
                sc += 10 * (h1 - hot[dest]);
                if (star_at(s, dest) && s.tank[i].level < 2) sc += 190;
                if (type_of(i) == FAST && s.tank[i].level < 2) {
                    int sd1 = star_dist[t.pos] >= ATK_INF ? 60 : star_dist[t.pos];
                    int sd2 = star_dist[dest] >= ATK_INF ? 60 : star_dist[dest];
                    sc += 32 * (sd1 - sd2);
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

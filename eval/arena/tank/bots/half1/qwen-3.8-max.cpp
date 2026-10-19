// bot.cpp —— 坦克大战
//
// 胜负只看基地血量（每回合最多扣 1 血），本质是"攻城竞速"：
//   谁先把一辆车送到对方基地的炮位上、并且每回合打一发，谁就赢。
// 评估里有三股力：攻城（approach / siege）、拦截（intercept：打掉或挡住正在攻我方
// 基地的车）、争星。用 tank::step 精确前推，在裁剪过的联合命令表上做 max-min 搜索
// （同时行动博弈，假设对手最优应答）。
#include "tank.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <queue>
#include <utility>

using namespace tank;

// 对比实验时改成正数（固定节点数，结果可复现）；提交保持 0（按预算停）。
#define FIXED_NODES 0

// ---------------------------------------------------------------- 权重
static const double W_BASE     = 300.0;  // 基地每点血（胜负与第一顺位比较项）
static const double W_KILL     = 34.0;   // 击毁差（第二顺位）
static const double W_STAR     = 38.0;   // 吃星差（第三顺位）
static const double W_ARMY     = 24.0;   // 每点坦克血
static const double W_LEVEL    = 30.0;   // 每级
static const double W_SIEGE    = 340.0;  // 下回合就能打到对方基地（距离<=2 且炮线通）
static const double W_SIEGE2   = 100.0;  // 距离 3..4 炮线通
static const double W_APPROACH = 11.0;   // 每"回合"到攻城位的距离（负）
static const double W_THREAT   = 22.0;   // 每发即将命中的子弹
static const double W_EXPOSED  = 0.0;    // 站在对方能开炮的车的炮线上（实测：会让车变得畏缩，先关掉）
static const double W_STARPOS  = 8.0;    // 每回合到补给点的距离（负）
static const double W_DANGER0  = 70.0;   // 对方有一辆车正指着我方基地
static const double W_DANGER1  = 15.0;   // 每回合的拦截距离（负）
static const double W_JAM      = 18.0;   // 两辆己方车贴在一起（互相挡路）

static const int INF = 9999;

// ---------------------------------------------------------------- 场
static int g_dshot[2][CELLS];         // [seat] 从这格到"能打到对方基地的位置"的步数代价
static int g_dint[2][CELLS];          // [seat] 从这格到"需要拦截的点"的步数
static int g_dsup[CELLS];             // 到最近补给点的步数
static unsigned char g_threat[CELLS]; // bit0: 小步1后子弹到这格  bit1: 小步2后
static unsigned char g_occ[CELLS];    // 0 空 1 = 0 号方的车 2 = 1 号方的车
static unsigned char g_exp[2][CELLS]; // [seat] 这格正被对方能开炮的车瞄着（炮线通、距离<=5）
static bool g_has_dint[2];

static long g_nodes = 0;
static bool g_abort = false;

static inline void tick() {
    if ((++g_nodes & 15) == 0) {
#if FIXED_NODES
        if (g_nodes > FIXED_NODES) g_abort = true;
#else
        if (tank::budget_left() < 3.0) g_abort = true;
#endif
    }
}

static inline int spd(const State& s, int i) { return s.P.types[type_of(i)].speed; }
// 步数换成"要几个回合走到"（快速型一回合两步）。
static inline double turns(const State& s, int i, int steps) {
    if (steps >= INF) steps = 60;
    if (steps > 60) steps = 60;
    return double(steps) / spd(s, i);
}

// ---------------------------------------------------------------- 炮线
// 连坦克一起看：路上有坦克就挡住。返回到 base 的最短距离。
static int shot_now(const State& s, int pos, int base) {
    int best = INF;
    for (int d = 0; d < 4; ++d) {
        int c = pos, n = 0;
        for (;;) {
            c += DIR_DELTA[d];
            if (++n > 6) break;
            if (c == base) { if (n < best) best = n; break; }
            const uint8_t g = s.grid[c];
            if (g != EMPTY && g != WATER) break;
            if (tank_at(s, c) >= 0) break;
        }
    }
    return best;
}

// 从 pos 打到 base 的"攻城代价"：最多允许一堵砖，越近越便宜；打不到返回 INF。
static int ray_cost(const State& s, int pos, int base) {
    int best = INF;
    for (int d = 0; d < 4; ++d) {
        int c = pos, n = 0, nb = 0;
        for (;;) {
            c += DIR_DELTA[d];
            if (++n > 5) break;
            if (c == base) {
                const int cost = 3 * nb + 2 * (n > 2 ? n - 2 : 0);
                if (cost < best) best = cost;
                break;
            }
            const uint8_t g = s.grid[c];
            if (g == EMPTY || g == WATER) continue;
            if (g == BRICK) { if (++nb > 1) break; continue; }
            break;
        }
    }
    return best;
}

// ---------------------------------------------------------------- 场计算
static void dijkstra_attack(const State& s, int seat) {
    int* dist = g_dshot[seat];
    for (int c = 0; c < CELLS; ++c) dist[c] = INF;
    const int base = s.base_pos[1 - seat];
    const unsigned char ofoe = (unsigned char)(1 - seat + 1), ome = (unsigned char)(seat + 1);
    const int n = s.P.size;
    std::priority_queue<std::pair<int, int>, std::vector<std::pair<int, int>>, std::greater<>> pq;
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            const int c = cell_of(x, y);
            if (s.grid[c] != EMPTY) continue;
            int cost = ray_cost(s, c, base);
            if (cost >= INF) continue;
            if (g_occ[c] == ofoe) cost += 10;        // 敌人占着炮位
            else if (g_occ[c] == ome) cost += 4;
            if (cost < dist[c]) { dist[c] = cost; pq.push({cost, c}); }
        }
    while (!pq.empty()) {
        const std::pair<int, int> top = pq.top(); pq.pop();
        const int d = top.first, c = top.second;
        if (d != dist[c]) continue;
        for (int k = 0; k < 4; ++k) {
            const int nb = c + DIR_DELTA[k];
            const uint8_t g = s.grid[nb];
            int w;
            if (g == EMPTY) w = 1;
            else if (g == BRICK) w = 3;
            else continue;
            if (g_occ[nb] == ofoe) w += 7;           // 绕开挡路的敌车
            else if (g_occ[nb] == ome) w += 3;       // 别挤在一起
            if (dist[nb] > d + w) { dist[nb] = d + w; pq.push(std::make_pair(dist[nb], nb)); }
        }
    }
}

// seat 需要拦截的点：正在指 seat 基地的敌车，以及它炮线上可以挡的格子。
static void bfs_intercept(const State& s, int seat) {
    static int q[CELLS];
    static unsigned char src[CELLS];
    for (int c = 0; c < CELLS; ++c) { g_dint[seat][c] = INF; src[c] = 0; }
    const int mybase = s.base_pos[seat];
    const int foe = 1 - seat;
    int head = 0, tail = 0;
    int ndanger = 0;
    for (int i = 0; i < 6; ++i) {
        if (seat_of(i) != foe || !s.tank[i].alive) continue;
        const int d = shot_now(s, s.tank[i].pos, mybase);
        if (d > 5) continue;
        ++ndanger;
        const int pc = s.tank[i].pos;
        if (!src[pc]) { src[pc] = 1; g_dint[seat][pc] = 0; q[tail++] = pc; }
        for (int k = 0; k < 4; ++k) {                 // 炮线上、我和它之间的格子：站上去就能挡
            int c = pc, n = 0;
            for (;;) {
                c += DIR_DELTA[k];
                if (++n >= d) break;
                if (s.grid[c] != EMPTY) break;
                if (!src[c]) { src[c] = 1; g_dint[seat][c] = 0; q[tail++] = c; }
            }
        }
    }
    g_has_dint[seat] = ndanger > 0;
    if (!ndanger) return;
    while (head < tail) {
        const int c = q[head++];
        for (int k = 0; k < 4; ++k) {
            const int nb = c + DIR_DELTA[k];
            const uint8_t g = s.grid[nb];
            if (g != EMPTY && g != BRICK) continue;
            if (g_dint[seat][nb] > g_dint[seat][c] + 1) { g_dint[seat][nb] = g_dint[seat][c] + 1; q[tail++] = nb; }
        }
    }
}

static void bfs_supply(const State& s) {
    static int q[CELLS];
    for (int c = 0; c < CELLS; ++c) g_dsup[c] = INF;
    int head = 0, tail = 0;
    for (int k = 0; k < s.n_supply; ++k) {
        const int c = s.supply[k];
        if (g_dsup[c] == INF) { g_dsup[c] = 0; q[tail++] = c; }
    }
    while (head < tail) {
        const int c = q[head++];
        for (int d = 0; d < 4; ++d) {
            const int nb = c + DIR_DELTA[d];
            const uint8_t g = s.grid[nb];
            if (g != EMPTY && g != BRICK) continue;
            if (g_dsup[nb] > g_dsup[c] + 1) { g_dsup[nb] = g_dsup[c] + 1; q[tail++] = nb; }
        }
    }
}

static void compute_fields(const State& s) {
    for (int c = 0; c < CELLS; ++c) { g_threat[c] = 0; g_occ[c] = 0; g_exp[0][c] = 0; g_exp[1][c] = 0; }
    for (int i = 0; i < 6; ++i)
        if (s.tank[i].alive) g_occ[s.tank[i].pos] = (unsigned char)(seat_of(i) + 1);
    for (int k = 0; k < s.n_bullets; ++k) {
        const Bullet& b = s.bullet[k];
        int c = b.pos;
        for (int st = 0; st < 2; ++st) {
            c += DIR_DELTA[b.dir];
            if (c < 0 || c >= CELLS) break;
            g_threat[c] |= (unsigned char)(1 << st);
            const uint8_t g = s.grid[c];
            if (g != EMPTY && g != WATER) break;
            if (tank_at(s, c) >= 0) break;
        }
    }
    // 对方下回合能开炮的车，它的四条炮线上前 5 格都是危险格。
    // 面对面互射会永远抵消（谁也不亏谁也不赚），站在这种格子上等于把节奏让给对方。
    for (int i = 0; i < 6; ++i) {
        if (!s.tank[i].alive || !can_fire(s, i)) continue;
        const int victim = 1 - seat_of(i);
        for (int d = 0; d < 4; ++d) {
            int c = s.tank[i].pos;
            for (int n = 0; n < 5; ++n) {
                c += DIR_DELTA[d];
                if (c < 0 || c >= CELLS) break;
                const uint8_t g = s.grid[c];
                if (g == BRICK || g == STEEL || g == OUTSIDE || g == BASE0 || g == BASE1) break;
                g_exp[victim][c] = 1;
                if (tank_at(s, c) >= 0) break;
            }
        }
    }
    dijkstra_attack(s, 0);
    dijkstra_attack(s, 1);
    bfs_intercept(s, 0);
    bfs_intercept(s, 1);
    bfs_supply(s);
}

// ---------------------------------------------------------------- 评估
static int incoming_hits(const State& s, int seat) {
    int hits = 0;
    for (int k = 0; k < s.n_bullets; ++k) {
        const Bullet& b = s.bullet[k];
        int c = b.pos;
        for (int st = 0; st < 2; ++st) {
            c += DIR_DELTA[b.dir];
            if (c < 0 || c >= CELLS) break;
            const int v = tank_at(s, c);
            if (v >= 0) { if (seat_of(v) == seat) ++hits; break; }
            const uint8_t g = s.grid[c];
            if (g != EMPTY && g != WATER) break;
        }
    }
    return hits;
}

static double side_eval(const State& s, int seat) {
    const int foe = 1 - seat;
    double v = 0;
    int pos[6], np = 0;
    for (int i = 0; i < 6; ++i) {
        if (seat_of(i) != seat) continue;
        const Tank& t = s.tank[i];
        if (!t.alive) continue;
        pos[np++] = t.pos;
        v += W_ARMY * t.hp + W_LEVEL * t.level;
        // 站在对方炮线上：1 血的车尤其危险；正在攻城的車可以顶着（siege 的奖励更大）。
        if (g_exp[seat][t.pos]) v -= W_EXPOSED * (t.hp <= 1 ? 1.0 : 0.55);
    }
    for (int a = 0; a < np; ++a)                      // 挤在一起会互相挡路
        for (int b = a + 1; b < np; ++b) {
            const int dx = cell_x(pos[a]) - cell_x(pos[b]);
            const int dy = cell_y(pos[a]) - cell_y(pos[b]);
            const int m = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
            if (m <= 1) v -= W_JAM;
        }

    // 攻城：有车能直接打到对方基地就给大额；否则所有车都按"还要几个回合到炮位"扣分。
    double b1 = -1e9, b2 = -1e9;
    bool any = false, can_shoot = false;
    double sum_app = 0;
    for (int i = 0; i < 6; ++i) {
        if (seat_of(i) != seat || !s.tank[i].alive) continue;
        double a;
        const int d = shot_now(s, s.tank[i].pos, s.base_pos[foe]);
        if (d <= 2) { a = W_SIEGE * (can_fire(s, i) ? 1.0 : 0.5); can_shoot = true; }
        else if (d <= 4) { a = W_SIEGE2; can_shoot = true; }
        else { a = -W_APPROACH * turns(s, i, g_dshot[seat][s.tank[i].pos]); sum_app += a; }
        any = true;
        if (a > b1) { b2 = b1; b1 = a; } else if (a > b2) b2 = a;
    }
    if (any) v += can_shoot ? (b1 + 0.3 * (b2 > -1e8 ? b2 : 0.0)) : sum_app;

    // 拦截：对方有车指着我方基地时，按最近的车还要几个回合赶到扣分。
    if (g_has_dint[seat]) {
        int nd = 0;
        for (int i = 0; i < 6; ++i)
            if (seat_of(i) == foe && s.tank[i].alive && shot_now(s, s.tank[i].pos, s.base_pos[seat]) <= 5) ++nd;
        double best = 1e9;
        for (int i = 0; i < 6; ++i) {
            if (seat_of(i) != seat || !s.tank[i].alive) continue;
            const double tt = turns(s, i, g_dint[seat][s.tank[i].pos]);
            if (tt < best) best = tt;
        }
        if (best > 1e8) best = 40;
        if (best > 24) best = 24;
        v -= W_DANGER0 * nd + W_DANGER1 * best;
    }
    return v;
}

// 星星是这个游戏的胜负手：升到 2 级 = 当前血 +4、不再装填（火力型子弹上限 2）、能破钢。
// 一辆 2 级快速型（5 血、一回合两步）能把对面三辆 1 血的车当猎物打。
// 而且没被吃掉的星星会一直留在场上，所以"场上有星就去吃"任何时候都成立。
static double star_eval(const State& s, int seat) {
    const int nt = s.turn + 1;
    const bool supply_soon = nt >= s.P.supply_first && (nt - s.P.supply_first) % s.P.supply_period == 0;
    const bool any_star = s.star_mask != 0;
    if (!any_star && !supply_soon) return 0;
    const double w = any_star ? 1.0 : 0.45;
    double best = 0;
    for (int i = 0; i < 6; ++i) {
        if (seat_of(i) != seat || !s.tank[i].alive) continue;
        if (s.tank[i].level >= s.P.max_level) continue;
        const double v = w * (58.0 - W_STARPOS * turns(s, i, g_dsup[s.tank[i].pos]));
        if (v > best) best = v;
    }
    return best;
}

static double eval(const State& s, int me) {
    if (s.done) {
        if (s.winner == me) return 1e7 + (400 - s.turn);
        if (s.winner < 0) return 0.0;
        return -1e7 - (400 - s.turn);
    }
    const int foe = 1 - me;
    double v = 0;
    v += W_BASE * (s.base_hp[foe] - s.base_hp[me]);
    v += W_KILL * (s.kills[me] - s.kills[foe]);
    v += W_STAR * (s.stars_eaten[me] - s.stars_eaten[foe]);
    v += side_eval(s, me) - side_eval(s, foe);
    v += W_THREAT * (incoming_hits(s, foe) - incoming_hits(s, me));
    v += star_eval(s, me) - star_eval(s, foe);
    return v;
}

// ---------------------------------------------------------------- 命令生成
static int fire_score(const State& s, int i, int d) {
    const int me = seat_of(i), foe = 1 - me;
    int c = s.tank[i].pos;
    int sc = 0, n = 0, bricks = 0;
    for (;;) {
        c += DIR_DELTA[d];
        if (++n > 18) break;
        const int v = tank_at(s, c);
        if (v >= 0) {
            if (seat_of(v) != me) {
                sc += 100 - 5 * n;
                if (s.tank[v].hp <= 1) sc += 45;
            } else {
                sc -= 150;
            }
            break;
        }
        if (c == s.base_pos[foe]) { sc += bricks == 0 ? (440 - 12 * n) : 175; break; }
        if (c == s.base_pos[me]) { sc -= 480; break; }
        const uint8_t g = s.grid[c];
        if (g == BRICK) { if (++bricks > 1) { sc += 6; break; } continue; }
        if (g == STEEL) { sc += (s.tank[i].level >= 2 && bricks == 0) ? 12 : -8; break; }
        if (g == OUTSIDE) { sc -= 4; break; }
    }
    return sc;
}

// 这辆车的"目标距离"：主要朝攻城位走；我方基地正被指时，把拦截点也算进去。
static inline int goal_of(const State& s, int i, int c) {
    const int me = seat_of(i);
    int g = g_dshot[me][c];
    if (g >= INF) g = 90;
    if (g > 60) g = 60;
    if (g_has_dint[me]) {
        int d = g_dint[me][c];
        if (d >= INF) d = 90;
        if (d > 60) d = 60;
        if (d < g) g = d;
    }
    // 场上有星、这辆车又没满级、而且补给点比攻城位近得多时，才把补给点当目标。
    if (s.star_mask && s.tank[i].alive && s.tank[i].level < s.P.max_level) {
        int d = g_dsup[c];
        if (d >= INF) d = 90;
        if (d > 60) d = 60;
        if (d + 4 < g) g = d;
    }
    return g;
}

static int qscore(const State& s, int i, Cmd cmd) {
    const Tank& t = s.tank[i];
    if (cmd == STAY) return g_threat[t.pos] ? -26 : 0;
    if (is_fire(cmd)) return fire_score(s, i, cmd - 5);
    const int d1 = is_move(cmd) ? cmd - 1 : (cmd - 9) >> 2;
    const int p = t.pos;
    const int p1 = p + DIR_DELTA[d1];
    int fin = p1;
    if (is_move2(cmd)) {
        const int d2 = (cmd - 9) & 3;
        const int base = (s.grid[p1] == EMPTY) ? p1 : p;
        const int p2 = base + DIR_DELTA[d2];
        fin = (s.grid[p2] == EMPTY) ? p2 : base;
    }
    if (s.grid[fin] != EMPTY) fin = p;
    int sc = 8 * (goal_of(s, i, p) - goal_of(s, i, fin));
    if (g_threat[fin]) sc -= 34;
    if (g_threat[p]) sc += 22;
    if (star_at(s, fin)) sc += 45;
    return sc;
}

struct Cand {
    Cmd c[3];
    int sc;
    Orders to_orders() const { Orders o; o.cmd[0] = c[0]; o.cmd[1] = c[1]; o.cmd[2] = c[2]; return o; }
};

static const int MAXC = 240;

static int gen_side(const State& s, int seat, Cand* out, int maxout) {
    Cmd list[3][N_CMDS];
    Cmd keep[3][10];
    int ksc[3][10];
    int nk[3];
    for (int t = 0; t < 3; ++t) {
        const int i = tank_id(seat, t);
        const int nl = legal_commands(s, i, list[t]);
        if (nl == 1) { nk[t] = 1; keep[t][0] = STAY; ksc[t][0] = 0; continue; }
        int sc[N_CMDS], idx[N_CMDS];
        for (int k = 0; k < nl; ++k) { sc[k] = qscore(s, i, list[t][k]); idx[k] = k; }
        int K = (t == FAST) ? 7 : 5;
        if (K > nl) K = nl;
        std::nth_element(idx, idx + K, idx + nl, [&](int a, int b) { return sc[a] > sc[b]; });
        std::sort(idx, idx + K, [&](int a, int b) { return sc[a] > sc[b]; });
        bool has_stay = false;
        for (int k = 0; k < K; ++k) if (list[t][idx[k]] == STAY) has_stay = true;
        if (!has_stay && K < 10) { idx[K] = 0; ++K; }   // list[t][0] 就是 STAY
        nk[t] = K;
        for (int k = 0; k < K; ++k) { keep[t][k] = list[t][idx[k]]; ksc[t][k] = sc[idx[k]]; }
    }
    int n = 0;
    for (int a = 0; a < nk[0]; ++a)
        for (int b = 0; b < nk[1]; ++b)
            for (int c = 0; c < nk[2]; ++c) {
                if (n >= MAXC) break;
                out[n].c[0] = keep[0][a]; out[n].c[1] = keep[1][b]; out[n].c[2] = keep[2][c];
                out[n].sc = ksc[0][a] + ksc[1][b] + ksc[2][c];
                ++n;
            }
    std::stable_sort(out, out + n, [](const Cand& x, const Cand& y) { return x.sc > y.sc; });
    return n < maxout ? n : maxout;
}

// ---------------------------------------------------------------- 搜索
static const int WM[4] = {0, 16, 10, 8};
static const int WO[4] = {0, 9, 6, 5};

static double rec(const State& s, int me, int depth) {
    tick();
    if (g_abort) return 0.0;
    if (s.done || depth <= 0) return eval(s, me);
    Cand myc[MAXC], opc[MAXC];
    const int dm = depth > 3 ? 3 : depth;
    const int nm = gen_side(s, me, myc, WM[dm]);
    const int no = gen_side(s, 1 - me, opc, WO[dm]);
    double best = -1e18;
    for (int a = 0; a < nm; ++a) {
        double worst = 1e18;
        const Orders mo = myc[a].to_orders();
        for (int b = 0; b < no; ++b) {
            State t = s;
            step_as(t, me, mo, opc[b].to_orders());
            const double v = rec(t, me, depth - 1);
            if (g_abort) return 0.0;
            if (v < worst) worst = v;
            if (worst <= best) break;
        }
        if (worst > best) best = worst;
    }
    return best;
}

// ---------------------------------------------------------------- 对外接口
void init(const State& s, int me) { (void)s; (void)me; g_nodes = 0; g_abort = false; }

Orders decide(const State& s, int me) {
    g_nodes = 0;
    g_abort = false;
    compute_fields(s);

    static Cand root[MAXC];
    const int nroot = gen_side(s, me, root, MAXC);
    Orders best = nroot > 0 ? root[0].to_orders() : Orders{};
    if (s.done) return best;

    static Cand opc[MAXC];
    for (int depth = 1; depth <= 3; ++depth) {
        g_abort = false;
        double bestv = -1e18;
        Orders local = best;
        bool completed = true;
        const int nm = gen_side(s, me, root, depth == 1 ? 20 : (depth == 2 ? 12 : 9));
        const int no = gen_side(s, 1 - me, opc, depth == 1 ? 10 : (depth == 2 ? 7 : 5));
        for (int a = 0; a < nm; ++a) {
            double worst = 1e18;
            const Orders mo = root[a].to_orders();
            for (int b = 0; b < no; ++b) {
                State t = s;
                step_as(t, me, mo, opc[b].to_orders());
                const double v = (depth == 1) ? eval(t, me) : rec(t, me, depth - 1);
                if (g_abort) { completed = false; break; }
                if (v < worst) worst = v;
                if (worst <= bestv) break;
            }
            if (!completed) break;
            if (worst > bestv) { bestv = worst; local = mo; }
        }
        if (completed && bestv > -1e17) best = local;
        if (g_abort) break;
#if !FIXED_NODES
        if (tank::budget_left() < 12.0) break;
#endif
    }
    return best;
}

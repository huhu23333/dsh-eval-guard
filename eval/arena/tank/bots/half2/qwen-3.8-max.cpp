// bot.cpp —— 坦克大战（策略型，不做搜索）
//
// 上半场的教训：带 max-min 搜索的版本每回合烧 26 CPU 毫秒却排在基准之下，而成绩表里
// 第 5 名每回合只用 0.5 毫秒。这个游戏的胜负手是"攻城竞速 + 星星等级差"，是长期规划
// 问题，2~3 层的战术搜索看不见它，反而让车变得畏缩（任何动作都被假想的全知对手惩罚）。
// 所以这一版完全不做搜索：分工 + 明确的优先级规则，每回合只用几十微秒。
//
// 三条主线：
//   攻城 —— 一辆车去对方基地的炮位（<=2 格且炮线通 = 每回合稳定 1 点伤害），路上先用炮开砖。
//   防守 —— 对方有车瞄着我方基地时，派一辆车去挡炮线 / 打那辆车。
//   争星 —— 场上有星就派一辆确实更近的车去吃；2 级快速型（5 血、不装填、两步）能碾压 1 血车。
#include "tank.hpp"

#include <algorithm>
#include <cstring>

using namespace tank;

static const int INF = 9999;

// ---------------------------------------------------------------- 场
static int g_datt[2][CELLS];          // [seat] 到"能打到对方基地的炮位"的代价（砖算 3 步）
static int g_dsup[CELLS];             // 到最近补给点的步数
static int g_dsp[MAX_SUPPLY][CELLS];  // 到第 k 个补给点的步数（分工时按点分配，避免都挤一个点）
static int g_tgt[6];                  // R_STAR 的车去哪个补给点
static int g_dhunt[2][CELLS];         // [seat] 到最近敌方坦克的步数
static int g_ddef[2][CELLS];          // [seat] 到拦截点（挡炮线 / 打攻城车）的步数
static bool g_need_def[2];
static int g_attacker_dist[2];        // [seat] 对方攻城车到我方基地的距离（没有则 INF）
static unsigned char g_threat[CELLS]; // 本回合会有子弹飞到这格
static unsigned char g_exp[2][CELLS]; // [seat] 这格在对方能开炮的车的炮线上
static unsigned char g_mine[CELLS];   // 这格有我方车
static bool g_has_star;

enum Role { R_SIEGE = 0, R_DEF, R_STAR, R_HUNT };
static int g_role[6];

// ---------------------------------------------------------------- 基础几何
// 连坦克一起看：从 pos 到 base 的炮线是否通，返回距离。
static int shot_now(const State& s, int pos, int base) {
    int best = INF;
    for (int d = 0; d < 4; ++d) {
        int c = pos, n = 0;
        for (;;) {
            c += DIR_DELTA[d];
            if (++n > 8) break;
            if (c == base) { if (n < best) best = n; break; }
            const uint8_t g = s.grid[c];
            if (g != EMPTY && g != WATER) break;
            if (tank_at(s, c) >= 0) break;
        }
    }
    return best;
}

// 只看地形：从 pos 打到 base 的攻城代价（最多允许一堵砖），打不到返回 INF。
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
static void bfs_cost(const State& s, const unsigned char* src, int* dist) {
    static int q[CELLS];
    for (int c = 0; c < CELLS; ++c) dist[c] = INF;
    int head = 0, tail = 0;
    for (int c = 0; c < CELLS; ++c)
        if (src[c]) { dist[c] = 0; q[tail++] = c; }
    while (head < tail) {
        const int c = q[head++];
        for (int k = 0; k < 4; ++k) {
            const int nb = c + DIR_DELTA[k];
            const uint8_t g = s.grid[nb];
            if (g != EMPTY && g != BRICK) continue;
            if (dist[nb] > dist[c] + 1) { dist[nb] = dist[c] + 1; q[tail++] = nb; }
        }
    }
}

static void compute_fields(const State& s, int me) {
    static unsigned char src[CELLS];
    for (int c = 0; c < CELLS; ++c) {
        g_threat[c] = 0; g_exp[0][c] = 0; g_exp[1][c] = 0; g_mine[c] = 0; src[c] = 0;
    }
    for (int i = 0; i < 6; ++i)
        if (s.tank[i].alive && seat_of(i) == me) g_mine[s.tank[i].pos] = 1;

    // 已在飞的子弹：本回合两个小步会飞到哪
    for (int k = 0; k < s.n_bullets; ++k) {
        const Bullet& b = s.bullet[k];
        int c = b.pos;
        for (int st = 0; st < 2; ++st) {
            c += DIR_DELTA[b.dir];
            if (c < 0 || c >= CELLS) break;
            g_threat[c] = 1;
            const uint8_t g = s.grid[c];
            if (g != EMPTY && g != WATER) break;
            if (tank_at(s, c) >= 0) break;
        }
    }
    // 对方能开炮的车，炮线前 5 格是危险格（站上去就是面对面互射，子弹互相抵消、白送节奏）
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

    // 攻城场：炮位 = 从这格沿某方向最多 5 格、途中最多一堵砖就能打到对方基地
    for (int seat = 0; seat < 2; ++seat) {
        const int base = s.base_pos[1 - seat];
        for (int c = 0; c < CELLS; ++c) src[c] = 0;
        const int n = s.P.size;
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x) {
                const int c = cell_of(x, y);
                if (s.grid[c] != EMPTY) continue;
                if (ray_cost(s, c, base) < INF) src[c] = 1;
            }
        bfs_cost(s, src, g_datt[seat]);
    }

    // 补给点场：既算"到最近补给点"，也算"到第 k 个补给点"
    for (int c = 0; c < CELLS; ++c) src[c] = 0;
    for (int k = 0; k < s.n_supply; ++k) src[s.supply[k]] = 1;
    bfs_cost(s, src, g_dsup);
    for (int k = 0; k < s.n_supply; ++k) {
        for (int c = 0; c < CELLS; ++c) src[c] = 0;
        src[s.supply[k]] = 1;
        bfs_cost(s, src, g_dsp[k]);
    }
    g_has_star = s.star_mask != 0;

    // 猎杀场：到最近的敌方坦克
    for (int seat = 0; seat < 2; ++seat) {
        for (int c = 0; c < CELLS; ++c) src[c] = 0;
        for (int i = 0; i < 6; ++i)
            if (s.tank[i].alive && seat_of(i) != seat) src[s.tank[i].pos] = 1;
        bfs_cost(s, src, g_dhunt[seat]);
    }

    // 拦截场：对方有车瞄着 seat 的基地时，把那些车和它们炮线上的格子当目标
    for (int seat = 0; seat < 2; ++seat) {
        for (int c = 0; c < CELLS; ++c) src[c] = 0;
        const int mybase = s.base_pos[seat];
        int nd = 0, bestd = INF;
        for (int i = 0; i < 6; ++i) {
            if (!s.tank[i].alive || seat_of(i) == seat) continue;
            const int d = shot_now(s, s.tank[i].pos, mybase);
            if (d > 5) continue;
            ++nd;
            if (d < bestd) bestd = d;
            src[s.tank[i].pos] = 1;
            for (int k = 0; k < 4; ++k) {   // 炮线上、我和它之间的空格：站上去就挡住这一炮
                int c = s.tank[i].pos, n = 0;
                for (;;) {
                    c += DIR_DELTA[k];
                    if (++n >= d) break;
                    if (s.grid[c] != EMPTY) break;
                    src[c] = 1;
                }
            }
        }
        g_need_def[seat] = nd > 0;
        g_attacker_dist[seat] = bestd;
        if (nd) bfs_cost(s, src, g_ddef[seat]);
        else for (int c = 0; c < CELLS; ++c) g_ddef[seat][c] = INF;
    }
}

// ---------------------------------------------------------------- 打分
// 这一炮打出去会打到什么。
static int fire_score(const State& s, int i, int d) {
    const int me = seat_of(i), foe = 1 - me;
    int c = s.tank[i].pos;
    int n = 0, bricks = 0;
    for (;;) {
        c += DIR_DELTA[d];
        if (++n > 16) break;
        const int v = tank_at(s, c);
        if (v >= 0) {
            if (seat_of(v) != me) {
                int sc = 900 - 40 * n;
                if (s.tank[v].hp <= 1) sc += 500;          // 一发带走
                if (s.tank[v].level >= 1) sc += 200;       // 打掉对面的等级更值
                return sc;
            }
            return -4000;                                  // 打队友
        }
        if (c == s.base_pos[foe]) return bricks == 0 ? (20000 - 300 * n) : 1200;
        if (c == s.base_pos[me]) return -20000;            // 打自己基地
        const uint8_t g = s.grid[c];
        if (g == BRICK) {
            if (++bricks > 1) return 40;
            // 打掉这堵砖之后炮线就通了？继续往前看能不能到对方基地
            int cc = c, nn = n;
            for (;;) {
                cc += DIR_DELTA[d];
                if (++nn > 6) break;
                if (cc == s.base_pos[foe]) return 1700 - 120 * nn;
                if (cc == s.base_pos[me]) return -3000;
                const uint8_t g2 = s.grid[cc];
                if (g2 == EMPTY || g2 == WATER) continue;
                if (tank_at(s, cc) >= 0) break;
                break;
            }
            return 90;
        }
        if (g == STEEL) return (s.tank[i].level >= 2) ? 60 : -60;
        if (g == OUTSIDE) return -30;
        // 空地 / 水：继续飞
    }
    return -20;
}

static inline int goal_dist(const State& s, int i, int c) {
    int g;
    switch (g_role[i]) {
        case R_DEF:   g = g_ddef[seat_of(i)][c]; break;
        case R_STAR:  g = (g_tgt[i] < s.n_supply) ? g_dsp[g_tgt[i]][c] : g_dsup[c]; break;
        case R_HUNT:  g = g_dhunt[seat_of(i)][c]; break;
        default:      g = g_datt[seat_of(i)][c]; break;
    }
    if (g >= INF) g = 120;
    if (g > 120) g = 120;
    return g;
}

static int score_cmd(const State& s, int i, Cmd cmd) {
    const Tank& t = s.tank[i];
    const int me = seat_of(i);

    if (is_fire(cmd)) return fire_score(s, i, cmd - 5);

    if (cmd == STAY) {
        int sc = 0;
        if (g_threat[t.pos]) sc -= 700;
        if (g_exp[me][t.pos]) sc -= 90;
        if (g_has_star && star_at(s, t.pos) && t.level < s.P.max_level) sc += 3000;
        // 已经占住补给点、就等这批星出在脚下：别动
        if (g_role[i] == R_STAR && g_tgt[i] < s.n_supply && t.pos == s.supply[g_tgt[i]]) sc += 1500;
        return sc;
    }

    const int d1 = is_move(cmd) ? cmd - 1 : (cmd - 9) >> 2;
    const int p = t.pos;
    const int p1 = p + DIR_DELTA[d1];
    int mid = p1, fin = p1;
    if (s.grid[p1] != EMPTY) { mid = p; fin = p; }        // 第一步走不成
    if (is_move2(cmd)) {
        const int d2 = (cmd - 9) & 3;
        const int p2 = mid + DIR_DELTA[d2];
        if (s.grid[p2] == EMPTY) fin = p2;
    }

    int sc = 130 * (goal_dist(s, i, p) - goal_dist(s, i, fin));

    if (g_threat[fin]) sc -= 900;                          // 走进子弹的落点（必中，一定要躲）
    if (g_threat[mid]) sc -= 500;
    if (g_threat[p]) sc += 400;                            // 离开子弹的落点
    // 面对面互射 = 子弹互相抵消、谁也不赚。但这只是"不划算"，不能压过推进：
    // 上半场就是这一项给得太大，结果三辆车在原地横着挪、永远到不了对方基地。
    if (g_exp[me][fin]) sc -= 0;
    if (g_exp[me][p]) sc += 0;

    if (g_mine[fin]) sc -= 200;                            // 和队友抢格（会双双走不成）
    if (g_has_star && star_at(s, fin) && t.level < s.P.max_level) sc += 2500;
    if (is_move2(cmd) && fin != mid) sc += 25;             // 快速型一回合两步
    return sc;
}

// ---------------------------------------------------------------- 分工
// 下一批星星还要几个回合末才出（已经出在场上则返回 0）。
static int star_wait(const State& s) {
    if (s.star_mask) return 0;
    const int nt = s.turn + 1;
    if (nt < s.P.supply_first) return s.P.supply_first - nt;
    const int k = (nt - s.P.supply_first) % s.P.supply_period;
    return k == 0 ? 0 : s.P.supply_period - k;
}

static void assign_roles(const State& s, int me) {
    for (int k = 0; k < 6; ++k) { g_role[k] = R_HUNT; g_tgt[k] = 0; }
    int alive[3], na = 0;
    for (int t = 0; t < 3; ++t) {
        const int i = tank_id(me, t);
        if (s.tank[i].alive) alive[na++] = i;
    }
    if (na == 0) return;

    auto tt = [&](int i, const int* field) {
        int d = field[s.tank[i].pos];
        if (d >= INF) d = 200;
        return double(d) / s.P.types[type_of(i)].speed;
    };
    bool used[6] = {};

    // ---- 1) 攻城永远先留一辆 ----
    // 基地每回合最多掉 1 血，两边都在攻城就是平局节奏；只有我攻城、对方不攻才是赢。
    // 所以哪怕基地正挨打，也必须留一辆去啃对方基地，否则会输在基地血量这个第一顺位比较项上。
    {
        int best = -1; double bv = 1e18;
        for (int k = 0; k < na; ++k) { const int i = alive[k]; const double v = tt(i, g_datt[me]); if (v < bv) { bv = v; best = i; } }
        if (best >= 0) { g_role[best] = R_SIEGE; used[best] = true; }
    }

    // ---- 2) 防守：对方有车瞄着我方基地 ----
    if (g_need_def[me]) {
        int best = -1; double bv = 1e18;
        for (int k = 0; k < na; ++k) { const int i = alive[k]; if (used[i]) continue; const double v = tt(i, g_ddef[me]); if (v < bv) { bv = v; best = i; } }
        if (best >= 0) { g_role[best] = R_DEF; used[best] = true; }
    }

    // ---- 3) 争星：只用剩下的一辆 ----
    // 这是本局的胜负手（2 级车能碾压 1 血车），而且必须按"点"分配，否则几辆车挤同一个点。
    // 没出星但一批快到了也要提前占位：出星那回合站在点上，下一回合末就吃到。
    const int wait = star_wait(s);
    if (wait <= 12) {
        int nk = 0, ks[MAX_SUPPLY];
        for (int k = 0; k < s.n_supply; ++k) {
            const bool has = (s.star_mask >> k) & 1;
            if (has || wait <= 12) ks[nk++] = k;
        }
        bool taken[MAX_SUPPLY] = {}; int sent = 0;
        for (int a = 0; a < na; ++a) {
            const int i = alive[a];
            if (used[i] || s.tank[i].level >= s.P.max_level) continue;
            int bk = -1; double bv = 1e18;
            for (int q = 0; q < nk; ++q) {
                const int k = ks[q];
                if (taken[k]) continue;
                const double v = tt(i, g_dsp[k]);
                const double adj = ((s.star_mask >> k) & 1) ? v : v + 1.0;   // 已出星的点更急
                if (adj < bv) { bv = adj; bk = k; }
            }
            if (bk < 0) continue;
            taken[bk] = true;
            g_role[i] = R_STAR; g_tgt[i] = bk; used[i] = true;
            if (++sent >= 2) break;
        }
    }
    // 剩下的：也去攻城（打基地才是胜负），别在中路和对面换命
    for (int k = 0; k < na; ++k) if (!used[alive[k]]) g_role[alive[k]] = R_SIEGE;
}

// ---------------------------------------------------------------- 对外接口
void init(const State& s, int me) { (void)s; (void)me; }

Orders decide(const State& s, int me) {
    Orders orders;
    compute_fields(s, me);
    assign_roles(s, me);

    for (int t = 0; t < 3; ++t) {
        const int i = tank_id(me, t);
        if (!s.tank[i].alive) { orders.cmd[t] = STAY; continue; }
        Cmd list[N_CMDS];
        const int n = legal_commands(s, i, list);
        int bestk = 0, bestsc = score_cmd(s, i, list[0]);
        for (int k = 1; k < n; ++k) {
            const int sc = score_cmd(s, i, list[k]);
            if (sc > bestsc) { bestsc = sc; bestk = k; }
        }
        orders.cmd[t] = list[bestk];
    }
    return orders;
}

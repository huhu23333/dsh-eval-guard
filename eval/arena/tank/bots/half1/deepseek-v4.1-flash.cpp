// bot.cpp —— 坦克大战 bot
//
// 结构：
//   1) 评估函数：基地血量差为主，加上坦克血量/等级、击毁、吃星、以及两个势场——
//      "我方最快那辆车还要几个动作才能打到对方基地"（进攻）和"敌方最靠近我基地的那辆车
//      还要几个动作"（防守），再加追击项（逼近威胁我基地的敌车）和暴露度。
//   2) 搜索：用真实引擎 step 前推。先做几轮 best-response 迭代得到基准计划，
//      再枚举我方每辆车的前 K 条命令组成候选，对每个候选算对手的最佳应对，
//      取"最坏情况最好"的那个（一层 minimax）。
#include "tank.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

using namespace tank;

static const int INF = 1 << 20;

// ==================== 权重 ====================
static double W_BASE = 250.0;   // 每点基地血量差
static double W_DMG = 90;    // 预期未来伤害（基地血量单位）
static double W_HP = 120.0;     // 每点坦克血量
static double W_LVL = 100.0;    // 每级
static double W_DEAD = 200.0;   // 一辆车不在场的固定惩罚
static double W_RESPAWN = 35.0; // 每多等一回合
static double W_KILL = 120.0;   // 每次击毁
static double W_STAR = 180.0;   // 每颗吃到的星
static double W_POT = 14.0;     // 每"半回合"的势能（进攻主车 / 防守）
static double W_POT2 = 5.0;     // 其余车的进攻势能
static double W_CHASE = 60;   // 追击威胁我基地的敌车
static double W_EXP = 10;     // 每辆敌人有清晰炮线指着我
static double W_STARP = 6.0;    // 靠近星星

// ==================== 全局 ====================
static int g_me = 0;
static int g_opp = 1;

// g_potTo[b][c]：一辆坦克在格子 c，要开始对基地 b 造成伤害还需要多少"半回合"
static int g_potTo[2][CELLS];
static uint64_t g_potHash = 0;
static bool g_potValid = false;

// 到最近一颗星星的走位代价
static int g_starDist[CELLS];
static uint64_t g_starHash = 0;
static uint8_t g_starMask = 0;
static bool g_starValid = false;

static bool passable(uint8_t g) { return g == EMPTY || g == BRICK; }

// 从 cell 沿直线看 base：返回中间砖墙数；有钢墙/别的挡路（非基地）则不可用。
static int lineBricksTo(const State& s, int cell, int base) {
    const int x0 = cell_x(cell), y0 = cell_y(cell);
    const int x1 = cell_x(base), y1 = cell_y(base);
    if (x0 != x1 && y0 != y1) return INF;
    if (x0 == x1 && y0 == y1) return INF;
    int step;
    if (y0 == y1) step = (x1 > x0) ? 1 : -1;
    else step = (y1 > y0) ? STRIDE : -STRIDE;
    int bricks = 0;
    for (int c = cell + step; c != base; c += step) {
        const uint8_t g = s.grid[c];
        if (g == BRICK) ++bricks;
        else if (g == STEEL) return INF;
        else if (g != EMPTY && g != WATER) return INF;
    }
    return bricks;
}

static void computePot(const State& s, int base, int* out) {
    for (int c = 0; c < CELLS; ++c) out[c] = INF;
    static int head[1024];
    static int nxt[CELLS];
    for (int i = 0; i < 1024; ++i) head[i] = -1;
    int maxc = 0;
    auto push = [&](int c, int cost) {
        if (cost < 0 || cost >= 1024) return;
        nxt[c] = head[cost];
        head[cost] = c;
        if (cost > maxc) maxc = cost;
    };
    const int xb = cell_x(base), yb = cell_y(base);
    for (int c = 0; c < CELLS; ++c) {
        const int x = cell_x(c), y = cell_y(c);
        if (x < 0 || y < 0 || x >= s.P.size || y >= s.P.size) continue;
        if (x != xb && y != yb) continue;
        if (c == base) continue;
        const uint8_t g = s.grid[c];
        if (!passable(g)) continue;
        const int lb = lineBricksTo(s, c, base);
        if (lb >= INF) continue;
        const int L = std::abs(x - xb) + std::abs(y - yb);
        // 单位是"半回合"：子弹每回合飞 2 格，第一发要 L/2 回合；每多一堵砖墙多打一发。
        int cost = (lb + 1) * L;
        if (g == BRICK) cost += 4;
        if (cost < out[c]) { out[c] = cost; push(c, cost); }
    }
    for (int d = 0; d <= maxc; ++d) {
        for (int c = head[d]; c != -1; c = nxt[c]) {
            if (out[c] != d) continue;
            const int ec = (s.grid[c] == BRICK) ? 4 : 2;
            for (int k = 0; k < 4; ++k) {
                const int n = c + DIR_DELTA[k];
                if (!passable(s.grid[n])) continue;
                if (out[c] + ec < out[n]) { out[n] = out[c] + ec; push(n, out[n]); }
            }
        }
    }
}

static void ensurePot(const State& s) {
    if (g_potValid && g_potHash == s.wall_hash) return;
    computePot(s, s.base_pos[0], g_potTo[0]);
    computePot(s, s.base_pos[1], g_potTo[1]);
    g_potHash = s.wall_hash;
    g_potValid = true;
}

static void ensureStar(const State& s) {
    if (g_starValid && g_starHash == s.wall_hash && g_starMask == s.star_mask) return;
    g_starMask = s.star_mask;
    for (int c = 0; c < CELLS; ++c) g_starDist[c] = INF;
    if (s.star_mask) {
        static int head[1024];
        static int nxt[CELLS];
        for (int i = 0; i < 1024; ++i) head[i] = -1;
        int maxc = 0;
        auto push = [&](int c, int cost) {
            if (cost < 0 || cost >= 1024) return;
            nxt[c] = head[cost];
            head[cost] = c;
            if (cost > maxc) maxc = cost;
        };
        for (int k = 0; k < s.n_supply; ++k) {
            if (!((s.star_mask >> k) & 1)) continue;
            const int c = s.supply[k];
            if (g_starDist[c] > 0) { g_starDist[c] = 0; push(c, 0); }
        }
        for (int d = 0; d <= maxc; ++d) {
            for (int c = head[d]; c != -1; c = nxt[c]) {
                if (g_starDist[c] != d) continue;
                const int ec = (s.grid[c] == BRICK) ? 4 : 2;
                for (int k = 0; k < 4; ++k) {
                    const int n = c + DIR_DELTA[k];
                    if (!passable(s.grid[n])) continue;
                    if (g_starDist[c] + ec < g_starDist[n]) { g_starDist[n] = g_starDist[c] + ec; push(n, g_starDist[n]); }
                }
            }
        }
    }
    g_starHash = s.wall_hash;
    g_starValid = true;
}

static bool clearShot(const State& s, int from, int to) {
    if (from == to) return false;
    const int x0 = cell_x(from), y0 = cell_y(from);
    const int x1 = cell_x(to), y1 = cell_y(to);
    if (x0 != x1 && y0 != y1) return false;
    int step;
    if (y0 == y1) step = (x1 > x0) ? 1 : -1;
    else step = (y1 > y0) ? STRIDE : -STRIDE;
    for (int c = from + step; c != to; c += step) {
        const uint8_t g = s.grid[c];
        if (g == BRICK || g == STEEL || g == BASE0 || g == BASE1 || g == OUTSIDE) return false;
    }
    return true;
}

// ==================== 评估 ====================
static double evalState(const State& s, int seat) {
    if (s.done) return s.winner == seat ? 1e9 : (s.winner < 0 ? 0.0 : -1e9);
    const int opp = 1 - seat;
    ensurePot(s);
    double v = 0.0;
    v += W_BASE * double(s.base_hp[seat] - s.base_hp[opp]);
    for (int i = 0; i < 6; ++i) {
        double m;
        if (s.tank[i].alive) m = W_HP * s.tank[i].hp + W_LVL * s.tank[i].level;
        else m = -(W_DEAD + W_RESPAWN * respawn_in(s, i));
        v += (seat_of(i) == seat) ? m : -m;
    }
    v += W_KILL * double(s.kills[seat] - s.kills[opp]);
    v += W_STAR * double(s.stars_eaten[seat] - s.stars_eaten[opp]);

    const int* potToEnemy = g_potTo[opp];   // 我方坦克用
    const int* potToMine = g_potTo[seat];   // 敌方坦克用

    // 预期还能对基地打出多少伤害：越早到位、射速越高越好。
    // 0 级的快速型/装甲型开炮后要装填，射速只有火力型的一半——所以火力型才是主攻手。
    const double T = double(std::max(0, s.P.max_turns - s.turn));
    double offMe = 0.0, offOpp = 0.0;
    {
        double b1 = 0.0, b2 = 0.0;
        for (int i = 0; i < 6; ++i) {
            if (seat_of(i) != seat || !s.tank[i].alive) continue;
            const double setup = std::min(potToEnemy[s.tank[i].pos], 160) / 2.0;
            const double rate = 1.0 / (1.0 + double(reload_turns(s, i)));
            const double val = std::max(0.0, T - setup) * rate;
            if (val > b1) { b2 = b1; b1 = val; }
            else if (val > b2) b2 = val;
        }
        offMe = b1 + 0.25 * b2;
    }
    {
        double b1 = 0.0, b2 = 0.0;
        for (int i = 0; i < 6; ++i) {
            if (seat_of(i) == seat || !s.tank[i].alive) continue;
            const double setup = std::min(potToMine[s.tank[i].pos], 160) / 2.0;
            const double rate = 1.0 / (1.0 + double(reload_turns(s, i)));
            const double val = std::max(0.0, T - setup) * rate;
            if (val > b1) { b2 = b1; b1 = val; }
            else if (val > b2) b2 = val;
        }
        offOpp = b1 + 0.25 * b2;
    }
    v += W_DMG * (offMe - offOpp);

    // 追击：逼近那些威胁我基地的敌车
    for (int j = 0; j < 6; ++j) {
        if (seat_of(j) == seat || !s.tank[j].alive) continue;
        const int pj = potToMine[s.tank[j].pos];
        if (pj >= 40) continue;
        int dmin = 99;
        for (int i = 0; i < 6; ++i) {
            if (seat_of(i) != seat || !s.tank[i].alive) continue;
            const int d = std::abs(cell_x(s.tank[i].pos) - cell_x(s.tank[j].pos)) +
                          std::abs(cell_y(s.tank[i].pos) - cell_y(s.tank[j].pos));
            dmin = std::min(dmin, d);
        }
        v -= W_CHASE * double(40 - pj) / 40.0 * double(std::min(dmin, 15));
    }
    // 暴露度
    for (int i = 0; i < 6; ++i) {
        if (seat_of(i) != seat || !s.tank[i].alive) continue;
        int cnt = 0;
        for (int j = 0; j < 6; ++j) {
            if (seat_of(j) == seat || !s.tank[j].alive) continue;
            if (clearShot(s, s.tank[j].pos, s.tank[i].pos)) ++cnt;
        }
        v -= W_EXP * double(cnt);
    }
    // 星星
    if (s.star_mask) {
        ensureStar(s);
        for (int i = 0; i < 6; ++i) {
            if (!s.tank[i].alive) continue;
            const int d = std::min(g_starDist[s.tank[i].pos], 40);
            if (seat_of(i) == seat) v -= W_STARP * double(d);
            else v += W_STARP * double(d);
        }
    }
    return v;
}

// ==================== 搜索 ====================
static double planEval(const State& root, int seat, const Orders& mine, const Orders& theirs) {
    State t = root;
    step_as(t, seat, mine, theirs);
    return evalState(t, seat);
}

static Orders bestResponse(const State& root, int seat, const Orders& other, int rounds, Orders cur = Orders{}) {
    for (int r = 0; r < rounds; ++r) {
        bool changed = false;
        for (int ty = 0; ty < 3; ++ty) {
            const int i = tank_id(seat, ty);
            if (!root.tank[i].alive) { cur.cmd[ty] = STAY; continue; }
            Cmd list[N_CMDS];
            const int n = legal_commands(root, i, list);
            const Cmd keep = cur.cmd[ty];
            double bestv = -1e18;
            Cmd bestc = keep;
            for (int k = 0; k < n; ++k) {
                cur.cmd[ty] = list[k];
                const double v = planEval(root, seat, cur, other);
                if (v > bestv) { bestv = v; bestc = list[k]; }
            }
            cur.cmd[ty] = bestc;
            if (bestc != keep) changed = true;
        }
        if (!changed) break;
    }
    return cur;
}

// 每辆车按"其它车不动"的评估排序，取前 K 条
static void topCommands(const State& root, int seat, const Orders& other, Cmd out[3][8], int K, int cnt[3]) {
    for (int ty = 0; ty < 3; ++ty) {
        const int i = tank_id(seat, ty);
        cnt[ty] = 1;
        if (!root.tank[i].alive) { out[ty][0] = STAY; continue; }
        Cmd list[N_CMDS];
        const int n = legal_commands(root, i, list);
        double sc[N_CMDS];
        Orders probe;
        for (int k = 0; k < n; ++k) {
            probe.cmd[ty] = list[k];
            sc[k] = planEval(root, seat, probe, other);
        }
        int idx[N_CMDS];
        for (int k = 0; k < n; ++k) idx[k] = k;
        const int kk = std::min(n, K);
        for (int a = 0; a < kk; ++a) {
            int best = a;
            for (int b = a + 1; b < n; ++b)
                if (sc[idx[b]] > sc[idx[best]]) best = b;
            std::swap(idx[a], idx[best]);
            out[ty][a] = list[idx[a]];
        }
        cnt[ty] = kk;
    }
}

void init(const State& s, int me) {
    g_me = me;
    g_opp = 1 - me;
    g_potValid = false;
    g_starValid = false;
    (void)s;
}

Orders decide(const State& s, int me) {
    g_me = me;
    g_opp = 1 - me;
    g_potValid = false;
    g_starValid = false;
    ensurePot(s);

    const Orders stay;
    // 1) 基准：几轮 best-response 迭代
    Orders opp = bestResponse(s, g_opp, stay, 2);
    Orders mine = bestResponse(s, me, opp, 3);
    for (int it = 0; it < 3; ++it) {
        Orders no = bestResponse(s, g_opp, mine, 2, opp);
        if (no == opp) break;
        opp = no;
        mine = bestResponse(s, me, opp, 3, mine);
    }

    // 2) 一层 minimax：枚举我方候选组合，每个都算对手的最佳应对
    const int K = 4;
    Cmd cand[3][8];
    int cnt[3];
    topCommands(s, me, opp, cand, K, cnt);
    const int n0 = cnt[0], n1 = cnt[1], n2 = cnt[2];

    double bestv = -1e18;
    Orders best = mine;
    int evals = 0;
    bool stop = false;
    for (int a = 0; a < n0 && !stop; ++a)
        for (int b = 0; b < n1 && !stop; ++b)
            for (int c = 0; c < n2; ++c) {
                Orders M;
                M.cmd[0] = cand[0][a];
                M.cmd[1] = cand[1][b];
                M.cmd[2] = cand[2][c];
                Orders R = bestResponse(s, g_opp, M, 1, opp);
                const double v = planEval(s, me, M, R);
                ++evals;
                if (v > bestv) { bestv = v; best = M; }
                if ((evals & 7) == 0 && tank::budget_left() < 6.0) { stop = true; break; }
            }
    return best;
}

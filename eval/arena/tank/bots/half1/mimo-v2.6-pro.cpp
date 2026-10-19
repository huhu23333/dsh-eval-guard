// bot.cpp —— 评估驱动 + 联合搜索（v2）
//   1) 每回合预计算到双方基地 / 补给点的 BFS 距离
//   2) 对手模型：对方按"单坦克贪心 + 评估函数"行动
//   3) 我方：每辆车单坦克模拟排序取 top-K，联合枚举 K^3，1-ply 评估后对 top-M 做 2-ply rollout 重排
//   4) 评估：基地血量为纲（打基地/防基地为第一要务），含子弹落点、射线能力、防守牵引、抢星
// 全部工作量固定 → 完全确定性，便于对比实验。
#include "tank.hpp"
#include <algorithm>

using namespace tank;

namespace {

constexpr int K_TOP = 8;       // 每辆车进入联合枚举的候选数
constexpr int M_ROLLOUT = 288; // 做 rollout 重排的组合数

double dist_base[2][CELLS];    // [seat] 到该座位基地的步数（砖墙当可走）
double dist_supply[CELLS];     // 到最近补给点的步数

// ---------------------------------------------------------------- 距离
void bfs_dist(const State& s, const int16_t* seeds, int nseeds, double* out) {
    static int q[CELLS];
    for (int c = 0; c < CELLS; ++c) out[c] = 1e9;
    int head = 0, tail = 0;
    for (int k = 0; k < nseeds; ++k) { out[seeds[k]] = 0; q[tail++] = seeds[k]; }
    while (head < tail) {
        const int c = q[head++];
        const double nd = out[c] + 1.0;
        for (int d = 0; d < 4; ++d) {
            const int n = c + DIR_DELTA[d];
            const uint8_t g = s.grid[n];
            if ((g == EMPTY || g == BRICK) && out[n] > nd) { out[n] = nd; q[tail++] = n; }
        }
    }
}

void prepare(const State& s) {
    int16_t seed0 = s.base_pos[0], seed1 = s.base_pos[1];
    bfs_dist(s, &seed0, 1, dist_base[0]);
    bfs_dist(s, &seed1, 1, dist_base[1]);
    bfs_dist(s, s.supply, s.n_supply, dist_supply);
}

inline int manh(int a, int b) {
    int dx = cell_x(a) - cell_x(b), dy = cell_y(a) - cell_y(b);
    return (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
}

// ---------------------------------------------------------------- 射线
// 从 pos 沿 dir 看：返回第一个"挡子弹或有坦克"的格子，dist 是格数。
int ray_first(const State& s, int pos, int dir, int& dist) {
    int d = 0;
    for (int c = pos + DIR_DELTA[dir];; c += DIR_DELTA[dir]) {
        ++d;
        const uint8_t g = s.grid[c];
        if (g == OUTSIDE) { dist = d; return c; }
        if (tank_at(s, c) >= 0) { dist = d; return c; }
        if (g == BRICK || g == STEEL || g == BASE0 || g == BASE1) { dist = d; return c; }
    }
}

inline bool is_base_cell(const State& s, int c, int seat) {
    return s.grid[c] == BASE0 + seat;
}

// 基地每回合最多扣 1 血、子弹上限 1：离基地越近开火频率越高（≤2 格每回合一发，越远越稀疏）
inline double rate_factor(int d) {
    return d <= 2 ? 1.5 : (d <= 4 ? 1.0 : (d <= 6 ? 0.6 : 0.35));
}

// ---------------------------------------------------------------- 评估
double eval_state(const State& s, int me) {
    const int opp = 1 - me;
    if (s.done) {
        if (s.winner == me) return 1e7;
        if (s.winner < 0) return 0.0;
        return -1e7;
    }
    double sc = 0.0;
    sc += 35.0 * (s.base_hp[me] - s.base_hp[opp]);
    sc += 45.0 * (s.kills[me] - s.kills[opp]);
    sc += 28.0 * (s.stars_eaten[me] - s.stars_eaten[opp]);

    // 坦克材质 + 走位（朝敌方基地推进）
    for (int i = 0; i < 6; ++i) {
        const Tank& t = s.tank[i];
        if (!t.alive) continue;
        const bool mine = seat_of(i) == me;
        double v = 6.0 + 3.5 * t.hp + 10.0 * t.level;
        double d = dist_base[1 - seat_of(i)][t.pos];
        if (d > 26.0) d = 26.0;
        v -= 3.0 * d;
        sc += mine ? v : -v;
    }

    // 在飞的子弹：先撞上什么
    for (int k = 0; k < s.n_bullets; ++k) {
        const Bullet& b = s.bullet[k];
        int d = 0;
        const int hitc = ray_first(s, b.pos, b.dir, d);
        const bool mine = seat_of(b.owner) == me;
        const double w = d <= 2 ? 1.0 : (d <= 4 ? 0.6 : 0.3);
        double v = 0.0;
        const int tgt = tank_at(s, hitc);
        if (tgt >= 0) {
            const bool tgt_mine = seat_of(tgt) == me;
            if (tgt_mine == mine) v = -10.0;                 // 打队友
            else if (s.tank[tgt].hp <= 1) v = 14.0;          // 能击杀
            else v = 8.0;
        } else if (s.grid[hitc] == BASE0 || s.grid[hitc] == BASE1) {
            const bool base_mine = (s.grid[hitc] - BASE0) == me;
            v = base_mine == mine ? -22.0 : 38.0;
        } else if (s.grid[hitc] == BRICK) {
            const double dg = dist_base[1 - seat_of(b.owner)][hitc];
            v = 2.0 + (dg <= 2.0 ? 14.0 : (dg <= 5.0 ? 5.0 : 0.0));   // 拆敌方基地围墙加钱
        } else if (s.grid[hitc] == STEEL) {
            v = 0.2;
        }
        sc += mine ? w * v : -w * v;
    }

    // 射线能力：现在能打到谁 / 能打到基地（持续输出的能力）
    for (int i = 0; i < 6; ++i) {
        const Tank& t = s.tank[i];
        if (!t.alive || !can_fire(s, i)) continue;
        const bool mine = seat_of(i) == me;
        for (int dir = 0; dir < 4; ++dir) {
            int d = 0;
            const int hitc = ray_first(s, t.pos, dir, d);
            double v = 0.0;
            const int tgt = tank_at(s, hitc);
            if (tgt >= 0) {
                if (seat_of(tgt) == seat_of(i)) v = -4.0;
                else v = s.tank[tgt].hp <= 1 ? 8.0 : 4.5;
            } else if (s.grid[hitc] == BASE0 || s.grid[hitc] == BASE1) {
                v = ((s.grid[hitc] - BASE0) == seat_of(i)) ? -24.0 : 36.0;
            }
            const double w = d <= 4 ? 1.0 : 0.6;
            sc += mine ? w * v : -w * v;
        }
    }

    // 防守牵引：谁的基地被瞄准 / 被子弹盯上，谁靠基地最近的 1-2 辆车回防（其余继续进攻）
    for (int X = 0; X < 2; ++X) {
        int tc[8];
        int nt = 0;
        for (int i = 0; i < 6 && nt < 8; ++i) {
            const Tank& t = s.tank[i];
            if (!t.alive || seat_of(i) == X || !can_fire(s, i)) continue;
            for (int dir = 0; dir < 4 && nt < 8; ++dir) {
                int d;
                const int hitc = ray_first(s, t.pos, dir, d);
                if (is_base_cell(s, hitc, X)) tc[nt++] = t.pos;
            }
        }
        for (int k = 0; k < s.n_bullets && nt < 8; ++k) {
            const Bullet& b = s.bullet[k];
            if (seat_of(b.owner) == X) continue;
            int d;
            const int hitc = ray_first(s, b.pos, b.dir, d);
            if (is_base_cell(s, hitc, X)) tc[nt++] = b.pos;
        }
        if (!nt) continue;
        // 按离自家基地的远近排序，最近的车全额牵引、第二辆半额
        const int home = s.base_pos[X];
        int order[3] = {0, 1, 2};
        int md[3];
        for (int k = 0; k < 3; ++k) {
            const int i = tank_id(X, k);
            md[k] = s.tank[i].alive ? manh(s.tank[i].pos, home) : 999;
        }
        for (int a = 0; a < 3; ++a)
            for (int b2 = a + 1; b2 < 3; ++b2)
                if (md[order[b2]] < md[order[a]]) std::swap(order[a], order[b2]);
        for (int r = 0; r < 2; ++r) {
            const int i = tank_id(X, order[r]);
            if (!s.tank[i].alive) continue;
            const int dd = manh(s.tank[i].pos, home);
            const double v = (r == 0 ? 3.2 : 1.4) * (dd < 12 ? 12 - dd : 0);
            sc += X == me ? v : -v;
        }
    }

    // 星星：走近补给点（未满级）。有星时争抢，没星时小幅提前落位
    {
        const double wgt = s.star_mask ? 2.2 : 0.8;
        for (int i = 0; i < 6; ++i) {
            const Tank& t = s.tank[i];
            if (!t.alive || t.level >= s.P.max_level) continue;
            double d = dist_supply[t.pos];
            if (d > 10.0) d = 10.0;
            const double v = -wgt * d;
            sc += (seat_of(i) == me) ? v : -v;
        }
    }
    // 避免压在对方出生点上被复活炸毁（白送击毁）
    for (int i = 0; i < 6; ++i) {
        const Tank& t = s.tank[i];
        if (!t.alive) continue;
        for (int e = 0; e < 3; ++e) {
            const int j = tank_id(1 - seat_of(i), e);
            if (s.tank[j].alive) continue;
            if (s.spawn[j] == t.pos && respawn_in(s, j) <= 2) {
                sc += (seat_of(i) == me) ? -25.0 : 25.0;
            }
        }
    }
    return sc;
}

// ---------------------------------------------------------------- 贪心（对手模型 / rollout）
// opp_orders：假定对手这一回合怎么动（默认不动）
Orders greedy(const State& s, int seat, const Orders& opp_orders) {
    Orders res;
    for (int type = 0; type < 3; ++type) {
        const int i = tank_id(seat, type);
        if (!s.tank[i].alive) continue;
        Cmd list[N_CMDS];
        const int n = legal_commands(s, i, list);
        double best = -1e18;
        Cmd bc = STAY;
        for (int k = 0; k < n; ++k) {
            State tmp = s;
            Orders mine;
            mine.cmd[type] = list[k];
            step_as(tmp, seat, mine, opp_orders);
            const double sc = eval_state(tmp, seat);
            if (sc > best) { best = sc; bc = list[k]; }
        }
        res.cmd[type] = bc;
    }
    return res;
}

struct Cand { Cmd cmd; double sc; };
struct Combo { double sc; Cmd c[3]; };

} // namespace

void init(const State& s, int me) { (void)s; (void)me; }

Orders decide(const State& s, int me) {
    prepare(s);
    const int opp = 1 - me;
    const Orders stay_orders;
    const Orders enemy_pred = greedy(s, opp, stay_orders);

    // ---- 每辆车候选排序（别人不动，敌人按 enemy_pred）----
    Cand rk[3][N_CMDS];
    int rn[3] = {0, 0, 0};
    for (int type = 0; type < 3; ++type) {
        const int i = tank_id(me, type);
        if (!s.tank[i].alive) { rk[type][0] = {STAY, 0.0}; rn[type] = 1; continue; }
        Cmd list[N_CMDS];
        const int n = legal_commands(s, i, list);
        for (int k = 0; k < n; ++k) {
            State tmp = s;
            Orders mine;
            mine.cmd[type] = list[k];
            step_as(tmp, me, mine, enemy_pred);
            rk[type][k] = {list[k], eval_state(tmp, me)};
        }
        rn[type] = n;
        std::sort(rk[type], rk[type] + n, [](const Cand& a, const Cand& b) {
            return a.sc != b.sc ? a.sc > b.sc : a.cmd < b.cmd;
        });
        if (rn[type] > K_TOP) rn[type] = K_TOP;
    }

    // ---- 联合枚举：1-ply（对 enemy_pred 与敌方不动各评一次，取平均）----
    static Combo combos[512];
    int ncombo = 0;
    for (int a = 0; a < rn[0]; ++a) {
        for (int b = 0; b < rn[1]; ++b) {
            for (int c = 0; c < rn[2]; ++c) {
                Orders mine;
                mine.cmd[0] = rk[0][a].cmd;
                mine.cmd[1] = rk[1][b].cmd;
                mine.cmd[2] = rk[2][c].cmd;
                State t1 = s;
                step_as(t1, me, mine, enemy_pred);
                double sc = eval_state(t1, me);
                State t2 = s;
                step_as(t2, me, mine, stay_orders);
                sc = 0.5 * sc + 0.5 * eval_state(t2, me);
                combos[ncombo].sc = sc;
                combos[ncombo].c[0] = mine.cmd[0];
                combos[ncombo].c[1] = mine.cmd[1];
                combos[ncombo].c[2] = mine.cmd[2];
                ++ncombo;
            }
        }
    }
    std::sort(combos, combos + ncombo, [](const Combo& x, const Combo& y) { return x.sc > y.sc; });

    // ---- 2/3-ply rollout 重排 top-M：我动 → 双方按贪心再走（对方先按我不动出招，我再应招）----
    const int m = ncombo < M_ROLLOUT ? ncombo : M_ROLLOUT;
    double best_final = -1e18;
    int best_idx = 0;
    for (int j = 0; j < m; ++j) {
        // 预算保险：机器慢、局面大时及早收手，用已有的最好组合（至少看 8 个）
        if (j >= 8 && budget_left() < 14.0) break;
        Orders mine;
        mine.cmd[0] = combos[j].c[0];
        mine.cmd[1] = combos[j].c[1];
        mine.cmd[2] = combos[j].c[2];
        State t1 = s;
        step_as(t1, me, mine, enemy_pred);
        double final;
        if (t1.done) {
            final = eval_state(t1, me);
        } else {
            const Orders eg = greedy(t1, opp, stay_orders);
            const Orders mg = greedy(t1, me, eg);
            State t2 = t1;
            step_as(t2, me, mg, eg);
            double e2 = eval_state(t2, me);
            if (!t2.done) {
                const Orders eg2 = greedy(t2, opp, stay_orders);
                const Orders mg2 = greedy(t2, me, eg2);
                State t3 = t2;
                step_as(t3, me, mg2, eg2);
                final = 0.3 * eval_state(t1, me) + 0.4 * e2 + 0.3 * eval_state(t3, me);
            } else {
                final = 0.4 * eval_state(t1, me) + 0.6 * e2;
            }
        }
        if (final > best_final) { best_final = final; best_idx = j; }
    }

    Orders out;
    out.cmd[0] = combos[best_idx].c[0];
    out.cmd[1] = combos[best_idx].c[1];
    out.cmd[2] = combos[best_idx].c[2];
    return out;
}

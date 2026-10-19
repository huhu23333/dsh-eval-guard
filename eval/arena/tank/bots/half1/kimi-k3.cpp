// bot.cpp —— 一层 maxmin 精确前推 + 启发式评估。
#include "tank.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>

using namespace tank;

static int g_me;
static int dist_base[2][CELLS];   // dist_base[seat][c]：c 到 seat 的基地要走几步（砖墙当可走）
static bool danger[CELLS];        // 敌方子弹本回合两个小步内会经过的格子
static bool g_threat;             // 有敌车能直接打到 / 贴近我的基地

static void bfs(const State& s, int src, int* dist) {
    static int queue[CELLS];
    for (int c = 0; c < CELLS; ++c) dist[c] = 9999;
    int head = 0, tail = 0;
    dist[src] = 0;
    queue[tail++] = src;
    while (head < tail) {
        const int c = queue[head++];
        for (int d = 0; d < 4; ++d) {
            const int nx = c + DIR_DELTA[d];
            const uint8_t g = s.grid[nx];
            if ((g == EMPTY || g == BRICK) && dist[nx] > dist[c] + 1) {
                dist[nx] = dist[c] + 1;
                queue[tail++] = nx;
            }
        }
    }
}

// 从 pos 朝 d 看炮线（对 seat 而言）。bricks 输出途中砖墙数。
static double fire_line_score(const State& s, int seat, int pos, int d, int level, int* bricks = nullptr) {
    int nb = 0;
    for (int c = pos + DIR_DELTA[d];; c += DIR_DELTA[d]) {
        const uint8_t g = s.grid[c];
        if (g == EMPTY || g == WATER) {
            const int t = tank_at(s, c);
            if (t >= 0) {
                if (bricks) *bricks = nb;
                return seat_of(t) == seat ? -70.0 : 55.0;
            }
            continue;
        }
        if (bricks) *bricks = nb;
        if (g == BRICK) { ++nb; continue; }
        if (g == STEEL) return (level >= 2) ? 6.0 : -2.0;
        if (g == BASE0 + (1 - seat)) return 45.0 - 6.0 * nb;   // 隔砖也能打穿，慢一些
        if (g == BASE0 + seat) return -120.0;
        return -2.0;
    }
}

// 从 pos 朝某方向看不隔着任何墙就到 target_base（纯空地/水）。
static bool clear_line_to_base(const State& s, int pos, int base_cell) {
    const int bx = cell_x(base_cell), by = cell_y(base_cell);
    const int px = cell_x(pos), py = cell_y(pos);
    if (px != bx && py != by) return false;
    int d;
    if (px == bx) d = by > py ? DOWN : UP;
    else d = bx > px ? RIGHT : LEFT;
    for (int c = pos + DIR_DELTA[d];; c += DIR_DELTA[d]) {
        if (c == base_cell) return true;
        const uint8_t g = s.grid[c];
        if (g != EMPTY && g != WATER) return false;
    }
}

// 从 pos 到 foe 基地的四条直线里，最少隔几块砖（到不了返回 99；0 = 无墙直通）。
static int min_bricks_to_base(const State& s, int seat, int pos) {
    const int base = s.base_pos[1 - seat];
    if (cell_x(pos) != cell_x(base) && cell_y(pos) != cell_y(base)) return 99;
    int d;
    if (cell_x(pos) == cell_x(base)) d = cell_y(base) > cell_y(pos) ? DOWN : UP;
    else d = cell_x(base) > cell_x(pos) ? RIGHT : LEFT;
    int nb = 0;
    for (int c = pos + DIR_DELTA[d];; c += DIR_DELTA[d]) {
        if (c == base) return nb;
        const uint8_t g = s.grid[c];
        if (g == BRICK) { ++nb; continue; }
        if (g == EMPTY || g == WATER) continue;
        return 99;
    }
}

static double evaluate(const State& s) {
    const int me = g_me, opp = 1 - me;
    if (s.done) {
        if (s.winner == me) return 1e6 - s.turn;
        if (s.winner == opp) return -1e6 + s.turn;
        return 0.0;
    }
    double v = 0;
    v += 140.0 * (s.base_hp[me] - s.base_hp[opp]);
    v += 40.0 * (s.kills[me] - s.kills[opp]);
    v += 10.0 * (s.stars_eaten[me] - s.stars_eaten[opp]);
    for (int i = 0; i < 6; ++i) {
        const Tank& t = s.tank[i];
        const int seat = seat_of(i), foe = 1 - seat;
        double tv;
        if (t.alive) {
            tv = 20.0 + 5.0 * t.hp + 14.0 * t.level;
            const int dfb = dist_base[foe][t.pos];
            tv -= 4.0 * dfb;
            tv += 4.0 * (12 - dfb < 0 ? 0 : 12 - dfb);          // 贴近对方基地额外加分
            tv += 5.0 * (7 - dfb < 0 ? 0 : 7 - dfb);            // 非常贴近再加（攻其基地/防我基地都覆盖）
            double best_line = -1e9;
            for (int d = 0; d < 4; ++d) {
                const double ls = fire_line_score(s, seat, t.pos, d, t.level);
                if (ls > best_line) best_line = ls;
            }
            tv += 0.25 * best_line;
            // 能持续轰对方基地的位置极其关键：值一大段未来伤害
            const int nb2 = min_bricks_to_base(s, seat, t.pos);
            if (nb2 == 0) tv += 250.0;
            else if (nb2 < 99) tv += 60.0 - 8.0 * nb2;
            // 站在对方出生点上、对方下回合末复活：会被炸毁
            for (int j = 3 * foe; j < 3 * foe + 3; ++j)
                if (!s.tank[j].alive && t.pos == s.spawn[j] && respawn_in(s, j) <= 1) tv -= 30.0;
        } else {
            tv = -2.0 * respawn_in(s, i);
        }
        v += (seat == me ? tv : -tv);
    }
    return v;
}

struct Ranked { Cmd c; double sc; };

// 按类别挑候选命令：保证开炮、推进、躲避/回防都有代表进入模拟，而不是被单一分数剪没。
static int rank_commands(const State& s, int i, int seat, Ranked* out, int k) {
    Cmd list[N_CMDS];
    const int n = legal_commands(s, i, list);
    const Tank& t = s.tank[i];
    const int foe = 1 - seat;
    int m = 0;
    auto add = [&](Cmd c) {
        for (int j = 0; j < m; ++j) if (out[j].c == c) return;
        if (m < k) out[m++] = Ranked{c, 0};
    };
    add(STAY);
    // 移动落点（假定走成）
    auto endpoint = [&](Cmd c) {
        int d1, d2 = -1;
        if (is_move(c)) d1 = c - 1;
        else { d1 = (c - 9) >> 2; d2 = (c - 9) & 3; }
        int p1 = t.pos + DIR_DELTA[d1];
        if (s.grid[p1] != EMPTY) p1 = t.pos;
        int p2 = p1;
        if (d2 >= 0 && s.grid[p1 + DIR_DELTA[d2]] == EMPTY) p2 = p1 + DIR_DELTA[d2];
        return p2;
    };
    // 1) 开炮：炮线得分前 2
    for (int pick = 0; pick < 2; ++pick) {
        Cmd bc = 0; double bs = -1e9;
        for (int j = 0; j < n; ++j) {
            if (!is_fire(list[j])) continue;
            bool dup = false;
            for (int q = 0; q < m; ++q) if (out[q].c == list[j]) dup = true;
            if (dup) continue;
            const double sc = fire_line_score(s, seat, t.pos, list[j] - 5, t.level);
            if (sc > bs) { bs = sc; bc = list[j]; }
        }
        if (bc && bs > -5.0) add(bc);
    }
    // 2) 推进：向对方基地靠近最多的 3 步（危险落点扣一点，但不至于剪光）
    for (int pick = 0; pick < 3; ++pick) {
        Cmd bc = 0; double bs = -1e9;
        for (int j = 0; j < n; ++j) {
            if (!is_move(list[j]) && !is_move2(list[j])) continue;
            bool dup = false;
            for (int q = 0; q < m; ++q) if (out[q].c == list[j]) dup = true;
            if (dup) continue;
            const int p2 = endpoint(list[j]);
            double sc = 3.0 * (dist_base[foe][t.pos] - dist_base[foe][p2]);
            if (is_move2(list[j])) sc += 1.0;
            if (danger[p2]) sc -= 20.0;
            if (sc > bs) { bs = sc; bc = list[j]; }
        }
        if (bc) add(bc);
    }
    // 3) 躲避 / 回防 / 吃星：再补 2 条
    for (int pick = 0; pick < 2; ++pick) {
        Cmd bc = 0; double bs = -1e9;
        for (int j = 0; j < n; ++j) {
            if (!is_move(list[j]) && !is_move2(list[j])) continue;
            bool dup = false;
            for (int q = 0; q < m; ++q) if (out[q].c == list[j]) dup = true;
            if (dup) continue;
            const int p2 = endpoint(list[j]);
            double sc = 0;
            if (danger[t.pos] && !danger[p2]) sc += 30.0;
            if (danger[p2]) sc -= 30.0;
            if (g_threat) sc += 3.0 * (dist_base[seat][t.pos] - dist_base[seat][p2]);
            for (int q = 0; q < s.n_supply; ++q)
                if ((s.star_mask >> q) & 1) {
                    const int sp = s.supply[q];
                    sc += 6.0 * (std::abs(cell_x(sp) - cell_x(t.pos)) + std::abs(cell_y(sp) - cell_y(t.pos))
                               - std::abs(cell_x(sp) - cell_x(p2)) - std::abs(cell_y(sp) - cell_y(p2)));
                }
            if (sc > bs) { bs = sc; bc = list[j]; }
        }
        if (bc) add(bc);
    }
    // 给候选补一个粗分数，供联合命令排序（强对手命令优先，利于剪枝）
    for (int j = 0; j < m; ++j) {
        const Cmd c = out[j].c;
        if (c == STAY) out[j].sc = 0;
        else if (is_fire(c)) out[j].sc = fire_line_score(s, seat, t.pos, c - 5, t.level);
        else {
            const int p2 = endpoint(c);
            out[j].sc = 3.0 * (dist_base[foe][t.pos] - dist_base[foe][p2]) - (danger[p2] ? 20.0 : 0.0);
        }
    }
    return m;
}

static void compute_danger(const State& s, int me) {
    for (int c = 0; c < CELLS; ++c) danger[c] = false;
    for (int k = 0; k < s.n_bullets; ++k) {
        const Bullet& b = s.bullet[k];
        if (seat_of(b.owner) == me) continue;
        int c = b.pos;
        for (int stepn = 0; stepn < 2; ++stepn) {
            c += DIR_DELTA[b.dir];
            danger[c] = true;
            const uint8_t g = s.grid[c];
            if (g != EMPTY && g != WATER) break;
        }
    }
}

// rollout 用的贪心策略：每辆车独立挑一条"其它车不动时评估最好"的命令。
static Orders greedy_joint(const State& s, int seat, int k) {
    Orders o;
    for (int type = 0; type < 3; ++type) {
        const int i = tank_id(seat, type);
        o.cmd[type] = STAY;
        if (!s.tank[i].alive) continue;
        Ranked rc[9];
        const int m = rank_commands(s, i, seat, rc, k);
        double bv = -1e18;
        for (int j = 0; j < m; ++j) {
            State nx = s;
            Orders solo, zero;
            solo.cmd[type] = rc[j].c;
            step_as(nx, seat, solo, zero);
            const double v = seat == g_me ? evaluate(nx) : -evaluate(nx);
            if (v > bv) { bv = v; o.cmd[type] = rc[j].c; }
        }
    }
    return o;
}

// 从局面 s 起双方都用贪心策略打 depth 回合，返回终局评估（g_me 视角）。
static double rollout(State s, int depth) {
    for (int t = 0; t < depth && !s.done; ++t) {
        const Orders o0 = greedy_joint(s, 0, 6);
        const Orders o1 = greedy_joint(s, 1, 6);
        step(s, o0, o1);
    }
    return evaluate(s);
}

void init(const State& s, int me) {
    g_me = me;
    bfs(s, s.base_pos[0], dist_base[0]);
    bfs(s, s.base_pos[1], dist_base[1]);
}

Orders decide(const State& s, int me) {
    g_me = me;
    bfs(s, s.base_pos[0], dist_base[0]);
    bfs(s, s.base_pos[1], dist_base[1]);
    compute_danger(s, me);

    g_threat = false;
    for (int i = 3 * (1 - me); i < 3 * (1 - me) + 3; ++i)
        if (s.tank[i].alive &&
            (dist_base[me][s.tank[i].pos] <= 4 || clear_line_to_base(s, s.tank[i].pos, s.base_pos[me])))
            g_threat = true;

    const int K_MINE = 8, K_OPP = 6;
    Ranked mine[3][9], opps[3][9];
    int nm[3] = {1, 1, 1}, no[3] = {1, 1, 1};
    for (int type = 0; type < 3; ++type) {
        const int im = tank_id(me, type), io = tank_id(1 - me, type);
        if (s.tank[im].alive) nm[type] = rank_commands(s, im, me, mine[type], K_MINE);
        else { mine[type][0] = Ranked{STAY, 0}; }
        if (s.tank[io].alive) no[type] = rank_commands(s, io, 1 - me, opps[type], K_OPP);
        else { opps[type][0] = Ranked{STAY, 0}; }
    }

    struct Joint { Orders o; double sc; };
    static Joint opp_joint[1000];
    int njo = 0;
    for (int a = 0; a < no[0]; ++a)
        for (int b = 0; b < no[1]; ++b)
            for (int c = 0; c < no[2]; ++c) {
                Orders o;
                o.cmd[0] = opps[0][a].c; o.cmd[1] = opps[1][b].c; o.cmd[2] = opps[2][c].c;
                opp_joint[njo++] = Joint{o, opps[0][a].sc + opps[1][b].sc + opps[2][c].sc};
            }
    std::sort(opp_joint, opp_joint + njo, [](const Joint& x, const Joint& y) { return x.sc > y.sc; });

    Orders best;
    double best_val = -1e18;
    int checked = 0;
    // 主循环里记录前 12 个候选，稍后对它们做 rollout 再选
    static Orders top_ord[12];
    static double top_val[12];
    int n_top = 0;
    auto consider_top = [&](const Orders& mo, double v) {
        if (n_top < 12) {
            top_ord[n_top] = mo; top_val[n_top] = v; ++n_top;
        } else {
            int wi = 0;
            for (int q = 1; q < 12; ++q) if (top_val[q] < top_val[wi]) wi = q;
            if (v > top_val[wi]) { top_ord[wi] = mo; top_val[wi] = v; }
        }
    };
    for (int a = 0; a < nm[0]; ++a)
        for (int b = 0; b < nm[1]; ++b)
            for (int c = 0; c < nm[2]; ++c) {
                Orders mo;
                mo.cmd[0] = mine[0][a].c; mo.cmd[1] = mine[1][b].c; mo.cmd[2] = mine[2][c].c;
                double worst = 1e18, v_top = 0;
                int j = 0;
                for (; j < njo; ++j) {
                    State nx = s;
                    step_as(nx, me, mo, opp_joint[j].o);
                    const double v = evaluate(nx);
                    if (j == 0) v_top = v;
                    if (v < worst) worst = v;
                    if (worst < best_val && v_top < best_val) break;
                    if ((j & 15) == 15 && budget_left() < 8.0) break;
                }
                const double combined = 0.4 * worst + 0.6 * v_top;
                if (combined > best_val) {
                    best_val = combined;
                    best = mo;
                }
                if (j == njo) consider_top(mo, combined);   // 只收完整评估过的
                if (++checked >= 12) {
                    checked = 0;
                    if (budget_left() < 8.0) goto done;
                }
            }
done:
    (void)n_top;
    return best;
}

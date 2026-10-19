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
static int dist_supply[CELLS];    // 到最近补给点的步数（砖墙当可走）

// 多源 BFS：从全部补给点出发
static void bfs_supply(const State& s) {
    static int queue[CELLS];
    for (int c = 0; c < CELLS; ++c) dist_supply[c] = 9999;
    int head = 0, tail = 0;
    for (int k = 0; k < s.n_supply; ++k) {
        dist_supply[s.supply[k]] = 0;
        queue[tail++] = s.supply[k];
    }
    while (head < tail) {
        const int c = queue[head++];
        for (int d = 0; d < 4; ++d) {
            const int nx = c + DIR_DELTA[d];
            const uint8_t g = s.grid[nx];
            if ((g == EMPTY || g == BRICK) && dist_supply[nx] > dist_supply[c] + 1) {
                dist_supply[nx] = dist_supply[c] + 1;
                queue[tail++] = nx;
            }
        }
    }
}

// 开局抢星：第一批星第 10 回合末出现。让 fast 直奔补给点蹲守（有子弹在飞时交给搜索）。
// 返回是否接管了 fast 的命令。
static Cmd opening_star_cmd(const State& s, int me) {
    if (s.turn >= 9 || s.n_bullets > 0) return STAY;
    const int i = tank_id(me, FAST);
    if (!s.tank[i].alive) return STAY;
    const int p = s.tank[i].pos;
    if (dist_supply[p] == 0) return STAY;          // 已经在补给点上：蹲着等星
    // 优先两步、再一步，走到 dist_supply 更小的空格；挡路的是砖就朝它开炮
    Cmd bestc = STAY;
    int bestd = dist_supply[p];
    for (int d1 = 0; d1 < 4; ++d1) {
        const int p1 = p + DIR_DELTA[d1];
        const uint8_t g1 = s.grid[p1];
        if (g1 == BRICK) {
            if (dist_supply[p1] < bestd && can_fire(s, i)) { bestd = dist_supply[p1]; bestc = cmd_fire(d1); }
            continue;
        }
        if (g1 != EMPTY || tank_at(s, p1) >= 0) continue;
        if (dist_supply[p1] < bestd) { bestd = dist_supply[p1]; bestc = cmd_move(d1); }
        for (int d2 = 0; d2 < 4; ++d2) {
            const int p2 = p1 + DIR_DELTA[d2];
            if (s.grid[p2] != EMPTY || tank_at(s, p2) >= 0) continue;
            if (dist_supply[p2] < bestd) { bestd = dist_supply[p2]; bestc = cmd_move2(d1, d2); }
        }
    }
    return bestc;
}

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
            // 能持续轰对方基地的位置极其关键：值一大段未来伤害。
            // 不用装填的车（火力型 / 1 级以上）每回合都能开炮，磨血速度翻倍，位置更值钱。
            const bool no_reload = (type_of(i) == FIRE) || t.level >= 1;
            const int nb2 = min_bricks_to_base(s, seat, t.pos);
            if (nb2 == 0) tv += no_reload ? 320.0 : 150.0;
            else if (nb2 < 99) tv += (60.0 - 8.0 * nb2) * (no_reload ? 1.0 : 0.6);
            // 炮手的拦截/护航梯度：我是炮手时敌车靠近要扣分（对方视角对称，
            // 等价于"我的车靠近对方炮手会加分"——产生跨多回合的拦截拉力）
            if (nb2 == 0)
                for (int j = 3 * foe; j < 3 * foe + 3; ++j)
                    if (s.tank[j].alive) {
                        const int md = std::abs(cell_x(s.tank[j].pos) - cell_x(t.pos))
                                     + std::abs(cell_y(s.tank[j].pos) - cell_y(t.pos));
                        tv -= 24.0 / (1.0 + md);
                    }
            // 站在对方出生点上、对方下回合末复活：会被炸毁
            for (int j = 3 * foe; j < 3 * foe + 3; ++j)
                if (!s.tank[j].alive && t.pos == s.spawn[j] && respawn_in(s, j) <= 1) tv -= 30.0;
        } else {
            tv = -2.0 * respawn_in(s, i);
        }
        v += (seat == me ? tv : -tv);
    }
    // 星星争夺：有星或快出星时，我的车比对方更靠近补给点要加分（等级=射速+血量，是胜负手）
    {
        const bool soon = s.turn + 1 >= s.P.supply_first &&
                          (s.turn + 1 - s.P.supply_first) % s.P.supply_period >= s.P.supply_period - 5;
        if (s.star_mask || soon) {
            for (int q = 0; q < s.n_supply; ++q) {
                const bool has = (s.star_mask >> q) & 1;
                if (!has && !soon) continue;
                const int sp = s.supply[q];
                int dm = 99, dt = 99;
                for (int i = 0; i < 6; ++i) {
                    if (!s.tank[i].alive) continue;
                    const int d = std::abs(cell_x(sp) - cell_x(s.tank[i].pos)) + std::abs(cell_y(sp) - cell_y(s.tank[i].pos));
                    if (seat_of(i) == me) { if (d < dm) dm = d; }
                    else if (d < dt) dt = d;
                }
                v += (has ? 5.0 : 2.0) * (dt - dm);
            }
        }
    }
    return v;
}

struct Ranked { Cmd c; double sc; };

// 按类别挑候选命令：保证开炮、推进、躲避/回防都有代表进入模拟，而不是被单一分数剪没。
static int rank_commands(const State& s, int i, int seat, Ranked* out, int k, bool guard) {
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
    // 守家车：优先挑靠近己方基地、面向中场拦截的命令
    if (guard) {
        for (int pick = 0; pick < 2; ++pick) {
            Cmd bc = 0; double bs = -1e9;
            for (int j = 0; j < n; ++j) {
                if (!is_move(list[j]) && !is_move2(list[j])) continue;
                bool dup = false;
                for (int q = 0; q < m; ++q) if (out[q].c == list[j]) dup = true;
                if (dup) continue;
                int d1, d2 = -1;
                if (is_move(list[j])) d1 = list[j] - 1;
                else { d1 = (list[j] - 9) >> 2; d2 = (list[j] - 9) & 3; }
                int p1 = t.pos + DIR_DELTA[d1];
                if (s.grid[p1] != EMPTY) p1 = t.pos;
                int p2 = p1;
                if (d2 >= 0 && s.grid[p1 + DIR_DELTA[d2]] == EMPTY) p2 = p1 + DIR_DELTA[d2];
                double sc = 3.0 * (dist_base[seat][t.pos] - dist_base[seat][p2]);
                // 有敌车逼近时，改朝最近的敌车去拦截
                if (g_threat)
                    for (int e = 3 * foe; e < 3 * foe + 3; ++e)
                        if (s.tank[e].alive && dist_base[seat][s.tank[e].pos] <= 6)
                            sc += 2.0 * (std::abs(cell_x(s.tank[e].pos) - cell_x(t.pos)) + std::abs(cell_y(s.tank[e].pos) - cell_y(t.pos))
                                       - std::abs(cell_x(s.tank[e].pos) - cell_x(p2)) - std::abs(cell_y(s.tank[e].pos) - cell_y(p2)));
                if (sc > bs) { bs = sc; bc = list[j]; }
            }
            if (bc) add(bc);
        }
    }
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
    // 2) 推进：向对方基地靠近最多的 6 步（危险落点扣一点，但不至于剪光）
    for (int pick = 0; pick < 6; ++pick) {
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

// rollout 用的联合贪心：在少量联合候选里挑"对方按 prev 行动时"评估最优的。
static Orders joint_greedy(const State& s, int seat, const Orders& opp_prev, int k) {
    compute_danger(s, seat);
    bool threat = false;
    for (int i = 3 * (1 - seat); i < 3 * (1 - seat) + 3; ++i)
        if (s.tank[i].alive &&
            (dist_base[seat][s.tank[i].pos] <= 4 || clear_line_to_base(s, s.tank[i].pos, s.base_pos[seat])))
            threat = true;
    g_threat = threat;
    Ranked rc[3][11];
    int nn[3] = {1, 1, 1};
    for (int type = 0; type < 3; ++type) {
        const int i = tank_id(seat, type);
        if (s.tank[i].alive) nn[type] = rank_commands(s, i, seat, rc[type], k, false);
        else rc[type][0] = Ranked{STAY, 0};
    }
    Orders best;
    double bv = -1e18;
    for (int a = 0; a < nn[0]; ++a)
        for (int b = 0; b < nn[1]; ++b)
            for (int c = 0; c < nn[2]; ++c) {
                Orders o;
                o.cmd[0] = rc[0][a].c; o.cmd[1] = rc[1][b].c; o.cmd[2] = rc[2][c].c;
                State nx = s;
                step_as(nx, seat, o, opp_prev);
                const double v = seat == g_me ? evaluate(nx) : -evaluate(nx);
                if (v > bv) { bv = v; best = o; }
            }
    return best;
}

// 从局面 s 起双方交替最佳应手对推 depth 回合，返回终局评估（g_me 视角）。
static double rollout(State s, int depth) {
    Orders prev[2] = {};
    for (int t = 0; t < depth && !s.done; ++t) {
        const Orders o0 = joint_greedy(s, 0, prev[1], 4);
        const Orders o1 = joint_greedy(s, 1, prev[0], 4);
        prev[0] = o0; prev[1] = o1;
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
    bfs_supply(s);
    compute_danger(s, me);

    g_threat = false;
    for (int i = 3 * (1 - me); i < 3 * (1 - me) + 3; ++i)
        if (s.tank[i].alive &&
            (dist_base[me][s.tank[i].pos] <= 4 || clear_line_to_base(s, s.tank[i].pos, s.base_pos[me])))
            g_threat = true;

    const int K_MINE = 12, K_OPP = 6;
    Ranked mine[3][14], opps[3][14];
    int nm[3] = {1, 1, 1}, no[3] = {1, 1, 1};
    for (int type = 0; type < 3; ++type) {
        const int im = tank_id(me, type), io = tank_id(1 - me, type);
        if (s.tank[im].alive) nm[type] = rank_commands(s, im, me, mine[type], K_MINE, false);
        else { mine[type][0] = Ranked{STAY, 0}; }
        if (s.tank[io].alive) no[type] = rank_commands(s, io, 1 - me, opps[type], K_OPP, false);
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
    // rollout 阶段实测有害，停用（保留代码备查）
    if (false && budget_left() > 14.0) {
        double best2 = -1e18;
        for (int q = 0; q < n_top; ++q) {
            State leaf = s;
            step_as(leaf, me, top_ord[q], opp_joint[0].o);
            const double rv = rollout(leaf, 8);
            const double v2 = 0.6 * top_val[q] + 0.4 * rv;
            if (v2 > best2) { best2 = v2; best = top_ord[q]; }
            if (budget_left() < 8.0) break;
        }
    }
done:;
    return best;
}

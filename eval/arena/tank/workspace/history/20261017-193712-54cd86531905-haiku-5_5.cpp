// bot.cpp —— 两步前瞻（我方连续两回合），对手用两种模型（不动 / 朝我方可打的目标开炮），取平均。
#include "tank.hpp"

using namespace tank;

static int dist_enemy[CELLS];   // 每个格子到对方基地的步数（砖墙当作可走）

static void compute_distances(const State& s, int me) {
    static int queue[CELLS];
    for (int c = 0; c < CELLS; ++c) dist_enemy[c] = 9999;
    int head = 0, tail = 0;
    const int target = s.base_pos[1 - me];
    dist_enemy[target] = 0;
    queue[tail++] = target;
    while (head < tail) {
        const int c = queue[head++];
        for (int d = 0; d < 4; ++d) {
            const int next = c + DIR_DELTA[d];
            const uint8_t g = s.grid[next];
            if ((g == EMPTY || g == BRICK) && dist_enemy[next] > dist_enemy[c] + 1) {
                dist_enemy[next] = dist_enemy[c] + 1;
                queue[tail++] = next;
            }
        }
    }
}

// 子弹从 pos 沿 d 飞出去，先碰到什么：>=0 坦克编号；-2 基地 seat 0；-3 基地 seat 1；-1 墙或界外。
static int first_hit(const State& s, int pos, int d) {
    for (int c = pos + DIR_DELTA[d];; c += DIR_DELTA[d]) {
        const uint8_t g = s.grid[c];
        const int t = tank_at(s, c);
        if (t >= 0) return t;
        if (g == BASE0) return -2;
        if (g == BASE1) return -3;
        if (g == BRICK || g == STEEL || g == OUTSIDE) return -1;
    }
}

// 对手的"开炮模型"：每辆能开炮的车，朝第一个会打到我方坦克或我方基地的方向开炮。
static Orders enemy_greedy(const State& s, int me) {
    Orders o;
    const int eb = 1 - me;
    for (int type = 0; type < 3; ++type) {
        const int j = tank_id(eb, type);
        if (!s.tank[j].alive || !can_fire(s, j)) continue;
        for (int d = 0; d < 4; ++d) {
            const int h = first_hit(s, s.tank[j].pos, d);
            const bool ours = (h >= 0 && seat_of(h) == me) || (h <= -2 && (-2 - h) == me);
            if (ours) { o.cmd[type] = cmd_fire(d); break; }
        }
    }
    return o;
}

static double evaluate(const State& s, int me) {
    const int eb = 1 - me;
    if (s.done) {
        if (s.winner == me) return 1e7;
        if (s.winner == eb) return -1e7;
        return 0;
    }
    double v = 0;
    v += 250.0 * s.base_hp[me];
    v -= 250.0 * s.base_hp[eb];
    for (int type = 0; type < 3; ++type) {
        const int i = tank_id(me, type), j = tank_id(eb, type);
        if (s.tank[i].alive) {
            v += 80 + 20 * s.tank[i].hp;
            v -= 6.0 * dist_enemy[s.tank[i].pos];
            if (can_fire(s, i))
                for (int d = 0; d < 4; ++d) {
                    const int h = first_hit(s, s.tank[i].pos, d);
                    if (h >= 0 && seat_of(h) == eb) { v += 30; break; }
                }
        }
        if (s.tank[j].alive) v -= 80 + 20 * s.tank[j].hp;
    }
    // 飞行中的子弹：看它最终会打到什么。基地每回合最多扣 1 血，同一基地的第二发及之后价值很低。
    int hits_eb = 0, hits_me = 0;
    for (int k = 0; k < s.n_bullets; ++k) {
        const Bullet& b = s.bullet[k];
        const int owner_seat = seat_of(b.owner);
        const int h = first_hit(s, b.pos, b.dir);
        if (h >= 0) {
            if (seat_of(h) == owner_seat) v -= (owner_seat == me ? 25 : -25);
            else if (owner_seat == me) v += 40;
            else v -= 40 + 10 * s.tank[h].hp;
        } else if (h == -2 - eb) {
            if (owner_seat == me) v += hits_eb++ == 0 ? 90 : 20;
        } else if (h == -2 - me) {
            if (owner_seat == eb) v -= hits_me++ == 0 ? 90 : 20;
        }
    }
    // 站位：沿某个方向看过去，能打到某个基地（中间只有空地、水，最多一块砖）
    for (int seat = 0; seat < 2; ++seat) {
        const int target_base = seat == me ? eb : me;
        for (int type = 0; type < 3; ++type) {
            const int i = tank_id(seat, type);
            if (!s.tank[i].alive) continue;
            for (int d = 0; d < 4; ++d) {
                int bricks = 0;
                int c = s.tank[i].pos;
                for (;;) {
                    c += DIR_DELTA[d];
                    const uint8_t g = s.grid[c];
                    if (g == BASE0 || g == BASE1) {
                        if (g == BASE0 + target_base) {
                            const double bonus = bricks == 0 ? 12.0 : 5.0;
                            v += seat == me ? bonus : -bonus;
                        }
                        break;
                    }
                    if (g == BRICK) { if (++bricks > 1) break; continue; }
                    if (g != EMPTY && g != WATER) break;
                }
            }
        }
    }
    return v;
}

// 我方候选命令（死车只有 STAY）
static void candidates(const State& s, int me, Cmd cand[3][N_CMDS], int nc[3]) {
    for (int type = 0; type < 3; ++type) {
        const int i = tank_id(me, type);
        if (!s.tank[i].alive) { cand[type][0] = STAY; nc[type] = 1; }
        else nc[type] = legal_commands(s, i, cand[type]);
    }
}

// 用两种对手模型各自前推一回合，返回平均值
static double value_after(const State& s, int me, const Orders& mine, const Orders& eo) {
    State t = s;
    if (me == 0) step(t, mine, eo);
    else step(t, eo, mine);
    return evaluate(t, me);
}

// 一步搜索：返回所有组合的平均值（对两种对手模型）和最优组合
struct Node { double v; Orders o; };

static const int MAX_TOP = 3;

void init(const State& s, int me) {
    (void)s; (void)me;
}

Orders decide(const State& s, int me) {
    compute_distances(s, me);
    Cmd cand[3][N_CMDS];
    int nc[3];
    candidates(s, me, cand, nc);

    const Orders greedy = enemy_greedy(s, me);
    const Orders stay{};

    // 第一层：所有组合，对两种对手模型取平均
    static Node all[25 * 25 * 25];
    int n = 0;
    for (int a = 0; a < nc[0]; ++a)
        for (int b = 0; b < nc[1]; ++b)
            for (int c = 0; c < nc[2]; ++c) {
                Orders mine;
                mine.cmd[0] = cand[0][a];
                mine.cmd[1] = cand[1][b];
                mine.cmd[2] = cand[2][c];
                const double v = 0.5 * value_after(s, me, mine, stay) + 0.5 * value_after(s, me, mine, greedy);
                all[n++] = Node{v, mine};
            }
    // 取前 K 个
    int top_n = n < MAX_TOP ? n : MAX_TOP;
    for (int k = 0; k < top_n; ++k) {
        int bi = k;
        for (int j = k + 1; j < n; ++j) if (all[j].v > all[bi].v) bi = j;
        Node tmp = all[k]; all[k] = all[bi]; all[bi] = tmp;
    }

    Orders best = all[0].o;
    double best_v = -1e300;
    for (int k = 0; k < top_n; ++k) {
        if (k > 0 && tank::budget_left() < 8) break;
        const Orders mine = all[k].o;
        double worst_sum = 0;
        for (const Orders* eo : {&stay, &greedy}) {
            State t = s;
            if (me == 0) step(t, mine, *eo);
            else step(t, *eo, mine);
            // 第二层：我方在 t 上做一步搜索，对手同样两种模型
            Cmd c2[3][N_CMDS];
            int nc2[3];
            candidates(t, me, c2, nc2);
            const Orders g2 = enemy_greedy(t, me);
            double inner = -1e300;
            for (int a = 0; a < nc2[0]; ++a)
                for (int b = 0; b < nc2[1]; ++b)
                    for (int c = 0; c < nc2[2]; ++c) {
                        Orders m2;
                        m2.cmd[0] = c2[0][a];
                        m2.cmd[1] = c2[1][b];
                        m2.cmd[2] = c2[2][c];
                        const double v = 0.5 * value_after(t, me, m2, stay) + 0.5 * value_after(t, me, m2, g2);
                        if (v > inner) inner = v;
                    }
            worst_sum += 0.5 * inner;
        }
        if (worst_sum > best_v) { best_v = worst_sum; best = mine; }
    }
    return best;
}

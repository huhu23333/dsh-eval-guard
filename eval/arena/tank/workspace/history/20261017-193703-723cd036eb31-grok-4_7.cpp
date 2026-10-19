// 坦克大战 bot
//
// 打法：火力车压主攻线，装甲车扛线和补刀，快速车抢星兼侧袭。
// 每回合枚举己方全部合法命令，对「暗杀 / 冲基地 / 原地吃子弹」三种对手模型做一回合精确推演，
// 再把最好的一批和几套脚本策略放进多回合推演（双方都用同一套走子策略）里比较，取最坏情况下最好的一着。
// 墙图用「到最佳炮位的代价」做梯度；星星按刷新窗口加权。时间用 budget_left 卡住，超预算前停手。
#include "tank.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>

using namespace tank;

using Score = long long;

static constexpr int INF_D = 9999;
static constexpr int QCAP = 4096;
static constexpr Score WIN_SC = 1000000000000LL;

#ifndef FIXED_NODES
#define FIXED_NODES 0
#endif

static int g_me = 0, g_en = 1;
static int g_stuck[3] = {};
static int g_lastpos[3] = {-1, -1, -1};
static Cmd g_lastcmd[3] = {};
static long g_nodes = 0;
static bool g_stop = false;

enum Mode : int { M_BAL = 0, M_AGG = 1, M_STAR = 2, M_DEF = 3, M_N = 4 };

// --------------------------------------------------------------------------
// 小工具
// --------------------------------------------------------------------------

static bool time_ok(double reserve) {
#if FIXED_NODES
    (void)reserve;
    return g_nodes < FIXED_NODES;
#else
    return budget_left() >= reserve;
#endif
}

static int iabs(int x) { return x < 0 ? -x : x; }

static int manh(int a, int b) {
    return iabs(cell_x(a) - cell_x(b)) + iabs(cell_y(a) - cell_y(b));
}

// 离 seat 基地最近的活敌。base_d 小表示已经摸进家里。
struct Threat {
    int id;
    int base_d;
    int pos;
};

static Threat nearest_threat(const State& s, int seat) {
    Threat th{-1, 99, 0};
    const int b = s.base_pos[seat];
    for (int ty = 0; ty < 3; ++ty) {
        const int ei = tank_id(1 - seat, ty);
        if (!s.tank[ei].alive) continue;
        const int d = manh(s.tank[ei].pos, b);
        if (d < th.base_d) {
            th.id = ei;
            th.base_d = d;
            th.pos = s.tank[ei].pos;
        }
    }
    return th;
}

static int star_wait(int turn) {
    if (turn < 10) return 10 - turn;
    const int m = (turn - 10) % 20;
    return m == 0 ? 0 : 20 - m;
}

struct Ray {
    int kind;  // 0 出界/空, 1 砖, 2 钢, 3 敌方基地, 4 己方基地, 5 坦克
    int id;
    int dist;
    int cell;
};

static Ray trace(const State& s, int pos, int dir, int seat) {
    Ray r{0, -1, 0, pos};
    for (int c = pos + DIR_DELTA[dir], n = 1; n <= 20; ++n, c += DIR_DELTA[dir]) {
        const int ti = tank_at(s, c);
        if (ti >= 0) return Ray{5, ti, n, c};
        const uint8_t g = s.grid[c];
        if (g == EMPTY || g == WATER) continue;
        if (g == BRICK) return Ray{1, -1, n, c};
        if (g == STEEL) return Ray{2, -1, n, c};
        if (g == BASE0 || g == BASE1) return Ray{(g - BASE0) == seat ? 4 : 3, -1, n, c};
        return Ray{0, -1, n, c};
    }
    return r;
}

// 只看地形：沿方向到敌方基地要打掉几块墙。打不到返回 99。
static int bricks_to_base(const State& s, int cell, int dir, int enemy_base, bool steel_ok) {
    int br = 0;
    for (int c = cell + DIR_DELTA[dir], n = 0; n < 20; ++n, c += DIR_DELTA[dir]) {
        if (c == enemy_base) return br;
        const uint8_t g = s.grid[c];
        if (g == EMPTY || g == WATER) continue;
        if (g == BRICK) {
            ++br;
            continue;
        }
        if (g == STEEL) {
            if (!steel_ok) return 99;
            ++br;
            continue;
        }
        return 99;
    }
    return 99;
}

// --------------------------------------------------------------------------
// 距离场：每个空地到「好炮位」的行进代价，以及到补给点的代价。按墙哈希缓存。
// --------------------------------------------------------------------------

struct Entry {
    uint64_t hash = 0;
    bool valid = false;
    int atk[2][CELLS];
    int atkS[2][CELLS];
    int sup[MAX_SUPPLY][CELLS];
};

static Entry g_cache[24];
static int g_clock = 0;

static int enter_w(uint8_t g, int brick_w, int steel_w) {
    if (g == EMPTY) return 1;
    if (g == BRICK) return brick_w;
    if (g == STEEL) return steel_w;
    return INF_D;
}

static void spfa(const State& s, int* dist, int brick_w, int steel_w) {
    unsigned char inq[CELLS];
    std::memset(inq, 0, sizeof inq);
    int q[QCAP];
    int qh = 0, qt = 0, qc = 0;
    auto push = [&](int c) {
        if (inq[c] || dist[c] >= INF_D) return;
        inq[c] = 1;
        q[qt] = c;
        qt = (qt + 1) & (QCAP - 1);
        ++qc;
    };
    for (int c = 0; c < CELLS; ++c)
        if (dist[c] < INF_D) push(c);
    int guard = 0;
    while (qc > 0 && guard < CELLS * 16) {
        ++guard;
        const int c = q[qh];
        qh = (qh + 1) & (QCAP - 1);
        --qc;
        inq[c] = 0;
        const int dc = dist[c];
        for (int d = 0; d < 4; ++d) {
            const int n = c + DIR_DELTA[d];
            const int w = enter_w(s.grid[n], brick_w, steel_w);
            if (w >= INF_D) continue;
            const int nd = dc + w;
            if (nd < dist[n]) {
                dist[n] = nd;
                push(n);
            }
        }
    }
}

static void seed_attack(const State& s, int seat, bool steel_ok, int* dist) {
    const int eb = s.base_pos[1 - seat];
    for (int c = 0; c < CELLS; ++c) dist[c] = INF_D;
    const int sz = s.P.size;
    for (int y = 0; y < sz; ++y) {
        for (int x = 0; x < sz; ++x) {
            const int c = cell_of(x, y);
            if (s.grid[c] != EMPTY) continue;
            int best = 99, blen = 99;
            for (int d = 0; d < 4; ++d) {
                const int br = bricks_to_base(s, c, d, eb, steel_ok);
                if (br >= 99) continue;
                int len = 0;
                for (int p = c + DIR_DELTA[d]; p != eb && len < 20; p += DIR_DELTA[d]) ++len;
                if (br < best || (br == best && len < blen)) {
                    best = br;
                    blen = len;
                }
            }
            if (best >= 99) continue;
            // 长度 ≤2 的炮线当回合清弹，能连射；≤4 勉强。更长会把子弹上限锁住，
            // 代价要明显高于多拆一两块砖，搜索才会绕去贴脸的炮位。
            int lane = 0;
            if (blen <= 2) lane = 0;
            else if (blen <= 4) lane = 3;
            else lane = 12 + blen;
            dist[c] = best * 6 + lane;
        }
    }
}

static void build_entry(const State& s, Entry& e) {
    e.hash = s.wall_hash;
    e.valid = true;
    for (int seat = 0; seat < 2; ++seat) {
        seed_attack(s, seat, false, e.atk[seat]);
        spfa(s, e.atk[seat], 4, INF_D);
        seed_attack(s, seat, true, e.atkS[seat]);
        spfa(s, e.atkS[seat], 4, 4);
    }
    for (int k = 0; k < s.n_supply; ++k) {
        for (int c = 0; c < CELLS; ++c) e.sup[k][c] = INF_D;
        e.sup[k][s.supply[k]] = 0;
        spfa(s, e.sup[k], 4, INF_D);
    }
    for (int k = s.n_supply; k < MAX_SUPPLY; ++k)
        for (int c = 0; c < CELLS; ++c) e.sup[k][c] = INF_D;
}

static const Entry& info(const State& s) {
    for (const Entry& e : g_cache)
        if (e.valid && e.hash == s.wall_hash) return e;
    Entry& e = g_cache[g_clock];
    g_clock = (g_clock + 1) % 24;
    build_entry(s, e);
    return e;
}

static int attack_field(const State& s, int seat, int cell, bool steel_ok) {
    const Entry& e = info(s);
    const int v = steel_ok ? e.atkS[seat][cell] : e.atk[seat][cell];
    return v >= INF_D ? 80 : v;
}

// --------------------------------------------------------------------------
// 现有子弹会不会打中这辆车（其他人原地，没有新炮）。给策略用，搜索本身用 step。
// --------------------------------------------------------------------------

static int bullet_loss(const State& s, int self, Cmd cmd) {
    if (!s.tank[self].alive || s.n_bullets <= 0) return 0;
    int d1 = -1, d2 = -1;
    if (cmd >= 1 && cmd <= 4) d1 = cmd - 1;
    else if (cmd >= 9 && cmd <= 24 && s.P.types[type_of(self)].speed >= 2) {
        d1 = (cmd - 9) >> 2;
        d2 = (cmd - 9) & 3;
    }
    int pos[6];
    bool alive[6];
    for (int i = 0; i < 6; ++i) {
        pos[i] = s.tank[i].pos;
        alive[i] = s.tank[i].alive;
    }
    struct B {
        int p, dir, from;
        bool on;
    };
    B b[MAX_BULLETS];
    const int nb = s.n_bullets;
    for (int k = 0; k < nb; ++k) b[k] = B{s.bullet[k].pos, s.bullet[k].dir, s.bullet[k].pos, true};

    int loss = 0;
    for (int sub = 0; sub < 2; ++sub) {
        const int dirv = sub == 0 ? d1 : d2;
        int moved_from = -1, moved_to = -1;
        if (dirv >= 0 && alive[self]) {
            const int target = pos[self] + DIR_DELTA[dirv];
            bool occ = false;
            for (int i = 0; i < 6; ++i)
                if (alive[i] && pos[i] == target) occ = true;
            if (!occ && s.grid[target] == EMPTY) {
                moved_from = pos[self];
                moved_to = target;
                pos[self] = target;
            }
        }
        for (int k = 0; k < nb; ++k) {
            if (!b[k].on) continue;
            b[k].from = b[k].p;
            b[k].p += DIR_DELTA[b[k].dir];
        }
        for (int a = 0; a < nb; ++a) {
            if (!b[a].on) continue;
            for (int c = a + 1; c < nb; ++c) {
                if (!b[c].on) continue;
                if (b[c].dir == (b[a].dir ^ 1) && b[a].from == b[c].p && b[c].from == b[a].p)
                    b[a].on = b[c].on = false;
            }
        }
        if (moved_from >= 0) {
            for (int k = 0; k < nb; ++k) {
                if (!b[k].on) continue;
                if (b[k].p == moved_from && b[k].from == moved_to) {
                    b[k].on = false;
                    ++loss;
                }
            }
        }
        for (int k = 0; k < nb; ++k) {
            if (!b[k].on) continue;
            const uint8_t g = s.grid[b[k].p];
            if (g == EMPTY) {
                for (int i = 0; i < 6; ++i) {
                    if (alive[i] && pos[i] == b[k].p) {
                        b[k].on = false;
                        if (i == self) ++loss;
                        break;
                    }
                }
            } else if (g != WATER) {
                b[k].on = false;
            }
        }
        for (int a = 0; a < nb; ++a) {
            if (!b[a].on) continue;
            for (int c = a + 1; c < nb; ++c) {
                if (b[c].on && b[c].p == b[a].p) b[a].on = b[c].on = false;
            }
        }
    }
    return loss;
}

// 命令走完大概落在哪（别人不动）。开炮和不动停在原地。
static int dest_of(const State& s, int self, Cmd cmd) {
    const int p0 = s.tank[self].pos;
    int d1 = -1, d2 = -1;
    if (cmd >= 1 && cmd <= 4) d1 = cmd - 1;
    else if (cmd >= 9 && cmd <= 24 && s.P.types[type_of(self)].speed >= 2) {
        d1 = (cmd - 9) >> 2;
        d2 = (cmd - 9) & 3;
    } else {
        return p0;
    }
    auto occ = [&](int cell) {
        for (int i = 0; i < 6; ++i)
            if (i != self && s.tank[i].alive && s.tank[i].pos == cell) return true;
        return false;
    };
    int p = p0;
    if (d1 >= 0) {
        const int n = p + DIR_DELTA[d1];
        if (s.grid[n] == EMPTY && !occ(n)) p = n;
    }
    if (d2 >= 0) {
        const int n = p + DIR_DELTA[d2];
        if (s.grid[n] == EMPTY && !occ(n)) p = n;
    }
    return p;
}

// --------------------------------------------------------------------------
// 评估。只从 g_me 的视角打分，局面已经是 step 之后的结果。
// --------------------------------------------------------------------------

static int ring_bricks(const State& s, int seat) {
    const int b = s.base_pos[seat];
    const int front = seat == 0 ? UP : DOWN;
    const int cells[5] = {
        b + DIR_DELTA[LEFT],
        b + DIR_DELTA[RIGHT],
        b + DIR_DELTA[front],
        b + DIR_DELTA[front] + DIR_DELTA[LEFT],
        b + DIR_DELTA[front] + DIR_DELTA[RIGHT],
    };
    int n = 0;
    for (int c : cells)
        if (s.grid[c] == BRICK) ++n;
    return n;
}

static Score bullet_future(const State& s) {
    Score sc = 0;
    for (int k = 0; k < s.n_bullets; ++k) {
        const Bullet& b = s.bullet[k];
        int pos = b.pos;
        for (int step = 1; step <= 14; ++step) {
            pos += DIR_DELTA[b.dir];
            const int ti = tank_at(s, pos);
            if (ti >= 0) {
                if (step <= 2) {
                    const Tank& t = s.tank[ti];
                    Score val = 25000 + Score(t.level) * 20000 + Score(t.hp) * 4000;
                    sc += seat_of(ti) == g_me ? -val : val;
                }
                break;
            }
            const uint8_t g = s.grid[pos];
            if (g == EMPTY || g == WATER) continue;
            if (g == BRICK || g == STEEL || g == OUTSIDE) break;
            if (g == BASE0 || g == BASE1) {
                const int bs = g - BASE0;
                Score val = step <= 2 ? 90000 : step <= 4 ? 72000 : step <= 8 ? 42000 : 18000;
                sc += bs == g_me ? -val : val;
            }
            break;
        }
    }
    return sc;
}

static Score tank_value(const State& s, int i) {
    const int seat = seat_of(i);
    const int sgn = seat == g_me ? 1 : -1;
    const Tank& t = s.tank[i];
    if (!t.alive) {
        const int w = respawn_in(s, i);
        return Score(sgn) * -(70000 + 9000 * w);
    }
    const int ty = type_of(i);
    Score v = 80000;
    v += Score(t.hp) * 15000;
    v += Score(t.level) * 70000;
    if (t.level >= 1) v += ty == FIRE ? 28000 : 14000;
    if (t.level >= 2) v += 18000;
    const int f = attack_field(s, seat, t.pos, t.level >= 2);
    const int fc = f > 70 ? 70 : f;
    v += Score(70 - fc) * 700;

    // 敌人贴着我方基地：最近的一两辆车改去贴他，其余继续打炮。
    bool hunting = false;
    if (seat != g_me) {
        const int bd = manh(t.pos, s.base_pos[g_me]);
        if (bd <= 12) v += Score(13 - bd) * (13 - bd) * 1400;
    } else {
        const Threat th = nearest_threat(s, g_me);
        if (th.id >= 0 && th.base_d <= 10 && !(ty == FIRE && th.base_d > 2)) {
            const int urgency = 11 - th.base_d;
            const int hd = manh(t.pos, th.pos);
            int closer = 0;
            for (int ty2 = 0; ty2 < 3; ++ty2) {
                const int fi = tank_id(g_me, ty2);
                if (fi == i || !s.tank[fi].alive) continue;
                const int od = manh(s.tank[fi].pos, th.pos);
                if (od < hd || (od == hd && fi < i)) ++closer;
            }
            const bool primary = closer == 0 || (closer == 1 && th.base_d <= 5);
            if (primary) {
                hunting = true;
                if (th.base_d <= 6) v -= Score(70 - fc) * 700;
                v += Score(24 - (hd > 24 ? 24 : hd)) * urgency * 1000;
            }
        }
    }
    if (t.level < 2 && !hunting) {
        const Entry& e = info(s);
        const int wait = star_wait(s.turn);
        const bool soon = wait <= 8;
        int best = INF_D;
        int present = INF_D;
        for (int k = 0; k < s.n_supply; ++k) {
            const int d = e.sup[k][t.pos];
            if ((s.star_mask >> k) & 1) {
                if (d < present) present = d;
            } else if (soon && d < best) {
                best = d;
            }
        }
        if (present < INF_D) v += Score(46 - (present > 46 ? 46 : present)) * 650;
        else if (best < INF_D && wait <= 8) {
            const int wgt = wait <= 3 ? 380 : wait <= 6 ? 220 : 120;
            v += Score(46 - (best > 46 ? 46 : best)) * wgt;
        }
    }
    return Score(sgn) * v;
}

static Score evaluate(const State& s) {
    if (s.done) {
        if (s.winner == g_me) return WIN_SC + s.base_hp[g_me];
        if (s.winner < 0) return 0;
        return -WIN_SC - s.base_hp[g_en];
    }
    Score sc = 0;
    sc += Score(s.base_hp[g_me] - s.base_hp[g_en]) * 100000;
    sc += Score(s.kills[g_me] - s.kills[g_en]) * 28000;
    sc += Score(s.stars_eaten[g_me] - s.stars_eaten[g_en]) * 24000;
    sc += Score(ring_bricks(s, g_me) - ring_bricks(s, g_en)) * 16000;
    for (int i = 0; i < 6; ++i) sc += tank_value(s, i);
    sc += bullet_future(s);
    return sc;
}

// --------------------------------------------------------------------------
// 策略：给一方生成一整套命令。不调用 step，搜索外面才精确推演。
// --------------------------------------------------------------------------

static bool on_hot_line(const State& s, int seat, int cell) {
    const int my_base = s.base_pos[seat];
    for (int ty = 0; ty < 3; ++ty) {
        const int ei = tank_id(1 - seat, ty);
        const Tank& e = s.tank[ei];
        if (!e.alive) continue;
        if (cell_x(e.pos) != cell_x(cell) && cell_y(e.pos) != cell_y(cell)) continue;
        int dir = -1;
        if (cell_x(e.pos) == cell_x(cell)) dir = cell_y(cell) > cell_y(e.pos) ? DOWN : UP;
        else dir = cell_x(cell) > cell_x(e.pos) ? RIGHT : LEFT;
        // 这格是否夹在敌车和我方基地之间，且中间没有钢墙（砖可以）。
        if (cell_x(e.pos) != cell_x(my_base) && cell_y(e.pos) != cell_y(my_base)) continue;
        int bdir = -1;
        if (cell_x(e.pos) == cell_x(my_base)) bdir = cell_y(my_base) > cell_y(e.pos) ? DOWN : UP;
        else bdir = cell_x(my_base) > cell_x(e.pos) ? RIGHT : LEFT;
        if (dir != bdir) continue;
        bool between = false;
        int br = 0;
        bool blocked = false;
        for (int c = e.pos + DIR_DELTA[bdir], n = 0; n < 20; ++n, c += DIR_DELTA[bdir]) {
            if (c == cell) between = true;
            if (c == my_base) break;
            const uint8_t g = s.grid[c];
            if (g == STEEL) {
                blocked = true;
                break;
            }
            if (g == BRICK) ++br;
            else if (g != EMPTY && g != WATER) {
                blocked = true;
                break;
            }
        }
        if (between && !blocked && br <= 1) return true;
    }
    return false;
}

static int exposure(const State& s, int self, int cell) {
    const int seat = seat_of(self);
    const Tank& me = s.tank[self];
    if (!me.alive) return 0;
    int pen = 0;
    const int per = me.hp <= 1 ? 30000 : me.hp == 2 ? 7000 : 3000;
    for (int ty = 0; ty < 3; ++ty) {
        const int ei = tank_id(1 - seat, ty);
        if (!can_fire(s, ei)) continue;
        const Tank& e = s.tank[ei];
        for (int d = 0; d < 4; ++d) {
            int br = 0;
            bool blocked = false;
            int dist = 0;
            for (int c = e.pos + DIR_DELTA[d], n = 1; n <= 12; ++n, c += DIR_DELTA[d]) {
                if (c == cell) {
                    dist = n;
                    break;
                }
                const int ti = tank_at(s, c);
                if (ti >= 0 && ti != self) {
                    blocked = true;
                    break;
                }
                const uint8_t g = s.grid[c];
                if (g == EMPTY || g == WATER) continue;
                if (g == BRICK) {
                    ++br;
                    if (br > 0) {
                        blocked = true;
                        break;
                    }
                } else {
                    blocked = true;
                    break;
                }
            }
            if (!blocked && dist > 0 && dist <= 8) {
                pen += per - dist * 800;
                break;
            }
        }
    }
    return pen < 0 ? 0 : pen;
}

static int nearest_sup(const State& s, int cell, int& dist_out, bool& present) {
    const Entry& e = info(s);
    int best_k = -1;
    int best_d = INF_D;
    present = false;
    const int wait = star_wait(s.turn);
    for (int k = 0; k < s.n_supply; ++k) {
        const int d = e.sup[k][cell];
        if (d >= INF_D) continue;
        const bool has = (s.star_mask >> k) & 1;
        if (!has && wait > 10) continue;
        int key = d - (has ? 6 : 0);
        if (key < best_d) {
            best_d = key;
            best_k = k;
            dist_out = d;
            present = has;
        }
    }
    if (best_k < 0) dist_out = INF_D;
    return best_k;
}

struct Ctx {
    bool base_claimed;
    bool sup_claimed[MAX_SUPPLY];
};

static int score_cmd(const State& s, int seat, int type, Cmd cmd, Mode mode, const Ctx& ctx) {
    const int self = tank_id(seat, type);
    const Tank& t = s.tank[self];
    if (!t.alive) return cmd == STAY ? 0 : -100000000;
    const bool steel_ok = t.level >= 2;
    const int here = t.pos;
    const int dest = (is_fire(cmd) || cmd == STAY) ? here : dest_of(s, self, cmd);
    const int field_here = attack_field(s, seat, here, steel_ok);
    const int field_dest = attack_field(s, seat, dest, steel_ok);

    int sc = 0;
    // 火力型 0 级装填为 0，贴脸炮位可以每回合打基地。它的走位优先炮线，星星让给快速型。
    sc -= field_dest * (type == FIRE ? 480 : 220);

    const int loss = bullet_loss(s, self, cmd);
    if (loss > 0) {
        sc -= loss * 45000;
        if (loss >= t.hp) sc -= 120000 + t.level * 80000;
    }

    // 站在敌方即将复活的出生点上会被撞毁。
    for (int ty = 0; ty < 3; ++ty) {
        const int ei = tank_id(1 - seat, ty);
        if (!s.tank[ei].alive && respawn_in(s, ei) <= 1 && dest == s.spawn[ei]) sc -= 150000;
        const int fi = tank_id(seat, ty);
        if (fi != self && !s.tank[fi].alive && respawn_in(s, fi) <= 1 && dest == s.spawn[fi]) sc -= 40000;
    }

    sc -= exposure(s, self, dest) * (mode == M_DEF ? 1 : mode == M_AGG ? 0 : 1);
    // 暗杀模式自己也要躲必死子弹（上面的 loss 已经管了），但不怕「可能被瞄」。
    if (mode == M_AGG) {
        // 把 exposure 加回去一半？上面乘了 0，等于不怕被瞄，专门冲人。保持 0。
    }

    if (on_hot_line(s, seat, dest)) sc += mode == M_DEF ? 14000 : 6000;

    if (seat == g_me && g_stuck[type] >= 5 && cmd == STAY) sc -= 6000;
    if (seat == g_me && cmd == g_lastcmd[type] && is_move(cmd)) sc += 200;

    const bool behind = s.base_hp[seat] + 5 < s.base_hp[1 - seat];
    const bool ahead = s.base_hp[1 - seat] + 6 < s.base_hp[seat];

    const Threat th = nearest_threat(s, seat);
    bool hunter = false;
    int urgency = 0;
    // 火力型负责连射，除非敌人已经贴到基地上，否则不改去追人。
    if (th.id >= 0 && th.base_d <= 10 && !(type == FIRE && th.base_d > 2)) {
        urgency = 11 - th.base_d;
        const int hd_here = manh(here, th.pos);
        const int hd_dest = manh(dest, th.pos);
        int closer = 0;
        for (int ty2 = 0; ty2 < 3; ++ty2) {
            const int fi = tank_id(seat, ty2);
            if (fi == self || !s.tank[fi].alive) continue;
            const int od = manh(s.tank[fi].pos, th.pos);
            if (od < hd_here || (od == hd_here && fi < self)) ++closer;
        }
        hunter = closer == 0 || (closer == 1 && th.base_d <= 5);
        if (hunter) {
            if (th.base_d <= 6) sc += field_dest * 190;
            sc += (hd_here - hd_dest) * urgency * 900;
            if (hd_dest <= 2 && th.base_d <= 6) sc += 5000 * (3 - hd_dest);
        }
    }
    if (s.P.types[type].speed >= 2 && cmd >= 9 && manh(here, dest) >= 2 && loss == 0) sc += 900;

    // 已在飞向我方基地的子弹：血量够的车去挡一发（每回合基地最多掉 1）。
    if (!is_fire(cmd) && t.hp >= 2 && loss == 0) {
        const int mb = s.base_pos[seat];
        for (int k = 0; k < s.n_bullets; ++k) {
            const Bullet& b = s.bullet[k];
            if (seat_of(b.owner) == seat) continue;
            int c = b.pos;
            bool on_path = false;
            bool reaches = false;
            for (int n = 0; n < 12; ++n) {
                c += DIR_DELTA[b.dir];
                if (c == dest && c != mb) on_path = true;
                if (c == mb) {
                    reaches = true;
                    break;
                }
                const uint8_t g = s.grid[c];
                if (g != EMPTY && g != WATER && c != dest) break;
            }
            if (reaches && on_path) sc += 22000;
        }
    }

    if (is_fire(cmd)) {
        const int dir = cmd - 5;
        const Ray r = trace(s, here, dir, seat);
        if (r.kind == 5) {
            const int tid = r.id;
            if (seat_of(tid) == seat) {
                sc -= 80000;
            } else {
                const Tank& e = s.tank[tid];
                int v = 18000 + e.level * 14000;
                if (e.hp <= 1) v += 22000;
                else if (e.hp == 2) v += 8000;
                const int ef = attack_field(s, 1 - seat, e.pos, e.level >= 2);
                if (ef <= 10) v += 18000;  // 对方的炮位车
                const int md = manh(e.pos, s.base_pos[seat]);
                if (md <= 3) v += 70000;
                else if (md <= 6) v += (7 - md) * (7 - md) * 1200;
                if (r.dist <= 2) v += 6000;
                if (mode == M_AGG) v += 22000;
                if (ahead) v += 4000;
                sc += v;
            }
        } else if (r.kind == 3) {
            int v = 56000;
            if (r.dist <= 2) v += 24000;
            else if (r.dist <= 4) v += 14000;
            else if (r.dist >= 8) v -= 10000;
            if (behind) v += 10000;
            if (mode == M_AGG) v -= 6000;
            if (mode == M_STAR) v -= 8000;
            if (ctx.base_claimed) v -= 34000;
            if (ahead && t.hp <= 1) v -= 6000;  // 领先就别用脆皮换一发
            if (hunter && th.base_d <= 3 && r.dist > 3) v -= 30000;
            sc += v;
        } else if (r.kind == 4) {
            sc -= 120000;
        } else if (r.kind == 1 || (r.kind == 2 && steel_ok)) {
            const int bc = r.cell;
            const int eb = s.base_pos[1 - seat];
            const int mb = s.base_pos[seat];
            const bool enemy_ring = iabs(cell_x(bc) - cell_x(eb)) + iabs(cell_y(bc) - cell_y(eb)) <= 2 &&
                                    iabs(cell_x(bc) - cell_x(eb)) <= 1 && iabs(cell_y(bc) - cell_y(eb)) <= 1;
            // 曼哈顿不太准，直接看是不是围墙五格之一。
            int front = seat == 0 ? UP : DOWN;  // 这是「我的」前方；敌方基地的前方相反
            (void)front;
            const int efront = (1 - seat) == 0 ? UP : DOWN;
            const int ringc[5] = {
                eb + DIR_DELTA[LEFT],
                eb + DIR_DELTA[RIGHT],
                eb + DIR_DELTA[efront],
                eb + DIR_DELTA[efront] + DIR_DELTA[LEFT],
                eb + DIR_DELTA[efront] + DIR_DELTA[RIGHT],
            };
            bool is_ering = false, is_mring = false;
            const int mfront = seat == 0 ? UP : DOWN;
            const int mring[5] = {
                mb + DIR_DELTA[LEFT],
                mb + DIR_DELTA[RIGHT],
                mb + DIR_DELTA[mfront],
                mb + DIR_DELTA[mfront] + DIR_DELTA[LEFT],
                mb + DIR_DELTA[mfront] + DIR_DELTA[RIGHT],
            };
            for (int c : ringc)
                if (c == bc) is_ering = true;
            for (int c : mring)
                if (c == bc) is_mring = true;
            (void)enemy_ring;
            if (is_mring) sc -= 50000;
            else if (is_ering) sc += 22000;
            else {
                // 这堵墙在不在自己的进攻梯度上：墙格的攻击场比自己低，说明拆了能靠近炮位。
                const int fb = attack_field(s, seat, bc, steel_ok);
                if (fb + 1 < field_here) sc += 6000 + (field_here - fb) * 180;
                else sc += 800;
            }
        } else if (r.kind == 2) {
            sc -= 4000;  // 打不掉的钢
        } else {
            sc -= 3500;  // 空炮
        }
        // 迎着飞来的子弹开炮，能抵消。
        if (s.n_bullets > 0) {
            const int dirv = cmd - 5;
            for (int k = 0; k < s.n_bullets; ++k) {
                const Bullet& b = s.bullet[k];
                if (b.dir != (dirv ^ 1)) continue;
                if (seat_of(b.owner) == seat) continue;
                bool aligned = false;
                int dist = 0;
                for (int c = here + DIR_DELTA[dirv], n = 1; n <= 8; ++n, c += DIR_DELTA[dirv]) {
                    if (c == b.pos) {
                        aligned = true;
                        dist = n;
                        break;
                    }
                    const uint8_t g = s.grid[c];
                    if (g != EMPTY && g != WATER) break;
                }
                if (aligned && dist <= 6) sc += dist <= 3 ? 18000 : 8000;
            }
        }
    } else {
        // 走子或不动：星星梯度。
        if (t.level < 2) {
            int sd = INF_D;
            bool present = false;
            const int k = nearest_sup(s, dest, sd, present);
            if (k >= 0 && sd < INF_D) {
                int w = 0;
                const int wait = star_wait(s.turn);
                if (present) w = 1600;
                else if (wait <= 3) w = 1100;
                else if (wait <= 6) w = 700;
                else if (wait <= 10) w = 280;
                if (type == FAST && t.level < 2) w = w * 2;
                if (mode == M_STAR) w = w * 3 / 2;
                if (mode == M_AGG) w = w / 3;
                if (mode == M_DEF) w = w / 2;
                if (field_here <= 8 && mode != M_STAR && type != FAST) w /= 3;
                if (s.base_hp[1 - seat] <= 6) w /= 3;
                if (ctx.sup_claimed[k]) w /= 4;
                sc += (42 - (sd > 42 ? 42 : sd)) * w / 3;
                if (present && sd == 0) sc += mode == M_AGG ? 8000 : 22000;
            }
        }
        // 已经站在绝佳炮位就别闲逛，除非在躲弹（loss 已经另算）。
        if (cmd == STAY && field_here <= 6) sc += 2500;
    }
    return sc;
}

static Orders make_policy(const State& s, int seat, Mode mode) {
    Orders o;
    int order[3] = {FIRE, ARMOR, FAST};
    if (mode == M_STAR) {
        order[0] = FAST;
        order[1] = FIRE;
        order[2] = ARMOR;
    }
    // 会被现有子弹打死的车先选，免得把「打基地」名额占掉。
    auto lethal = [&](int ty) {
        const int i = tank_id(seat, ty);
        if (!s.tank[i].alive) return false;
        const int loss = bullet_loss(s, i, STAY);
        return loss >= s.tank[i].hp && loss > 0;
    };
    for (int a = 0; a < 3; ++a) {
        if (!lethal(order[a])) continue;
        const int v = order[a];
        for (int b = a; b > 0; --b) order[b] = order[b - 1];
        order[0] = v;
    }
    Ctx ctx{};
    ctx.base_claimed = false;
    for (int k = 0; k < MAX_SUPPLY; ++k) ctx.sup_claimed[k] = false;

    for (int n = 0; n < 3; ++n) {
        const int ty = order[n];
        const int self = tank_id(seat, ty);
        if (!s.tank[self].alive) {
            o.cmd[ty] = STAY;
            continue;
        }
        Cmd list[N_CMDS];
        const int nc = legal_commands(s, self, list);
        int best_sc = -2000000000;
        Cmd best = STAY;
        for (int i = 0; i < nc; ++i) {
            const int sc = score_cmd(s, seat, ty, list[i], mode, ctx);
            if (sc > best_sc) {
                best_sc = sc;
                best = list[i];
            }
        }
        o.cmd[ty] = best;
        if (is_fire(best)) {
            const Ray r = trace(s, s.tank[self].pos, best - 5, seat);
            if (r.kind == 3) ctx.base_claimed = true;
        } else if (s.tank[self].level < 2) {
            int sd = INF_D;
            bool present = false;
            const int dest = dest_of(s, self, best);
            const int k = nearest_sup(s, dest, sd, present);
            if (k >= 0 && sd <= 8) ctx.sup_claimed[k] = true;
        }
    }
    return o;
}

static Orders tilt_fire(const State& s, int seat, Orders base, int want) {
    // want 1: 能打到敌方基地就打。want 2: 能打到敌方坦克就打。
    for (int ty = 0; ty < 3; ++ty) {
        const int self = tank_id(seat, ty);
        if (!can_fire(s, self)) continue;
        const int pos = s.tank[self].pos;
        for (int d = 0; d < 4; ++d) {
            const Ray r = trace(s, pos, d, seat);
            if (want == 1 && r.kind == 3) {
                base.cmd[ty] = cmd_fire(d);
                break;
            }
            if (want == 2 && r.kind == 5 && seat_of(r.id) != seat) {
                base.cmd[ty] = cmd_fire(d);
                break;
            }
        }
    }
    return base;
}

// 贴脸炮线推进：同一回合只派一辆车打基地（基地每回合最多掉 1），其余沿攻击场靠近短炮线。
// fast_star 时，还没满级的快速型改去吃星星。
static Orders rush_base(const State& s, int seat, bool fast_star) {
    Orders o;
    bool assigned[3] = {false, false, false};
    const int target = s.base_pos[1 - seat];
    for (int ty = 0; ty < 3; ++ty) o.cmd[ty] = STAY;

    if (fast_star) {
        const int self = tank_id(seat, FAST);
        if (s.tank[self].alive && s.tank[self].level < 2 &&
            (s.star_mask != 0 || star_wait(s.turn) <= 8)) {
            Cmd list[N_CMDS];
            const int nc = legal_commands(s, self, list);
            int best_sc = -1000000000;
            Cmd best = STAY;
            for (int i = 0; i < nc; ++i) {
                if (is_fire(list[i])) continue;
                const int dest = dest_of(s, self, list[i]);
                int sd = INF_D;
                bool present = false;
                nearest_sup(s, dest, sd, present);
                int sc = -(sd >= INF_D ? 80 : sd) * 200;
                if (present && sd == 0) sc += 8000;
                const int loss = bullet_loss(s, self, list[i]);
                if (loss >= s.tank[self].hp && loss > 0) sc -= 80000;
                if (sc > best_sc) {
                    best_sc = sc;
                    best = list[i];
                }
            }
            o.cmd[FAST] = best;
            assigned[FAST] = true;
        }
    }

    int best_ty = -1, best_d = -1, best_br = 99, best_len = 99;
    for (int ty = 0; ty < 3; ++ty) {
        if (assigned[ty]) continue;
        const int self = tank_id(seat, ty);
        if (!s.tank[self].alive || !can_fire(s, self)) continue;
        const int pos = s.tank[self].pos;
        const bool steel = s.tank[self].level >= 2;
        for (int d = 0; d < 4; ++d) {
            const int br = bricks_to_base(s, pos, d, target, steel);
            if (br > 1) continue;
            const Ray r = trace(s, pos, d, seat);
            if (r.kind == 4) continue;
            if (r.kind == 5 && seat_of(r.id) == seat) continue;
            int len = 0;
            for (int p = pos + DIR_DELTA[d]; p != target && len < 20; p += DIR_DELTA[d]) ++len;
            const bool prefer_fire = (br == best_br && len == best_len && ty == FIRE && best_ty != FIRE);
            if (br < best_br || (br == best_br && len < best_len) || prefer_fire) {
                best_br = br;
                best_len = len;
                best_d = d;
                best_ty = ty;
            }
        }
    }
    if (best_ty >= 0) {
        o.cmd[best_ty] = cmd_fire(best_d);
        assigned[best_ty] = true;
    }

    for (int ty = 0; ty < 3; ++ty) {
        if (assigned[ty]) continue;
        const int self = tank_id(seat, ty);
        if (!s.tank[self].alive) continue;
        const int pos = s.tank[self].pos;
        const bool steel = s.tank[self].level >= 2;
        Cmd list[N_CMDS];
        const int nc = legal_commands(s, self, list);
        int best_sc = -1000000000;
        Cmd best = STAY;
        const int here_f = attack_field(s, seat, pos, steel);
        for (int i = 0; i < nc; ++i) {
            if (is_fire(list[i])) continue;
            const int dest = dest_of(s, self, list[i]);
            const int dest_f = attack_field(s, seat, dest, steel);
            int sc = (here_f - dest_f) * 1000;
            const int loss = bullet_loss(s, self, list[i]);
            if (loss >= s.tank[self].hp && loss > 0) sc -= 80000;
            else sc -= loss * 5000;
            if (sc > best_sc) {
                best_sc = sc;
                best = list[i];
            }
        }
        o.cmd[ty] = best;
    }
    return o;
}

// --------------------------------------------------------------------------
// 决策
// --------------------------------------------------------------------------

static bool same_ord(const Orders& a, const Orders& b) {
    return a.cmd[0] == b.cmd[0] && a.cmd[1] == b.cmd[1] && a.cmd[2] == b.cmd[2];
}

static Score rollout(State st, int plies) {
    for (int p = 0; p < plies && !st.done; ++p) {
        if (!time_ok(3.2)) break;
        const Orders my = make_policy(st, g_me, M_BAL);
        const Orders en = make_policy(st, g_en, M_BAL);
        step_as(st, g_me, my, en);
        ++g_nodes;
    }
    return evaluate(st);
}

static Score vs_list(const State& s, const Orders& mine, const Orders* ens, int ne, int plies) {
    Score worst = WIN_SC;
    for (int j = 0; j < ne; ++j) {
        if (!time_ok(3.0)) return -WIN_SC;  // 没比完，不能拿来当最好
        State st = s;
        step_as(st, g_me, mine, ens[j]);
        ++g_nodes;
        const Score sc = plies <= 0 ? evaluate(st) : rollout(st, plies);
        if (sc < worst) worst = sc;
    }
    return worst;
}

void init(const State& s, int me) {
    (void)s;
    g_me = me;
    g_en = 1 - me;
    for (int i = 0; i < 3; ++i) {
        g_stuck[i] = 0;
        g_lastpos[i] = -1;
        g_lastcmd[i] = STAY;
    }
    for (Entry& e : g_cache) e.valid = false;
    g_clock = 0;
    g_nodes = 0;
    g_stop = false;
}

Orders decide(const State& s, int me) {
    g_me = me;
    g_en = 1 - me;
    g_nodes = 0;
    g_stop = false;
    for (int ty = 0; ty < 3; ++ty) {
        const Tank& t = s.tank[tank_id(me, ty)];
        if (!t.alive) {
            g_stuck[ty] = 0;
            g_lastpos[ty] = -1;
            continue;
        }
        g_stuck[ty] = (t.pos == g_lastpos[ty]) ? g_stuck[ty] + 1 : 0;
        g_lastpos[ty] = t.pos;
    }

    // 先把距离场建好，避免枚举中途卡在第一次填充上。
    (void)info(s);

    Orders mine_pol[M_N];
    for (int m = 0; m < M_N; ++m) mine_pol[m] = make_policy(s, me, Mode(m));

    Orders ens[12];
    int ne = 0;
    auto push_en = [&](Orders o) {
        for (int i = 0; i < ne; ++i)
            if (same_ord(ens[i], o)) return;
        if (ne < 12) ens[ne++] = o;
    };
    Orders en_pol[M_N];
    for (int m = 0; m < M_N; ++m) {
        en_pol[m] = make_policy(s, g_en, Mode(m));
        push_en(en_pol[m]);
    }
    push_en(Orders{});
    push_en(tilt_fire(s, g_en, en_pol[M_BAL], 1));
    push_en(tilt_fire(s, g_en, en_pol[M_AGG], 2));
    const Orders rush_en = rush_base(s, g_en, false);
    push_en(rush_en);

    // 一回合枚举用的三个威胁：暗杀、朝基地推进、原地。
    const Orders trio[3] = {
        en_pol[M_AGG],
        rush_en,
        Orders{},
    };

    Cmd c0[N_CMDS], c1[N_CMDS], c2[N_CMDS];
    const int n0 = legal_commands(s, tank_id(me, FAST), c0);
    const int n1 = legal_commands(s, tank_id(me, FIRE), c1);
    const int n2 = legal_commands(s, tank_id(me, ARMOR), c2);

    constexpr int TOPN = 16;
    Orders top_o[TOPN];
    Score top_s[TOPN];
    int nt = 0;
    auto consider = [&](const Orders& o, Score sc) {
        if (nt < TOPN) {
            top_o[nt] = o;
            top_s[nt] = sc;
            ++nt;
            return;
        }
        int w = 0;
        for (int i = 1; i < nt; ++i)
            if (top_s[i] < top_s[w]) w = i;
        if (sc > top_s[w]) {
            top_s[w] = sc;
            top_o[w] = o;
        }
    };

    bool enum_done = true;
    for (int i = 0; i < n0 && enum_done; ++i) {
        for (int j = 0; j < n1 && enum_done; ++j) {
            for (int k = 0; k < n2; ++k) {
                if (!time_ok(16.0)) {
                    enum_done = false;
                    break;
                }
                Orders o;
                o.cmd[FAST] = c0[i];
                o.cmd[FIRE] = c1[j];
                o.cmd[ARMOR] = c2[k];
                Score worst = WIN_SC;
                for (int t = 0; t < 3; ++t) {
                    State st = s;
                    step_as(st, me, o, trio[t]);
                    ++g_nodes;
                    const Score sc = evaluate(st);
                    if (sc < worst) worst = sc;
                }
                consider(o, worst);
            }
        }
    }

    Orders fins[24];
    int nf = 0;
    auto push_fin = [&](Orders o) {
        for (int i = 0; i < nf; ++i)
            if (same_ord(fins[i], o)) return;
        if (nf < 24) fins[nf++] = o;
    };
    // 一回合里看起来最好的排前面，时间不够时至少它们被完整看过。
    int idx[TOPN];
    for (int i = 0; i < nt; ++i) idx[i] = i;
    for (int a = 0; a < nt; ++a) {
        int b = a;
        for (int c = a + 1; c < nt; ++c)
            if (top_s[idx[c]] > top_s[idx[b]]) b = c;
        const int tmp = idx[a];
        idx[a] = idx[b];
        idx[b] = tmp;
    }
    for (int i = 0; i < nt; ++i) push_fin(top_o[idx[i]]);
    for (int m = 0; m < M_N; ++m) push_fin(mine_pol[m]);
    push_fin(rush_base(s, me, true));

    const double left0 = budget_left();
    int plies = 7;
    if (left0 > 600) plies = 14;
    else if (left0 > 80) plies = 10;
    else if (left0 < 22) plies = 3;
    else if (left0 < 35) plies = 5;

    // 终选时用全部对手模型，但时间紧就只留前几个最危险的。
    int use_ne = ne;
    if (left0 < 28) use_ne = 3;
    else if (left0 < 45) use_ne = 5;

    Score best_sc = -WIN_SC;
    Orders best = mine_pol[M_BAL];
    bool any = false;
    for (int i = 0; i < nf; ++i) {
        if (budget_left() < 6.0 && any) break;
#if !FIXED_NODES
        if (g_stop && any) break;
#endif
        const int use_plies = (budget_left() < 12.0 && any) ? 0 : plies;
        const Score sc = vs_list(s, fins[i], ens, use_ne, use_plies);
        if (!any || sc > best_sc) {
            best_sc = sc;
            best = fins[i];
            any = true;
        }
    }

    for (int ty = 0; ty < 3; ++ty) g_lastcmd[ty] = best.cmd[ty];

    std::fprintf(stderr, "t=%d me=%d n=%ld left=%.1f plies=%d fins=%d ens=%d sc=%lld [%s %s %s]\n",
                 int(s.turn), me, g_nodes, budget_left(), plies, nf, use_ne,
                 (long long)best_sc, cmd_text(best.cmd[0]).c_str(), cmd_text(best.cmd[1]).c_str(),
                 cmd_text(best.cmd[2]).c_str());
    return best;
}

// bot.cpp —— 坦克大战 bot
//
// 设计：脚本化角色策略（决定"该干什么"）+ 单步精确 maximin（决定"这一回合怎么安全地干"）。
//
// 角色：
//   火力型 FIRE：主攻。沿"侧翼开炮走廊"（与敌方基地同行的一行）冲到敌基旁，清砖、点基地。
//   装甲型 ARMOR：防守反击。守在己方基地侧翼的开炮位：谁靠近我方基地就打谁、挡炮线；
//                 我方基地安全时再沿第二条走廊前压。
//   快速型 FAST：抢星（场上星→最近补给点），顺手点杀/绕后；满级后转侧翼火力。
//
// 每辆车每回合的命令由"策略建议"（开炮优先级：敌车>敌基地>挡路砖；否则沿走廊移动）给出，
// 再用精确 step 做一层 maximin：在双方候选命令的笛卡尔积上，
// 只允许"无论对方怎么下、本回合都不会被白白打中"的我方联合命令，
// 在其中选角色目标推进最多 / 评估值最高的一条。搜索量很小，预算充裕。
#include "tank.hpp"

#include <algorithm>

using namespace tank;

namespace {

constexpr int INF = 1000000;
constexpr int MAXJ = 400;

int G_ME;

// 距离场
int g_atk[2][CELLS];              // 到某基地"开炮位"的代价（砖代价 2）
int g_walk[CELLS];                // 纯空地 BFS（不穿砖）
int g_star[MAX_SUPPLY][CELLS];
uint64_t g_wall_hash = ~0ULL;

// 每个基地的两条侧翼开炮走廊：lane_cell[base][side] = 与基地同行、x = base.x±k 的开炮格
int g_lane[2][2];                 // [base][0/1 左右]
int g_basecol_dir[2][2];          // 朝基地的开炮方向（LEFT/RIGHT）

// ---------------- 距离场 ----------------
void dijkstra(const State& s, const int* seeds, const int* costs, int nseed, int* dist, int brickW) {
    bool done[CELLS] = {};
    for (int c = 0; c < CELLS; ++c) dist[c] = INF;
    for (int k = 0; k < nseed; ++k) dist[seeds[k]] = std::min(dist[seeds[k]], costs[k]);
    for (int it = 0; it < CELLS; ++it) {
        int u = -1, best = INF;
        for (int c = 0; c < CELLS; ++c)
            if (!done[c] && dist[c] < best) { best = dist[c]; u = c; }
        if (u < 0) break;
        done[u] = true;
        for (int d = 0; d < 4; ++d) {
            const int v = u + DIR_DELTA[d];
            const uint8_t g = s.grid[v];
            int w = 0;
            if (g == EMPTY) w = 1;
            else if (g == BRICK && brickW > 0) w = brickW;
            if (w && dist[u] + w < dist[v]) dist[v] = dist[u] + w;
        }
    }
}

void bfs_walk(const State& s, int src, int* dist) {
    for (int c = 0; c < CELLS; ++c) dist[c] = INF;
    int q[CELLS], head = 0, tail = 0;
    dist[src] = 0; q[tail++] = src;
    while (head < tail) {
        const int u = q[head++];
        for (int d = 0; d < 4; ++d) {
            const int v = u + DIR_DELTA[d];
            if (s.grid[v] == EMPTY && dist[v] == INF) { dist[v] = dist[u] + 1; q[tail++] = v; }
        }
    }
}

// 找到基地同行（侧翼）最近的可站立开炮格；返回格子，朝基地的方向写回 fd。
// 只用当前地形：格子本身空或砖（砖可以清），与基地之间无钢墙/水以外的阻挡（砖可清，水不挡弹）。
int find_lane_cell(const State& s, int bcell, int lr, int& fd) {
    const int bx = cell_x(bcell), by = cell_y(bcell);
    const int dx = lr == 0 ? -1 : 1;
    for (int k = 2; k < 14; ++k) {
        const int x = bx + dx * k;
        if (x < 0 || x > 14) break;
        const int c = cell_of(x, by);
        const uint8_t g = s.grid[c];
        if (g != EMPTY && g != BRICK) break;      // 钢/水/界外，这条走廊断
        // 与基地之间不能有钢墙
        bool steel = false;
        for (int xx = bx + dx; xx != x; xx += dx)
            if (s.grid[cell_of(xx, by)] == STEEL) { steel = true; break; }
        if (steel) continue;
        fd = dx < 0 ? RIGHT : LEFT;
        return c;
    }
    return NO_CELL;
}

// 进攻距离场：seed = 所有"炮线能到基地"的格子，代价 = 清掉射线上砖的开炮回合数
void build_attack(const State& s, int bcell, int* dist) {
    int seeds[CELLS], costs[CELLS], n = 0;
    const int sz = s.P.size;
    for (int y = 0; y < sz; ++y)
        for (int x = 0; x < sz; ++x) {
            const int c = cell_of(x, y);
            const uint8_t g = s.grid[c];
            if (g != EMPTY && g != BRICK) continue;
            int dir = -1;
            if (x == cell_x(bcell) && y != cell_y(bcell)) dir = cell_y(bcell) < y ? UP : DOWN;
            else if (y == cell_y(bcell) && x != cell_x(bcell)) dir = cell_x(bcell) < x ? LEFT : RIGHT;
            else continue;
            int bricks = 0; bool bad = false;
            for (int p = c + DIR_DELTA[dir]; p != bcell; p += DIR_DELTA[dir]) {
                const uint8_t gg = s.grid[p];
                if (gg == STEEL || gg == OUTSIDE || gg == BASE0 || gg == BASE1) { bad = true; break; }
                if (gg == BRICK) ++bricks;
            }
            if (bad) continue;
            seeds[n] = c; costs[n] = bricks + (g == BRICK ? 1 : 0); ++n;
        }
    dijkstra(s, seeds, costs, n, dist, 2);
}

void ensure_dist(const State& s) {
    if (s.wall_hash == g_wall_hash) return;
    g_wall_hash = s.wall_hash;
    build_attack(s, s.base_pos[0], g_atk[0]);
    build_attack(s, s.base_pos[1], g_atk[1]);
    bfs_walk(s, s.base_pos[0], g_walk);   // 占位，后面每回合按我方基地重算
    for (int k = 0; k < s.n_supply; ++k) bfs_walk(s, s.supply[k], g_star[k]);
    for (int b = 0; b < 2; ++b)
        for (int lr = 0; lr < 2; ++lr) {
            int fd = -1;
            g_lane[b][lr] = find_lane_cell(s, s.base_pos[b], lr, fd);
            g_basecol_dir[b][lr] = fd;
        }
}

inline int adist(int base, int c) { int d = g_atk[base][c]; return d >= INF ? 99 : d; }

// ---------------- 射线 ----------------
struct Ray { int kind, tank, seat, cell, dist; };
Ray raycast(const State& s, int pos, int d) {
    int c = pos;
    for (int k = 1; k <= 16; ++k) {
        c += DIR_DELTA[d];
        const int tv = tank_at(s, c);
        if (tv >= 0) return {1, tv, -1, c, k};
        const uint8_t g = s.grid[c];
        if (g == BASE0 || g == BASE1) return {2, -1, g - BASE0, c, k};
        if (g == BRICK) return {3, -1, -1, c, k};
        if (g == STEEL) return {4, -1, -1, c, k};
        if (g == OUTSIDE) return {6, -1, -1, c, k};
    }
    return {0, -1, -1, NO_CELL, 0};
}

// 一辆坦克在我方某方命令 o 下、对方全不动，结算后是否会被打中
bool tank_hit(const State& s, int i, const Orders& mine) {
    State t = s;
    const int side = seat_of(i);
    Orders blank;
    if (side == 0) step(t, mine, blank); else step(t, blank, mine);
    return !t.tank[i].alive || t.tank[i].hp < s.tank[i].hp;
}

// ---------------- 角色：单辆车的建议命令 ----------------
// 距离场（空地 BFS）每回合在我家基地附近重算，用于防守走位
int g_home[CELLS];

// 朝距离场最小的可走邻格走；快速型给两步。返回 true 给了移动命令。
bool step_toward(const State& s, int i, const int* field, Cmd& out, int cap) {
    const int pos = s.tank[i].pos;
    int bestD = field[pos], bd = -1;
    for (int d = 0; d < 4; ++d) {
        const int c = pos + DIR_DELTA[d];
        if (s.grid[c] != EMPTY) continue;
        if (tank_at(s, c) >= 0) continue;
        if (field[c] < bestD) { bestD = field[c]; bd = d; }
    }
    if (bd < 0) return false;
    if (type_of(i) != FAST || cap < 2) { out = cmd_move(bd); return true; }
    // 第二步继续沿场走
    const int p1 = pos + DIR_DELTA[bd];
    int d2 = -1, b2 = field[p1];
    for (int d = 0; d < 4; ++d) {
        int c = p1 + DIR_DELTA[d];
        if (s.grid[c] != EMPTY) continue;
        if (tank_at(s, c) >= 0) continue;
        if (field[c] < b2) { b2 = field[c]; d2 = d; }
    }
    out = d2 >= 0 ? cmd_move2(bd, d2) : cmd_move(bd);
    return true;
}

// 高价值开炮方向：敌车 / 敌基地 / 挡路砖。返回 -1 表示没有。
int aim_dir(const State& s, int i, bool allowBrick, const int* brickField) {
    const int me = seat_of(i), foe = 1 - me;
    int best = -1; double bestV = -1e18;
    for (int d = 0; d < 4; ++d) {
        if (!can_fire(s, i)) break;
        const Ray r = raycast(s, s.tank[i].pos, d);
        double v = -1e18;
        if (r.kind == 1) {
            const int tv = r.tank;
            if (seat_of(tv) == foe) v = 1000 - r.dist * 10 + s.tank[tv].hp * 20;
            else v = -1e9;   // 队友
        } else if (r.kind == 2) {
            if (r.seat == foe) v = 900 - r.dist;
            else v = -1e9;   // 自己基地
        } else if (r.kind == 3 && allowBrick && brickField) {
            if (brickField[r.cell] < brickField[s.tank[i].pos]) v = 500 - r.dist;
        }
        if (v > bestV) { bestV = v; best = d; }
    }
    return bestV > 0 ? best : -1;
}

// 每辆车的候选命令：策略建议 + 合法移动 + STAY
int policy_candidates(const State& s, int i, Cmd* out, const int homeBfs[CELLS]) {
    Cmd leg[N_CMDS];
    const int L = legal_commands(s, i, leg);
    Cmd set[40]; int n = 0;
    auto add = [&](Cmd c) {
        for (int k = 0; k < n; ++k) if (set[k] == c) return;
        if (n < 40) set[n++] = c;
    };
    add(STAY);
    const int me = seat_of(i), foe = 1 - me, ty = type_of(i);

    // 选这一回合跟随的距离场
    const int* field = nullptr;
    bool brickOk = true;
    if (ty == FAST) {
        int sk = -1;
        for (int k = 0; k < s.n_supply; ++k)
            if (((s.star_mask >> k) & 1) && (sk < 0 || g_star[k][s.tank[i].pos] < g_star[sk][s.tank[i].pos])) sk = k;
        if (sk < 0) {
            if (s.tank[i].level < 2) {
                for (int k = 0; k < s.n_supply; ++k) if (sk < 0 || g_star[k][s.tank[i].pos] < g_star[sk][s.tank[i].pos]) sk = k;
                field = sk >= 0 ? g_star[sk] : g_atk[foe];
            } else field = g_atk[foe];
        } else field = g_star[sk];
        brickOk = false;
    } else if (ty == FIRE) {
        field = g_atk[foe];
    } else {
        // ARMOR：敌方威胁近则守家（homeBfs 以我方基地侧翼开炮位为源），否则进攻
        int threat = INF;
        for (int j = 3 * foe; j < 3 * foe + 3; ++j)
            if (s.tank[j].alive) threat = std::min(threat, adist(me, s.tank[j].pos));
        field = (threat <= 22) ? homeBfs : g_atk[foe];
        brickOk = true;
    }

    // 开炮建议
    const int ad = aim_dir(s, i, brickOk && field == g_atk[foe], g_atk[foe]);
    if (ad >= 0) add(cmd_fire(ad));
    // 装甲守家时：朝靠近我方基地的敌车方向开砖/敌车已在 aim_dir 覆盖；再允许守家位补位移动

    // 移动建议（沿场）
    Cmd mv = STAY;
    if (step_toward(s, i, field, mv, 2)) add(mv);

    // 补充：所有直接朝敌车/敌基地的炮
    for (int d = 0; d < 4; ++d) {
        if (!can_fire(s, i)) break;
        const Ray r = raycast(s, s.tank[i].pos, d);
        if (r.kind == 1 && seat_of(r.tank) == foe) add(cmd_fire(d));
        if (r.kind == 2 && r.seat == foe) add(cmd_fire(d));
    }
    // 补充：所有安全的合法移动（躲闪选项），最多再加几条
    for (int k = 0; k < L && n < 12; ++k)
        if (is_move(leg[k]) || is_move2(leg[k])) add(leg[k]);
    // 所有能打的炮（含清砖）
    for (int k = 0; k < L && n < 14; ++k)
        if (is_fire(leg[k])) add(leg[k]);

    for (int k = 0; k < n; ++k) out[k] = set[k];
    return n;
}

// ---------------- 评估（用于在安全命令里挑） ----------------
double evaluate(const State& t, int me) {
    if (t.done) return t.winner < 0 ? 0.0 : (t.winner == me ? 1e9 - t.turn : -1e9 + t.turn);
    const int foe = 1 - me;
    double v = 300.0 * (t.base_hp[me] - t.base_hp[foe]);
    for (int i = 0; i < 6; ++i) {
        const Tank& tk = t.tank[i];
        const double sg = seat_of(i) == me ? 1 : -1;
        if (tk.alive) v += sg * (60 + 40 * tk.hp + 50 * tk.level);
    }
    v += 10.0 * (t.kills[me] - t.kills[foe]);
    v += 24.0 * (t.stars_eaten[me] - t.stars_eaten[foe]);
    for (int i = 0; i < 6; ++i) {
        const Tank& tk = t.tank[i];
        if (!tk.alive) continue;
        if (seat_of(i) == me) v -= 4.0 * adist(foe, tk.pos);
        else v += 4.0 * adist(me, tk.pos);
    }
    // 在飞子弹
    for (int b = 0; b < t.n_bullets; ++b) {
        const Bullet& bu = t.bullet[b];
        int c = bu.pos;
        for (int k = 1; k <= 4; ++k) {
            c += DIR_DELTA[bu.dir];
            const int tv = tank_at(t, c);
            if (tv >= 0) {
                const double sg = seat_of(tv) == me ? -1 : 1;
                v += sg * (120 - 12 * k) * (seat_of(bu.owner) == seat_of(tv) ? 1.3 : 1.0);
                break;
            }
            const uint8_t g = t.grid[c];
            if (g == BASE0 || g == BASE1) {
                const int seat = g - BASE0;
                const double sg = seat == me ? -1 : 1;
                v += sg * (200 - 14 * k);
                break;
            }
            if (g == BRICK || g == STEEL || g == OUTSIDE) break;
        }
    }
    return v;
}

struct SC { Cmd c[3][20]; int n[3]; };

void side_candidates(const State& s, int side, SC& sc, const int homeBfs[CELLS]) {
    for (int ty = 0; ty < 3; ++ty) {
        const int i = tank_id(side, ty);
        if (!s.tank[i].alive) { sc.c[ty][0] = STAY; sc.n[ty] = 1; }
        else sc.n[ty] = policy_candidates(s, i, sc.c[ty], homeBfs);
    }
}

}  // namespace

void init(const State& s, int me) {
    G_ME = me;
    g_wall_hash = ~0ULL;
    ensure_dist(s);
}

Orders decide(const tank::State& s, int me) {
    G_ME = me;
    ensure_dist(s);

    // 防守距离场：以我方基地两条侧翼开炮行为源（走到侧翼行即可）
    {
        int seeds[6], costs[6], n = 0;
        for (int lr = 0; lr < 2; ++lr) {
            const int c = g_lane[me][lr];
            if (c != NO_CELL) { seeds[n] = c; costs[n] = 0; ++n; }
        }
        // 加上基地正前一列的几个防守格
        dijkstra(s, seeds, costs, n, g_home, 2);
    }

    SC mine, theirs;
    side_candidates(s, me, mine, g_home);
    side_candidates(s, 1 - me, theirs, g_home);

    static Orders A[MAXJ], B[MAXJ];
    int na = 0, nb = 0;
    for (int a = 0; a < mine.n[0] && na < MAXJ; ++a)
        for (int b = 0; b < mine.n[1] && na < MAXJ; ++b)
            for (int c = 0; c < mine.n[2] && na < MAXJ; ++c) {
                A[na].cmd[0] = mine.c[0][a]; A[na].cmd[1] = mine.c[1][b]; A[na].cmd[2] = mine.c[2][c]; ++na;
            }
    for (int a = 0; a < theirs.n[0] && nb < MAXJ; ++a)
        for (int b = 0; b < theirs.n[1] && nb < MAXJ; ++b)
            for (int c = 0; c < theirs.n[2] && nb < MAXJ; ++c) {
                B[nb].cmd[0] = theirs.c[0][a]; B[nb].cmd[1] = theirs.c[1][b]; B[nb].cmd[2] = theirs.c[2][c]; ++nb;
            }

    // 一层 maximin：worst[a] = 对方最优应对下的评估值；同时记录我方坦克是否在某分支挨打
    double worst[MAXJ];
    int danger[MAXJ];   // 该命令下，对方是否能让我方某辆"本可以不死"的车挨打
    for (int a = 0; a < na; ++a) { worst[a] = 1e18; danger[a] = 0; }

    // 基线（双方不动）下我方每辆车的存活/血量，用于区分"这一回合白挨打"
    State base = s; Orders blank; step(base, blank, blank);

    for (int b = 0; b < nb; ++b)
        for (int a = 0; a < na; ++a) {
            State t = s;
            if (me == 0) step(t, A[a], B[b]); else step(t, B[b], A[a]);
            const double v = evaluate(t, me);
            if (v < worst[a]) worst[a] = v;
            for (int ty = 0; ty < 3; ++ty) {
                const int i = tank_id(me, ty);
                if (s.tank[i].alive && (!t.tank[i].alive || t.tank[i].hp < base.tank[i].hp)) {
                    // 我方比"不动"时状态更差 → 危险
                    if (!t.tank[i].alive || t.tank[i].hp < base.tank[i].hp) danger[a] |= 1 << ty;
                }
            }
        }

    // 选 worst 最大、且不白挨打的命令；危险命令加巨大罚分
    int best = 0; double bv = -1e18;
    for (int a = 0; a < na; ++a) {
        double v = worst[a] - (danger[a] ? 100000.0 * __builtin_popcount((unsigned)danger[a]) : 0);
        if (v > bv) { bv = v; best = a; }
    }
    return A[best];
}

// bot.cpp —— 坦克大战 bot（下半场：baseline 式可靠推进 + 躲闪 + 抢星 + 专守装甲）
//
// 上半场教训：联合 maximin 过度保守，三辆车在中线反复横跳、从不点基地（很多图中轴有钢墙，
// 必须绕侧翼），被对手慢慢磨掉基地。本版用"已被证明能走到基地"的 BFS 推进核：
//   距离场 = 从敌方基地做 BFS，空地、砖都可走（砖能打掉），钢/水/基地不可走。
//   沿距离场下降方向走；下一格是砖就先开炮；炮线能到基地就点基地。
// 在这个核上叠加：
//   - FAST：开局直冲中线抢第一批星（速度 2，约 T3 到、T11 吃到），之后变侧翼攻击手；
//   - FIRE：主攻，沿场推进、清砖、点基地；
//   - ARMOR：默认一起进攻；当敌车逼近我方基地时回防，朝最危险敌车 EMPTY-BFS 拦截并击杀；
//   - 所有车：本回合会被在飞子弹命中时，优先选"仍朝目标前进"的安全躲闪；躲不掉才开炮抵消或硬扛最小损失；
//   - 开炮优先级：能直接点基地（炮线无钢）> 打威胁我方基地/高等级的敌车 > 清挡路砖；不朝队友炮线开炮。
#include "tank.hpp"

#include <algorithm>

using namespace tank;

namespace {

constexpr int INF = 1000000;
int G_ME;

int g_atk[2][CELLS];              // 从基地出发、穿空地+砖的 BFS
int g_starBfs[MAX_SUPPLY][CELLS]; // 从补给点出发、只走空地的 BFS
uint64_t g_wall_hash = ~0ULL;

// 从 src 做 BFS：passBrick=true 时空地与砖都可走（砖代价1），否则只走空地。
void bfs_from(const State& s, int src, int* dist, bool passBrick) {
    for (int c = 0; c < CELLS; ++c) dist[c] = INF;
    int q[CELLS], hh = 0, tt = 0;
    dist[src] = 0; q[tt++] = src;
    while (hh < tt) {
        const int c = q[hh++];
        for (int d = 0; d < 4; ++d) {
            const int n = c + DIR_DELTA[d];
            const uint8_t g = s.grid[n];
            const bool ok = (g == EMPTY) || (passBrick && g == BRICK);
            if (ok && dist[n] == INF) { dist[n] = dist[c] + 1; q[tt++] = n; }
        }
    }
}

void ensure_dist(const State& s) {
    if (s.wall_hash == g_wall_hash) return;
    g_wall_hash = s.wall_hash;
    bfs_from(s, s.base_pos[0], g_atk[0], true);
    bfs_from(s, s.base_pos[1], g_atk[1], true);
    for (int k = 0; k < s.n_supply; ++k) bfs_from(s, s.supply[k], g_starBfs[k], false);
}

inline int adist(int base, int c) { int d = g_atk[base][c]; return d >= INF ? 999 : d; }

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

// 不考虑坦克，炮线能否打到某基地（中间只有空/水/砖，无钢、无界外、无另一基地）
bool base_lane(const State& s, int pos, int d, int baseSeat) {
    for (int c = pos + DIR_DELTA[d];; c += DIR_DELTA[d]) {
        const uint8_t g = s.grid[c];
        if (g == BASE0 + baseSeat) return true;
        if (g == STEEL || g == OUTSIDE || g == BASE0 + (1 - baseSeat)) return false;
    }
}

// 仅这辆车行动（其余全部 STAY，敌方不开新炮）后是否挨打——用于判断"在飞子弹"的威胁。
bool would_hit(const State& s, int i, Cmd c) {
    State t = s;
    Orders o[2];
    o[seat_of(i)].cmd[type_of(i)] = c;
    step(t, o[0], o[1]);
    return !t.tank[i].alive || t.tank[i].hp < s.tank[i].hp;
}

bool safe_cmd(const State& s, int i, Cmd c) { return !would_hit(s, i, c); }

// 敌车这一回合若原地朝 target 开火能否命中 target 格（不考虑它会不会真的开，只看几何）。
bool enemy_aims_cell(const State& s, int j, int target) {
    if (!s.tank[j].alive || !can_fire(s, j)) return false;
    const int from = s.tank[j].pos;
    if (from == target) return false;
    const int fx = cell_x(from), fy = cell_y(from), tx = cell_x(target), ty = cell_y(target);
    if (fx != tx && fy != ty) return false;
    const int d = fx == tx ? (ty < fy ? UP : DOWN) : (tx < fx ? LEFT : RIGHT);
    for (int c = from + DIR_DELTA[d]; c != target; c += DIR_DELTA[d]) {
        const uint8_t g = s.grid[c];
        if (g == STEEL || g == OUTSIDE || g == BASE0 || g == BASE1) return false;
        if (tank_at(s, c) >= 0) return false;
    }
    return true;
}

// ---------------- 角色 ----------------
// 0=攻击（沿 g_atk[foe]），1=抢星，2=专守基地
int role_of(const State& s, int i) {
    const int ty = type_of(i), me = seat_of(i), foe = 1 - me;
    if (ty == ARMOR) {
        int threat = INF;
        for (int j = 3 * foe; j < 3 * foe + 3; ++j)
            if (s.tank[j].alive) threat = std::min(threat, adist(me, s.tank[j].pos));
        return threat <= 16 ? 2 : 0;
    }
    if (ty == FIRE) return 0;
    // FAST
    if (s.tank[i].level >= 2) return 0;
    bool starSoon = false;
    if (s.star_mask) {
        for (int k = 0; k < s.n_supply; ++k) if ((s.star_mask >> k) & 1) starSoon = true;
    } else {
        for (int kk = s.turn + 1; kk <= s.turn + 7; ++kk)
            if (kk >= s.P.supply_first && (kk - s.P.supply_first) % s.P.supply_period == 0) { starSoon = true; break; }
    }
    return starSoon ? 1 : 0;
}

// 选目标距离场（防守时动态算到最危险敌车的 EMPTY BFS）
const int* role_field(const State& s, int i, int role, int tmp[CELLS]) {
    const int me = seat_of(i), foe = 1 - me;
    if (role == 0) return g_atk[foe];
    if (role == 1) {
        int sk = -1;
        for (int k = 0; k < s.n_supply; ++k)
            if (((s.star_mask >> k) & 1) && (sk < 0 || g_starBfs[k][s.tank[i].pos] < g_starBfs[sk][s.tank[i].pos])) sk = k;
        if (sk < 0)
            for (int k = 0; k < s.n_supply; ++k)
                if (sk < 0 || g_starBfs[k][s.tank[i].pos] < g_starBfs[sk][s.tank[i].pos]) sk = k;
        return sk >= 0 ? g_starBfs[sk] : g_atk[foe];
    }
    // 专守：驻到基地侧翼的守卫格；有敌车在我方基地炮线上时，优先走到能打它的位置。
    const int myBase = s.base_pos[me];
    int target = -1;
    int laneThreat = INF;
    for (int j = 3 * foe; j < 3 * foe + 3; ++j) {
        if (!s.tank[j].alive) continue;
        const int d = adist(me, s.tank[j].pos);
        // 已能点我方基地（与基地同行/列且无钢），或已压入我方半场很深
        bool canHitBase = false;
        const int ex = cell_x(s.tank[j].pos), ey = cell_y(s.tank[j].pos);
        const int bx = cell_x(myBase), by = cell_y(myBase);
        if (ex == bx || ey == by) {
            canHitBase = true;
            const int dd = (ex == bx) ? (ey < by ? UP : DOWN) : (ex < bx ? LEFT : RIGHT);
            for (int c = s.tank[j].pos + DIR_DELTA[dd]; c != myBase; c += DIR_DELTA[dd]) {
                const uint8_t g = s.grid[c];
                if (g == STEEL || g == OUTSIDE) { canHitBase = false; break; }
            }
        }
        const bool deep = me == 0 ? ey >= 10 : ey <= 4;
        if ((canHitBase || deep) && d < laneThreat) { laneThreat = d; target = j; }
    }
    if (target >= 0) {
        // 只走到敌车附近、不越过中线；用 EMPTY BFS 靠近
        const int ey = cell_y(s.tank[target].pos);
        const bool myHalf = me == 0 ? ey >= 6 : ey <= 8;
        if (myHalf) { bfs_from(s, s.tank[target].pos, tmp, false); return tmp; }
    }
    // 守卫格：与基地同行的侧翼空格（左/右 k 格），选最近且当前可站的
    const int bx = cell_x(myBase), by = cell_y(myBase);
    int guard = -1, gd = INF;
    for (int k = 2; k <= 6; ++k)
        for (int sx : {-1, 1}) {
            const int xx = bx + sx * k;
            if (xx < 0 || xx > 14) continue;
            const int c = cell_of(xx, by);
            if (s.grid[c] != EMPTY && s.grid[c] != BRICK) continue;
            bfs_from(s, c, tmp, false);
            const int dd = tmp[s.tank[i].pos];
            if (dd < gd) { gd = dd; guard = c; }
        }
    if (guard < 0) return g_atk[me];
    bfs_from(s, guard, tmp, false);
    return tmp;
}

// 朝场下降方向移动：返回命令；要求安全。快速型走两步。允许第一步/第二步踩砖（第二步砖可能本回合被打掉则走 canonical 等步）。
// 这里只走当前确为空地的格；砖由开炮处理。
bool move_along(const State& s, int i, const int* field, Cmd& out, bool requireSafe, bool allowSide) {
    const int pos = s.tank[i].pos;
    const bool fast = type_of(i) == FAST;
    struct O { int d1, d2, cell; int gain; };
    O list[40]; int n = 0;
    auto occ = [&](int c) { return s.grid[c] != EMPTY || tank_at(s, c) >= 0; };
    for (int d1 = 0; d1 < 4; ++d1) {
        const int p1 = pos + DIR_DELTA[d1];
        if (occ(p1)) continue;
        int gain1 = field[pos] - field[p1];
        list[n++] = {d1, -1, p1, gain1};
        if (fast) {
            for (int d2 = 0; d2 < 4; ++d2) {
                const int p2 = p1 + DIR_DELTA[d2];
                if (occ(p2)) continue;
                list[n++] = {d1, d2, p2, field[pos] - field[p2]};
            }
        }
    }
    // 先严格按 gain 排序；gain<=0 时若 allowSide，保留横向（gain==0）用于躲闪/绕路
    std::sort(list, list + n, [](const O& a, const O& b) { return a.gain > b.gain; });
    for (int k = 0; k < n; ++k) {
        if (!allowSide && list[k].gain <= 0) break;
        if (allowSide && list[k].gain < 0) break;
        Cmd c = list[k].d2 < 0 ? cmd_move(list[k].d1) : cmd_move2(list[k].d1, list[k].d2);
        if (!requireSafe || safe_cmd(s, i, c)) { out = c; return true; }
    }
    return false;
}

// 最佳开炮方向。优先级：敌基地 > 危险敌车 > 普通敌车 > 挡路砖。返回 -1 无。
int aim(const State& s, int i, int role, const int* field) {
    const int me = seat_of(i), foe = 1 - me;
    int best = -1; double bv = -1e18;
    for (int d = 0; d < 4; ++d) {
        double v = -1e18;
        // 基地炮线（无钢）：先看这条线上会不会先碰到坦克
        if (base_lane(s, s.tank[i].pos, d, foe)) {
            const Ray r = raycast(s, s.tank[i].pos, d);
            if (r.kind == 2 && r.seat == foe) v = 5000 - r.dist;
            else if (r.kind == 1 && seat_of(r.tank) == foe) v = 3500 - r.dist;            // 敌车堵在炮线上，清掉它
            else if (r.kind == 3) v = 3200 - r.dist;                                       // 围墙/挡路砖
        } else {
            const Ray r = raycast(s, s.tank[i].pos, d);
            if (r.kind == 1) {
                const int tv = r.tank;
                if (seat_of(tv) == foe) {
                    double pri = 2500 - r.dist * 6 + s.tank[tv].level * 80;
                    pri += (120 - std::min(120, adist(me, s.tank[tv].pos))) * 8;          // 越接近我方基地越该打
                    if (role == 2) pri += 2000;                                            // 防守车专注打敌车
                    v = pri;
                }
            } else if (r.kind == 3 && field && field[r.cell] < field[s.tank[i].pos]) {
                v = 900 - r.dist;                                                          // 清去路上的砖
            }
        }
        if (v > bv) { bv = v; best = d; }
    }
    return bv > 0 ? best : -1;
}

Cmd decide_tank(const State& s, int i) {
    if (!s.tank[i].alive) return STAY;
    const int me = seat_of(i);
    int tmp[CELLS];
    const int role = role_of(s, i);
    const int* field = role_field(s, i, role, tmp);
    const bool threatened = would_hit(s, i, STAY);

    // 1) 受威胁：优先沿目标方向安全躲闪
    if (threatened) {
        Cmd dodge = STAY;
        if (move_along(s, i, field, dodge, true, true)) return dodge;
        // 尝试开炮抵消/反杀（结算后不掉血）
        if (can_fire(s, i))
            for (int d = 0; d < 4; ++d) { Cmd fc = cmd_fire(d); if (safe_cmd(s, i, fc)) return fc; }
        // 都不行：选结算后血量最高的命令
        Cmd leg[N_CMDS]; int L = legal_commands(s, i, leg), best = STAY, bsc = -100000;
        for (int k = 0; k < L; ++k) {
            State t = s; Orders o[2]; o[me].cmd[type_of(i)] = leg[k]; step(t, o[0], o[1]);
            int sc = t.tank[i].alive ? 1000 + t.tank[i].hp : t.tank[i].hp;
            if (sc > bsc) { bsc = sc; best = leg[k]; }
        }
        return best;
    }

    // 2) 能开高价值炮就开（点基地 / 敌车）
    if (can_fire(s, i)) {
        const int fd = aim(s, i, role, field);
        if (fd >= 0) { Cmd fc = cmd_fire(fd); if (safe_cmd(s, i, fc)) return fc; }
    }

    // 3) 沿目标推进
    Cmd mv = STAY;
    if (move_along(s, i, field, mv, true, false)) return mv;
    // 推进方向被砖挡：开炮清砖
    if (can_fire(s, i)) {
        for (int d = 0; d < 4; ++d) {
            const int n = s.tank[i].pos + DIR_DELTA[d];
            if (s.grid[n] == BRICK && field[n] < field[s.tank[i].pos]) { Cmd fc = cmd_fire(d); if (safe_cmd(s, i, fc)) return fc; }
        }
    }
    // 被车/地形卡住：横向换一格（安全）
    if (move_along(s, i, field, mv, true, true)) return mv;
    return STAY;
}

}  // namespace

void init(const State&, int me) { G_ME = me; g_wall_hash = ~0ULL; }

Orders decide(const State& s, int me) {
    G_ME = me;
    ensure_dist(s);
    Orders o;
    for (int ty = 0; ty < 3; ++ty) o.cmd[ty] = decide_tank(s, tank_id(me, ty));
    return o;
}

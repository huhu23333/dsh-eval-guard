// 策略：每辆车逐条枚举合法命令，用 tank::step 精确前推一回合（对手取三种模型：不动 / 见线就打 /
// 预判我落点开炮），再做第二层"是否存在安全应对"的检查；局面评估 = 基地血量差 + 坦克得失 + 到目标的距离场。
// 目标：攻击手走到离敌方基地 2 格内的炮位；敌人逼近我方基地时最近的车转为猎杀。
#include "tank.hpp"
#include <queue>
#include <vector>
#include <cstring>
#include <algorithm>

using namespace tank;

#define FIXED_PASSES 0

static const int INF = 100000;
static int ME, OP;
static int atkField[2][CELLS];      // atkField[seat]：seat 方去打对方基地的代价场
static int huntField[3][CELLS];     // 针对敌方第 k 辆车的猎杀场
static int starField[CELLS];
static int goalOf[3];               // 0 = 攻击，1..3 = 猎杀敌车 k-1，4 = 吃星
static const int* goalField[3];

// 调参
static int FAR_ATK = 1000, FAR_K = 5, HUNT_KEEP = 4;
static double L2 = 0.5;
static int W_HUNT = 14, W_HUNT_HOT = 40;
static int W_STEP = 10, V_BASE = 100, V_MYBASE = 110, V_DEATH = 250, V_UNSAFE = 150, V_KILL = 40, V_KILLY = 12;
static int V_HP = 40, V_LEVEL = 60, B_BRICK = 6, B_FAR = 5, B_FARSTILL = 15, B_BASEFAR = 8;
static double M_STAY = 0.55, M_THREAT = 0.45, M_ANT = 0.0;

static int danger[CELLS];            // 敌车视线内的格子，寻路时加代价
static bool useDanger = false;
static int wStep[3] = {10, 10, 10};
static int DANGER_NEAR = 0, DANGER_FAR = 0;
static uint8_t lane[CELLS];          // 敌方炮线（打我基地的线、飞行中的敌弹前方）
static void dijkstra(const State& s, int* f, bool useLane = false) {
    typedef std::pair<int, int> PI;
    std::priority_queue<PI, std::vector<PI>, std::greater<PI>> pq;
    for (int c = 0; c < CELLS; ++c) if (f[c] < INF) pq.push({f[c], c});
    while (!pq.empty()) {
        PI p = pq.top(); pq.pop();
        if (p.first > f[p.second]) continue;
        for (int d = 0; d < 4; ++d) {
            int n = p.second + DIR_DELTA[d];
            uint8_t g = s.grid[n];
            int w;
            if (g == EMPTY) w = 1; else if (g == BRICK) w = 3; else continue;
            if (useLane && lane[n]) w += 8;
            if (useDanger) w += danger[n];
            if (f[n] > p.first + w) { f[n] = p.first + w; pq.push({f[n], n}); }
        }
    }
}

static bool buildAtk(const State& s, int seat, int* f, int only = -1) {
    for (int c = 0; c < CELLS; ++c) f[c] = INF;
    int B = s.base_pos[1 - seat];
    bool any = false;
    for (int d = 0; d < 4; ++d) {
        if (only >= 0 && d != only) continue;
        int bricks = 0;
        for (int k = 1; k <= 9; ++k) {
            int c = B + k * DIR_DELTA[d];
            uint8_t g = s.grid[c];
            int far = k > 2 ? (k - 2) * 2 : 0;
            if (g == EMPTY) { f[c] = std::min(f[c], 2 * bricks + far); any = true; }
            else if (g == BRICK) { f[c] = std::min(f[c], 2 * bricks + far + 2); ++bricks; any = true; }
            else if (g == WATER) continue;
            else break;
        }
    }
    useDanger = only >= 0;
    dijkstra(s, f);
    useDanger = false;
    return any;
}
static int sideField[3][CELLS];
static bool sideOk[3];
static int sideOf[3] = {-1, -1, -1};
static int lastBest[3] = {INF, INF, INF}, stuckCnt[3];
static int sidePen[3][3], sidePenUntil[3][3];

static void buildHunt(const State& s, int e, int* f) {
    for (int c = 0; c < CELLS; ++c) f[c] = INF;
    int P = s.tank[e].pos;
    for (int d = 0; d < 4; ++d) {
        for (int k = 1; k <= 7; ++k) {
            int c = P + k * DIR_DELTA[d];
            uint8_t g = s.grid[c];
            if (g == EMPTY) { if (!lane[c]) f[c] = (k == 1 ? 2 : (k == 2 ? 0 : (k - 2))); }
            else if (g == WATER) continue;
            else break;
        }
    }
    dijkstra(s, f, true);
}

static void buildLane(const State& s) {
    memset(lane, 0, sizeof lane);
    memset(danger, 0, sizeof danger);
    for (int t = 0; t < 3; ++t) {
        int e = tank_id(OP, t);
        if (!s.tank[e].alive) continue;
        for (int d = 0; d < 4; ++d)
            for (int k = 1; k <= 5; ++k) {
                int c = s.tank[e].pos + k * DIR_DELTA[d];
                if (s.grid[c] != EMPTY && s.grid[c] != WATER) break;
                danger[c] += k <= 2 ? DANGER_NEAR : DANGER_FAR;
            }
    }
    for (int t = 0; t < 3; ++t) {
        int e = tank_id(OP, t);
        if (!s.tank[e].alive) continue;
        for (int d = 0; d < 4; ++d) {
            int k = 1; bool hit = false;
            for (; k <= 14; ++k) {
                uint8_t g = s.grid[s.tank[e].pos + k * DIR_DELTA[d]];
                if (g == BASE0 + ME) { hit = true; break; }
                if (g != EMPTY && g != WATER && g != BRICK) break;
            }
            if (hit) for (int j = 1; j < k; ++j) lane[s.tank[e].pos + j * DIR_DELTA[d]] = 1;
        }
    }
    for (int b = 0; b < s.n_bullets; ++b) {
        if (seat_of(s.bullet[b].owner) == ME) continue;
        for (int k = 1; k <= 6; ++k) {
            int c = s.bullet[b].pos + k * DIR_DELTA[s.bullet[b].dir];
            if (s.grid[c] != EMPTY && s.grid[c] != WATER) break;
            lane[c] = 1;
        }
    }
}

// 敌车 e（任意一方的车）"见线就打"的命令：打 foe 方的车或基地
static Cmd threatCmd(const State& s, int e) {
    const Tank& t = s.tank[e];
    if (!t.alive || !can_fire(s, e)) return STAY;
    int side = seat_of(e);
    int best = -1, bestP = 0;
    for (int d = 0; d < 4; ++d) {
        int bricks = 0;
        for (int k = 1; k <= 14; ++k) {
            int c = t.pos + k * DIR_DELTA[d];
            uint8_t g = s.grid[c];
            if (g == WATER) continue;
            if (g == BRICK) { ++bricks; continue; }
            if (g == BASE0 + (1 - side)) { int p = 20 - bricks; if (p > bestP) { bestP = p; best = d; } break; }
            if (g != EMPTY) break;
            int v = tank_at(s, c);
            if (v >= 0) {
                if (seat_of(v) != side && bricks == 0 && k <= 8) { int p = 100 - k; if (p > bestP) { bestP = p; best = d; } }
                break;
            }
        }
    }
    return best < 0 ? STAY : cmd_fire(best);
}

static Orders threatOrders(const State& s, int side) {
    Orders o;
    for (int t = 0; t < 3; ++t) o.cmd[t] = threatCmd(s, tank_id(side, t));
    return o;
}

static int destOf(const State& s, int i, Cmd c) {
    int p = s.tank[i].pos;
    auto go = [&](int d) { int n = p + DIR_DELTA[d]; if (s.grid[n] == EMPTY && tank_at(s, n) < 0) p = n; };
    if (is_move(c)) go(c - 1);
    else if (is_move2(c)) { go((c - 9) / 4); go((c - 9) % 4); }
    return p;
}

// 预判模型：能开炮的敌车朝我车 i 的落点（2 格内同线）开炮
static Orders anticipateOrders(const State& s, int side, int i, int dest) {
    Orders o = threatOrders(s, side);
    for (int t = 0; t < 3; ++t) {
        int e = tank_id(side, t);
        if (!s.tank[e].alive || !can_fire(s, e)) continue;
        for (int d = 0; d < 4; ++d) {
            for (int k = 1; k <= 2; ++k) {
                int c = s.tank[e].pos + k * DIR_DELTA[d];
                uint8_t g = s.grid[c];
                if (g != EMPTY && g != WATER) break;
                if (c == dest) { o.cmd[t] = cmd_fire(d); break; }
                int v = tank_at(s, c);
                if (v >= 0 && v != i) break;
            }
        }
    }
    return o;
}

static double evalState(const State& s0, const State& s) {
    double v = 0;
    if (s.done) {
        if (s.winner == ME) v += 100000; else if (s.winner == OP) v -= 100000;
    }
    v += V_BASE * (s0.base_hp[OP] - s.base_hp[OP]);
    v -= V_MYBASE * (s0.base_hp[ME] - s.base_hp[ME]);
    for (int t = 0; t < 3; ++t) {
        int i = tank_id(ME, t), e = tank_id(OP, t);
        const Tank& a0 = s0.tank[i]; const Tank& a = s.tank[i];
        if (a0.alive) {
            if (!a.alive) v -= V_DEATH + 60 * a0.level;
            else {
                int dh = a.hp - 2 * (a.level - a0.level) - a0.hp;
                v += V_HP * dh;
                v += V_LEVEL * (a.level - a0.level);
                int f = goalField[t][a.pos];
                if (f >= INF) f = 60;
                v -= wStep[t] * f;
            }
        }
        const Tank& b0 = s0.tank[e]; const Tank& b = s.tank[e];
        if (b0.alive) {
            if (!b.alive) {
                int dy = std::abs(cell_y(b0.pos) - cell_y(s.spawn[e]));
                v += V_KILL + V_KILLY * dy + 60 * b0.level;
            } else {
                int dh = b.hp - 2 * (b.level - b0.level) - b0.hp;
                v -= V_HP * dh;
                v -= V_LEVEL * (b.level - b0.level);
            }
        }
    }
    return v;
}

// 命令本身的启发加分（前推一回合看不到的远期收益）
static double cmdBonus(const State& s, int i, Cmd c, const int* field) {
    if (!is_fire(c)) return 0;
    int d = c - 5;
    const Tank& t = s.tank[i];
    for (int k = 1; k <= 14; ++k) {
        int cc = t.pos + k * DIR_DELTA[d];
        uint8_t g = s.grid[cc];
        if (g == WATER) continue;
        if (g == BRICK) {
            if (field[cc] <= field[t.pos] && field[cc] < INF) return B_BRICK;
            // 炮线后面是敌方基地
            for (int kk = k + 1; kk <= 14; ++kk) {
                uint8_t g2 = s.grid[t.pos + kk * DIR_DELTA[d]];
                if (g2 == BASE0 + OP) return B_BRICK;
                if (g2 != EMPTY && g2 != WATER && g2 != BRICK) break;
            }
            return -3;
        }
        if (g == BASE0 + OP) return k > 2 ? B_BASEFAR : 0;
        if (g == BASE0 + ME) return -400;
        if (g != EMPTY) return -3;
        int v = tank_at(s, cc);
        if (v >= 0) {
            if (seat_of(v) == ME) return -300;
            if (k <= 2) return 0;
            if (field[t.pos] <= FAR_ATK && k > FAR_K) return -3;
            bool still = s.tank[v].last_fire >= s.turn - 1;
            return (still ? B_FARSTILL : B_FAR) - k;
        }
    }
    return -3;
}

static bool safeNext(const State& s1, int i) {
    const Tank& t = s1.tank[i];
    if (!t.alive) return true;
    Orders eo = threatOrders(s1, OP);
    Cmd list[N_CMDS];
    int n = legal_commands(s1, i, list);
    for (int k = 0; k < n; ++k) {
        State s2 = s1;
        Orders mo; mo.cmd[type_of(i)] = list[k];
        step_as(s2, ME, mo, eo);
        if (s2.tank[i].alive && s2.tank[i].hp >= t.hp) return true;
        if (s2.done && s2.winner == ME) return true;
    }
    return false;
}

void init(const State& s, int me) { ME = me; OP = 1 - me; }

Orders decide(const State& s, int me) {
    ME = me; OP = 1 - me;
    Orders orders;
    buildAtk(s, ME, atkField[ME]);
    buildAtk(s, OP, atkField[OP]);
    buildLane(s);

    // 吃星场
    bool anyStar = false;
    for (int c = 0; c < CELLS; ++c) starField[c] = INF;
    for (int k = 0; k < s.n_supply; ++k) if (s.star_mask >> k & 1) { starField[s.supply[k]] = 0; anyStar = true; }
    if (anyStar) dijkstra(s, starField);

    // 三个方向的炮位分配：尽量一车一个方向，避免挤在同一条路上互相挡
    {
        const int sd[3] = {ME == 0 ? DOWN : UP, LEFT, RIGHT};
        for (int k = 0; k < 3; ++k) sideOk[k] = buildAtk(s, ME, sideField[k], sd[k]);
        for (int t = 0; t < 3; ++t) {
            int i = tank_id(ME, t);
            if (!s.tank[i].alive) { stuckCnt[t] = 0; lastBest[t] = INF; continue; }
            if (sideOf[t] >= 0) {
                int f = sideField[sideOf[t]][s.tank[i].pos];
                if (f < lastBest[t]) { lastBest[t] = f; stuckCnt[t] = 0; }
                else if (f > 0 && ++stuckCnt[t] >= 8) {
                    sidePen[t][sideOf[t]] = 25; sidePenUntil[t][sideOf[t]] = s.turn + 30;
                    stuckCnt[t] = 0; lastBest[t] = INF;
                }
            }
        }
        int bestTot = INF, bestA[3] = {0, 0, 0};
        for (int a = 0; a < 27; ++a) {
            int as[3] = {a % 3, a / 3 % 3, a / 9};
            int tot = 0, cnt[3] = {0, 0, 0};
            for (int t = 0; t < 3; ++t) {
                int i = tank_id(ME, t);
                int k = as[t];
                if (!sideOk[k]) { tot += 1000; continue; }
                cnt[k]++;
                int pos = s.tank[i].alive ? s.tank[i].pos : s.spawn[i];
                int f = sideField[k][pos]; if (f >= INF) f = 200;
                if (t == FAST) f = (f + 1) / 2;
                if (s.turn < sidePenUntil[t][k]) f += sidePen[t][k];
                if (k != sideOf[t]) f += 3;
                tot += f;
            }
            for (int k = 0; k < 3; ++k) if (cnt[k] > 1) tot += 12 * (cnt[k] - 1);
            if (tot < bestTot) { bestTot = tot; for (int t = 0; t < 3; ++t) bestA[t] = as[t]; }
        }
        for (int t = 0; t < 3; ++t) {
            if (bestA[t] != sideOf[t]) { sideOf[t] = bestA[t]; stuckCnt[t] = 0; lastBest[t] = INF; }
            goalOf[t] = 0;
            goalField[t] = sideOk[sideOf[t]] ? sideField[sideOf[t]] : atkField[ME];
        }
    }

    // 猎杀分配
    for (int t = 0; t < 3; ++t) wStep[t] = W_STEP;
    bool huntBuilt[3] = {false, false, false};
    int order[3] = {0, 1, 2};
    std::sort(order, order + 3, [&](int a, int b) {
        int ea = tank_id(OP, a), eb = tank_id(OP, b);
        int fa = s.tank[ea].alive ? atkField[OP][s.tank[ea].pos] : INF;
        int fb = s.tank[eb].alive ? atkField[OP][s.tank[eb].pos] : INF;
        return fa < fb;
    });
    bool used[3] = {false, false, false};
    int nHunt = 0;
    for (int oi = 0; oi < 3; ++oi) {
        int k = order[oi], e = tank_id(OP, k);
        if (!s.tank[e].alive) continue;
        int thr = atkField[OP][s.tank[e].pos];
        if (thr > 7) continue;
        buildHunt(s, e, huntField[k]); huntBuilt[k] = true;
        int best = -1, bd = INF;
        for (int t = 0; t < 3; ++t) {
            int i = tank_id(ME, t);
            if (used[t] || !s.tank[i].alive) continue;
            int dd = huntField[k][s.tank[i].pos];
            if (t == FAST) dd = (dd + 1) / 2;
            if (goalField[t][s.tank[i].pos] <= HUNT_KEEP && dd > 2) continue;
            if (dd < bd) { bd = dd; best = t; }
        }
        if (best >= 0 && bd <= 14 && nHunt < (s.base_hp[ME] > s.base_hp[OP] + 6 ? 0 : 1)) { ++nHunt; used[best] = true; goalOf[best] = 1 + k; goalField[best] = huntField[k]; wStep[best] = thr <= 1 ? W_HUNT_HOT : W_HUNT; }
    }
    // 吃星：星近就顺路吃
    if (anyStar) {
        for (int t = 0; t < 3; ++t) {
            int i = tank_id(ME, t);
            if (!s.tank[i].alive || goalOf[t] != 0 || s.tank[i].level >= 2) continue;
            int sd = starField[s.tank[i].pos];
            int lim = s.tank[i].level == 0 ? 5 : 3;
            if (sd <= lim && goalField[t][s.tank[i].pos] > 0) { goalOf[t] = 4; goalField[t] = starField; }
        }
    }

    const int seq[3] = {FIRE, ARMOR, FAST};
    int passes = 0;
    for (int pass = 0; pass < 2; ++pass) {
        if (pass > 0) {
            if (FIXED_PASSES ? pass >= FIXED_PASSES : budget_left() < 25) break;
        }
        ++passes;
        bool changed = false;
        for (int si = 0; si < 3; ++si) {
            int t = seq[si], i = tank_id(ME, t);
            if (!s.tank[i].alive) continue;
            Cmd list[N_CMDS];
            int n = legal_commands(s, i, list);
            bool cf = can_fire(s, i);
            Orders eStay, eThr = threatOrders(s, OP);
            double bestV = -1e18; Cmd bestC = STAY;
            for (int k = 0; k < n; ++k) {
                Cmd c = list[k];
                if (is_fire(c) && !cf) continue;
                Orders mo = orders; mo.cmd[t] = c;
                double v = cmdBonus(s, i, c, goalField[t]);
                Orders eAnt = anticipateOrders(s, OP, i, destOf(s, i, c));
                const Orders* models[3] = {&eStay, &eThr, &eAnt};
                const double wts[3] = {M_STAY, M_THREAT, M_ANT};
                for (int m = 0; m < 3; ++m) {
                    State s1 = s;
                    step_as(s1, ME, mo, *models[m]);
                    double ev = evalState(s, s1);
                    if (wts[m] <= 0) continue;
                    if (!s1.done && s1.tank[i].alive && L2 > 0) {
                        Cmd l2[N_CMDS];
                        int n2 = legal_commands(s1, i, l2);
                        bool cf2 = can_fire(s1, i);
                        Orders e2; if (m >= 1) e2 = threatOrders(s1, OP);
                        double b2 = -1e18;
                        for (int k2 = 0; k2 < n2; ++k2) {
                            if (is_fire(l2[k2]) && !cf2) continue;
                            State s2 = s1;
                            Orders m2; m2.cmd[t] = l2[k2];
                            step_as(s2, ME, m2, e2);
                            double e = evalState(s, s2) + cmdBonus(s1, i, l2[k2], goalField[t]);
                            if (e > b2) b2 = e;
                        }
                        ev = (1 - L2) * ev + L2 * b2;
                    }
                    if (!s1.done && s1.tank[i].alive && !safeNext(s1, i)) ev -= V_UNSAFE;
                    v += wts[m] * ev;
                }
                if (v > bestV) { bestV = v; bestC = c; }
            }
            if (bestC != orders.cmd[t]) changed = true;
            orders.cmd[t] = bestC;
        }
        if (!changed) break;
    }
#ifdef DBG
    for (int t = 0; t < 3; ++t) { int i = tank_id(ME, t); fprintf(stderr, "t%d goal%d side%d f%d w%d | ", t, goalOf[t], sideOf[t], s.tank[i].alive ? goalField[t][s.tank[i].pos] : -1, wStep[t]); }
    fputc(10, stderr);
#endif
    return orders;
}

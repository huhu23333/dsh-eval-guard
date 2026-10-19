// 由赛事脚本生成：sparring.cpp 加上配置里 sparring_defines 指定的宏。不要手改，改配置或源码。
#define SP_BUDGET 14000
#define SP_STARW 90
#define SP_LVV 70
#define SP_STAR_REACH 60
#line 1 "sparring.cpp"
// sparring.cpp —— 陪练（出题方的强力参照，不发给选手）。
//
// 分两层：
//
// 战略层（规则，每回合开头定一次，见 assign_roles）：每辆车本回合的任务是进攻、拦截某辆敌车、还是在家门口待命。
//   默认留一辆专职守家（装甲型），对方有车逼近时最多派一辆去拦，其余进攻；长时间没有进展就全员进攻。
//   任务只决定评估函数里这辆车"离目标多远"按哪张距离图算。
//
// 战术层（搜索）：六辆车各有一棵搜索树（"分散式 UCT"）。每次模拟从当前局面出发，六辆车各自在自己的树里
//   按 PUCT 选一条命令，合成双方的联合命令，用引擎精确前推一回合；这样往下走 SP_DEPTH 回合，
//   走出树的车改用一个便宜的规则策略接着走。沿途每回合用评估函数打分取平均，记回六棵树上走过的每个节点
//   （对方的车记相反数）。三辆车的联合动作空间有上千种，但每棵树只管自己那辆车的十几条命令，
//   车与车的配合、对手的应对都通过"一起被前推"体现在分数里。对手和我方用同一套搜索，
//   所以得到的是"双方都在找好棋"的近似解，不依赖对某个固定对手模型的猜测。
//
// 评估函数（evaluate）：基地血量、击毁数、每辆车离目标的回合数（被毁的车按出生点加复活时间算）、
//   血量和等级、星星、炮线上被打掉的砖、还在飞的子弹会打到什么。
//
// 剪枝（build_actions）：开炮只留"打得到东西"的方向（敌车、基地、挡路的砖、正飞来的子弹、敌车可能走进来的线）；
//   快速型的两步按终点去重，"出去再回来"只在有威胁时保留。每辆车剩十条左右，按规则先验排序。
//
// 思考预算有两种控制方式，用 SP_USE_TIME 切换：
//   0（默认，实验用）：每回合固定前推 SP_BUDGET 次 step，结果完全确定，可复现。
//   1（正式比赛用）：tank::budget_left() 小于 SP_TIME_MARGIN 就停；SP_BUDGET 仍作为上限兜底
//                    （budget_left 目前是占位实现，返回一个很大的数）。
//
// 可调的宏（都有默认值，build.py 的 bots.txt 里可以逐项覆盖）：见下面"配置"一节。
#include "tank.hpp"
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

using namespace tank;

// ====================================================================
// 配置
// ====================================================================
#ifndef SP_BUDGET
#define SP_BUDGET 20000        // 每回合前推多少次 step
#endif
#ifndef SP_USE_TIME
#define SP_USE_TIME 0          // 1 = 按 budget_left() 停
#endif
#ifndef SP_TIME_MARGIN
#define SP_TIME_MARGIN 4       // budget_left() 小于这个数就停（毫秒；引擎建议留 2–3 毫秒，每 16 次模拟查一次）
#endif
#ifndef SP_DEPTH
#define SP_DEPTH 6             // 每次模拟前推几回合
#endif
#ifndef SP_UCB
#define SP_UCB 80              // 探索系数 × 100（PUCT：Q + c·P·sqrt(N)/(1+n)）
#endif
#ifndef SP_PRIOR_T
#define SP_PRIOR_T 12          // 先验分换成概率时的温度：越小越相信规则给的先验
#endif
#ifndef SP_FPU
#define SP_FPU 0                // 没试过的命令的初始估值 = 节点均值 − 这个数 / 100
#endif
#ifndef SP_STARS
#define SP_STARS 1             // 0 = 评估里不看星星（"不抢星"的对照版本）
#endif
#ifndef SP_STARW
#define SP_STARW 26            // 星星的吸引力：离星星 0 回合时的加分，每远一回合少 2 分（"认真抢星"的版本用 90）
#endif
#ifndef SP_LVV
#define SP_LVV 20              // 评估里每一级值多少分（"认真抢星"的版本用 70）
#endif
#ifndef SP_STAR_REACH
#define SP_STAR_REACH 24       // 走位先验里，离星星不超过这么多（1/4 回合）才考虑绕过去
#endif
#ifndef SP_FFFIX
#define SP_FFFIX 1             // 1 = 出手前检查：选定的命令会不会当回合打中队友，会就把开炮的那辆换成次优命令
#endif
#ifndef SP_TV
#define SP_TV 22               // 快速型、装甲型耽误一回合值多少分（基地 1 血 = 100 分）
#endif
#ifndef SP_TV_FIRE
#define SP_TV_FIRE 34          // 火力型耽误一回合值多少分（它到位后每回合能打 1 血，是另外两种的两倍）
#endif
#ifndef SP_THREAT
#define SP_THREAT 40           // 敌车离"能打到我方基地"不到这么多（1/4 回合）就算威胁，要派车去拦
#endif
#ifndef SP_MAX_HUNTERS
#define SP_MAX_HUNTERS 1       // 最多几辆车去拦截（其余进攻）
#endif
#ifndef SP_EMERGENCY
#define SP_EMERGENCY 0         // 敌车离"能打到我方基地"不到这么多（1/4 回合）算紧急，紧急的有几辆就可以派几辆去拦；0 = 不启用
#endif
#ifndef SP_GUARDS
#define SP_GUARDS 1            // 专职守家的车数（先装甲型，再快速型）：对方有车过了半场就留在家门口等，没有才去进攻；9 = 按来犯数量自动定
#endif
#ifndef SP_GUARD_RANGE
#define SP_GUARD_RANGE 48      // 对方有车离"能打到我方基地"不到这么多（1/4 回合），守家的车就不出门
#endif
#ifndef SP_FAR_COST
#define SP_FAR_COST 3          // 射击位离基地每远一格（超过 2 格的部分）的代价（1/4 回合）：远了子弹飞得久、容易被挡被抵消
#endif
#ifndef SP_BLOCK_STILL
#define SP_BLOCK_STILL 0       // 炮线上的车连续这么多回合没动才算堵住炮线；0 = 只要在炮线上就算
#endif
#ifndef SP_HUNT_MAX
#define SP_HUNT_MAX 64         // 拦截的车最远从多远赶过去（1/4 回合）
#endif
#ifndef SP_SQUASH
#define SP_SQUASH 600          // 评估分压到 (-1, 1) 时的尺度
#endif
#ifndef SP_PATH_AVG
#define SP_PATH_AVG 1          // 1 = 一次模拟的分数取沿途每回合评估的平均；0 = 只看最后一回合（对照用）
#endif
#ifndef SP_METHOD
#define SP_METHOD 0            // 0 = 分散式 UCT（默认）；1 = 一回合前推 + 规则续推 + 轮流取最佳应对（对照用的另一种搜索）
#endif
#ifndef SP_IBR_ROUNDS
#define SP_IBR_ROUNDS 3        // SP_METHOD = 1 时轮流优化几轮
#endif
#ifndef SP_ROLLOUT_RANDOM
#define SP_ROLLOUT_RANDOM 4    // 树外策略每 N 次有一次乱走
#endif

static const int BIG = 30000;
static const int MAX_ACTS = 22;

// ====================================================================
// 每回合开头算一次的地图信息（墙没变就沿用）
// ====================================================================
struct Maps {
    bool valid = false;
    uint64_t wall_hash = 0;
    int16_t atk[2][CELLS];              // atk[seat][格]：seat 的慢车从这格出发，到"能有效射击对方基地"的代价，单位 1/4 回合
    int16_t sup[MAX_SUPPLY][CELLS];     // 到各补给点的代价，同单位
    int n_key[2];                       // key[seat]：seat 进攻炮线上的砖（打掉它对 seat 有利）
    int16_t key_cell[2][24];
    int16_t key_val[2][24];
};
static Maps M;

static void flood(const State& s, int16_t* dist) {
    static int queue[CELLS * 16];
    int head = 0, tail = 0;
    for (int c = 0; c < CELLS; ++c)
        if (dist[c] < BIG) queue[tail++] = c;
    while (head < tail) {
        const int c = queue[head++ % (CELLS * 16)];
        for (int d = 0; d < 4; ++d) {
            const int n = c + DIR_DELTA[d];
            const uint8_t g = s.grid[n];
            if (g != EMPTY && g != BRICK) continue;
            const int nd = dist[c] + (g == BRICK ? 12 : 4);     // 空地一步 1 回合；砖墙要先打掉，算 3 回合
            if (nd < dist[n]) { dist[n] = int16_t(nd); queue[tail++ % (CELLS * 16)] = n; }
        }
    }
}

static void build_maps(const State& s) {
    if (M.valid && M.wall_hash == s.wall_hash) return;
    M.valid = true;
    M.wall_hash = s.wall_hash;
    for (int seat = 0; seat < 2; ++seat) {
        int16_t* dist = M.atk[seat];
        for (int c = 0; c < CELLS; ++c) dist[c] = BIG;
        M.n_key[seat] = 0;
        // 基地只能沿它所在的行、列被打到。从对方基地朝四个方向走出去，沿途的空地就是射击位。
        for (int d = 0; d < 4; ++d) {
            int bricks = 0, k = 1;
            for (int c = s.base_pos[1 - seat] + DIR_DELTA[d];; c += DIR_DELTA[d], ++k) {
                const uint8_t g = s.grid[c];
                if (g == BRICK) {
                    if (M.n_key[seat] < 24) {
                        M.key_cell[seat][M.n_key[seat]] = int16_t(c);
                        M.key_val[seat][M.n_key[seat]++] = int16_t(bricks == 0 ? 16 : 8);
                    }
                    dist[c] = int16_t(8 * bricks + 12 + (k > 2 ? 2 * (k - 2) : 0));   // 打掉这块砖站进去也是射击位
                    ++bricks;
                    continue;
                }
                if (g == WATER) continue;
                if (g != EMPTY) break;
                dist[c] = int16_t(8 * bricks + (k > 2 ? 2 * (k - 2) : 0));   // 每块砖多打一炮；超过 2 格子弹当回合到不了
            }
        }
        flood(s, dist);
    }
    for (int k = 0; k < s.n_supply; ++k) {
        for (int c = 0; c < CELLS; ++c) M.sup[k][c] = BIG;
        M.sup[k][s.supply[k]] = 0;
        flood(s, M.sup[k]);
    }
}

// ====================================================================
// 评估函数：从 0 号方的角度给局面打分（基地 1 血 = 100 分）
// ====================================================================
static uint8_t root_grid[CELLS];        // 本回合开头的地形（用来看模拟里哪些关键的砖被打掉了）
static int soon_mask;                   // 马上要出星的补给点（提前去占位）

// 每辆车自己的进攻图：和 M.atk 一样，但把别的车此刻占着的格子算贵一些（多 2 回合），
// 这样通道被堵住（比如两辆车在窄道里对射僵住）时会绕路，而不是排在后面干等。每回合重算。
static int16_t atk_of[6][CELLS];

static void flood(const State& s, int16_t* dist);

static int still[6], still_pos[6];      // 每辆车已经连续几回合待在同一格

static void build_tank_maps(const State& s) {
    for (int j = 0; j < 6; ++j) {
        if (s.tank[j].alive && s.tank[j].pos == still_pos[j]) still[j] += 1;
        else still[j] = 0;
        still_pos[j] = s.tank[j].alive ? s.tank[j].pos : -1;
    }
    for (int i = 0; i < 6; ++i) {
        const int seat = i / 3;
        int16_t* dist = atk_of[i];
        for (int c = 0; c < CELLS; ++c) dist[c] = BIG;
        for (int d = 0; d < 4; ++d) {
            int bricks = 0, k = 1;
            for (int c = s.base_pos[1 - seat] + DIR_DELTA[d];; c += DIR_DELTA[d], ++k) {
                const uint8_t g = s.grid[c];
                if (g == BRICK) {
                    // 炮线上的砖：打掉之后站进去也是射击位（两侧被钢墙封住时，围墙那一格是唯一的侧面射击位）。
                    dist[c] = int16_t(8 * bricks + 12 + (k > 2 ? SP_FAR_COST * (k - 2) : 0));
                    ++bricks;
                    continue;
                }
                if (g == WATER) continue;
                if (g != EMPTY) break;
                dist[c] = int16_t(8 * bricks + (k > 2 ? SP_FAR_COST * (k - 2) : 0));
                // 炮线上有别的车（不是自己）：它后面的格子打不到基地，不算射击位。
                // （不这样的话，车会站在老远的地方隔着对方的车"打基地"，两边子弹互相抵消，谁也不动。）
                bool blocked = false;
                for (int j = 0; j < 6; ++j) blocked |= j != i && s.tank[j].alive && s.tank[j].pos == c && still[j] >= SP_BLOCK_STILL;
                if (blocked) break;
            }
        }
        // 带"别的车占着的格子更贵"的最短路。
        static int queue[CELLS * 16];
        static int16_t extra[CELLS];
        for (int c = 0; c < CELLS; ++c) extra[c] = 0;
        for (int j = 0; j < 6; ++j)
            if (j != i && s.tank[j].alive) extra[s.tank[j].pos] = int16_t(still[j] >= 3 ? 24 : 8);   // 站着不动好几回合的：当它是堵死的
        int head = 0, tail = 0;
        for (int c = 0; c < CELLS; ++c)
            if (dist[c] < BIG) queue[tail++] = c;
        while (head < tail) {
            const int c = queue[head++ % (CELLS * 16)];
            for (int d = 0; d < 4; ++d) {
                const int n = c + DIR_DELTA[d];
                const uint8_t g = s.grid[n];
                if (g != EMPTY && g != BRICK) continue;
                const int nd = dist[c] + (g == BRICK ? 12 : 4) + extra[c];      // 从 n 走进 c：c 上有别的车就多算
                if (nd < dist[n]) { dist[n] = int16_t(nd); queue[tail++ % (CELLS * 16)] = n; }
            }
        }
    }
}

// 一辆车离"能有效射击对方基地"还差多少，单位 1/4 回合。快速型按 0.6 倍算。
static inline int progress_cost(int i, int pos) {
    int a = atk_of[i][pos];
    if (a > 240) a = 240;
    return i % 3 == FAST ? a * 5 / 8 : a;
}

// ---- 分工（每回合开头按规则定，搜索期间不变）----
// 我方每辆车要么进攻（目标是对方基地的射击位），要么拦截某一辆逼近我方基地的敌车。
// 搜索负责"怎么走、什么时候开炮"，分工只决定评估函数里这辆车"离目标多远"按哪张图算。
static int root_me;
static int hunt_target[6];              // -1 = 进攻；否则是要拦截的敌车编号
static int16_t hunt_map[3][CELLS];      // hunt_map[k]：到"和第 k 辆敌车对上炮线"的代价
static int16_t post_map[CELLS];         // 到家门口的代价（守家的车没有目标时用）
static bool holding[6];                 // 这辆车本回合在守家待命
static int prev_target[3];

static int stale_turns;                 // 已经连续多少回合"什么都没发生"（基地血量、击毁数、墙都没变）
static int stale_hp[2], stale_kills[2];
static uint64_t stale_walls;

static void assign_roles(const State& s, int me) {
    // 僵持检测：局面长时间没有进展，就减少拦截的车、多派车去进攻，把僵局打破。
    if (s.base_hp[0] != stale_hp[0] || s.base_hp[1] != stale_hp[1] || s.kills[0] != stale_kills[0] ||
        s.kills[1] != stale_kills[1] || s.wall_hash != stale_walls) {
        stale_turns = 0;
        stale_hp[0] = s.base_hp[0]; stale_hp[1] = s.base_hp[1];
        stale_kills[0] = s.kills[0]; stale_kills[1] = s.kills[1];
        stale_walls = s.wall_hash;
    } else {
        stale_turns += 1;
    }
    int max_hunters = SP_MAX_HUNTERS;
    if (stale_turns >= 10 && max_hunters > 1) max_hunters = 1;
    if (stale_turns >= 30) max_hunters = 0;
    int n_hunters = 0;
#if SP_EMERGENCY
    // 已经有敌车到位、正在打我方基地（或一两回合内就能打）：有几辆就最多派几辆去拦，不受上面的名额限制。
    {
        int urgent = 0;
        for (int k = 0; k < 3; ++k) {
            const int foe = 3 * (1 - me) + k;
            if (!s.tank[foe].alive) continue;
            int c = M.atk[1 - me][s.tank[foe].pos];
            if (k == FAST) c = c * 5 / 8;
            urgent += c <= SP_EMERGENCY;
        }
        if (urgent > max_hunters && stale_turns < 10) max_hunters = urgent;
    }
#endif
    root_me = me;
    for (int i = 0; i < 6; ++i) { hunt_target[i] = -1; holding[i] = false; }
    const int foe0 = 3 * (1 - me);
    int threat[3], threat_cost[3], n = 0;
    for (int k = 0; k < 3; ++k) {
        if (!s.tank[foe0 + k].alive) continue;
        int c = M.atk[1 - me][s.tank[foe0 + k].pos];
        if (k == FAST) c = c * 5 / 8;
        if (c > SP_THREAT) continue;
        int at = n++;
        while (at > 0 && threat_cost[at - 1] > c) { threat[at] = threat[at - 1]; threat_cost[at] = threat_cost[at - 1]; --at; }
        threat[at] = k;
        threat_cost[at] = c;
    }
    bool used[3] = {};
    int now_target[3] = {-1, -1, -1};
    for (int q = 0; q < n && n_hunters < max_hunters; ++q) {
        const int k = threat[q], foe = foe0 + k;
        int16_t* dist = hunt_map[k];
        for (int c = 0; c < CELLS; ++c) dist[c] = BIG;
        for (int d = 0; d < 4; ++d) {
            int steps = 1;
            for (int c = s.tank[foe].pos + DIR_DELTA[d]; s.grid[c] == EMPTY || s.grid[c] == WATER; c += DIR_DELTA[d], ++steps)
                if (s.grid[c] == EMPTY) dist[c] = int16_t(steps <= 2 ? 0 : 2 * (steps - 2));
        }
        flood(s, dist);
        int best = -1, best_eta = SP_HUNT_MAX;
        for (int t = 0; t < 3; ++t) {
            if (used[t]) continue;
            const int i = 3 * me + t;
            const bool alive = s.tank[i].alive;
            const int pos = alive ? s.tank[i].pos : s.spawn[i];
            int eta = dist[pos] >= BIG ? BIG : (t == FAST ? dist[pos] * 5 / 8 : dist[pos]);
            if (!alive) eta += 4 * respawn_in(s, i);
            if (alive && progress_cost(i, pos) <= 4 && eta > 8) continue;        // 已经在打对方基地、离得又远：不回头
            if (prev_target[t] == foe) eta -= 8;                                 // 上回合就在追它：优先接着追
#if SP_GUARDS
            if (!(t == ARMOR || (SP_GUARDS >= 2 && t == FAST) || SP_GUARDS == 3)) eta += 12;   // 有专职守家的车时，优先派它们
#endif
            if (eta < best_eta) { best_eta = eta; best = t; }
        }
        if (best >= 0) {
            ++n_hunters;
            used[best] = true;
            hunt_target[3 * me + best] = foe;
            now_target[best] = foe;
        }
    }
    for (int t = 0; t < 3; ++t) prev_target[t] = now_target[t];
#if SP_GUARDS
    // 专职守家的车：没被派去拦截、而对方有车已经过来了，就在家门口待命。
    {
        bool coming = false;
        for (int k = 0; k < 3; ++k) {
            if (!s.tank[foe0 + k].alive) continue;
            int c = M.atk[1 - me][s.tank[foe0 + k].pos];
            if (k == FAST) c = c * 5 / 8;
            coming |= c <= SP_GUARD_RANGE;
        }
        static const int order[3] = {ARMOR, FAST, FIRE};
        int n_guards = SP_GUARDS;
        if (SP_GUARDS == 9) {                                    // 9 = 自动：对方来几辆，就留"来的数量减一"辆（最多两辆）
            int n_coming = 0;
            for (int k = 0; k < 3; ++k) {
                int c = M.atk[1 - me][s.tank[foe0 + k].alive ? s.tank[foe0 + k].pos : s.spawn[foe0 + k]];
                if (k == FAST) c = c * 5 / 8;
                n_coming += s.tank[foe0 + k].alive && c <= SP_GUARD_RANGE;
            }
            n_guards = n_coming - 1 - n_hunters;
            if (n_guards > 2) n_guards = 2;
        }
        for (int q = 0; q < n_guards && q < 3; ++q) {
            const int i = 3 * me + order[q];
            if (coming && hunt_target[i] < 0 && stale_turns < 30) holding[i] = true;
        }
        for (int c = 0; c < CELLS; ++c) post_map[c] = BIG;
        const int fwd = me == 0 ? UP : DOWN;
        const int post = s.base_pos[me] + 3 * DIR_DELTA[fwd];
        for (int d = 0; d < 4; ++d) {
            const int c = post + DIR_DELTA[d];
            if (s.grid[c] == EMPTY) post_map[c] = 0;
        }
        if (s.grid[post] == EMPTY) post_map[post] = 0;
        flood(s, post_map);
    }
#endif
}

// 评估和走位用的"离目标多远"：拦截的车看离目标敌车多远（它被打掉了就算到位），其余看进攻图。
static inline int tank_cost(const State& s, int i, int pos) {
    const int target = hunt_target[i];
    if (holding[i]) { const int a = post_map[pos]; return a > 240 ? 240 : (i % 3 == FAST ? a * 5 / 8 : a); }
    if (target < 0) return progress_cost(i, pos);
    if (!s.tank[target].alive) return 0;
    int a = hunt_map[target % 3][pos];
    if (a > 240) a = 240;
    return i % 3 == FAST ? a * 5 / 8 : a;
}

static int evaluate(const State& s) {
    if (s.done) {
        if (s.winner < 0) return 0;
        return (s.winner == 0 ? 6000 : -6000) + 10 * (s.base_hp[0] - s.base_hp[1]);
    }
    static const int tv[3] = {SP_TV, SP_TV_FIRE, SP_TV};
    int e[2] = {0, 0};
    for (int seat = 0; seat < 2; ++seat) {
        e[seat] += 100 * s.base_hp[seat] + 8 * s.kills[seat] + 2 * s.stars_eaten[seat];
        for (int k = 0; k < M.n_key[seat]; ++k)
            if (s.grid[M.key_cell[seat][k]] != BRICK && root_grid[M.key_cell[seat][k]] == BRICK) e[seat] += M.key_val[seat][k];
    }
    for (int i = 0; i < 6; ++i) {
        const Tank& t = s.tank[i];
        const int seat = i / 3, type = i % 3;
        if (t.alive) {
            e[seat] -= tv[type] * tank_cost(s, i, t.pos) / 4;
            e[seat] += 15 * (t.hp - 1) + SP_LVV * t.level;
        } else {
            e[seat] -= tv[type] * (tank_cost(s, i, s.spawn[i]) + 4 * respawn_in(s, i)) / 4 + 10;
        }
    }
#if SP_STARS
    // 星星：没满级的车离星星越近越好（吃到之后体现在等级和血量上）。
    const int mask = s.star_mask | soon_mask;
    if (mask) {
        for (int k = 0; k < s.n_supply; ++k) {
            if (!((mask >> k) & 1)) continue;
            for (int seat = 0; seat < 2; ++seat) {
                int best = BIG;
                for (int i = 3 * seat; i < 3 * seat + 3; ++i) {
                    if (!s.tank[i].alive || s.tank[i].level >= s.P.max_level) continue;
                    int a = M.sup[k][s.tank[i].pos];
                    if (i % 3 == FAST) a = a * 5 / 8;
                    if (a < best) best = a;
                }
                const int bonus = SP_STARW - best / 2;                 // 每远一回合少 2 分
                if (bonus > 0) e[seat] += bonus;
            }
        }
    }
#endif
    // 还在飞的子弹：沿弹道看它会先碰到什么。
    for (int b = 0; b < s.n_bullets; ++b) {
        const Bullet& bl = s.bullet[b];
        int c = bl.pos;
        for (int k = 1; k <= 8; ++k) {
            c += DIR_DELTA[bl.dir];
            const uint8_t g = s.grid[c];
            if (g == WATER) continue;
            if (g == BASE0 || g == BASE1) { e[g - BASE0] -= 70; break; }
            if (g != EMPTY) break;
            int v = -1;
            for (int i = 0; i < 6; ++i)
                if (s.tank[i].alive && s.tank[i].pos == c) v = i;
            if (v >= 0) {
                e[v / 3] -= k <= 2 ? 20 : 5;
                break;
            }
        }
    }
    return e[0] - e[1];
}

// ====================================================================
// 候选命令：给一辆车列出值得搜的命令，按粗略的好坏排好序（没访问过的按这个顺序先试）
// ====================================================================
struct Scan {             // 朝一个方向看过去的结果
    bool useful;          // 这个方向开炮有没有意义
    int prior;            // 开炮的先验分
    bool danger;          // 这个方向有威胁（子弹正飞来，或 2 格内有能开炮的敌车）
};

static Scan scan_dir(const State& s, int i, int d) {
    Scan r{false, 0, false};
    const int seat = i / 3;
    int bricks = 0, c = s.tank[i].pos;
    for (int k = 1; k <= 14; ++k) {
        c += DIR_DELTA[d];
        const uint8_t g = s.grid[c];
        if (g == WATER) continue;
        if (g == BRICK) {
            if (bricks == 0) {
                bool key = false;
                for (int q = 0; q < M.n_key[seat]; ++q) key |= M.key_cell[seat][q] == c;
                if (key && k <= 6) { r.useful = true; r.prior = r.prior > 62 ? r.prior : 62; }
                else if (k <= 2) { r.useful = true; r.prior = r.prior > 30 ? r.prior : 30; }
            }
            ++bricks;
            continue;
        }
        if (g == BASE0 + (1 - seat)) {
            if (k <= 6) { r.useful = true; const int p = 96 - 10 * bricks; r.prior = r.prior > p ? r.prior : p; }
            break;
        }
        if (g == STEEL) {
            if (bricks == 0 && k <= 2 && s.tank[i].level >= 2 && s.P.level2_steel) { r.useful = true; r.prior = r.prior > 25 ? r.prior : 25; }
            break;
        }
        if (g != EMPTY) break;
        if (bricks) continue;
        for (int b = 0; b < s.n_bullets; ++b)
            if (s.bullet[b].pos == c && s.bullet[b].dir == (d ^ 1) && k <= 6) {
                r.useful = true;
                r.danger = true;
                r.prior = r.prior > 85 ? r.prior : 85;
            }
        int v = -1;
        for (int j = 0; j < 6; ++j)
            if (s.tank[j].alive && s.tank[j].pos == c) v = j;
        if (v >= 0) {
            if (v / 3 == seat) break;                         // 队友挡着
            r.useful = true;
            const bool armed = can_fire(s, v);
            if (k <= 2 && armed) r.danger = true;
            const int p = k <= 2 ? (armed ? 88 : 99) : (k <= 4 ? 64 : 44);
            r.prior = r.prior > p ? r.prior : p;
            break;
        }
        // 敌车就在炮线旁边一格，可能走进来：预判射击。
        if (k <= 5 && !r.useful) {
            const int side = (d < 2) ? 1 : STRIDE;
            for (int j = 3 * (1 - seat); j < 3 * (1 - seat) + 3; ++j)
                if (s.tank[j].alive && (s.tank[j].pos == c + side || s.tank[j].pos == c - side)) { r.useful = true; r.prior = 36; }
        }
    }
    return r;
}

struct Node {
    int32_t visits;
    float total;                // 经过这个节点的所有模拟的分数之和
    int32_t child[MAX_ACTS];
    int32_t n[MAX_ACTS];
    float sum[MAX_ACTS];
    float p[MAX_ACTS];          // 先验概率
    Cmd act[MAX_ACTS];
    uint8_t n_acts;
};

static int goal_cost(const State& s, int i, int pos) {       // 走位的先验：离目标（或星星）多远
    if (hunt_target[i] >= 0) return hunt_map[hunt_target[i] % 3][pos];
    if (holding[i]) return post_map[pos];
    int a = atk_of[i][pos];
#if SP_STARS
    if (s.tank[i].level < s.P.max_level && (s.star_mask | soon_mask)) {
        for (int k = 0; k < s.n_supply; ++k)
            if (((s.star_mask | soon_mask) >> k) & 1) {
                const int b = M.sup[k][pos] + 8;
                if (b < a && M.sup[k][pos] <= SP_STAR_REACH) a = b;
            }
    }
#else
    (void)s;
#endif
    return a;
}

static void build_actions(Node& nd, const State& s, int i) {
    Cmd list[N_CMDS];
    const int n = legal_commands(s, i, list);
    const Tank& t = s.tank[i];
    Scan sc[4];
    bool danger = false;
    for (int d = 0; d < 4; ++d) { sc[d] = scan_dir(s, i, d); danger |= sc[d].danger; }
    const int here = goal_cost(s, i, t.pos);
    Cmd acts[N_CMDS];
    int prior[N_CMDS], m = 0;
    int ends[N_CMDS], n_ends = 0;     // 两步命令按终点去重
    for (int k = 0; k < n; ++k) {
        const Cmd c = list[k];
        int p;
        if (c == STAY) {
            p = 46;
        } else if (is_fire(c)) {
            if (!sc[c - 5].useful) continue;
            p = sc[c - 5].prior;
        } else if (is_move(c)) {
            const int to = t.pos + DIR_DELTA[c - 1];
            p = 50 + (here - goal_cost(s, i, to)) * 2;
            if (danger && !sc[c - 1].danger && !sc[(c - 1) ^ 1].danger) p += 30;      // 横向让开
        } else {
            const int d1 = (c - 9) >> 2, d2 = (c - 9) & 3;
            const int mid = t.pos + DIR_DELTA[d1];
            if (s.grid[mid] != EMPTY) continue;                     // "原地等一小步再走"：用处不大，不搜
            const int to = mid + DIR_DELTA[d2];
            if (to == t.pos) {                                      // 出去再回来：只在有威胁时才值得（晃过一发子弹）
                if (!danger || sc[d1].danger || sc[d2].danger) continue;
                p = 70;
            } else {
                if (s.grid[to] != EMPTY) continue;                  // 第二步多半走不成，等于一步
                bool dup = false;
                for (int q = 0; q < n_ends; ++q) dup |= ends[q] == to;
                if (dup) continue;                                  // 同一个终点的两条折线只留一条
                ends[n_ends++] = to;
                p = 52 + (here - goal_cost(s, i, to)) * 2;
                if (danger) p += 20;
            }
        }
        acts[m] = c;
        prior[m++] = p;
    }
    // 按先验分从高到低排，最多留 MAX_ACTS 条。
    for (int a = 1; a < m; ++a)
        for (int b = a; b > 0 && prior[b] > prior[b - 1]; --b) {
            const int tp = prior[b]; prior[b] = prior[b - 1]; prior[b - 1] = tp;
            const Cmd tc = acts[b]; acts[b] = acts[b - 1]; acts[b - 1] = tc;
        }
    if (m > MAX_ACTS) m = MAX_ACTS;
    nd.n_acts = uint8_t(m);
    float z = 0;
    for (int a = 0; a < m; ++a) {
        nd.act[a] = acts[a];
        nd.child[a] = -1;
        nd.n[a] = 0;
        nd.sum[a] = 0;
        nd.p[a] = std::exp(float(prior[a] - prior[0]) / float(SP_PRIOR_T));
        z += nd.p[a];
    }
    for (int a = 0; a < m; ++a) nd.p[a] /= z;
}

// ====================================================================
// 树外策略：便宜的规则走法（朝进攻位走，看得到目标就开炮）
// ====================================================================
static uint64_t rng_state;
static inline uint32_t rng() {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return uint32_t(rng_state >> 16);
}

static Cmd rollout_cmd(const State& s, int i) {
    const Tank& t = s.tank[i];
    const int seat = i / 3;
    const uint32_t r = rng();
    const bool armed = can_fire(s, i);
    if (armed && (r & 7) != 0) {
        for (int d = 0; d < 4; ++d) {
            int c = t.pos, bricks = 0;
            for (int k = 1; k <= 4; ++k) {
                c += DIR_DELTA[d];
                const uint8_t g = s.grid[c];
                if (g == WATER) continue;
                if (g == BRICK) { if (++bricks > 1) break; continue; }
                if (g == BASE0 + (1 - seat)) return cmd_fire(d);
                if (g != EMPTY) break;
                if (bricks) continue;
                int v = -1;
                for (int j = 0; j < 6; ++j)
                    if (s.tank[j].alive && s.tank[j].pos == c) v = j;
                if (v >= 0) {
                    if (v / 3 != seat && k <= 2) return cmd_fire(d);
                    break;
                }
            }
        }
    }
    const int16_t* dist = hunt_target[i] >= 0 ? hunt_map[hunt_target[i] % 3] : (holding[i] ? post_map : atk_of[i]);
    int pos = t.pos, dirs[2] = {-1, -1};
    const int steps = (i % 3 == FAST) ? 2 : 1;
    for (int k = 0; k < steps; ++k) {
        int best = -1, best_cost = dist[pos];
        if (((r >> (8 + 4 * k)) & 15) < 16 / SP_ROLLOUT_RANDOM) {
            const int d = (r >> (16 + 2 * k)) & 3;
            if (s.grid[pos + DIR_DELTA[d]] == EMPTY) best = d;
        } else {
            for (int d = 0; d < 4; ++d) {
                const int to = pos + DIR_DELTA[d];
                if (dist[to] < best_cost) { best_cost = dist[to]; best = d; }
            }
        }
        if (best < 0) break;
        if (s.grid[pos + DIR_DELTA[best]] == BRICK) {
            if (k == 0 && armed) return cmd_fire(best);
            break;
        }
        dirs[k] = best;
        pos += DIR_DELTA[best];
    }
    if (dirs[0] < 0) return STAY;
    if (dirs[1] < 0) return cmd_move(dirs[0]);
    return cmd_move2(dirs[0], dirs[1]);
}

// ====================================================================
// 搜索
// ====================================================================
static std::vector<Node> pool;
static int pool_used;
static long long total_steps;           // 统计用：本局累计前推次数

static inline int new_node() {
    if (pool_used >= int(pool.size())) return -1;
    Node& nd = pool[pool_used];
    nd.visits = 0;
    nd.total = 0;
    nd.n_acts = 0;
    return pool_used++;
}

static inline int select_action(const Node& nd) {
    // PUCT：估值 + 探索项。探索项按先验概率分配，所以先验高的命令先被试、试得多；
    // 没试过的命令估值取"节点均值减一点"，不会六辆车同时乱试。
    if (nd.visits == 0) return 0;                       // 第一次来：走先验最高的
    const float sq = std::sqrt(float(nd.visits)) * (SP_UCB / 100.0f);
    const float fpu = nd.total / float(nd.visits) - SP_FPU / 100.0f;
    int best = 0;
    float best_u = -1e9f;
    for (int a = 0; a < nd.n_acts; ++a) {
        const float q = nd.n[a] ? nd.sum[a] / float(nd.n[a]) : fpu;
        const float u = q + sq * nd.p[a] / float(1 + nd.n[a]);
        if (u > best_u) { best_u = u; best = a; }
    }
    return best;
}

void init(const State& s, int me) {
    (void)me;
    M.valid = false;
    total_steps = 0;
    for (int t = 0; t < 3; ++t) prev_target[t] = -1;
    for (int j = 0; j < 6; ++j) { still[j] = 0; still_pos[j] = -1; }
    stale_turns = 0;
    stale_hp[0] = stale_hp[1] = stale_kills[0] = stale_kills[1] = -1;
    stale_walls = 0;
    const size_t want = size_t(SP_BUDGET) / SP_DEPTH * 6 + 64;
    if (pool.size() != (want < 600000 ? want : 600000)) pool.assign(want < 600000 ? want : 600000, Node{});
    build_maps(s);
}

#if SP_METHOD == 1
// 对照用的另一种搜索：每辆车把自己的每条候选命令前推一回合（其余五辆用当前选定的命令），
// 之后所有车按树外的规则策略续推到 SP_DEPTH 回合，沿途评估取平均；六辆车轮流这样取最佳应对，做 SP_IBR_ROUNDS 轮。
// 这是"规则型 + 一回合前推 + 续推"的常规写法，预算用不满（每轮约 6 辆 × 十几条命令 × SP_DEPTH 次前推）。
static Orders decide_ibr(const State& s, int me) {
    static Node nd[6];
    Cmd choice[6];
    for (int i = 0; i < 6; ++i) {
        choice[i] = STAY;
        nd[i].n_acts = 0;
        if (!s.tank[i].alive) continue;
        build_actions(nd[i], s, i);
        choice[i] = nd[i].act[0];
    }
    const uint64_t seed = rng_state;
    for (int round = 0; round < SP_IBR_ROUNDS; ++round) {
        bool changed = false;
        for (int k = 0; k < 6; ++k) {
            const int i = (round & 1) ? 5 - k : k;
            if (nd[i].n_acts <= 1) continue;
            int best = -1;
            long long best_score = 0;
            for (int a = 0; a < nd[i].n_acts; ++a) {
                Orders o[2];
                for (int j = 0; j < 6; ++j) o[j / 3].cmd[j % 3] = j == i ? nd[i].act[a] : choice[j];
                State st = s;
                step(st, o[0], o[1]);
                long long sum = evaluate(st);
                rng_state = seed;                                  // 每条候选用同一串随机数，比较才公平
                for (int d = 1; d < SP_DEPTH; ++d) {
                    if (st.done) { sum += evaluate(st); continue; }
                    Orders q[2];
                    for (int j = 0; j < 6; ++j)
                        if (st.tank[j].alive) q[j / 3].cmd[j % 3] = rollout_cmd(st, j);
                    step(st, q[0], q[1]);
                    sum += evaluate(st);
                }
                if (i >= 3) sum = -sum;
                if (best < 0 || sum > best_score) { best_score = sum; best = a; }
            }
            if (nd[i].act[best] != choice[i]) { choice[i] = nd[i].act[best]; changed = true; }
        }
        if (!changed) break;
    }
    Orders out;
    for (int type = 0; type < 3; ++type) out.cmd[type] = choice[tank_id(me, type)];
    return out;
}
#endif

Orders decide(const State& s, int me) {
    build_maps(s);
    build_tank_maps(s);
    assign_roles(s, me);
    std::memcpy(root_grid, s.grid, sizeof(root_grid));
    // 下一批星星 8 回合内出现：把没有星星的补给点也当作目标，提前去占位。
    soon_mask = 0;
    {
        const int t = s.turn;
        int next = s.P.supply_first;
        if (t >= next) next += ((t - s.P.supply_first) / s.P.supply_period + 1) * s.P.supply_period;
        if (next - t <= 8) soon_mask = ((1 << s.n_supply) - 1) & ~s.star_mask;
    }
    rng_state = 0x9E3779B97F4A7C15ULL ^ (uint64_t(s.turn) * 0x100000001B3ULL) ^ s.wall_hash;
    for (int k = 0; k < 4; ++k) rng();

#if SP_METHOD == 1
    return decide_ibr(s, me);
#endif
    pool_used = 0;
    int root[6];
    for (int i = 0; i < 6; ++i) root[i] = new_node();

    int steps = 0, sims = 0;
    struct Visit { int32_t node; int8_t act; };
    Visit path[6][SP_DEPTH];
    int path_len[6];
    for (;;) {
        if (steps + SP_DEPTH > SP_BUDGET) break;
#if SP_USE_TIME
        if ((sims & 15) == 0 && budget_left() < SP_TIME_MARGIN) break;
#endif
        State st = s;
        int cur[6];
        bool expanded[6] = {};
        for (int i = 0; i < 6; ++i) { cur[i] = root[i]; path_len[i] = 0; }
        int e_sum = 0, depth_done = 0;
        for (int depth = 0; depth < SP_DEPTH && !st.done; ++depth) {
            Orders o[2];
            for (int i = 0; i < 6; ++i) {
                if (!st.tank[i].alive) continue;
                Cmd c;
                if (cur[i] >= 0) {
                    Node& nd = pool[cur[i]];
                    if (nd.n_acts == 0) build_actions(nd, st, i);
                    const int a = select_action(nd);
                    c = nd.act[a];
                    path[i][path_len[i]++] = Visit{cur[i], int8_t(a)};
                    if (nd.child[a] < 0 && !expanded[i] && nd.n[a] > 0) {      // 第二次走到这条命令时才往下长一层
                        nd.child[a] = new_node();
                        expanded[i] = true;
                    }
                    cur[i] = nd.child[a];
                } else {
                    c = rollout_cmd(st, i);
                }
                o[i / 3].cmd[i % 3] = c;
            }
            step(st, o[0], o[1]);
            ++steps;
            ++depth_done;
            e_sum += evaluate(st);
            if (st.done) e_sum += (SP_DEPTH - depth_done) * evaluate(st);     // 提前结束：剩下的回合都按终局分算
        }
        // 一次模拟的分数取沿途每回合评估的平均，而不是只看最后一回合：
        // 这是个拼时间的游戏，"早三回合打到""中途死过一次又复活"都应该体现在分数里。
#if SP_PATH_AVG
        const int e = e_sum / SP_DEPTH;
#else
        const int e = evaluate(st);
#endif
        const float v = float(e) / (float(SP_SQUASH) + float(e < 0 ? -e : e));
        for (int i = 0; i < 6; ++i) {
            const float vi = i < 3 ? v : -v;
            for (int k = 0; k < path_len[i]; ++k) {
                Node& nd = pool[path[i][k].node];
                nd.visits += 1;
                nd.total += vi;
                nd.n[path[i][k].act] += 1;
                nd.sum[path[i][k].act] += vi;
            }
        }
        ++sims;
    }
    total_steps += steps;
#ifdef SP_DEBUG
    if (me == SP_DEBUG) {
        std::fprintf(stderr, "T%d sims=%d nodes=%d\n", s.turn + 1, sims, pool_used);
        for (int i = 0; i < 6; ++i) {
            const Node& nd = pool[root[i]];
            std::fprintf(stderr, "  tank%d Q=%.3f:", i, nd.visits ? nd.total / nd.visits : 0.f);
            for (int a = 0; a < nd.n_acts; ++a)
                std::fprintf(stderr, " %s(p%.2f n%d q%.3f)", cmd_text(nd.act[a]).c_str(), nd.p[a], nd.n[a], nd.n[a] ? nd.sum[a] / nd.n[a] : 0.f);
            std::fputc(10, stderr);
        }
    }
#endif

    Orders out;
    for (int type = 0; type < 3; ++type) {
        const Node& nd = pool[root[tank_id(me, type)]];
        int best = -1;
        for (int a = 0; a < nd.n_acts; ++a)
            if (best < 0 || nd.n[a] > nd.n[best] || (nd.n[a] == nd.n[best] && nd.n[a] > 0 && nd.sum[a] > nd.sum[best])) best = a;
        if (best >= 0) out.cmd[type] = nd.act[best];
    }
#if SP_FFFIX
    // 三辆车的命令是各自在自己的树里选的，偶尔会凑成"一辆开炮、另一辆正好走进炮线"。
    // 出手前用对方最可能的命令前推一回合看一眼：有队友被自己人打中，就把开炮的那辆换成它访问次数次多的非开炮命令。
    for (int round = 0; round < 3; ++round) {
        Orders o[2];
        o[me] = out;
        for (int type = 0; type < 3; ++type) {
            const Node& nd = pool[root[tank_id(1 - me, type)]];
            int best = -1;
            for (int a = 0; a < nd.n_acts; ++a)
                if (best < 0 || nd.n[a] > nd.n[best]) best = a;
            if (best >= 0) o[1 - me].cmd[type] = nd.act[best];
        }
        State st = s;
        EventLog log;
        step(st, o[0], o[1], &log);
        int culprit = -1;
        for (const Event& e : log.list)
            if ((e.type == EV_HIT || e.type == EV_HIT_SWAP) && e.a / 3 == me && e.b / 3 == me && is_fire(out.cmd[e.b % 3])) culprit = e.b;
        if (culprit < 0) break;
        const Node& nd = pool[root[culprit]];
        int best = -1;
        for (int a = 0; a < nd.n_acts; ++a)
            if (!is_fire(nd.act[a]) && (best < 0 || nd.n[a] > nd.n[best])) best = a;
        out.cmd[culprit % 3] = best >= 0 ? nd.act[best] : STAY;
    }
#endif
    return out;
}

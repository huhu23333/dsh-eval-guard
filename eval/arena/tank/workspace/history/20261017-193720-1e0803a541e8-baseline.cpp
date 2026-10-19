// baseline.cpp —— 基准（正式比赛的 0 分线）。
//
// 规则型，思路常规，完全确定（不看时间，不用随机数）：
//   分工：火力型主攻，走一条侧路；快速型有星星先抢星，然后走另一条侧路进攻；
//         装甲型走正面，但有敌车闯到家门口而自己离得不远时回防去打它。刚复活的车也先打家门口的敌车。
//   每辆车把自己的每条合法命令都用引擎前推一回合（假设对方不动、子弹照飞），
//   看会不会挨打、会不会误伤，再加上"离目标近了多少""这一炮打得到什么"的规则分，取最高的。
//   这样自然就会躲已经在飞的子弹；另外单独处理"近处有敌车能开炮正对着我"的情况。
//   装填：开不了炮时合法命令里没有开炮，自然会去走位；贴脸又开不了炮时会让开炮线。
// 最多前推一回合，没有搜索。
#include "tank.hpp"

using namespace tank;

static const int FAR = 9999;
#ifndef BL_GUARD
#define BL_GUARD 0              // 1 = 装甲型专职守家：没有敌车闯进来时站在基地正前方第 3 格，不去进攻
#endif
#ifndef BL_ALLSTAR
#define BL_ALLSTAR 0            // 1 = 三辆车都先吃星（升级值钱的规则下用）
#endif
#ifndef BL_NOSTAR
#define BL_NOSTAR 0             // 1 = 快速型不绕路吃星
#endif
#ifndef GUARD_REACH
#define GUARD_REACH 10          // 装甲型离闯入的敌车不超过这么多格就回防
#endif
static int my_seat;
static int last_pos[3], last_cmd[3], stuck[3];     // 上一回合的位置和命令：同一步连着走不成就降权，免得卡死
static int lane_of[3];                              // 每辆车进攻走哪条炮线（方向位掩码），开局定好
static int far_shots[3], kills_then[3], bored_until[3];   // 朝敌车连开几炮都没打掉（多半是互相抵消）：先不打了，绕路

// 带权最短路。dist 里事先填好起点代价（其余 FAR）；空地一步 1，砖墙 brick_cost（要先打掉），其他不可走。
static bool crowded[CELLS];                         // 队友占着的格子：寻路时加代价，免得排队堵在一条道上
static void flood(const State& s, int* dist, int brick_cost) {
    static int queue[CELLS * 8];
    int head = 0, tail = 0;
    for (int c = 0; c < CELLS; ++c)
        if (dist[c] < FAR) queue[tail++] = c;
    while (head < tail) {
        const int c = queue[head++ % (CELLS * 8)];
        for (int d = 0; d < 4; ++d) {
            const int n = c + DIR_DELTA[d];
            const uint8_t g = s.grid[n];
            if (g != EMPTY && g != BRICK) continue;
            const int nd = dist[c] + (g == BRICK ? brick_cost : 1) + (crowded[n] ? 4 : 0);
            if (nd < dist[n]) { dist[n] = nd; queue[tail++ % (CELLS * 8)] = n; }
        }
    }
}

// 进攻图：到"能打到对方基地的格子"的代价。基地只能沿它所在的行和列被打到，所以一共三条炮线：
// 正面一列、左右两条沿底边的行。lanes 是允许走的炮线（按方向的位掩码），两辆进攻的车各走一边。
static void attack_map(const State& s, int me, int lanes, int* dist) {
    for (int c = 0; c < CELLS; ++c) dist[c] = FAR;
    for (int d = 0; d < 4; ++d) {
        if (!((lanes >> d) & 1)) continue;
        int bricks = 0, k = 1;
        for (int c = s.base_pos[1 - me] + DIR_DELTA[d];; c += DIR_DELTA[d], ++k) {
            const uint8_t g = s.grid[c];
            const int far = k <= 4 ? 0 : k - 4;            // 4 格内算到位；更远的还得往前走
            if (g == BRICK) {                              // 炮线上的砖：打掉之后站进去也是射击位
                dist[c] = 2 * bricks + 3 + far;            // （两侧被钢墙封住时，围墙那一格是唯一的侧面射击位）
                ++bricks;
                continue;
            }
            if (g == WATER) continue;
            if (g != EMPTY) break;
            dist[c] = 2 * bricks + far;                    // 中间每块砖要多打一炮
        }
    }
    flood(s, dist, 3);
}

// 到某些格子的距离图。
static void target_map(const State& s, const int* cells, int n, int* dist) {
    for (int c = 0; c < CELLS; ++c) dist[c] = FAR;
    for (int k = 0; k < n; ++k) dist[cells[k]] = 0;
    flood(s, dist, 3);
}

// 追击图：到"和敌车 e 在同一条畅通炮线上"的格子的距离。
static void hunt_map(const State& s, int e, int* dist) {
    for (int c = 0; c < CELLS; ++c) dist[c] = FAR;
    for (int d = 0; d < 4; ++d) {
        int k = 1;
        for (int c = s.tank[e].pos + DIR_DELTA[d]; s.grid[c] == EMPTY || s.grid[c] == WATER; c += DIR_DELTA[d], ++k)
            if (s.grid[c] == EMPTY) dist[c] = k / 2;
    }
    flood(s, dist, 3);
}

static int manhattan(int a, int b) {
    const int dx = cell_x(a) - cell_x(b), dy = cell_y(a) - cell_y(b);
    return (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
}

// a、b 在同一行或同一列，中间没有墙和坦克（水不挡子弹）：返回距离，否则 0。
static int clear_line(const State& s, int a, int b) {
    for (int d = 0; d < 4; ++d) {
        int k = 1;
        for (int c = a + DIR_DELTA[d]; s.grid[c] == EMPTY || s.grid[c] == WATER; c += DIR_DELTA[d], ++k) {
            if (c == b) return k;
            if (tank_at(s, c) >= 0) break;
        }
    }
    return 0;
}

// 朝方向 d 开一炮值多少分：看炮线上先碰到什么。
static int fire_value(const State& s, int me, int i, int d, bool brick_on_path, bool bored, bool* far_tank) {
    int bricks = 0, k = 1;
    for (int c = s.tank[i].pos + DIR_DELTA[d];; c += DIR_DELTA[d], ++k) {
        const uint8_t g = s.grid[c];
        if (g == WATER) continue;
        if (g == BRICK) { ++bricks; continue; }
        if (g == BASE0 + (1 - me)) {                                  // 对方基地（隔着砖也值得打）：比打车优先，车三回合就复活，基地血不会回
            if (k <= 4) return 700 - 40 * bricks;
            break;                                                    // 太远不打：子弹飞到之前不能再开炮，还容易被挡
        }
        if (g == BASE0 + me) return bricks ? -500 : -1000;            // 自己的基地
        if (g != EMPTY) break;                                        // 钢墙、界外
        if (bricks) continue;                                         // 砖后面的车和子弹先不管
        for (int b = 0; b < s.n_bullets; ++b)                         // 正对着飞来的敌方子弹：开炮能把它抵消掉
            if (s.bullet[b].pos == c && s.bullet[b].dir == (d ^ 1) && seat_of(s.bullet[b].owner) != me && k <= 6) return 150;
        const int v = tank_at(s, c);
        if (v < 0) continue;
        if (seat_of(v) == me) return -1000;                           // 队友
        *far_tank = true;                                             // 这一炮是朝敌车打的（记数用）
        if (k <= 2 && !can_fire(s, v)) return 600;                    // 2 格内而且它正在装填：抵消不了，最值得打
        if (bored) return -5;                                         // 对着打了几炮都被抵消：先不打，绕开
        if (k <= 2) return 500;                                       // 2 格内：当回合就到
        return k <= 4 ? 200 : -5;                                     // 远一些：对方来得及躲或者开炮抵消，再远就不浪费时间了
    }
    return (bricks && brick_on_path) ? 120 : -5;                      // 挡路的砖 / 空炮
}

void init(const State& s, int me) {
    my_seat = me;
    // 火力型走离自己出生点近的那一侧，快速型走另一侧；正面那一列对着对方的出生点，容易被堵，不走。
    const int fire_side = cell_x(s.spawn[tank_id(me, FIRE)]) <= cell_x(s.base_pos[me]) ? LEFT : RIGHT;
    lane_of[FIRE] = 1 << fire_side;
    lane_of[FAST] = 1 << (fire_side ^ 1);
    lane_of[ARMOR] = 1 << (me == 0 ? DOWN : UP);            // 装甲型走正面（从对方基地看过来的那一列）
    for (int k = 0; k < 3; ++k) { last_pos[k] = -1; last_cmd[k] = STAY; stuck[k] = 0; far_shots[k] = 0; kills_then[k] = 0; bored_until[k] = 0; }
}

Orders decide(const State& s, int me) {
    static int goal[CELLS];
    Orders mine, theirs;
    const int home = s.base_pos[me];


    for (int type = 0; type < 3; ++type) {
        const int i = tank_id(me, type);
        const Tank& t = s.tank[i];
        if (!t.alive) { last_pos[type] = -1; stuck[type] = 0; continue; }

        // 上一回合下了移动命令却没动：记一次，同一条命令这回合降权。
        if (last_pos[type] == t.pos && last_cmd[type] != STAY && !is_fire(Cmd(last_cmd[type]))) stuck[type] += 1;
        else stuck[type] = 0;

        // ---- 这辆车的目标：填好 goal[] ----
        for (int c = 0; c < CELLS; ++c) crowded[c] = false;
        for (int j = 3 * me; j < 3 * me + 3; ++j)
            if (j != i && s.tank[j].alive) crowded[s.tank[j].pos] = true;
        // 闯到我方基地附近（7 格内）的敌车：装甲型离它不太远就回防去追；另外两辆只有自己还在家门口（比如刚复活）
        // 时才先去打它，出了门就不回头，免得在半路来回摇摆。
        int intruder = -1, intruder_dist = FAR;
        for (int e = 3 * (1 - me); e < 3 * (1 - me) + 3; ++e) {
            if (!s.tank[e].alive || manhattan(s.tank[e].pos, home) > 7) continue;
            const int gap = manhattan(s.tank[e].pos, t.pos);
            if ((type == ARMOR ? (BL_GUARD || gap <= GUARD_REACH) : manhattan(t.pos, home) <= 5) && gap < intruder_dist) { intruder = e; intruder_dist = gap; }
        }
        if (intruder >= 0) {
            hunt_map(s, intruder, goal);                             // 去和它对上炮线
        } else if (BL_GUARD && type == ARMOR) {
            int post = home + 3 * DIR_DELTA[me == 0 ? UP : DOWN];
            if (s.grid[post] != EMPTY) post = s.spawn[i];
            target_map(s, &post, 1, goal);
        } else {
            attack_map(s, me, lane_of[type], goal);
            if (goal[t.pos] >= FAR) attack_map(s, me, 15, goal);      // 这条炮线被钢墙封死了：哪条都行
            // 快速型：有星星、自己没满级、而且还没打到对方家门口（进攻图上还差 4 步以上）时，先去吃星。
            if (!BL_NOSTAR && (type == FAST || BL_ALLSTAR) && s.star_mask && t.level < s.P.max_level && goal[t.pos] > 3) {
                int stars[MAX_SUPPLY], n = 0;
                for (int k = 0; k < s.n_supply; ++k)
                    if ((s.star_mask >> k) & 1) stars[n++] = s.supply[k];
                target_map(s, stars, n, goal);
            }
        }
        int best_step = -1;                                           // 朝目标最近的邻格方向（判断"挡路的砖"用）
        for (int d = 0, best = goal[t.pos]; d < 4; ++d)
            if (goal[t.pos + DIR_DELTA[d]] < best) { best = goal[t.pos + DIR_DELTA[d]]; best_step = d; }

        // ---- 逐条命令打分 ----
        Cmd list[N_CMDS];
        const int n = legal_commands(s, i, list);
        int best_score = -1000000;
        Cmd best_cmd = STAY;
        bool best_far = false;
        for (int k = 0; k < n; ++k) {
            const Cmd c = list[k];
            Orders trial = mine;                                      // 前面的车已经定下的命令也带上
            trial.cmd[type] = c;
            State next = s;
            step_as(next, me, trial, theirs);                         // 前推一回合：对方不动，子弹照飞

            int score = 0;
            // 挨打、误伤、打中：直接看前推结果。
            for (int j = 3 * me; j < 3 * me + 3; ++j)
                if (s.tank[j].alive) score -= 1000 * (s.tank[j].hp - (next.tank[j].alive ? next.tank[j].hp : 0));
            for (int e = 3 * (1 - me); e < 3 * (1 - me) + 3; ++e)
                if (s.tank[e].alive) score += 400 * (s.tank[e].hp - (next.tank[e].alive ? next.tank[e].hp : 0));
            score -= 1000 * (s.base_hp[me] - next.base_hp[me]);
            score += 300 * (s.base_hp[1 - me] - next.base_hp[1 - me]);

            const int pos = next.tank[i].alive ? next.tank[i].pos : t.pos;
            // 近处（2 格内，子弹当回合就到）有能开炮的敌车正对着我：除非我朝它开炮（两发会抵消），否则危险。
            for (int e = 3 * (1 - me); e < 3 * (1 - me) + 3; ++e) {
                if (!s.tank[e].alive || !can_fire(s, e)) continue;
                const int gap = clear_line(s, s.tank[e].pos, pos);
                if (gap == 0 || gap > 2) continue;
                bool answering = false;
                if (is_fire(c)) {
                    int probe = t.pos;
                    for (int step_k = 0; step_k < gap; ++step_k) probe += DIR_DELTA[c - 5];
                    answering = probe == s.tank[e].pos;
                }
                if (!answering) score -= 250;
            }
            // 前推之后还有子弹正对着我飞来（下一回合到）：尽量别待在这条线上。
            for (int b = 0; b < next.n_bullets; ++b) {
                int probe = next.bullet[b].pos;
                for (int step_k = 0; step_k < 4; ++step_k) {
                    probe += DIR_DELTA[next.bullet[b].dir];
                    if (next.grid[probe] != EMPTY && next.grid[probe] != WATER) break;
                    if (probe == pos) { score -= 40; break; }
                }
            }
            // 规则分：开炮看打得到什么，移动看离目标近了多少。
            bool far_tank = false;
            if (is_fire(c))
                score += fire_value(s, me, i, c - 5, best_step == c - 5 && s.grid[t.pos + DIR_DELTA[c - 5]] == BRICK,
                                    s.turn < bored_until[type], &far_tank);
            score -= 10 * (goal[pos] < FAR ? goal[pos] : 200);
            if (c == STAY) score -= 1;
            if (c == last_cmd[type]) score -= 30 * stuck[type];
            if (score > best_score) { best_score = score; best_cmd = c; best_far = far_tank; }
        }
        // 朝敌车连开 3 炮没有战果（多半是面对面互相抵消）：歇 8 回合，期间只打正在装填的敌车。
        if (s.kills[me] != kills_then[type]) { kills_then[type] = s.kills[me]; far_shots[type] = 0; }
        if (best_far && ++far_shots[type] >= 3) { far_shots[type] = 0; bored_until[type] = s.turn + 8; }
        mine.cmd[type] = best_cmd;
        last_pos[type] = t.pos;
        last_cmd[type] = best_cmd;
    }
    return mine;
}

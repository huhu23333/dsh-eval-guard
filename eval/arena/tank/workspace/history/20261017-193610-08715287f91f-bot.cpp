// template.cpp —— 起步模板。能合法行动，打法最朴素：三辆车都朝对方基地走，
// 炮线上是对方基地（中间只隔着砖）就开炮，路被砖挡住就把砖打掉。
// 它不躲子弹、不分工、不看对方坦克，也不用引擎前推，所以很弱。请把它当作接口的用法示例，然后重写。
//
// 你要实现的只有两个函数：
//   void init(const tank::State& s, int me);            每局开始调用一次，me 是你的座位（0 在下方，1 在上方）
//   tank::Orders decide(const tank::State& s, int me);  每回合调用一次，返回三辆车的命令
// 全局变量可以跨回合保留。tank.hpp 里有完整的规则实现：tank::step 可以把局面精确前推一回合。
#include "tank.hpp"

using namespace tank;

static int dist_to_base[CELLS];   // 每个格子到对方基地要走几步（砖墙当作可以走，因为能打掉）

// 跨回合记的东西（全局变量会保留）：用来发现"卡住了"。
static int last_pos[3];           // 每辆车上一回合在哪一格
static int stuck_turns[3];        // 每辆车已经连续几回合没挪地方
static int last_enemy_hp;         // 上一回合对方基地的血量
static int quiet_turns;           // 对方基地已经连续几回合没掉血

// 从对方基地出发做一次广度优先搜索，填好 dist_to_base。钢墙、水、己方基地、界外都走不了。
static void compute_distances(const State& s, int me) {
    static int queue[CELLS];
    for (int c = 0; c < CELLS; ++c) dist_to_base[c] = 9999;
    int head = 0, tail = 0;
    const int target = s.base_pos[1 - me];
    dist_to_base[target] = 0;
    queue[tail++] = target;
    while (head < tail) {
        const int c = queue[head++];
        for (int d = 0; d < 4; ++d) {
            const int next = c + DIR_DELTA[d];            // 四周有一圈界外格，不用判断越界
            const uint8_t g = s.grid[next];
            if ((g == EMPTY || g == BRICK) && dist_to_base[next] > dist_to_base[c] + 1) {
                dist_to_base[next] = dist_to_base[c] + 1;
                queue[tail++] = next;
            }
        }
    }
}

// 从 pos 朝方向 d 看过去：一路只有空地、水、砖墙，然后是对方基地，就返回 true。
// 注意：这里只看地形（s.grid），不看这条线上有没有坦克；线上的坦克会先挨这一炮，要另用 tank_at 查。
static bool base_in_line(const State& s, int me, int pos, int d) {
    for (int c = pos + DIR_DELTA[d];; c += DIR_DELTA[d]) {
        const uint8_t g = s.grid[c];
        if (g == BASE0 + (1 - me)) return true;
        if (g != EMPTY && g != WATER && g != BRICK) return false;   // 钢墙、己方基地、界外
    }
}

void init(const State& s, int me) {
    // 首回合有 2 秒，可以在这里做预计算。模板只把跨回合的记录清零。
    for (int type = 0; type < 3; ++type) { last_pos[type] = -1; stuck_turns[type] = 0; }
    last_enemy_hp = s.base_hp[1 - me];
    quiet_turns = 0;
}

Orders decide(const State& s, int me) {
    Orders orders;                // 默认三辆车都不动
    compute_distances(s, me);     // 砖墙会被打掉，所以每回合重算（很快）
    quiet_turns = s.base_hp[1 - me] < last_enemy_hp ? 0 : quiet_turns + 1;
    last_enemy_hp = s.base_hp[1 - me];

    for (int type = 0; type < 3; ++type) {       // 0 快速型，1 火力型，2 装甲型
        const int i = tank_id(me, type);         // 这辆车在 s.tank[] 里的编号
        const Tank& t = s.tank[i];
        if (!t.alive) { last_pos[type] = -1; continue; }   // 被毁后等复活，命令会被忽略
        const bool ready = can_fire(s, i);       // 装填完毕、场上子弹没到上限
        stuck_turns[type] = t.pos == last_pos[type] ? stuck_turns[type] + 1 : 0;
        last_pos[type] = t.pos;

        // 0) 站在原地 4 回合了，对方基地这 4 回合也没掉血：多半是和对面的车对射、子弹互相抵消，
        //    或者被别的车堵住了。往旁边挪一格（三辆车、不同回合挑的方向不一样），换个位置再说。
        if (stuck_turns[type] >= 4 && quiet_turns >= 4) {
            for (int k = 0; k < 4; ++k) {
                const int d = (2 + type + s.turn / 4 + k) % 4;
                if (s.grid[t.pos + DIR_DELTA[d]] == EMPTY && tank_at(s, t.pos + DIR_DELTA[d]) < 0) {
                    orders.cmd[type] = cmd_move(d);
                    break;
                }
            }
            continue;
        }

        // 1) 对方基地在炮线上：开炮（装填中就原地等）。
        int fire_dir = -1;
        for (int d = 0; d < 4; ++d)
            if (base_in_line(s, me, t.pos, d)) fire_dir = d;
        if (fire_dir >= 0) {
            if (ready) orders.cmd[type] = cmd_fire(fire_dir);
            continue;
        }

        // 2) 否则朝离对方基地更近的邻格走；那一格是砖墙就先把它打掉。
        int best_dir = -1, best = dist_to_base[t.pos];
        for (int d = 0; d < 4; ++d) {
            const int next = t.pos + DIR_DELTA[d];
            if (dist_to_base[next] < best) { best = dist_to_base[next]; best_dir = d; }
        }
        if (best_dir < 0) continue;
        if (s.grid[t.pos + DIR_DELTA[best_dir]] == BRICK) {
            if (ready) orders.cmd[type] = cmd_fire(best_dir);
        } else {
            orders.cmd[type] = cmd_move(best_dir);   // 快速型可以用 cmd_move2(d1, d2) 一回合走两步，模板没有用
        }
    }
    return orders;
}

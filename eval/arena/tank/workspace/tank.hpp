// tank.hpp —— 坦克大战规则引擎（单头文件，C++20，只用标准库）
//
// 这份头文件是规则的权威出处：裁判用它结算，bot 直接包含它做精确前推。
// 结算完全确定、没有随机，对两个座位完全对称。
//
// 第一次读，先看这三处：
//
//   规则数值在哪    第 2 节的 struct Params，前半部分"本届比赛的规则"：地图、基地血量、三种车型、
//                   升级效果、星星、复活、胜负。每一项的注释就是规则。局面里带着一份：s.P。
//
//   step 怎么用     tank::State next = s;                 // 局面是定长结构，直接按值拷贝
//                   tank::step(next, 命令0, 命令1);       // 精确结算一回合；命令0 / 命令1 是 0 号方 / 1 号方的 Orders
//                   tank::step_as(next, me, 我方, 对方);  // 同上，按自己的视角给
//                   每辆车能下哪些命令：legal_commands(s, 坦克编号, out)。
//                   结算的每一步对应哪条规则，写在第 5 节 step 的注释里。
//
//   预算怎么查      tank::budget_left() 返回本回合还剩多少 CPU 毫秒（首回合 2000，之后每回合 50，见第 9 节）：
//                   while (tank::budget_left() > 3) { /* 再搜一层 */ }
//
// 目录（按出现顺序）：
//   1. 坐标、方向、命令编码
//   2. Params：规则数值
//   3. State：完整局面
//   4. 查询：子弹上限、装填、复活倒计时、某格上的坦克
//   5. step：结算一回合
//   6. 合法命令枚举
//   7. 地图生成与开局：generate_map / new_game / make_game
//   8. 局面与文本互转：to_text / from_text / 命令行的读写
//   9. budget_left：思考预算
//
// 约定：
//   坐标 x 向右、y 向下，左上角是 (0,0)。0 号方在下（y 大的一侧），1 号方在上。
//   坦克编号 0/1/2 = 0 号方的 快速/火力/装甲，3/4/5 = 1 号方的 快速/火力/装甲。
//   s.turn 是"已经结算完的回合数"，开局为 0；第一次 step 结算的是第 1 回合。
#pragma once

#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>
#if !defined(_WIN32)
#include <time.h>   // 思考预算：线程 CPU 时间（见第 9 节）
#endif

namespace tank {

inline constexpr const char* ENGINE_VERSION = "tank-cpp-0.1";

// ====================================================================
// 1. 坐标、方向、命令编码
// ====================================================================

inline constexpr int MAX_SIZE = 21;             // 地图边长上限
inline constexpr int STRIDE = MAX_SIZE + 2;     // 格子编号的行宽：地图四周各留一圈"界外"
inline constexpr int CELLS = STRIDE * STRIDE;
inline constexpr int MAX_SUPPLY = 5;            // 补给点数量上限
inline constexpr int MAX_BULLETS = 24;          // 场上子弹数量上限（6 辆 × 每辆最多 4 发）
inline constexpr int NO_CELL = 0;               // 不在场的坦克的位置；换算成坐标是 (-1,-1)

// 格子编号：局面里所有位置都用一个整数表示，四周有一圈 OUTSIDE，所以"向某方向走一格"
// 就是加一个常数，不用判断边界。
constexpr int cell_of(int x, int y) { return (y + 1) * STRIDE + (x + 1); }
constexpr int cell_x(int c) { return c % STRIDE - 1; }
constexpr int cell_y(int c) { return c / STRIDE - 1; }

// 地形。BASE0 / BASE1 是两方基地；OUTSIDE 是界外。
enum Cell : uint8_t { EMPTY = 0, BRICK = 1, STEEL = 2, WATER = 3, BASE0 = 4, BASE1 = 5, OUTSIDE = 6 };
// （界外叫 OUTSIDE 而不叫 OUT，是因为 <windows.h> 把 OUT 定义成了宏。）
inline constexpr char CELL_CHARS[] = ".#@~01";  // 文本里的写法（OUTSIDE 不出现在文本里）

// 方向：0=U（y-1） 1=D（y+1） 2=L（x-1） 3=R（x+1）。d ^ 1 是反方向。
enum Dir : int { UP = 0, DOWN = 1, LEFT = 2, RIGHT = 3 };
inline constexpr int DIR_DELTA[4] = {-STRIDE, STRIDE, -1, 1};
inline constexpr int DIR_DX[4] = {0, 0, -1, 1};
inline constexpr int DIR_DY[4] = {-1, 1, 0, 0};
inline constexpr char DIR_CHARS[] = "UDLR";

// 车型。
enum Type : int { FAST = 0, FIRE = 1, ARMOR = 2 };
inline constexpr const char* TYPE_NAMES[3] = {"fast", "fire", "armor"};
constexpr int tank_id(int seat, int type) { return 3 * seat + type; }
constexpr int seat_of(int tank) { return tank / 3; }
constexpr int type_of(int tank) { return tank % 3; }

// 命令编码（一辆车一个字节）：
//   0        不动                      文本 S
//   1..4     向 U/D/L/R 走一步          文本 U D L R
//   5..8     向 U/D/L/R 开炮            文本 FU FD FL FR
//   9..24    快速型连走两步，9 + 4*d1 + d2（先 d1 后 d2）   文本两个方向字母，如 UL
// 其他数值一律当作不动。非快速型给了两步时只执行第一步。
using Cmd = uint8_t;
inline constexpr Cmd STAY = 0;
inline constexpr int N_CMDS = 25;
constexpr Cmd cmd_move(int d) { return Cmd(1 + d); }
constexpr Cmd cmd_fire(int d) { return Cmd(5 + d); }
constexpr Cmd cmd_move2(int d1, int d2) { return Cmd(9 + 4 * d1 + d2); }
constexpr bool is_move(Cmd c) { return c >= 1 && c <= 4; }
constexpr bool is_fire(Cmd c) { return c >= 5 && c <= 8; }
constexpr bool is_move2(Cmd c) { return c >= 9 && c <= 24; }

// 一方一回合的命令：cmd[FAST]、cmd[FIRE]、cmd[ARMOR]。不在场的坦克的命令会被忽略。
struct Orders {
    Cmd cmd[3] = {STAY, STAY, STAY};
    bool operator==(const Orders&) const = default;
};

// ====================================================================
// 2. Params：规则数值
// ====================================================================
// 分两组。前一组是本届比赛的规则，默认值就是正式赛用的数值。
// 后一组在本届比赛中固定为默认值，仅用于测试，写 bot 时不用关心。
// Params 存在 State 里（s.P），step 只读它。

struct TankType {
    int hp;        // 初始血量
    int speed;     // 每回合最多走几步（1 或 2）
    int bullets;   // 0 级时的场上子弹上限
    int reload;    // 0 级时开炮后要装填几回合（0 = 不用装填，可以每回合都开炮）
    bool operator==(const TankType&) const = default;
};

// 超时或同归于尽时依次比较的项目。
enum Tiebreak : int { TB_BASE = 0, TB_KILLS = 1, TB_STARS = 2, TB_DAMAGE = 3 };
inline constexpr const char* TIEBREAK_NAMES[4] = {"base", "kills", "stars", "damage"};

struct Params {
    // ================================================================
    // 第一组：本届比赛的规则
    // ================================================================

    // ---- 地图 ----
    int size = 15;                // 地图边长
    int ring = 1;                 // 基地外面围几层砖墙
    bool spawn_near = true;       // 三辆车都在围墙外一格出生：围墙前面那一行的左前、正前、右前
    bool spawn_flip = true;       // 两边的车型排布左右对调（地形上下镜像；比如 1 号方的快速型在左，0 号方的就在右）
    int supply_points = 3;        // 补给点数量，都在中线上

    // ---- 基地 ----
    int base_hp = 24;             // 基地血量
    int base_hit_cap = 1;         // 基地每回合最多扣 1 血：同一回合里不管几发子弹命中（两个小步合计，
                                  // 敌我的子弹都算），只有先命中的那一发扣血，其余的照样消失但不扣血

    // ---- 车型：顺序固定为 快速、火力、装甲 ----
    TankType types[3] = {
        {1, 2, 1, 1},             // 快速型：1 血，每回合 2 步，子弹上限 1，开炮后装填 1 回合
        {1, 1, 1, 0},             // 火力型：1 血，每回合 1 步，子弹上限 1，不用装填（0 级时唯一能连射的车）
        {2, 1, 1, 1},             // 装甲型：2 血，每回合 1 步，子弹上限 1，开炮后装填 1 回合
    };

    // ---- 升级（吃一颗星升一级；被毁后等级清零，升级得到的东西全部失去）----
    //            每升一级            到 1 级                              到 2 级
    //   快速型   当前血量 +2         不再需要装填（子弹上限仍是 1）       子弹能打掉钢墙
    //   火力型   当前血量 +2         子弹上限 +1（变成 2；它本来就不装填）  子弹能打掉钢墙
    //   装甲型   当前血量 +2         不再需要装填（子弹上限仍是 1）       子弹能打掉钢墙
    // 1 级的效果到 2 级仍然保留。满级的坦克不再拾取星星。
    int max_level = 2;            // 最高等级
    int level_hp = 2;             // 每升一级，当前血量加多少
    bool level1_no_reload = true; // 1 级及以上：要装填的车型不再需要装填；本来就不装填的车型改为子弹上限 + level1_bullets
    int level1_bullets = 1;       // 见上一行
    bool level2_steel = true;     // 2 级及以上：打出的子弹能打掉钢墙（按开炮时的等级定）

    // ---- 星星 ----
    int supply_first = 10;        // 第一批星星在第 10 回合末出现
    int supply_period = 20;       // 之后每 20 回合一批（第 30、50、70… 回合末）；已经有星星的补给点不再出

    // ---- 复活 ----
    int respawn = 3;                      // 第 T 回合末被毁，第 T+3 回合末在自己的出生点复活
    bool respawn_telefrag = true;         // 复活时出生点被敌方坦克占着：那辆敌车被毁（算一次击毁）；被己方占着则顺延
    bool respawn_clear_bullets = true;    // 复活时出生点上的子弹直接消失

    // ---- 子弹与胜负 ----
    bool bullet_cancel = true;    // 两发子弹对穿或同格时互相抵消
    int max_turns = 300;          // 回合上限
    // 到回合上限、或两个基地同回合归零时，依次比 tiebreak[0..n_tiebreak)：基地血量 → 击毁数 → 吃星数，都相同算平局
    int n_tiebreak = 3;
    int tiebreak[4] = {TB_BASE, TB_KILLS, TB_STARS};   // 没用到的项保持 0

    // ================================================================
    // 第二组：本届比赛中固定为默认值，仅用于测试，写 bot 时不用关心
    // ================================================================
    int start_level[2] = {0, 0};     // 两方开局等级
    int base_armor = 0;              // 开炮时等级低于这个数的子弹打不动基地；0 = 谁都能打
    int base_regen = 0;              // 基地连续这么多回合没挨打就回 1 血；0 = 不回血
    // 下面三个是旧版规则的兼容开关，本届比赛中恒为 false：
    bool proto_base_dmg_by_bullet_order = false;   // true（旧）：同一小步里敌我子弹同时命中一个基地而只有一发扣血时，
                                                   // 伤害记给谁取决于子弹的排列顺序（对两个座位不对称）
                                                   // false（正式规则）：敌方的子弹优先（见 step 的判定 4）
    bool proto_supply_feeds_tank = false;   // true（旧）：出星时补给点上有坦克，星星不出现，坦克直接吃掉
                                            // false（正式规则）：星星照常出现，下一回合末的拾取步骤才被吃
    bool proto_pickup_at_max_level = false; // true（旧）：满级的坦克也拾取，星星消失、计入吃星数、没有别的效果
                                            // false（正式规则）：满级的坦克不拾取，星星留在原地，不计入吃星数

    bool operator==(const Params&) const = default;
};

// 逐项列出 Params 的名字和成员，文本读写都走这里。新增参数时在结构体里加成员，再在这里加一行。
// （tiebreak 是列表，文本读写单独处理。）
template <class P, class F>
void for_each_param(P& p, F&& f) {
    f("size", p.size);
    f("max_turns", p.max_turns);
    f("base_hp", p.base_hp);
    f("respawn", p.respawn);
    f("supply_first", p.supply_first);
    f("supply_period", p.supply_period);
    f("supply_points", p.supply_points);
    f("bullet_cancel", p.bullet_cancel);
    static const char* const type_keys[3][4] = {
        {"fast_hp", "fast_speed", "fast_bullets", "fast_reload"},
        {"fire_hp", "fire_speed", "fire_bullets", "fire_reload"},
        {"armor_hp", "armor_speed", "armor_bullets", "armor_reload"},
    };
    for (int k = 0; k < 3; ++k) {
        f(type_keys[k][0], p.types[k].hp);
        f(type_keys[k][1], p.types[k].speed);
        f(type_keys[k][2], p.types[k].bullets);
        f(type_keys[k][3], p.types[k].reload);
    }
    f("max_level", p.max_level);
    f("level_hp", p.level_hp);
    f("level1_bullets", p.level1_bullets);
    f("level2_steel", p.level2_steel);
    f("respawn_telefrag", p.respawn_telefrag);
    f("respawn_clear_bullets", p.respawn_clear_bullets);
    f("ring", p.ring);
    f("spawn_near", p.spawn_near);
    f("spawn_flip", p.spawn_flip);
    f("level1_no_reload", p.level1_no_reload);
    f("start_level0", p.start_level[0]);
    f("start_level1", p.start_level[1]);
    f("base_hit_cap", p.base_hit_cap);
    f("base_armor", p.base_armor);
    f("base_regen", p.base_regen);
    f("proto_base_dmg_by_bullet_order", p.proto_base_dmg_by_bullet_order);   // 兼容开关，恒为 false
    f("proto_supply_feeds_tank", p.proto_supply_feeds_tank);       // 兼容开关，恒为 false
    f("proto_pickup_at_max_level", p.proto_pickup_at_max_level);   // 兼容开关，恒为 false
}

// 参数是否在引擎支持的范围内。通过返回空串，否则返回原因。
inline std::string check_params(const Params& p) {
    if (p.size < 5 || p.size > MAX_SIZE) return "size 应在 5.." + std::to_string(MAX_SIZE);
    if (p.max_turns < 1 || p.max_turns > 30000) return "max_turns 应在 1..30000";
    if (p.base_hp < 1 || p.base_hp > 30000) return "base_hp 应在 1..30000";
    if (p.respawn < 0 || p.respawn > 30000) return "respawn 应在 0..30000";
    if (p.supply_first < 0 || p.supply_first > 30000) return "supply_first 应在 0..30000";
    if (p.supply_period < 1) return "supply_period 至少为 1";
    if (p.supply_points < 0 || p.supply_points > MAX_SUPPLY) return "supply_points 应在 0..5";
    for (const TankType& t : p.types) {
        if (t.hp < 1 || t.hp > 100) return "车型血量应在 1..100";
        if (t.speed < 1 || t.speed > 2) return "车型速度只能是 1 或 2";
        if (t.bullets < 0 || t.reload < 0 || t.reload > 100) return "车型的子弹上限和装填回合数不能为负";
        if (t.bullets + (p.level1_bullets > 0 ? p.level1_bullets : 0) > MAX_BULLETS / 6)
            return "每辆车的场上子弹上限（含升级）不能超过 " + std::to_string(MAX_BULLETS / 6);
    }
    if (p.max_level < 0 || p.max_level > 100) return "max_level 应在 0..100";
    if (p.level_hp < 0 || p.level_hp > 100 || p.level1_bullets < 0) return "level_hp / level1_bullets 不能为负";
    if (p.n_tiebreak < 0 || p.n_tiebreak > 4) return "n_tiebreak 应在 0..4";
    for (int k = 0; k < p.n_tiebreak; ++k)
        if (p.tiebreak[k] < 0 || p.tiebreak[k] > 3) return "tiebreak 项目未知";
    if (p.ring < 1 || p.ring > 2) return "ring 只能是 1 或 2";
    for (int lv : p.start_level)
        if (lv < 0 || lv > 100) return "start_level 应在 0..100";
    if (p.base_hit_cap < 0 || p.base_armor < 0 || p.base_regen < 0) return "base_hit_cap / base_armor / base_regen 不能为负";
    return "";
}

// ====================================================================
// 3. State：完整局面
// ====================================================================

inline constexpr int NEVER_FIRED = -99;   // last_fire 的初值：还没开过炮（或刚复活）

struct Tank {
    int16_t pos = NO_CELL;         // 所在格子；不在场时为 NO_CELL
    int16_t hp = 0;                // 当前血量；不在场时为 0
    int16_t level = 0;             // 等级；被毁后清零
    bool alive = false;            // 是否在场
    int16_t respawn_turn = 0;      // 不在场时：从这个回合的回合末起可以复活（出生点被己方占着会顺延）；在场时为 0
    int16_t last_fire = NEVER_FIRED;   // 上一次成功开炮的回合；不在场或复活后为 NEVER_FIRED
    bool operator==(const Tank&) const = default;
};

struct Bullet {
    int16_t pos;       // 所在格子
    int8_t dir;        // 飞行方向
    int8_t owner;      // 打出它的坦克编号
    bool steel;        // 能否打掉钢墙（按开炮时的等级定）
    int8_t level;      // 开炮时坦克的等级（只有第二组参数 base_armor 用到）
    bool operator==(const Bullet&) const = default;
};

// 结束原因。两个基地同回合归零时 reason 是决出胜负的那一项（或 R_DRAW），
// 是否"同归于尽"看 base_hp 是否都为 0。
enum Reason : uint8_t { R_NONE = 0, R_BASE = 1, R_HP = 2, R_KILLS = 3, R_STARS = 4, R_DAMAGE = 5, R_DRAW = 6 };

struct State {
    Params P;

    // ---- 布局里不变的部分 ----
    int16_t base_pos[2] = {NO_CELL, NO_CELL};    // 两方基地所在格
    int16_t spawn[6] = {};                        // 每辆坦克的出生点（按坦克编号）
    int16_t n_supply = 0;                         // 补给点数量
    int16_t supply[MAX_SUPPLY] = {};              // 补给点所在格

    // ---- 随对局变化的部分 ----
    int16_t turn = 0;              // 已经结算完的回合数
    bool done = false;             // 是否已结束
    int8_t winner = -1;            // 胜方座位；-1 = 未结束或平局
    Reason reason = R_NONE;        // 结束原因
    int16_t base_hp[2] = {0, 0};   // 基地血量
    int16_t base_quiet[2] = {0, 0};    // 基地已连续多少回合没挨打（只有第二组参数 base_regen 用到）
    int16_t kills[2] = {0, 0};     // 各方的击毁数：对方坦克被毁一次算一次，不论被谁打的
    int16_t stars_eaten[2] = {0, 0};   // 各方吃到的星星数（满级的坦克不拾取，不计数）
    int16_t base_dmg[2] = {0, 0};  // 各方对敌方基地造成的伤害（打自己基地不算）
    Tank tank[6];
    int16_t n_bullets = 0;
    Bullet bullet[MAX_BULLETS] = {};   // 前 n_bullets 项有效，按开炮先后排列；后面的项恒为全 0
    uint8_t star_mask = 0;         // 第 k 位为 1：supply[k] 上有一颗星星
    uint64_t wall_hash = 0;        // 当前砖墙和钢墙的指纹；墙没变它就不变，可以拿来缓存寻路结果
    uint8_t grid[CELLS] = {};      // 当前地形（Cell），用 cell_of(x, y) 取下标

    bool operator==(const State&) const = default;
};

// 一堵墙对 wall_hash 的贡献。wall_hash = 所有现存砖墙、钢墙格子的 wall_key 异或起来。
constexpr uint64_t wall_key(int cell) {
    uint64_t z = (uint64_t(cell) + 1) * 0x9E3779B97F4A7C15ULL;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

// ====================================================================
// 4. 查询
// ====================================================================
//
// ★★ 注意：s.grid 里只有地形（空地、砖墙、钢墙、水、基地），没有坦克、子弹和星星。 ★★
//    这一节的函数各管一件事，没有哪个替你回答"这一炮打出去会打到什么"。自己沿一个方向逐格看 s.grid
//    来判断炮线通不通时（模板 bot.cpp 里的 base_in_line 就是这种只看地形的写法），路上的坦克是看不到的：
//    子弹会命中途中遇到的第一辆坦克然后消失（不分敌我，队友也算），到不了它后面的墙和基地。
//    要把坦克算进去，每一格另外查 tank_at(s, c)。地形方面：砖墙、钢墙挡子弹（砖墙被打掉，
//    钢墙只有 2 级的子弹打得掉），水不挡子弹。
//    同类的"只看一部分"：
//      can_fire / reload_left     只回答炮打不打得出去，不管打到什么；reload_left 只算装填，不算子弹上限。
//      legal_commands             只看地形和这辆车自己的装填、子弹数，不看任何坦克（见第 6 节）。
//      base_open_sides            只看开局的地形（Layout），不看坦克，也不随墙被打掉而变（见第 7 节）。
//      star_at                    只在补给点上才可能为真；满级的车站在上面也不会吃掉它。

// 这一格上在场的坦克编号，没有则 -1。本回合内血量已被打到 0、但回合末才移除的坦克也算在场。
inline int tank_at(const State& s, int cell) {
    for (int i = 0; i < 6; ++i)
        if (s.tank[i].alive && s.tank[i].pos == cell) return i;
    return -1;
}

// 这一格是不是一个现在有星星的补给点。不是补给点的格子一律为 false。
inline bool star_at(const State& s, int cell) {
    for (int k = 0; k < s.n_supply; ++k)
        if (s.supply[k] == cell) return (s.star_mask >> k) & 1;
    return false;
}

// 第 i 辆坦克留在场上的子弹数。
inline int bullets_out(const State& s, int i) {
    int n = 0;
    for (int k = 0; k < s.n_bullets; ++k) n += (s.bullet[k].owner == i);
    return n;
}

// 第 i 辆坦克当前的场上子弹上限。0 级是车型的 bullets；1 级及以上，本来就不装填的车型（火力型）
// 上限 + level1_bullets，要装填的车型（快速、装甲）上限不变（它们 1 级的好处是不再装填）。
inline int bullet_cap(const State& s, int i) {
    const TankType& t = s.P.types[type_of(i)];
    if (s.tank[i].level >= 1 && (!s.P.level1_no_reload || t.reload == 0)) return t.bullets + s.P.level1_bullets;
    return t.bullets;
}

// 第 i 辆坦克开炮后要装填几回合（按它现在的等级）：0 级是车型的 reload，1 级及以上是 0。
inline int reload_turns(const State& s, int i) {
    int rl = s.P.types[type_of(i)].reload;
    if (rl && s.tank[i].level >= 1 && s.P.level1_no_reload) rl = 0;
    return rl;
}

// 第 i 辆坦克的装填还要等几个回合（0 = 装填完毕）。
// 注意：只算装填。装填完了但上一发子弹还在场上、已达子弹上限时照样打不出去——"下一回合能不能开炮"请用 can_fire。
inline int reload_left(const State& s, int i) {
    int left = reload_turns(s, i) - (s.turn - s.tank[i].last_fire);
    return left > 0 ? left : 0;
}

// 第 i 辆坦克下一回合下开炮命令，是否真的会打出子弹（在场、装填完毕、未达子弹上限）。
// 注意：只回答"打不打得出去"，和朝哪个方向、路上有什么无关。子弹数按此刻数：这一回合稍后才消失的子弹仍占名额。
inline bool can_fire(const State& s, int i) {
    return s.tank[i].alive && reload_left(s, i) == 0 && bullets_out(s, i) < bullet_cap(s, i) &&
           s.n_bullets < MAX_BULLETS;
}

// 不在场的坦克最早还要等几个回合末才复活（至少为 1；出生点被己方占着会一直停在 1）。在场时为 0。
inline int respawn_in(const State& s, int i) {
    if (s.tank[i].alive) return 0;
    int w = s.tank[i].respawn_turn - s.turn;
    return w > 1 ? w : 1;
}

inline bool is_over(const State& s) { return s.done; }
inline int winner(const State& s) { return s.winner; }       // 0 / 1；-1 = 未结束或平局
inline bool both_bases_down(const State& s) { return s.base_hp[0] <= 0 && s.base_hp[1] <= 0; }

// 各方的得分：胜 1、负 0、平 0.5（未结束时也是 0.5）。
inline double score(const State& s, int seat) { return s.winner < 0 ? 0.5 : (s.winner == seat ? 1.0 : 0.0); }

// ====================================================================
// 5. step：结算一回合
// ====================================================================

// 事件记录（可选）。给回放和查看器用；搜索时不要传，step 会快一些。
enum EventType : uint8_t {
    EV_FIRE,            // 开炮成功            a=坦克  b=方向
    EV_BLOCKED,         // 这一步没走成        a=坦克  sub
    EV_CANCEL_CROSS,    // 两发子弹对穿抵消    x y sub  a、b=两发的所属坦克
    EV_CANCEL_SAME,     // 同格子弹抵消        x y sub
    EV_HIT,             // 坦克被打中          a=被打中的坦克  b=开炮的坦克  x y sub
    EV_HIT_SWAP,        // 同上，是和子弹互换位置判的命中
    EV_BASE_HIT,        // 基地被打中          a=基地所属座位  b=开炮的坦克  sub
    EV_WALL_BRICK,      // 砖墙被打掉          x y sub
    EV_WALL_STEEL,      // 钢墙被打掉          x y sub
    EV_DEATH,           // 回合末被毁          a=坦克  x y  b=压出生点被谁的复活炸毁（否则 -1）
    EV_RESPAWN,         // 回合末复活          a=坦克  x y
    EV_BULLET_CLEARED,  // 复活时出生点上的子弹消失   x y  a=复活的坦克
    EV_STAR,            // 吃到星星            a=坦克  x y  b=吃完后的等级
    EV_SUPPLY,          // 补给点出了一颗星星  x y
    EV_BASE_REGEN,      // 基地回 1 血（第二组参数 base_regen）  a=座位
};

struct Event {
    EventType type;
    int8_t sub;         // 第几小步（1 或 2）；回合初、回合末的事件为 0
    int8_t a, b;
    int8_t x, y;
};

struct EventLog {
    std::vector<Event> list;   // 本回合发生的事，按发生顺序；每次 step 开头清空
};

namespace detail {
inline void add_event(EventLog* log, EventType type, int sub, int a, int b, int cell) {
    if (log) log->list.push_back(Event{type, int8_t(sub), int8_t(a), int8_t(b), int8_t(cell_x(cell)), int8_t(cell_y(cell))});
}

// 吃到一颗星星：升一级并加 level_hp 点血，吃星数 +1。
// （只有打开兼容开关时满级的坦克才会走到这里，那时不升级、不加血，只记吃星数。）
inline void level_up(State& s, int i, EventLog* log) {
    Tank& t = s.tank[i];
    if (t.level < s.P.max_level) {
        t.level += 1;
        t.hp += int16_t(s.P.level_hp);
    }
    s.stars_eaten[seat_of(i)] += 1;
    add_event(log, EV_STAR, 0, i, t.level, t.pos);
}

// 坦克被毁：从场上移除，等级清零，定下复活回合。
inline void destroy_tank(State& s, int i) {
    Tank& t = s.tank[i];
    t.alive = false;
    t.pos = NO_CELL;
    t.hp = 0;
    t.level = 0;
    t.last_fire = NEVER_FIRED;
    t.respawn_turn = int16_t(s.turn + s.P.respawn);
}

inline void tiebreak_values(const State& s, int key, int& a, int& b) {
    switch (key) {
        case TB_BASE:  a = s.base_hp[0];     b = s.base_hp[1];     break;
        case TB_KILLS: a = s.kills[0];       b = s.kills[1];       break;
        case TB_STARS: a = s.stars_eaten[0]; b = s.stars_eaten[1]; break;
        default:       a = s.base_dmg[0];    b = s.base_dmg[1];    break;
    }
}
}  // namespace detail

// 结算一回合。seat0 / seat1 分别是 0 号方、1 号方的命令。已结束的局面不再变化。
//
// 一回合的顺序：
//   开炮 → 小步 1（移动、子弹前进、判定）→ 小步 2（同上）→ 回合末（被毁、复活、拾取、补给）→ 胜负
inline void step(State& s, const Orders& seat0, const Orders& seat1, EventLog* log = nullptr) {
    if (log) log->list.clear();
    if (s.done) return;
    const Params& P = s.P;
    const int turn = ++s.turn;
    using detail::add_event;

    int move1[6], move2[6];          // 每辆车第 1、2 小步要走的方向；-1 = 这一小步不走
    int base_hits_now[2] = {0, 0};   // 本回合每个基地已经挨了几发（两个小步合计；基地每回合限伤用）

    // ---------------- 开炮（回合开始时）----------------
    // 子弹生成在开炮坦克自己所在的格子，带方向和所属；本回合的两个小步里它照常前进。
    // 子弹上限按此刻（回合开始、任何子弹前进之前）场上的子弹数算；装填中或已达上限时开炮命令视为不动，
    // 也不重新计装填。开炮的回合不移动。
    for (int i = 0; i < 6; ++i) {
        move1[i] = move2[i] = -1;
        Tank& t = s.tank[i];
        if (!t.alive) continue;                       // 不在场的坦克的命令被忽略
        const int c = (i < 3 ? seat0 : seat1).cmd[i % 3];
        if (c >= 1 && c <= 4) {
            move1[i] = c - 1;
        } else if (c >= 5 && c <= 8) {
            if (bullets_out(s, i) < bullet_cap(s, i) && turn - t.last_fire > reload_turns(s, i) &&
                s.n_bullets < MAX_BULLETS) {
                t.last_fire = int16_t(turn);
                s.bullet[s.n_bullets++] = Bullet{t.pos, int8_t(c - 5), int8_t(i),
                                                 t.level >= 2 && P.level2_steel, int8_t(t.level)};
                add_event(log, EV_FIRE, 0, i, c - 5, t.pos);
            }
        } else if (c >= 9 && c <= 24) {
            move1[i] = (c - 9) >> 2;
            if (P.types[i % 3].speed >= 2) move2[i] = (c - 9) & 3;   // 非快速型只执行第一步
        }
    }

    // ---------------- 两个小步 ----------------
    for (int sub = 1; sub <= 2; ++sub) {
        const int* mv = sub == 1 ? move1 : move2;

        // (a) 坦克移动。一步能走成的条件：
        //     目标格是空地（不是砖、钢、水、基地、界外）；
        //     目标格在本小步开始时没有坦克（所以面对面互换都不动，也不能跟在正要离开的车后面进它的格子）；
        //     没有别的坦克在本小步也要进同一格（两车抢同一格都不动，不分敌我）。
        //     快速型第一步没走成，第二步照常从当时所在的格子独立判定。
        //     本回合已经被打到 0 血的坦克回合末才移除，在那之前照常占格、照常走。
        int want_tank[6], want_cell[6], n_want = 0;
        for (int i = 0; i < 6; ++i) {
            if (mv[i] < 0) continue;
            const int target = s.tank[i].pos + DIR_DELTA[mv[i]];
            if (s.grid[target] == EMPTY && tank_at(s, target) < 0) {
                want_tank[n_want] = i;
                want_cell[n_want++] = target;
            } else {
                add_event(log, EV_BLOCKED, sub, i, -1, s.tank[i].pos);
            }
        }
        int moved_tank[6], moved_from[6], moved_to[6], n_moved = 0;
        for (int a = 0; a < n_want; ++a) {
            bool contested = false;
            for (int b = 0; b < n_want; ++b) contested |= (b != a && want_cell[b] == want_cell[a]);
            if (contested) {
                add_event(log, EV_BLOCKED, sub, want_tank[a], -1, s.tank[want_tank[a]].pos);
            } else {
                moved_tank[n_moved] = want_tank[a];
                moved_from[n_moved] = s.tank[want_tank[a]].pos;
                moved_to[n_moved++] = want_cell[a];
            }
        }
        // 走得成的同时走。它们的目标格在本小步开始时都是空的、彼此不同，所以逐个改位置就等于同时改。
        for (int m = 0; m < n_moved; ++m) s.tank[moved_tank[m]].pos = int16_t(moved_to[m]);

        const int nb = s.n_bullets;
        if (nb == 0) continue;

        // (b) 所有子弹前进 1 格。
        for (int k = 0; k < nb; ++k) s.bullet[k].pos += int16_t(DIR_DELTA[s.bullet[k].dir]);

        // (c) 判定。gone[k] = 第 k 发子弹在本小步消失。
        bool gone[MAX_BULLETS] = {};

        // 判定 1（对穿，子弹对子弹）：两发子弹在本小步互换了位置，都消失。关掉 bullet_cancel 时不做。
        if (P.bullet_cancel && nb > 1) {
            for (int a = 0; a < nb; ++a) {
                const Bullet& ba = s.bullet[a];
                for (int b = a + 1; b < nb; ++b) {
                    const Bullet& bb = s.bullet[b];
                    if (bb.dir == (ba.dir ^ 1) && ba.pos - DIR_DELTA[ba.dir] == bb.pos) {
                        gone[a] = gone[b] = true;
                        add_event(log, EV_CANCEL_CROSS, sub, ba.owner, bb.owner, ba.pos);
                    }
                }
            }
        }
        // 判定 1（对穿，子弹对坦克）：一发子弹和一辆坦克在本小步互换了位置，算命中这辆坦克。
        // 已经在子弹对穿里抵消的子弹不再参与。这一条不受 bullet_cancel 影响。
        for (int m = 0; m < n_moved; ++m) {
            Tank& t = s.tank[moved_tank[m]];
            for (int k = 0; k < nb; ++k) {
                const Bullet& b = s.bullet[k];
                if (!gone[k] && b.pos == moved_from[m] && b.pos - DIR_DELTA[b.dir] == t.pos) {
                    gone[k] = true;
                    t.hp -= 1;
                    add_event(log, EV_HIT_SWAP, sub, moved_tank[m], b.owner, t.pos);
                }
            }
        }
        // 判定 2–5：按子弹所在格的内容处理。墙体的变化攒到这一轮结束再生效，
        // 所以同一小步打中同一堵墙的几发子弹都消失。
        int16_t brick_hit[MAX_BULLETS], steel_hit[MAX_BULLETS];
        int n_brick = 0, n_steel = 0;
        int own_took[2] = {0, 0};        // 本小步里每个基地被自己人的子弹扣掉的血（记伤害时用，见判定 4）
        for (int k = 0; k < nb; ++k) {
            if (gone[k]) continue;
            const Bullet& b = s.bullet[k];
            switch (s.grid[b.pos]) {
                case EMPTY: {
                    // 判定 5：格里有坦克，坦克扣 1 血，子弹消失；同一格里有几发算几发。不分敌我。
                    const int v = tank_at(s, b.pos);
                    if (v >= 0) {
                        gone[k] = true;
                        s.tank[v].hp -= 1;
                        add_event(log, EV_HIT, sub, v, b.owner, b.pos);
                    }
                    break;
                }
                case WATER:      // 水不挡子弹
                    break;
                case BRICK:      // 判定 3：砖墙和子弹都消失
                    gone[k] = true;
                    brick_hit[n_brick++] = b.pos;
                    break;
                case STEEL:      // 判定 3：子弹消失；能破钢的子弹把钢墙也打掉
                    gone[k] = true;
                    if (b.steel) steel_hit[n_steel++] = b.pos;
                    break;
                case OUTSIDE:        // 判定 2：出界的子弹消失
                    gone[k] = true;
                    break;
                default: {       // 判定 4：子弹消失，基地扣 1 血。打自己的基地也扣血。
                    // 限伤：每个基地每回合最多扣 base_hit_cap 血。本回合命中这个基地的子弹（两个小步合计，
                    // 不分敌我）合在一起数，超出的照样消失但不扣血。基地扣多少血与子弹的先后无关。
                    // "对敌方基地的伤害"（base_dmg）记给谁：先到的小步优先；同一小步里敌方的子弹优先于
                    // 基地自己人的子弹（自己人打中自己的基地不记伤害）。
                    const int seat = s.grid[b.pos] - BASE0;
                    const bool enemy = seat_of(b.owner) != seat;
                    gone[k] = true;
                    base_hits_now[seat] += 1;
                    if (P.base_armor && b.level < P.base_armor) {
                        base_hits_now[seat] -= 1;                 // 第二组参数 base_armor：打不动，不算挨打
                    } else if (s.base_hp[seat] > 0 && !(P.base_hit_cap && base_hits_now[seat] > P.base_hit_cap)) {
                        s.base_hp[seat] -= 1;
                        if (enemy) s.base_dmg[1 - seat] += 1;
                        else own_took[seat] += 1;
                    } else if (enemy && own_took[seat] > 0 && !P.proto_base_dmg_by_bullet_order) {
                        // 这一小步里扣掉的那点血先被自己人的子弹"占"了：改记给同一小步命中的这发敌方子弹
                        own_took[seat] -= 1;
                        s.base_dmg[1 - seat] += 1;
                    }
                    add_event(log, EV_BASE_HIT, sub, seat, b.owner, b.pos);
                    break;
                }
            }
        }
        for (int k = 0; k < n_brick; ++k)
            if (s.grid[brick_hit[k]] == BRICK) {
                s.grid[brick_hit[k]] = EMPTY;
                s.wall_hash ^= wall_key(brick_hit[k]);
                add_event(log, EV_WALL_BRICK, sub, -1, -1, brick_hit[k]);
            }
        for (int k = 0; k < n_steel; ++k)
            if (s.grid[steel_hit[k]] == STEEL) {
                s.grid[steel_hit[k]] = EMPTY;
                s.wall_hash ^= wall_key(steel_hit[k]);
                add_event(log, EV_WALL_STEEL, sub, -1, -1, steel_hit[k]);
            }
        // 判定 6：剩下的子弹里，同一格有两发以上的都消失（不要求方向相对，不分敌我，水面上也一样）。
        // 关掉 bullet_cancel 时不做。
        if (P.bullet_cancel) {
            bool left[MAX_BULLETS];
            for (int k = 0; k < nb; ++k) left[k] = !gone[k];
            for (int k = 1; k < nb; ++k) {
                if (!left[k]) continue;
                for (int j = 0; j < k; ++j)
                    if (left[j] && s.bullet[j].pos == s.bullet[k].pos) {
                        gone[k] = gone[j] = true;
                        add_event(log, EV_CANCEL_SAME, sub, -1, -1, s.bullet[k].pos);
                        break;
                    }
            }
        }
        // 去掉消失的子弹，其余保持原来的先后顺序。
        int keep = 0;
        for (int k = 0; k < nb; ++k)
            if (!gone[k]) s.bullet[keep++] = s.bullet[k];
        for (int k = keep; k < nb; ++k) s.bullet[k] = Bullet{};
        s.n_bullets = int16_t(keep);
    }

    // ---------------- 回合末：被毁 → 复活 → 拾取 → 补给 ----------------

    // 被毁：血量 ≤ 0 的坦克从场上移除，等级清零，对方击毁数 +1（不论被谁打的），respawn 回合后复活。
    for (int i = 0; i < 6; ++i) {
        Tank& t = s.tank[i];
        if (t.alive && t.hp <= 0) {
            add_event(log, EV_DEATH, 0, i, -1, t.pos);
            detail::destroy_tank(s, i);
            s.kills[1 - seat_of(i)] += 1;
        }
    }

    // 复活：到了回合数的坦克在自己的出生点复活，血量回到车型初始值，不带装填。
    //   出生点被己方坦克占着：顺延到空出来为止。
    //   出生点被敌方坦克占着：那辆敌车被毁（算己方一次击毁），本车照常复活。
    //   出生点上的子弹直接消失。
    // 先按复活前的局面定下谁复活、谁被炸毁，再一起执行，结果与编号顺序无关。
    // 本回合刚被毁的车让出的格子，当回合就能用来复活。
    int plan_tank[6], plan_victim[6], n_plan = 0;
    for (int i = 0; i < 6; ++i) {
        if (s.tank[i].alive || s.tank[i].respawn_turn > turn) continue;
        const int v = tank_at(s, s.spawn[i]);
        if (v >= 0 && (seat_of(v) == seat_of(i) || !P.respawn_telefrag)) continue;
        plan_tank[n_plan] = i;
        plan_victim[n_plan++] = v;
    }
    for (int k = 0; k < n_plan; ++k) {
        const int v = plan_victim[k];
        if (v < 0) continue;
        add_event(log, EV_DEATH, 0, v, plan_tank[k], s.tank[v].pos);
        detail::destroy_tank(s, v);
        s.kills[seat_of(plan_tank[k])] += 1;
    }
    for (int k = 0; k < n_plan; ++k) {
        const int i = plan_tank[k];
        Tank& t = s.tank[i];
        t.alive = true;
        t.pos = s.spawn[i];
        t.hp = int16_t(P.types[i % 3].hp);
        t.level = 0;
        t.last_fire = NEVER_FIRED;
        t.respawn_turn = 0;
        add_event(log, EV_RESPAWN, 0, i, -1, t.pos);
        if (P.respawn_clear_bullets && s.n_bullets > 0) {
            int keep = 0;
            const int nb = s.n_bullets;
            for (int b = 0; b < nb; ++b)
                if (s.bullet[b].pos != t.pos) s.bullet[keep++] = s.bullet[b];
            if (keep != nb) {
                for (int b = keep; b < nb; ++b) s.bullet[b] = Bullet{};
                s.n_bullets = int16_t(keep);
                add_event(log, EV_BULLET_CLEARED, 0, i, -1, t.pos);
            }
        }
    }

    // 拾取：站在有星星的补给点上的坦克吃掉星星，升一级，吃星数 +1。
    // 满级的坦克不拾取：星星留在原地，不计入吃星数（别的车来了照样能吃）。
    // 刚被毁的坦克不会拾取；刚复活的会。
    // （proto_pickup_at_max_level 是旧版规则的兼容开关：满级也拾取，星星消失，只记吃星数。本届比赛中不启用。）
    if (s.star_mask) {
        for (int i = 0; i < 6; ++i) {
            if (!s.tank[i].alive) continue;
            if (s.tank[i].level >= P.max_level && !P.proto_pickup_at_max_level) continue;
            for (int k = 0; k < s.n_supply; ++k)
                if (((s.star_mask >> k) & 1) && s.supply[k] == s.tank[i].pos) {
                    s.star_mask &= uint8_t(~(1u << k));
                    detail::level_up(s, i, log);
                }
        }
    }

    // 补给：第 supply_first 回合末出第一批，之后每 supply_period 回合一批。
    // 每个当前没有星星的补给点出一颗。点上有坦克也照常出现，要到下一回合末的拾取步骤才被吃
    // （那时还站在上面的话）。补给在拾取之后，所以一辆车一回合最多升一级。
    // （proto_supply_feeds_tank 是旧版规则的兼容开关：点上有坦克时星星不出现，坦克直接吃掉。本届比赛中不启用。）
    if (turn >= P.supply_first && (turn - P.supply_first) % P.supply_period == 0) {
        for (int k = 0; k < s.n_supply; ++k) {
            if ((s.star_mask >> k) & 1) continue;
            const int v = P.proto_supply_feeds_tank ? tank_at(s, s.supply[k]) : -1;
            if (v < 0) {
                s.star_mask |= uint8_t(1u << k);
                add_event(log, EV_SUPPLY, 0, -1, -1, s.supply[k]);
            } else {
                detail::level_up(s, v, log);
            }
        }
    }

    // 基地回血（第二组参数 base_regen，本届比赛不启用）。
    if (P.base_regen) {
        for (int seat = 0; seat < 2; ++seat) {
            if (base_hits_now[seat] || s.base_hp[seat] <= 0) {
                s.base_quiet[seat] = 0;
            } else if (s.base_hp[seat] < P.base_hp) {
                s.base_quiet[seat] += 1;
                if (s.base_quiet[seat] >= P.base_regen) {
                    s.base_quiet[seat] = 0;
                    s.base_hp[seat] += 1;
                    add_event(log, EV_BASE_REGEN, 0, seat, -1, NO_CELL);
                }
            }
        }
    }

    // ---------------- 胜负 ----------------
    // 一方基地归零，另一方获胜。基地在第 1 小步就归零时这一回合照常结算完，
    // 所以可能两个基地同回合归零。两个基地同回合归零，或到回合上限：
    // 依次比 tiebreak 里的项目（默认 基地血量 → 击毁数 → 吃星数），都相同算平局。
    const int h0 = s.base_hp[0], h1 = s.base_hp[1];
    if (h0 <= 0 || h1 <= 0 || turn >= P.max_turns) {
        s.done = true;
        if (h0 <= 0 && h1 > 0) {
            s.winner = 1;
            s.reason = R_BASE;
        } else if (h1 <= 0 && h0 > 0) {
            s.winner = 0;
            s.reason = R_BASE;
        } else {
            s.winner = -1;
            s.reason = R_DRAW;
            for (int k = 0; k < P.n_tiebreak; ++k) {
                int a, b;
                detail::tiebreak_values(s, P.tiebreak[k], a, b);
                if (a != b) {
                    s.winner = a > b ? 0 : 1;
                    s.reason = Reason(R_HP + P.tiebreak[k]);
                    break;
                }
            }
        }
    }
}

// 从某一方的视角结算：me 的命令和对手的命令。
inline void step_as(State& s, int me, const Orders& mine, const Orders& theirs, EventLog* log = nullptr) {
    if (me == 0) step(s, mine, theirs, log);
    else step(s, theirs, mine, log);
}

// ====================================================================
// 6. 合法命令枚举
// ====================================================================

// 把一条命令换成"效果必然相同的最简写法"。必然相同指：不论其他五辆车下什么命令，
// 这一回合结算出的局面都完全一样（只有 EV_BLOCKED 事件可能不同）。规则：
//   不在场的坦克、编码超出范围            → 不动
//   走一步，但目标格现在不是空地          → 不动（移动在子弹之前，墙来不及被打掉）
//   开炮，但装填中或已达子弹上限          → 不动
//   非快速型的两步                        → 只留第一步
//   快速型两步，第二步无论如何都走不成    → 只留第一步
//   快速型两步，第一步目标现在不是空地    → "第 1 小步原地等，第 2 小步走 d2"，统一写成同一个编码
// "无论如何都走不成"只认水、基地、界外；砖墙和钢墙可能在第 1 小步被打掉，第 2 小步就能进去。
inline Cmd canonical_command(const State& s, int i, Cmd c) {
    const Tank& t = s.tank[i];
    if (!t.alive || c == STAY || c >= N_CMDS) return STAY;
    auto open_now = [&](int cell) { return s.grid[cell] == EMPTY; };
    auto never_open = [&](int cell) {
        const uint8_t g = s.grid[cell];
        return g == WATER || g == BASE0 || g == BASE1 || g == OUTSIDE;
    };
    if (is_move(c)) return open_now(t.pos + DIR_DELTA[c - 1]) ? c : STAY;
    if (is_fire(c)) return can_fire(s, i) ? c : STAY;
    const int d1 = (c - 9) >> 2, d2 = (c - 9) & 3;
    if (s.P.types[type_of(i)].speed < 2) return canonical_command(s, i, cmd_move(d1));
    if (!open_now(t.pos + DIR_DELTA[d1])) {
        if (never_open(t.pos + DIR_DELTA[d2])) return STAY;
        for (int e = 0; e < 4; ++e)
            if (!open_now(t.pos + DIR_DELTA[e])) return cmd_move2(e, d2);
    }
    if (never_open(t.pos + DIR_DELTA[d1] + DIR_DELTA[d2]) && never_open(t.pos + DIR_DELTA[d2])) return cmd_move(d1);
    return c;
}

// 第 i 辆坦克下一回合"有意义"的命令。写进 out（至少 N_CMDS 项），返回条数；按编码从小到大排，第一条总是 STAY。
//
// ★★ 它是"去重后的命令表"，不是"保证能执行成功的命令表"。用之前记住三件事： ★★
//   1. 去掉了哪些写法——效果必然与留下的某一条相同的（规则见上面的 canonical_command）：
//        走一步，但目标格现在不是空地                        等于不动
//        开炮，但装填中或子弹已达上限                        等于不动
//        快速型两步，第二步无论如何都走不成                  等于只走第一步
//        快速型两步，第一步的目标格现在不是空地              等于"第 1 小步原地等、第 2 小步走 d2"，
//                                                            同一个 d2 的几种写法只留一种（d1 取编号最小的那个走不成的方向）
//      不在场的坦克只有 STAY。被去掉的写法照样可以下，不算犯规，只是效果和留下的某一条完全一样。
//   2. 留下的命令仍可能走不成。这里只看地形和这辆车自己的装填、子弹数，不看任何坦克：
//      目标格上现在站着坦克（敌我都算）、或者别的坦克在同一小步也要进那一格时，这一步走不成、留在原地，
//      这取决于其他五辆车这一回合怎么下。开炮命令只保证子弹打得出去，不保证打到东西。
//   3. 快速型两步命令保留得比较宽："无论如何都走不成"只认水、基地、界外。第二步朝砖墙或钢墙走的保留
//      （墙可能在第 1 小步被打掉，第 2 小步就能进去）；第二步有两个可能的落点（第一步走成后的那一格，
//      和第一步被别的坦克挡住时从原地走的那一格），两个都是水、基地、界外才去掉。
//      所以表里会有"按现在的地形第二步走不进去"的两步命令：墙没被打掉时，它们的效果等于只走第一步。
//
// 结果恰好是所有满足 canonical_command(s, i, c) == c 的 c（这里直接算，比逐条调用快）。
inline int legal_commands(const State& s, int i, Cmd* out) {
    int n = 0;
    out[n++] = STAY;
    const Tank& t = s.tank[i];
    if (!t.alive) return n;
    // 四个邻格：现在能不能进（空地）、是不是永远进不了（水、基地、界外）。
    bool open_now[4], never_open[4];
    for (int d = 0; d < 4; ++d) {
        const uint8_t g = s.grid[t.pos + DIR_DELTA[d]];
        open_now[d] = g == EMPTY;
        never_open[d] = g >= WATER;
    }
    for (int d = 0; d < 4; ++d)
        if (open_now[d]) out[n++] = cmd_move(d);
    if (can_fire(s, i))
        for (int d = 0; d < 4; ++d) out[n++] = cmd_fire(d);
    if (s.P.types[type_of(i)].speed >= 2) {
        int first_blocked = -1;                       // 编号最小的"第一步必然走不成"的方向
        for (int d = 3; d >= 0; --d)
            if (!open_now[d]) first_blocked = d;
        for (int d1 = 0; d1 < 4; ++d1)
            for (int d2 = 0; d2 < 4; ++d2) {
                bool keep;
                if (!open_now[d1]) {
                    keep = d1 == first_blocked && !never_open[d2];          // "原地等一小步再走 d2"，只留一种写法
                } else {
                    const uint8_t g = s.grid[t.pos + DIR_DELTA[d1] + DIR_DELTA[d2]];
                    keep = !(g >= WATER && never_open[d2]);                 // 第二步无论如何走不成的，等于只走一步
                }
                if (keep) out[n++] = cmd_move2(d1, d2);
            }
    }
    return n;
}

// 协议允许第 i 辆坦克写的全部命令（不看局面）：快速型 25 条，其余 9 条。
inline int all_commands(const State& s, int i, Cmd* out) {
    const int n = s.P.types[type_of(i)].speed >= 2 ? N_CMDS : 9;
    for (int c = 0; c < n; ++c) out[c] = Cmd(c);
    return n;
}

// ====================================================================
// 7. 地图生成与开局
// ====================================================================

struct XY {
    int x, y;
    bool operator==(const XY&) const = default;
};

// 一张地图的布局。rows[y][x] 是 CELL_CHARS 里的字符。
struct Layout {
    int size = 0;
    char rows[MAX_SIZE][MAX_SIZE + 1] = {};
    XY base[2] = {};            // 两方基地
    XY spawn[6] = {};           // 六辆坦克的出生点（按坦克编号）
    int n_supply = 0;
    XY supply[MAX_SUPPLY] = {}; // 补给点
    int ring = 1;               // 基地外的砖墙层数（只有 check_layout 用）
};

// 按布局和参数开一局。布局里的 size 优先于 P.size。
inline State new_game(const Params& params, const Layout& lay) {
    State s;
    s.P = params;
    s.P.size = lay.size;
    for (int c = 0; c < CELLS; ++c) s.grid[c] = OUTSIDE;
    for (int y = 0; y < lay.size; ++y)
        for (int x = 0; x < lay.size; ++x) {
            Cell g = EMPTY;
            switch (lay.rows[y][x]) {
                case '#': g = BRICK; break;
                case '@': g = STEEL; break;
                case '~': g = WATER; break;
                case '0': g = BASE0; break;
                case '1': g = BASE1; break;
                default:  g = EMPTY; break;
            }
            s.grid[cell_of(x, y)] = g;
            if (g == BRICK || g == STEEL) s.wall_hash ^= wall_key(cell_of(x, y));
        }
    for (int seat = 0; seat < 2; ++seat) {
        s.base_pos[seat] = int16_t(cell_of(lay.base[seat].x, lay.base[seat].y));
        s.base_hp[seat] = int16_t(s.P.base_hp);
    }
    s.n_supply = int16_t(lay.n_supply);
    for (int k = 0; k < lay.n_supply; ++k) s.supply[k] = int16_t(cell_of(lay.supply[k].x, lay.supply[k].y));
    for (int i = 0; i < 6; ++i) {
        Tank& t = s.tank[i];
        s.spawn[i] = int16_t(cell_of(lay.spawn[i].x, lay.spawn[i].y));
        t.pos = s.spawn[i];
        t.alive = true;
        t.level = int16_t(s.P.start_level[seat_of(i)]);
        t.hp = int16_t(s.P.types[type_of(i)].hp + s.P.level_hp * t.level);
    }
    return s;
}

// 从局面取回布局（地形是当前的，被打掉的墙不会回来）。
inline Layout layout_of(const State& s) {
    Layout lay;
    lay.size = s.P.size;
    lay.ring = s.P.ring;
    for (int y = 0; y < lay.size; ++y)
        for (int x = 0; x < lay.size; ++x) lay.rows[y][x] = CELL_CHARS[s.grid[cell_of(x, y)]];
    for (int seat = 0; seat < 2; ++seat) lay.base[seat] = XY{cell_x(s.base_pos[seat]), cell_y(s.base_pos[seat])};
    for (int i = 0; i < 6; ++i) lay.spawn[i] = XY{cell_x(s.spawn[i]), cell_y(s.spawn[i])};
    lay.n_supply = s.n_supply;
    for (int k = 0; k < s.n_supply; ++k) lay.supply[k] = XY{cell_x(s.supply[k]), cell_y(s.supply[k])};
    return lay;
}

// 地图生成用的随机数：splitmix64，加拒绝采样取范围。只用整数运算，
// 所以同一个种子在任何编译器、任何标准库下都生成同一张图。
struct Rng {
    uint64_t state;
    explicit constexpr Rng(uint64_t seed) : state(seed) {}
    constexpr uint64_t next() {
        uint64_t z = (state += 0x9E3779B97F4A7C15ULL);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }
    // [0, n) 上的均匀整数，n ≥ 1。
    constexpr int below(int n) {
        const uint64_t m = uint64_t(n);
        const uint64_t limit = UINT64_MAX - UINT64_MAX % m;   // m 的整数倍，丢掉尾巴保证均匀
        uint64_t r = next();
        while (r >= limit) r = next();
        return int(r % m);
    }
};

// 地图风格。各风格里砖、钢、水占"上半场可放格子"的千分比。
enum Style : int { STYLE_OPEN = 0, STYLE_BRICK = 1, STYLE_WATER = 2, STYLE_STEEL = 3 };
inline constexpr int N_STYLES = 4;
inline constexpr const char* STYLE_NAMES[N_STYLES] = {"open", "brick", "water", "steel"};
inline constexpr int STYLE_DENSITY[N_STYLES][3] = {
    // 砖   钢   水
    {100,  20,  20},    // 开阔
    {340,  30,  20},    // 多砖
    {100,  20, 170},    // 多水
    {130, 140,  20},    // 多钢
};

// seat 号方的基地能从左、右、正前中的几个方向被打到（定义见 check_layout 的第 4 条）。
// 注意：这是对开局地图（Layout）的静态检查，只看地形：砖墙、水不算挡，不看坦克；
// 对局中墙被打掉、有车堵在炮线上，它都不知道。它不是"此刻有没有炮线"的查询。
inline int base_open_sides(const Layout& lay, int seat) {
    const int n = lay.size, R = lay.ring;
    const XY b = lay.base[seat];
    auto in_wall = [&](int x, int y) {      // 这个基地和它的围墙占的那一块
        return x >= b.x - R && x <= b.x + R && y >= b.y - R && y <= b.y + R;
    };
    // 进攻方（另一方）的坦克从出生点出发、不进围墙能走到哪些格子。
    bool reach[MAX_SIZE][MAX_SIZE] = {};
    XY queue[MAX_SIZE * MAX_SIZE];
    int head = 0, tail = 0;
    for (int k = 0; k < 3; ++k) {
        const XY p = lay.spawn[3 * (1 - seat) + k];
        if (!reach[p.y][p.x] && !in_wall(p.x, p.y)) {
            reach[p.y][p.x] = true;
            queue[tail++] = p;
        }
    }
    while (head < tail) {
        const XY p = queue[head++];
        for (int d = 0; d < 4; ++d) {
            const int x = p.x + DIR_DX[d], y = p.y + DIR_DY[d];
            if (x < 0 || y < 0 || x >= n || y >= n || reach[y][x] || in_wall(x, y)) continue;
            const char ch = lay.rows[y][x];
            if (ch != '.' && ch != '#') continue;
            reach[y][x] = true;
            queue[tail++] = XY{x, y};
        }
    }
    // 左、右、正前（朝地图中间）三条线。
    const int line_dx[3] = {-1, 1, 0};
    const int line_dy[3] = {0, 0, b.y < n / 2 ? 1 : -1};
    int open = 0;
    for (int side = 0; side < 3; ++side) {
        for (int k = 1;; ++k) {
            const int x = b.x + k * line_dx[side], y = b.y + k * line_dy[side];
            if (x < 0 || y < 0 || x >= n || y >= n) break;
            const char ch = lay.rows[y][x];
            if (ch == '@' || ch == '0' || ch == '1') break;       // 钢墙挡住了这条线
            if (!in_wall(x, y) && reach[y][x]) {                  // 围墙外、走得到、能站：一个开炮位
                ++open;
                break;
            }
        }
    }
    return open;
}

// 检查一张布局是否满足生成器的保证。通过返回空串，否则返回原因。
//   1. 地形上下镜像对称（两个基地互为镜像）；
//   2. 出生点、补给点都是空地，补给点都在中线上；
//   3. 把砖墙当空地（钢墙和水不能过）时，两个基地、六个出生点、所有补给点互相连通；
//   4. 每个基地至少能从左、右、正前三个方向中的两个被打到。一个方向"能被打到"是指：从基地沿这个方向
//      往外的那条线上，围墙之外有一格可以站坦克（空地，或者能打掉的砖墙），它和基地之间没有钢墙
//      （砖墙能打掉、水不挡子弹，都不算挡），而且进攻方的坦克不穿过这个基地的围墙就能走到这一格
//      （把砖墙当空地，钢墙和水不能过）。
inline std::string check_layout(const Layout& lay) {
    const int n = lay.size;
    if (n < 5 || n > MAX_SIZE) return "尺寸超出范围";
    auto at = [&](int x, int y) { return lay.rows[y][x]; };
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            const char a = at(x, y), b = at(x, n - 1 - y);
            const bool bases = (a == '0' && b == '1') || (a == '1' && b == '0');
            if (a != b && !bases) return "地形不对称";
        }
    for (int i = 0; i < 6; ++i)
        if (at(lay.spawn[i].x, lay.spawn[i].y) != '.') return "出生点不是空地";
    for (int k = 0; k < lay.n_supply; ++k)
        if (at(lay.supply[k].x, lay.supply[k].y) != '.' || lay.supply[k].y != n / 2) return "补给点不是中线空地";
    bool seen[MAX_SIZE][MAX_SIZE] = {};
    XY queue[MAX_SIZE * MAX_SIZE];
    int head = 0, tail = 0;
    queue[tail++] = lay.base[0];
    seen[lay.base[0].y][lay.base[0].x] = true;
    while (head < tail) {
        const XY p = queue[head++];
        for (int d = 0; d < 4; ++d) {
            const int x = p.x + DIR_DX[d], y = p.y + DIR_DY[d];
            if (x < 0 || y < 0 || x >= n || y >= n || seen[y][x]) continue;
            const char ch = at(x, y);
            if (ch != '.' && ch != '#' && ch != '0' && ch != '1') continue;
            seen[y][x] = true;
            queue[tail++] = XY{x, y};
        }
    }
    if (!seen[lay.base[1].y][lay.base[1].x]) return "两个基地不连通";
    for (int i = 0; i < 6; ++i)
        if (!seen[lay.spawn[i].y][lay.spawn[i].x]) return "出生点不连通";
    for (int k = 0; k < lay.n_supply; ++k)
        if (!seen[lay.supply[k].y][lay.supply[k].x]) return "补给点不连通";
    for (int seat = 0; seat < 2; ++seat)
        if (base_open_sides(lay, seat) < 2) return "基地能被打到的方向不足两个";
    return "";
}

// 按种子生成一张地图。style 传 -1 表示按种子轮换四种风格。
// 用到的参数：size（不小于 11 的奇数）、supply_points（1..5）、ring、spawn_near、spawn_flip。
// 同样的种子、风格和这几项参数总是生成同一张图；spawn_flip 只改 0 号方的车型排布，不改地形。
//
// 生成的图满足 check_layout 的全部保证（对称、连通、每个基地至少两个方向能被打到），另外：
//   基地所在的那几行里，围墙两侧各 3 格之内没有钢墙和水；
//   基地在己方底边正中，外面围 ring 层砖；补给点在中线上、左右对称；
//   哪种车型用哪个出生位由种子决定；1 号方三辆车的出生位上下翻过去就是 0 号方的三个出生位，
//   spawn_flip 时 0 号方的车型排布再左右对调（比如 1 号方的快速型在左，0 号方的快速型就在右）。
inline Layout generate_map(uint64_t seed, int style, const Params& P) {
    if (style < 0 || style >= N_STYLES) style = int(seed % N_STYLES);
    const int n = P.size, cx = n / 2, mid = n / 2, R = P.ring;

    // 种子和影响地形的参数混在一起作为随机数的起点。
    uint64_t h = Rng(seed).next();
    for (int v : {style, n, P.supply_points, R, int(P.spawn_near)}) h = Rng(h ^ (uint64_t(v) + 1) * 0xD6E8FEB86659FD93ULL).next();
    Rng rng(h);

    Layout lay;
    lay.size = n;
    lay.ring = R;
    lay.base[0] = XY{cx, n - 1};
    lay.base[1] = XY{cx, 0};

    // 1 号方（上方）的三个出生位。
    XY slot[3];
    const int front = R + 1;
    if (P.spawn_near) {
        slot[0] = XY{cx - R - 1, front};
        slot[1] = XY{cx + R + 1, front};
        slot[2] = XY{cx, front};
    } else {
        const int off = n / 4 + 1;
        slot[0] = XY{cx - off, 0};
        slot[1] = XY{cx + off, 0};
        slot[2] = XY{cx, front};
    }
    // 哪种车型用哪个出生位：洗牌。
    int perm[3] = {0, 1, 2};
    for (int k = 2; k > 0; --k) {
        const int j = rng.below(k + 1);
        const int tmp = perm[k];
        perm[k] = perm[j];
        perm[j] = tmp;
    }
    for (int k = 0; k < 3; ++k) {
        const XY p = slot[perm[k]];
        lay.spawn[3 + k] = p;
        lay.spawn[k] = P.spawn_flip ? XY{n - 1 - p.x, n - 1 - p.y} : XY{p.x, n - 1 - p.y};
    }

    // 补给点：都在中线上，左右对称。
    {
        const int d = n / 3, k = P.supply_points;
        int xs[MAX_SUPPLY] = {};
        if (k == 1) { xs[0] = cx; }
        else if (k == 2) { xs[0] = cx - d; xs[1] = cx + d; }
        else if (k == 3) { xs[0] = cx - d; xs[1] = cx; xs[2] = cx + d; }
        else if (k == 4) { xs[0] = cx - d; xs[1] = cx - d / 2; xs[2] = cx + d / 2; xs[3] = cx + d; }
        else if (k == 5) { xs[0] = cx - d; xs[1] = cx - d / 2; xs[2] = cx; xs[3] = cx + d / 2; xs[4] = cx + d; }
        lay.n_supply = k < 0 ? 0 : (k > MAX_SUPPLY ? MAX_SUPPLY : k);
        for (int j = 0; j < lay.n_supply; ++j) lay.supply[j] = XY{xs[j], mid};
    }

    // 上半场（含中线）里固定的格子：基地和围墙。fixed[y][x] 为 0 表示不固定。
    char fixed[MAX_SIZE][MAX_SIZE] = {};
    for (int y = 0; y <= R; ++y)
        for (int x = cx - R; x <= cx + R; ++x) fixed[y][x] = '#';
    fixed[0][cx] = '1';

    // 基地所在的那几行（围墙占的行）里，围墙两侧各 3 格之内不放钢墙和水，只可能是空地或砖墙。
    // 这样基地的左、右两条线总有开炮位，不会出现"两侧被钢墙或水封死、只剩正面一列能打"的图。
    auto base_side = [&](int x, int y) { return y <= R && x >= cx - R - 3 && x <= cx + R + 3; };

    // 不放随机障碍的格子：固定格、补给点、出生位和它四周。
    bool keep[MAX_SIZE][MAX_SIZE] = {};
    for (int y = 0; y <= mid; ++y)
        for (int x = 0; x < n; ++x) keep[y][x] = fixed[y][x] != 0;
    for (int j = 0; j < lay.n_supply; ++j) keep[mid][lay.supply[j].x] = true;
    for (const XY& p : slot) {
        keep[p.y][p.x] = true;
        for (int d = 0; d < 4; ++d) {
            const int x = p.x + DIR_DX[d], y = p.y + DIR_DY[d];
            if (x >= 0 && y >= 0 && x < n && y <= mid) keep[y][x] = true;
        }
    }
    XY free_cells[MAX_SIZE * MAX_SIZE];
    int n_free = 0;
    for (int y = 0; y <= mid; ++y)
        for (int x = 0; x < n; ++x)
            if (!keep[y][x]) free_cells[n_free++] = XY{x, y};

    char half[MAX_SIZE][MAX_SIZE];     // 上半场（含中线）的地形；下半场是它的镜像
    auto build_rows = [&]() {
        for (int y = 0; y < n; ++y) {
            for (int x = 0; x < n; ++x) {
                char ch = half[y <= mid ? y : n - 1 - y][x];
                if (y > mid && ch == '1') ch = '0';
                lay.rows[y][x] = ch;
            }
            lay.rows[y][n] = 0;
        }
    };
    auto reset_half = [&]() {
        for (int y = 0; y <= mid; ++y)
            for (int x = 0; x < n; ++x) half[y][x] = fixed[y][x] ? fixed[y][x] : '.';
    };
    // 撒一种障碍：反复随机挑一个起点、横或竖、长度 1..max_len，沿途放到目标数量为止；
    // 碰到保留格、已有障碍或出了上半场就停下这一条。
    auto bars = [&](char kind, int permille, int max_len) {
        const int target = (permille * n_free + 500) / 1000;
        int placed = 0;
        for (int guard = 0; placed < target && guard < 2000; ++guard) {
            const XY start = free_cells[rng.below(n_free)];
            const bool horizontal = rng.below(2) == 0;
            const int len = 1 + rng.below(max_len);
            for (int k = 0; k < len; ++k) {
                const int x = horizontal ? start.x + k : start.x;
                const int y = horizontal ? start.y : start.y + k;
                if (x >= n || y > mid || keep[y][x] || half[y][x] != '.') break;
                if (kind != '#' && base_side(x, y)) break;
                half[y][x] = kind;
                if (++placed >= target) break;
            }
        }
    };

    const int* dens = STYLE_DENSITY[style];
    for (int attempt = 0; attempt < 500 && n_free > 0; ++attempt) {
        reset_half();
        bars('~', dens[2], 4);
        bars('@', dens[1], 3);
        bars('#', dens[0], 4);
        build_rows();
        if (check_layout(lay).empty()) return lay;
    }
    // 500 次都不满足连通性（正常参数下不会发生）：退回一张没有随机障碍的图，它一定连通。
    reset_half();
    build_rows();
    return lay;
}

// 按种子开一局：generate_map + new_game。
inline State make_game(uint64_t seed, int style = -1, const Params& P = Params{}) {
    return new_game(P, generate_map(seed, style, P));
}

// ====================================================================
// 8. 局面与文本互转
// ====================================================================
// 格式见 协议.md。一份完整的局面文本 = 布局段（参数、基地、出生点、补给点）+ 回合段（其余全部）。
// to_text / from_text 互为逆运算：from_text(to_text(s)) == s。

inline std::string cmd_text(Cmd c) {
    if (c == STAY || c >= N_CMDS) return "S";
    if (c <= 4) return std::string(1, DIR_CHARS[c - 1]);
    if (c <= 8) return std::string("F") + DIR_CHARS[c - 5];
    return std::string(1, DIR_CHARS[(c - 9) >> 2]) + DIR_CHARS[(c - 9) & 3];
}

// 解析一条命令的文本。成功返回 true。
inline bool parse_cmd(std::string_view tok, Cmd& out) {
    auto dir = [](char ch) { return ch == 'U' ? 0 : ch == 'D' ? 1 : ch == 'L' ? 2 : ch == 'R' ? 3 : -1; };
    if (tok == "S") { out = STAY; return true; }
    if (tok.size() == 1 && dir(tok[0]) >= 0) { out = cmd_move(dir(tok[0])); return true; }
    if (tok.size() == 2 && tok[0] == 'F' && dir(tok[1]) >= 0) { out = cmd_fire(dir(tok[1])); return true; }
    if (tok.size() == 2 && dir(tok[0]) >= 0 && dir(tok[1]) >= 0) { out = cmd_move2(dir(tok[0]), dir(tok[1])); return true; }
    return false;
}

inline const char* reason_name(Reason r) {
    static const char* const names[] = {"none", "base", "hp", "kills", "stars", "damage", "draw"};
    return names[r];
}

// 结束原因的文本：none / base / hp / kills / stars / damage / draw，
// 两个基地同回合归零时前面加 both_。
inline std::string reason_text(const State& s) {
    if (!s.done) return "none";
    return (both_bases_down(s) ? std::string("both_") : std::string()) + reason_name(s.reason);
}

namespace detail {
struct Tokens {            // 按空白切词
    std::string_view text;
    size_t at = 0;
    bool next(std::string_view& tok) {
        while (at < text.size() && (text[at] == ' ' || text[at] == '\n' || text[at] == '\r' || text[at] == '\t')) ++at;
        if (at >= text.size()) return false;
        const size_t begin = at;
        while (at < text.size() && !(text[at] == ' ' || text[at] == '\n' || text[at] == '\r' || text[at] == '\t')) ++at;
        tok = text.substr(begin, at - begin);
        return true;
    }
    bool next_int(int& v) {
        std::string_view tok;
        if (!next(tok) || tok.empty()) return false;
        size_t k = 0;
        bool neg = false;
        if (tok[0] == '-') { neg = true; k = 1; }
        if (k >= tok.size() || tok.size() - k > 9) return false;
        int x = 0;
        for (; k < tok.size(); ++k) {
            if (tok[k] < '0' || tok[k] > '9') return false;
            x = x * 10 + (tok[k] - '0');
        }
        v = neg ? -x : x;
        return true;
    }
    bool expect(std::string_view word) {
        std::string_view tok;
        return next(tok) && tok == word;
    }
};

inline void put(std::string& out, int v) {
    out += std::to_string(v);
    out += ' ';
}
inline void end_line(std::string& out) {
    if (!out.empty() && out.back() == ' ') out.back() = '\n';
    else out += '\n';
}

inline bool fail(std::string* err, const char* msg) {
    if (err) *err = msg;
    return false;
}

inline bool in_map(const State& s, int x, int y) { return x >= 0 && y >= 0 && x < s.P.size && y < s.P.size; }

// 读布局段，写进 s 的 P、base_pos、spawn、supply。
inline bool read_layout(Tokens& in, State& s, std::string* err) {
    int v = 0;
    if (!in.expect("TANK-STATE") || !in.next_int(v) || v != 1) return fail(err, "开头应为 TANK-STATE 1");
    Params P;
    std::string_view tok;
    for (;;) {
        const size_t mark = in.at;
        if (!in.next(tok)) return fail(err, "布局段不完整");
        if (tok != "P") { in.at = mark; break; }
        std::string_view key;
        if (!in.next(key)) return fail(err, "P 后面缺参数名");
        if (key == "tiebreak") {
            if (!in.next_int(P.n_tiebreak) || P.n_tiebreak < 0 || P.n_tiebreak > 4) return fail(err, "tiebreak 的项数不对");
            for (int k = 0; k < P.n_tiebreak; ++k) {
                if (!in.next(tok)) return fail(err, "tiebreak 不完整");
                int found = -1;
                for (int j = 0; j < 4; ++j)
                    if (tok == TIEBREAK_NAMES[j]) found = j;
                if (found < 0) return fail(err, "tiebreak 项目未知");
                P.tiebreak[k] = found;
            }
            for (int k = P.n_tiebreak; k < 4; ++k) P.tiebreak[k] = 0;
            continue;
        }
        if (!in.next_int(v)) return fail(err, "参数值应为整数");
        bool known = false;
        for_each_param(P, [&](const char* name, auto& member) {
            if (key == name) {
                member = static_cast<std::remove_reference_t<decltype(member)>>(v);
                known = true;
            }
        });
        if (!known) return fail(err, "未知参数");
    }
    const std::string bad = check_params(P);
    if (!bad.empty()) {
        if (err) *err = bad;
        return false;
    }
    s.P = P;
    auto read_cell = [&](int16_t& cell) {
        int x, y;
        if (!in.next_int(x) || !in.next_int(y) || !in_map(s, x, y)) return false;
        cell = int16_t(cell_of(x, y));
        return true;
    };
    if (!in.expect("BASES") || !read_cell(s.base_pos[0]) || !read_cell(s.base_pos[1])) return fail(err, "BASES 行不对");
    if (!in.expect("SPAWNS")) return fail(err, "缺 SPAWNS");
    for (int i = 0; i < 6; ++i)
        if (!read_cell(s.spawn[i])) return fail(err, "SPAWNS 行不对");
    if (!in.expect("SUPPLY") || !in.next_int(v) || v < 0 || v > MAX_SUPPLY) return fail(err, "SUPPLY 行不对");
    s.n_supply = int16_t(v);
    for (int k = 0; k < MAX_SUPPLY; ++k) s.supply[k] = 0;
    for (int k = 0; k < s.n_supply; ++k)
        if (!read_cell(s.supply[k])) return fail(err, "SUPPLY 行不对");
    return true;
}

// 读回合段，写进 s 的其余部分。s 里要已经有布局段的内容。
inline bool read_turn(Tokens& in, State& s, std::string* err) {
    int a, b, c, d;
    std::string_view tok;
    if (!in.expect("TURN") || !in.next_int(a) || !in.next_int(b) || !in.next_int(c) || !in.next(tok))
        return fail(err, "TURN 行不对");
    if (a < 0 || a > 30000 || b < 0 || b > 1 || c < -1 || c > 1) return fail(err, "TURN 行的数值不对");
    s.turn = int16_t(a);
    s.done = b != 0;
    s.winner = int8_t(c);
    if (tok.substr(0, 5) == "both_") tok.remove_prefix(5);
    int reason = -1;
    for (int k = 0; k <= R_DRAW; ++k)
        if (tok == reason_name(Reason(k))) reason = k;
    if (reason < 0) return fail(err, "结束原因未知");
    s.reason = Reason(reason);
    auto pair = [&](const char* word, int16_t* dst) {
        if (!in.expect(word) || !in.next_int(a) || !in.next_int(b) || a < 0 || b < 0 || a > 30000 || b > 30000) return false;
        dst[0] = int16_t(a);
        dst[1] = int16_t(b);
        return true;
    };
    if (!pair("BASEHP", s.base_hp) || !pair("BASEQUIET", s.base_quiet) || !pair("KILLS", s.kills) ||
        !pair("STARCOUNT", s.stars_eaten) || !pair("BASEDMG", s.base_dmg))
        return fail(err, "BASEHP / BASEQUIET / KILLS / STARCOUNT / BASEDMG 行不对");
    if (!in.expect("MAP")) return fail(err, "缺 MAP");
    const int n = s.P.size;
    for (int k = 0; k < CELLS; ++k) s.grid[k] = OUTSIDE;
    s.wall_hash = 0;
    for (int y = 0; y < n; ++y) {
        if (!in.next(tok) || int(tok.size()) != n) return fail(err, "MAP 的行长不对");
        for (int x = 0; x < n; ++x) {
            int g = -1;
            for (int k = 0; k < 6; ++k)
                if (tok[x] == CELL_CHARS[k]) g = k;
            if (g < 0) return fail(err, "MAP 里有未知字符");
            s.grid[cell_of(x, y)] = uint8_t(g);
            if (g == BRICK || g == STEEL) s.wall_hash ^= wall_key(cell_of(x, y));
        }
    }
    if (!in.expect("TANKS") || !in.next_int(a) || a != 6) return fail(err, "TANKS 行不对");
    for (int i = 0; i < 6; ++i) {
        int id, x, y, hp, level, alive, respawn_turn, last_fire;
        if (!in.expect("T") || !in.next_int(id) || !in.next_int(x) || !in.next_int(y) || !in.next_int(hp) ||
            !in.next_int(level) || !in.next_int(alive) || !in.next_int(respawn_turn) || !in.next_int(last_fire))
            return fail(err, "T 行不完整");
        if (id != i || alive < 0 || alive > 1 || hp < -1000 || hp > 30000 || level < 0 || level > 100 ||
            respawn_turn < 0 || respawn_turn > 30000 || last_fire < NEVER_FIRED || last_fire > 30000)
            return fail(err, "T 行的数值不对");
        if (alive ? !in_map(s, x, y) : !(x == -1 && y == -1)) return fail(err, "T 行的坐标不对");
        Tank& t = s.tank[i];
        t.pos = int16_t(alive ? cell_of(x, y) : NO_CELL);
        t.hp = int16_t(hp);
        t.level = int16_t(level);
        t.alive = alive != 0;
        t.respawn_turn = int16_t(respawn_turn);
        t.last_fire = int16_t(last_fire);
    }
    if (!in.expect("BULLETS") || !in.next_int(a) || a < 0 || a > MAX_BULLETS) return fail(err, "BULLETS 行不对");
    s.n_bullets = int16_t(a);
    for (int k = 0; k < MAX_BULLETS; ++k) s.bullet[k] = Bullet{};
    for (int k = 0; k < s.n_bullets; ++k) {
        int x, y;
        if (!in.expect("B") || !in.next_int(x) || !in.next_int(y) || !in.next(tok) || !in.next_int(b) ||
            !in.next_int(c) || !in.next_int(d))
            return fail(err, "B 行不完整");
        int dir = -1;
        for (int j = 0; j < 4; ++j)
            if (tok.size() == 1 && tok[0] == DIR_CHARS[j]) dir = j;
        if (!in_map(s, x, y) || dir < 0 || b < 0 || b > 5 || c < 0 || c > 1 || d < 0 || d > 100) return fail(err, "B 行的数值不对");
        s.bullet[k] = Bullet{int16_t(cell_of(x, y)), int8_t(dir), int8_t(b), c != 0, int8_t(d)};
    }
    if (!in.expect("STARS") || !in.next_int(a) || a < 0 || a > s.n_supply) return fail(err, "STARS 行不对");
    s.star_mask = 0;
    for (int k = 0; k < a; ++k) {
        int x, y;
        if (!in.next_int(x) || !in.next_int(y)) return fail(err, "星星坐标不完整");
        int found = -1;
        for (int j = 0; j < s.n_supply; ++j)
            if (s.supply[j] == cell_of(x, y)) found = j;
        if (found < 0) return fail(err, "星星不在补给点上");
        s.star_mask |= uint8_t(1u << found);
    }
    if (!in.expect("END")) return fail(err, "缺 END");
    return true;
}
}  // namespace detail

// 布局段：参数、基地、出生点、补给点。一局之内不变。
inline std::string layout_text(const State& s) {
    using detail::put;
    using detail::end_line;
    std::string out = "TANK-STATE 1\n";
    for_each_param(s.P, [&](const char* name, const auto& member) {
        out += "P ";
        out += name;
        out += ' ';
        put(out, int(member));
        end_line(out);
    });
    out += "P tiebreak ";
    put(out, s.P.n_tiebreak);
    for (int k = 0; k < s.P.n_tiebreak; ++k) {
        out += TIEBREAK_NAMES[s.P.tiebreak[k]];
        out += ' ';
    }
    end_line(out);
    out += "BASES ";
    for (int seat = 0; seat < 2; ++seat) { put(out, cell_x(s.base_pos[seat])); put(out, cell_y(s.base_pos[seat])); }
    end_line(out);
    out += "SPAWNS ";
    for (int i = 0; i < 6; ++i) { put(out, cell_x(s.spawn[i])); put(out, cell_y(s.spawn[i])); }
    end_line(out);
    out += "SUPPLY ";
    put(out, s.n_supply);
    for (int k = 0; k < s.n_supply; ++k) { put(out, cell_x(s.supply[k])); put(out, cell_y(s.supply[k])); }
    end_line(out);
    return out;
}

// 回合段：随对局变化的全部内容。
inline std::string turn_text(const State& s) {
    using detail::put;
    using detail::end_line;
    std::string out;
    out.reserve(700);
    out += "TURN ";
    put(out, s.turn); put(out, s.done ? 1 : 0); put(out, s.winner);
    out += reason_text(s);
    end_line(out);
    auto pair = [&](const char* word, const int16_t* v) {
        out += word;
        out += ' ';
        put(out, v[0]); put(out, v[1]);
        end_line(out);
    };
    pair("BASEHP", s.base_hp);
    pair("BASEQUIET", s.base_quiet);
    pair("KILLS", s.kills);
    pair("STARCOUNT", s.stars_eaten);
    pair("BASEDMG", s.base_dmg);
    out += "MAP\n";
    for (int y = 0; y < s.P.size; ++y) {
        for (int x = 0; x < s.P.size; ++x) out += CELL_CHARS[s.grid[cell_of(x, y)]];
        out += '\n';
    }
    out += "TANKS 6\n";
    for (int i = 0; i < 6; ++i) {
        const Tank& t = s.tank[i];
        out += "T ";
        put(out, i); put(out, cell_x(t.pos)); put(out, cell_y(t.pos)); put(out, t.hp); put(out, t.level);
        put(out, t.alive ? 1 : 0); put(out, t.respawn_turn); put(out, t.last_fire);
        end_line(out);
    }
    out += "BULLETS ";
    put(out, s.n_bullets);
    end_line(out);
    for (int k = 0; k < s.n_bullets; ++k) {
        const Bullet& b = s.bullet[k];
        out += "B ";
        put(out, cell_x(b.pos)); put(out, cell_y(b.pos));
        out += DIR_CHARS[b.dir];
        out += ' ';
        put(out, b.owner); put(out, b.steel ? 1 : 0); put(out, b.level);
        end_line(out);
    }
    int n_stars = 0;
    for (int k = 0; k < s.n_supply; ++k) n_stars += (s.star_mask >> k) & 1;
    out += "STARS ";
    put(out, n_stars);
    end_line(out);
    for (int k = 0; k < s.n_supply; ++k)
        if ((s.star_mask >> k) & 1) {
            put(out, cell_x(s.supply[k])); put(out, cell_y(s.supply[k]));
            end_line(out);
        }
    out += "END\n";
    return out;
}

// 完整局面 → 文本。
inline std::string to_text(const State& s) { return layout_text(s) + turn_text(s); }

// 文本 → 完整局面。成功返回 true；失败返回 false，err 里是原因，s 的内容不确定。
inline bool from_text(std::string_view text, State& s, std::string* err = nullptr) {
    detail::Tokens in{text};
    s = State{};
    return detail::read_layout(in, s, err) && detail::read_turn(in, s, err);
}

// 只读回合段，更新 s 里随对局变化的部分（s 要已经有同一局的布局段内容）。
inline bool read_turn_text(std::string_view text, State& s, std::string* err = nullptr) {
    detail::Tokens in{text};
    return detail::read_turn(in, s, err);
}

// 一方一回合的回复行："<回合号> <快速型的命令> <火力型的命令> <装甲型的命令>"，回合号 = s.turn + 1。
inline std::string orders_text(const State& s, const Orders& o) {
    return std::to_string(s.turn + 1) + " " + cmd_text(o.cmd[0]) + " " + cmd_text(o.cmd[1]) + " " + cmd_text(o.cmd[2]);
}

// 解析回复行。'#' 之后是备注，忽略。以下情况整行非法、返回 false（裁判把这一回合三辆车都当不动）：
// 回合号不是 s.turn + 1；不是恰好 3 条命令；出现不认识的写法；给非快速型写了两步。
inline bool parse_orders(std::string_view line, const State& s, Orders& out, std::string* err = nullptr) {
    const size_t hash = line.find('#');
    if (hash != std::string_view::npos) line = line.substr(0, hash);
    detail::Tokens in{line};
    int t = 0;
    if (!in.next_int(t) || t != s.turn + 1) return detail::fail(err, "行首应为本回合编号");
    Orders o;
    std::string_view tok;
    for (int k = 0; k < 3; ++k) {
        if (!in.next(tok)) return detail::fail(err, "应为：回合号 + 3 条命令");
        if (!parse_cmd(tok, o.cmd[k])) return detail::fail(err, "命令只能是 S、U/D/L/R、两个方向字母或 FU/FD/FL/FR");
        if (is_move2(o.cmd[k]) && s.P.types[k].speed < 2) return detail::fail(err, "这种车型每回合只能走一步");
    }
    if (in.next(tok)) return detail::fail(err, "应为：回合号 + 3 条命令");
    out = o;
    return true;
}

// 一条事件的文本（回放用）。格式见 协议.md。
inline std::string event_text(const Event& e) {
    using detail::put;
    std::string out;
    auto xy = [&]() { put(out, e.x); put(out, e.y); };
    switch (e.type) {
        case EV_FIRE:           out = "fire "; put(out, e.a); out += DIR_CHARS[e.b]; break;
        case EV_BLOCKED:        out = "blocked "; put(out, e.a); put(out, e.sub); break;
        case EV_CANCEL_CROSS:   out = "cancel cross "; xy(); put(out, e.sub); put(out, e.a); put(out, e.b); break;
        case EV_CANCEL_SAME:    out = "cancel same "; xy(); put(out, e.sub); break;
        case EV_HIT:            out = "hit "; put(out, e.a); put(out, e.b); xy(); put(out, e.sub); put(out, 0); break;
        case EV_HIT_SWAP:       out = "hit "; put(out, e.a); put(out, e.b); xy(); put(out, e.sub); put(out, 1); break;
        case EV_BASE_HIT:       out = "base_hit "; put(out, e.a); put(out, e.b); put(out, e.sub); break;
        case EV_WALL_BRICK:     out = "wall "; xy(); out += "brick "; put(out, e.sub); break;
        case EV_WALL_STEEL:     out = "wall "; xy(); out += "steel "; put(out, e.sub); break;
        case EV_DEATH:          out = "death "; put(out, e.a); xy(); put(out, e.b); break;
        case EV_RESPAWN:        out = "respawn "; put(out, e.a); xy(); break;
        case EV_BULLET_CLEARED: out = "bullet_cleared "; xy(); put(out, e.a); break;
        case EV_STAR:           out = "star "; put(out, e.a); xy(); put(out, e.b); break;
        case EV_SUPPLY:         out = "supply "; xy(); break;
        case EV_BASE_REGEN:     out = "base_regen "; put(out, e.a); break;
    }
    if (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

// ====================================================================
// 9. 思考预算
// ====================================================================
// 每回合的思考有预算：首回合（init 加第一次 decide）BUDGET_FIRST_MS，之后每回合 BUDGET_TURN_MS。
// 单位是"CPU 毫秒"：量的是你的程序自己消耗的 CPU 时间，机器忙的时候被挂起、等输入的时间都不算。
// bot 只需要用 budget_left()：
//
//     while (tank::budget_left() > 3) { /* 再搜一层 */ }
//
// 起点由主办方的主循环设好，bot 不要自己调 budget_setup / budget_begin。
// 裁判量的是你的整个进程（所有线程合计），比 budget_left() 看到的多不到 1 毫秒，留 2–3 毫秒余量即可。
// 注意：budget_left() 只看得到调用它的那个线程。开了别的线程的话，那些线程的用量要自己加上。
// 超出预算的回合三辆车都不动，连续 3 次判负。
//
// 【计时的"缝"】怎么量只在两处：这里的 detail::budget_reading()（bot 侧），和裁判 core.py 里的
// meter_read()（裁判侧）。尺子有两把，由裁判在开局时告诉主循环用哪一把：
//   BUDGET_CPU   Windows 上是线程的周期计数（QueryThreadCycleTime），除以裁判给的"每 CPU 毫秒多少周期"；
//                Linux / macOS 上是线程的 CPU 时间（CLOCK_THREAD_CPUTIME_ID，按分析实现，未实测）。
//   BUDGET_WALL  墙上时钟。后备和调试用。

inline constexpr int BUDGET_FIRST_MS = 2000;        // 首回合的预算（含 init 和程序启动）
inline constexpr int BUDGET_TURN_MS = 50;           // 之后每回合的预算
inline constexpr int BUDGET_WALL_FIRST_MS = 4000;   // 按 CPU 计时时的墙上保险上限：首回合
inline constexpr int BUDGET_WALL_TURN_MS = 2000;    // 按 CPU 计时时的墙上保险上限：之后每回合（兜住睡眠、阻塞）

enum BudgetKind : int { BUDGET_NONE = 0, BUDGET_CPU = 1, BUDGET_WALL = 2 };

namespace detail {
#if defined(_WIN32)
namespace win {    // 只声明用到的两个系统函数，不引入 <windows.h>
extern "C" __declspec(dllimport) void* __stdcall GetCurrentThread();
extern "C" __declspec(dllimport) int __stdcall QueryThreadCycleTime(void* thread, unsigned long long* cycles);
}  // namespace win
inline constexpr bool BUDGET_CPU_AVAILABLE = true;
#elif defined(CLOCK_THREAD_CPUTIME_ID)
inline constexpr bool BUDGET_CPU_AVAILABLE = true;
#else
inline constexpr bool BUDGET_CPU_AVAILABLE = false;
#endif

struct BudgetState {
    int kind = BUDGET_NONE;   // 主循环设过没有、用哪把尺
    double per_ms = 1;        // 读数的多少个单位算 1 毫秒
    double start = 0;         // 本回合起点的读数
    double limit = 0;         // 本回合的预算（毫秒）
};
inline BudgetState budget_state;

// 【缝 · bot 侧】当前读数，只增不减。BUDGET_CPU：调用它的这个线程从创建起消耗的 CPU 量
// （Windows 为周期数，其他系统为纳秒）；否则是墙上时钟的毫秒数。
inline double budget_reading() {
    if (budget_state.kind == BUDGET_CPU) {
#if defined(_WIN32)
        unsigned long long cycles = 0;
        win::QueryThreadCycleTime(win::GetCurrentThread(), &cycles);
        return double(cycles);
#elif defined(CLOCK_THREAD_CPUTIME_ID)
        timespec ts{};
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
        return double(ts.tv_sec) * 1e9 + double(ts.tv_nsec);
#endif
    }
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
}  // namespace detail

// 选尺子。由主循环在开局时调用一次；bot 不要调用。per_ms：BUDGET_CPU 下读数的多少个单位算 1 CPU 毫秒
// （Windows 是裁判测定的每毫秒周期数，其他系统是 1000000）。这里没有 CPU 尺子可用时退回墙上时钟，返回实际用的那把。
inline int budget_setup(int kind, double per_ms) {
    if (kind == BUDGET_CPU && detail::BUDGET_CPU_AVAILABLE && per_ms > 0) {
        detail::budget_state.kind = BUDGET_CPU;
        detail::budget_state.per_ms = per_ms;
    } else {
        detail::budget_state.kind = BUDGET_WALL;
        detail::budget_state.per_ms = 1;
    }
    return detail::budget_state.kind;
}

// 设定本回合的起点（此刻）和预算。由主循环调用；bot 不要调用。
inline void budget_begin(double limit_ms) {
    detail::budget_state.limit = limit_ms;
    detail::budget_state.start = detail::budget_reading();
}

// 本回合已经用掉多少毫秒。主循环没有设过时为 0。
inline double budget_used() {
    if (detail::budget_state.kind == BUDGET_NONE) return 0.0;
    return (detail::budget_reading() - detail::budget_state.start) / detail::budget_state.per_ms;
}

// 本回合还剩多少思考预算（毫秒，带小数；用超了是负数）。
// 没有主循环时（比如自己写 main 直接调 decide 做实验）没有起点，返回一个很大的数。
inline double budget_left() {
    if (detail::budget_state.kind == BUDGET_NONE) return 1e12;
    return detail::budget_state.limit - budget_used();
}

}  // namespace tank

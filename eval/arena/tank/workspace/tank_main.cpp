// tank_main.cpp —— 主循环：负责读局面、调你的 init / decide、写命令、给每回合的思考预算设起点。
//
// 你的 bot.cpp 只需要定义这两个函数，不用写任何读写代码：
//
//     void init(const tank::State& s, int me);            // 每局开始调用一次
//     tank::Orders decide(const tank::State& s, int me);  // 每回合调用一次
//
// 正式编译命令（在选手包目录里）：
//
//     g++ -std=c++20 -O2 -static -I. submission/bot.cpp tank_main.cpp -o bot.exe
//
// 同一份文件有两种编法：
//   默认              独立程序，从标准输入读局面、往标准输出写命令（正式模式）。
//   -DTANK_FAST_DLL   动态库，导出几个函数给快速模式的对局程序直接调用（arena.py 自动处理）。
// 两种编法调用 init / decide 和设预算起点的代码是同一段（见 call_init / call_decide）。
//
// 这份文件不需要改，正式比赛用的是主办方手里的同一份。

#include "tank.hpp"

#include <cstdio>
#include <cstring>
#include <exception>
#include <string>

void init(const tank::State& s, int me);
tank::Orders decide(const tank::State& s, int me);

namespace {

bool g_first_turn = true;    // 首回合的预算从 init 之前算起，第一次 decide 不再重设

// 每局一次。started 为 true 表示调用者已经给首回合设过预算起点；否则首回合的预算从这里开始算。
void call_init(const tank::State& s, int me, bool started) {
    if (!started) tank::budget_begin(tank::BUDGET_FIRST_MS);
    g_first_turn = true;
    init(s, me);
}

// 每回合一次。started 为 true 表示调用者已经给本回合设过预算起点。
tank::Orders call_decide(const tank::State& s, int me, bool started) {
    if (g_first_turn) g_first_turn = false;
    else if (!started) tank::budget_begin(tank::BUDGET_TURN_MS);
    return decide(s, me);
}

}  // namespace

#ifdef TANK_FAST_DLL
// ====================================================================
// 快速模式：编成动态库，由对局程序加载。每局重新加载一次，所以全局变量每局都回到初始。
// ====================================================================
#ifdef _WIN32
#define TANK_EXPORT extern "C" __declspec(dllexport)
#else
#define TANK_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace {
int report(char* err, int cap, const char* what) {
    if (err && cap > 0) {
        std::strncpy(err, what, size_t(cap) - 1);
        err[cap - 1] = 0;
    }
    return 1;
}
}  // namespace

// 用来核对两边是不是同一份 tank.hpp 编出来的。
TANK_EXPORT int tankbot_abi(int* state_size, const char** engine_version) {
    *state_size = int(sizeof(tank::State));
    *engine_version = tank::ENGINE_VERSION;
    return 1;
}

// 选计时的尺子（见 tank.hpp 第 9 节），返回实际用的那把。每次加载后调用一次。
TANK_EXPORT int tankbot_setup(int kind, double per_ms) { return tank::budget_setup(kind, per_ms); }

// 返回 0 = 正常；1 = bot 抛出了没接住的异常（err 里是说明）。used_ms 是本回合到此刻的预算用量。
TANK_EXPORT int tankbot_init(const tank::State* s, int me, double* used_ms, char* err, int cap) {
    try {
        call_init(*s, me, false);
    } catch (const std::exception& e) {
        return report(err, cap, e.what());
    } catch (...) {
        return report(err, cap, "未知类型的异常");
    }
    *used_ms = tank::budget_used();
    return 0;
}

TANK_EXPORT int tankbot_decide(const tank::State* s, int me, tank::Orders* out, double* used_ms, char* err, int cap) {
    try {
        *out = call_decide(*s, me, false);
    } catch (const std::exception& e) {
        return report(err, cap, e.what());
    } catch (...) {
        return report(err, cap, "未知类型的异常");
    }
    *used_ms = tank::budget_used();
    return 0;
}

#else
// ====================================================================
// 正式模式：独立程序。
//   开局：  裁判发来一行 "SEAT <座位> BUDGET <cpu|wall> <每毫秒多少个读数单位>"，
//           接着是完整的局面文本（布局段 + 回合段，以 END 行结束）。
//   每回合：裁判发来一个回合段（以 END 行结束）；回一行 "<回合号> <快速> <火力> <装甲>"。
//   首回合的回合段就是开局那一份，所以开局后直接回第 1 回合的命令。
//   读到输入结束或已结束的局面就退出。
// ====================================================================
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#define TANK_DUP _dup
#define TANK_DUP2 _dup2
#define TANK_FDOPEN _fdopen
#else
#include <unistd.h>
#define TANK_DUP dup
#define TANK_DUP2 dup2
#define TANK_FDOPEN fdopen
#endif

namespace {

// 读到一行 END 为止，把这一段（含 END）追加进 text。一行一行地读，读到换行就返回，
// 不会为了凑满缓冲区而等下一回合的输入。第一行到达时调用 on_first（给预算设起点）。
// 输入结束返回 false。
template <class F>
bool read_block(std::FILE* in, std::string& text, F&& on_first) {
    char buf[1024];
    std::string line;
    bool first = true;
    while (std::fgets(buf, sizeof buf, in)) {
        if (first) {
            on_first();
            first = false;
        }
        line += buf;
        if (line.empty() || line.back() != '\n') continue;    // 一行比缓冲区长，接着读
        text += line;
        size_t a = 0, b = line.size();
        while (b > a && (line[b - 1] == '\n' || line[b - 1] == '\r' || line[b - 1] == ' ' || line[b - 1] == '\t')) --b;
        while (a < b && (line[a] == ' ' || line[a] == '\t')) ++a;
        if (b - a == 3 && line.compare(a, 3, "END") == 0) return true;
        line.clear();
    }
    return false;
}

}  // namespace

int main() {
    const double wall_at_entry = tank::detail::budget_reading();    // 还没选尺子，这是墙上时钟的读数

    // 协议用的输出单独留一个句柄，再把标准输出并到标准错误上：
    // bot 里不小心用 printf / cout 打的调试信息只会进调试输出，不会被裁判当成命令。
    std::FILE* out = stdout;
    std::fflush(stdout);
    const int fd = TANK_DUP(1);
    if (fd >= 0 && TANK_DUP2(2, 1) >= 0) {
        if (std::FILE* f = TANK_FDOPEN(fd, "w")) {
            out = f;
            std::setvbuf(stdout, nullptr, _IONBF, 0);
        }
    }

    std::string text;
    if (!read_block(stdin, text, [] {})) return 0;

    // 第一行：座位和计时方式；后面是完整局面。
    int me = -1;
    char kind_word[16] = "wall";
    double per_ms = 1;
    const size_t eol = text.find('\n');
    if (eol == std::string::npos || std::sscanf(text.c_str(), "SEAT %d BUDGET %15s %lf", &me, kind_word, &per_ms) < 1 ||
        me < 0 || me > 1) {
        std::fprintf(stderr, "tank_main: 开局的第一行应为 SEAT 0 或 SEAT 1\n");
        return 2;
    }
    tank::State s;
    std::string err;
    if (!tank::from_text(std::string_view(text).substr(eol + 1), s, &err)) {
        std::fprintf(stderr, "tank_main: 开局局面读不懂：%s\n", err.c_str());
        return 2;
    }

    // 首回合的预算：按 CPU 计时从本进程启动算起（裁判也是这么记的）；按墙上时钟从进入 main 算起。
    const bool want_cpu = std::strcmp(kind_word, "cpu") == 0;
    const bool cpu = tank::budget_setup(want_cpu ? tank::BUDGET_CPU : tank::BUDGET_WALL, per_ms) == tank::BUDGET_CPU;
    if (want_cpu && !cpu) std::fprintf(stderr, "tank_main: 这个系统上没有 CPU 计时可用，预算按墙上时钟算\n");
    tank::detail::budget_state.limit = tank::BUDGET_FIRST_MS;
    tank::detail::budget_state.start = cpu ? 0.0 : wall_at_entry;

    call_init(s, me, true);
    while (!tank::is_over(s)) {
        const tank::Orders o = call_decide(s, me, true);
        std::fputs((tank::orders_text(s, o) + "\n").c_str(), out);
        std::fflush(out);

        // 下一回合的预算起点。按 CPU 计时：从交出这一回合的命令算起（等输入不耗 CPU，裁判也是从这里记）。
        // 按墙上时钟：从下一回合的输入到达算起。
        if (cpu) tank::budget_begin(tank::BUDGET_TURN_MS);
        text.clear();
        if (!read_block(stdin, text, [cpu] { if (!cpu) tank::budget_begin(tank::BUDGET_TURN_MS); })) break;
        if (!tank::read_turn_text(text, s, &err)) {
            std::fprintf(stderr, "tank_main: 回合局面读不懂：%s\n", err.c_str());
            return 2;
        }
    }
    return 0;
}
#endif

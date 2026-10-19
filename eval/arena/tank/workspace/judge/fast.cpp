// fast.cpp —— 快速模式的对局程序：把两个 bot（各编成一个动态库）装进同一个进程直接互相调用。
//
// 用法：fast <A 的动态库> <B 的动态库> [--log] [--hang 秒数] [--wall]
// 标准输入每行一局："<编号> <种子> <风格> <换边>"，换边为 0 时 A 坐 0 号位，为 1 时 A 坐 1 号位。
// 标准输出每局：
//     B <编号>                      开始这一局（先打出来，进程中途死掉时知道死在哪一局）
//     O <回合> <0 号方命令>|<1 号方命令>     只有 --log 时：这一回合实际执行的命令
//     F <回合> <座位> <原因>         这一回合该座位的输出不合法（三辆都不动，记一次失败）
//     FINAL + 回合段文本             只有 --log 时：终局局面
//     G <编号> <种子> <风格> <换边> <回合数> <胜方座位> <原因> <签名> <A 的用量> <B 的用量>
//         用量 = 首回合毫秒:其余回合平均:其余回合最慢:超预算的回合数:不合法的回合数
// 出事时打一行 "X <编号> <哪一方：A/B/?> <说明>" 然后退出（退出码 3）。
//
// 和正式模式的对应关系（这几条保证不依赖时间的 bot 两种模式打出同样的对局）：
//   - 地图：同一个 tank::make_game(种子, 风格)。
//   - 调用顺序：每个 bot 先 init 一次，之后每回合 decide 一次；传进去的是同一个局面。
//   - bot 返回的命令先写成文本再按协议解析（和正式模式走同一个 parse_orders），不合法就三辆都不动、
//     记一次失败；连续 3 次判负，对局在那一回合结束、不再结算。
//   - 每局开始前把两个动态库卸掉重新加载，全局变量、静态变量都回到初始。
//   - 预算只统计、不判罚；崩溃不隔离。
//   - 预算的统计：每次调用 bot 前后读本线程的 CPU 用量（Windows 上是 QueryThreadCycleTime），
//     所以只算到调用 bot 的这个线程；--wall 时改用墙上时钟。

#include "../tank.hpp"
#include "cycles.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#endif

namespace {

const int MAX_STRIKES = 3;    // 与 core.py 的 MAX_STRIKES 相同

using AbiFn = int (*)(int*, const char**);
using SetupFn = int (*)(int, double);
using InitFn = int (*)(const tank::State*, int, double*, char*, int);
using DecideFn = int (*)(const tank::State*, int, tank::Orders*, double*, char*, int);

struct Lib {
    const char* path = nullptr;
    void* handle = nullptr;
    InitFn init = nullptr;
    DecideFn decide = nullptr;
};

int g_budget_kind = tank::BUDGET_CPU;       // 让 bot 用哪把尺
double g_per_ms = 1;                        // 每 CPU 毫秒多少个读数单位
std::FILE* g_out = nullptr;                 // 结果输出（原来的标准输出）
long long g_game = -1;                      // 正在打的那一局的编号
std::atomic<int> g_inside{-1};              // 正在哪个 bot 的代码里：0 = A，1 = B，-1 = 都不在
std::atomic<unsigned long long> g_calls{0}; // 调用计数，给看门狗判断有没有进展
const char WHO[] = "AB";

// 出事时的最后一行。只用最朴素的写法，因为可能是在崩溃处理里。
[[noreturn]] void die(const char* what) {
    const int who = g_inside.load();
    char line[600];
    const int n = std::snprintf(line, sizeof line, "X %lld %c %s\n", g_game, who < 0 ? '?' : WHO[who], what);
    if (g_out) {
        std::fflush(g_out);
#ifdef _WIN32
        _write(_fileno(g_out), line, unsigned(n));
#else
        if (write(fileno(g_out), line, size_t(n)) < 0) {}
#endif
    }
    std::_Exit(3);
}

void on_signal(int sig) {
    char what[64];
    std::snprintf(what, sizeof what, "%s", sig == SIGABRT ? "程序中止（abort / 未接住的异常 / 断言失败）"
                                         : sig == SIGFPE ? "算术错误（比如除以零）"
                                         : sig == SIGSEGV ? "非法内存访问" : "非法指令");
    die(what);
}

#ifdef _WIN32
LONG WINAPI on_seh(EXCEPTION_POINTERS* info) {
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    char what[96];
    const char* name = code == EXCEPTION_ACCESS_VIOLATION ? "非法内存访问"
                       : code == EXCEPTION_STACK_OVERFLOW ? "栈溢出（递归太深）"
                       : code == EXCEPTION_INT_DIVIDE_BY_ZERO ? "除以零"
                       : code == EXCEPTION_ILLEGAL_INSTRUCTION ? "非法指令" : "崩溃";
    std::snprintf(what, sizeof what, "%s（异常代码 0x%08lX）", name, static_cast<unsigned long>(code));
    die(what);
}
#endif

void on_exit_inside() {
    // bot 自己调了 exit()：正常跑完时 g_inside 是 -1，不会走到这里面。
    if (g_inside.load() >= 0) die("bot 调用了 exit()");
}

void* lib_open(const char* path) {
#ifdef _WIN32
    return static_cast<void*>(LoadLibraryA(path));
#else
    return dlopen(path, RTLD_NOW | RTLD_LOCAL);
#endif
}
void lib_close(void* h) {
#ifdef _WIN32
    FreeLibrary(static_cast<HMODULE>(h));
#else
    dlclose(h);
#endif
}
void* lib_symbol(void* h, const char* name) {
#ifdef _WIN32
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(h), name));
#else
    return dlsym(h, name);
#endif
}

// 重新加载：卸掉再装，库里的全局变量、静态变量全部重新初始化。
void reload(Lib& lib, int who) {
    g_inside = who;    // 全局对象的构造和析构也是 bot 的代码
    if (lib.handle) {
        lib_close(lib.handle);
#ifdef _WIN32
        if (GetModuleHandleA(lib.path)) die("动态库卸不掉（bot 可能留着后台线程），全局状态没法重置");
#endif
    }
    lib.handle = lib_open(lib.path);
    if (!lib.handle) die("动态库加载失败");
    const AbiFn abi = reinterpret_cast<AbiFn>(lib_symbol(lib.handle, "tankbot_abi"));
    lib.init = reinterpret_cast<InitFn>(lib_symbol(lib.handle, "tankbot_init"));
    lib.decide = reinterpret_cast<DecideFn>(lib_symbol(lib.handle, "tankbot_decide"));
    int size = 0;
    const char* version = "";
    if (!abi || !lib.init || !lib.decide || abi(&size, &version) != 1 || size != int(sizeof(tank::State)) ||
        std::strcmp(version, tank::ENGINE_VERSION) != 0)
        die("动态库和对局程序不是同一份 tank.hpp 编出来的");
    const SetupFn setup = reinterpret_cast<SetupFn>(lib_symbol(lib.handle, "tankbot_setup"));
    if (!setup) die("动态库和对局程序不是同一份主循环编出来的");
    setup(g_budget_kind, g_per_ms);
    g_inside = -1;
}

// CRC-32（和 Python 的 zlib.crc32 相同），用来给对局算签名。
uint32_t crc32_of(const std::string& text) {
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        ready = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (const unsigned char ch : text) c = table[(c ^ ch) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

struct Usage {
    double first = 0, total = 0, slowest = 0;
    int turns = 0, over = 0, illegal = 0;
    void add(double ms, bool first_turn) {
        if (first_turn) {
            first = ms;
            over += ms > tank::BUDGET_FIRST_MS;
        } else {
            total += ms;
            turns += 1;
            if (ms > slowest) slowest = ms;
            over += ms > tank::BUDGET_TURN_MS;
        }
    }
};

std::string three(const tank::Orders& o) {
    return tank::cmd_text(o.cmd[0]) + " " + tank::cmd_text(o.cmd[1]) + " " + tank::cmd_text(o.cmd[2]);
}

void play(Lib* libs, long long id, unsigned long long seed, int style, int swap, bool log) {
    g_game = id;
    std::fprintf(g_out, "B %lld\n", id);
    std::fflush(g_out);
    reload(libs[0], 0);
    reload(libs[1], 1);

    tank::State s = tank::make_game(seed, style);
    const int bot_at[2] = {swap ? 1 : 0, swap ? 0 : 1};    // bot_at[座位] = 0（A）或 1（B）
    Usage usage[2];                                        // 按 bot（A、B）记
    int strikes[2] = {0, 0};                               // 按座位记
    bool dead[2] = {false, false};
    double used = 0;
    char err[400];
    std::string orders_log;

    for (int seat = 0; seat < 2; ++seat) {
        const int who = bot_at[seat];
        g_inside = who;
        g_calls++;
        if (libs[who].init(&s, seat, &used, err, int(sizeof err))) die((std::string("init 抛出异常：") + err).c_str());
        g_inside = -1;
    }
    while (!s.done) {
        tank::Orders applied[2];
        for (int seat = 0; seat < 2; ++seat) {
            const int who = bot_at[seat];
            tank::Orders raw;
            g_inside = who;
            g_calls++;
            if (libs[who].decide(&s, seat, &raw, &used, err, int(sizeof err)))
                die((std::string("decide 抛出异常：") + err).c_str());
            g_inside = -1;
            usage[who].add(used, s.turn == 0);    // 首回合的用量含 init
            // 和正式模式一样：写成一行文本，再按协议解析。
            std::string why;
            if (tank::parse_orders(tank::orders_text(s, raw), s, applied[seat], &why)) {
                strikes[seat] = 0;
            } else {
                applied[seat] = tank::Orders{};
                usage[who].illegal += 1;
                std::fprintf(g_out, "F %d %d %s\n", s.turn + 1, seat, why.c_str());
                if (++strikes[seat] >= MAX_STRIKES) dead[seat] = true;
            }
        }
        if (dead[0] || dead[1]) break;    // 判负的那一回合不结算
        const std::string line = three(applied[0]) + "|" + three(applied[1]) + "\n";
        orders_log += line;
        if (log) std::fprintf(g_out, "O %d %s", s.turn + 1, line.c_str());
        tank::step(s, applied[0], applied[1]);
    }

    int win = s.winner;
    std::string reason = tank::reason_text(s);
    std::string tail = tank::turn_text(s);
    if (dead[0] || dead[1]) {
        win = dead[0] && dead[1] ? -1 : (dead[0] ? 1 : 0);
        reason = "fault";
        tail += std::string("FAULT ") + (dead[0] ? "1" : "0") + (dead[1] ? "1" : "0") + "\n";
    }
    if (log) std::fprintf(g_out, "FINAL\n%s", tank::turn_text(s).c_str());
    std::fprintf(g_out, "G %lld %llu %d %d %d %d %s %08x%08x", id, seed, style, swap, int(s.turn), win, reason.c_str(),
                 crc32_of(orders_log), crc32_of(tail));
    for (const Usage& u : usage)
        std::fprintf(g_out, " %.3f:%.4f:%.3f:%d:%d", u.first, u.turns ? u.total / u.turns : 0.0, u.slowest, u.over, u.illegal);
    std::fprintf(g_out, "\n");
    std::fflush(g_out);
}

}  // namespace

int main(int argc, char** argv) {
    bool log = false;
    double hang_seconds = 20;
    const char* paths[2] = {nullptr, nullptr};
    int n_paths = 0;
    for (int k = 1; k < argc; ++k) {
        if (!std::strcmp(argv[k], "--log")) log = true;
        else if (!std::strcmp(argv[k], "--wall")) g_budget_kind = tank::BUDGET_WALL;
        else if (!std::strcmp(argv[k], "--hang") && k + 1 < argc) hang_seconds = std::atof(argv[++k]);
        else if (n_paths < 2) paths[n_paths++] = argv[k];
    }
    if (n_paths != 2) {
        std::fprintf(stderr, "用法：fast <A 的动态库> <B 的动态库> [--log] [--hang 秒数] [--wall]\n");
        return 2;
    }
    g_per_ms = double(cycles::measure().per_ms);

    // 结果单独留一个句柄，再把标准输出并到标准错误上：bot 往标准输出打的东西不会混进结果里。
    std::fflush(stdout);
#ifdef _WIN32
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    const int fd = _dup(1);
    _dup2(2, 1);
    g_out = _fdopen(fd, "wb");
    ULONG guarantee = 64 * 1024;
    SetThreadStackGuarantee(&guarantee);    // 栈溢出时给崩溃处理留一点栈
    SetUnhandledExceptionFilter(on_seh);
#else
    const int fd = dup(1);
    dup2(2, 1);
    g_out = fdopen(fd, "w");
#endif
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (!g_out) return 2;
    for (const int sig : {SIGABRT, SIGFPE, SIGSEGV, SIGILL}) std::signal(sig, on_signal);
    std::atexit(on_exit_inside);

    // 看门狗：同一次 bot 调用超过 hang_seconds 秒没有返回，就当作死循环，报出来并退出。
    std::thread([hang_seconds] {
        unsigned long long last = ~0ULL;
        auto since = std::chrono::steady_clock::now();
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            const unsigned long long now = g_calls.load();
            const auto t = std::chrono::steady_clock::now();
            if (now != last || g_inside.load() < 0) {
                last = now;
                since = t;
            } else if (std::chrono::duration<double>(t - since).count() > hang_seconds) {
                char what[96];
                std::snprintf(what, sizeof what, "一次调用超过 %.0f 秒没有返回（多半是死循环）", hang_seconds);
                die(what);
            }
        }
    }).detach();

    Lib libs[2];
    libs[0].path = paths[0];
    libs[1].path = paths[1];
    char line[256];
    while (std::fgets(line, sizeof line, stdin)) {
        long long id;
        unsigned long long seed;
        int style, swap;
        if (std::sscanf(line, "%lld %llu %d %d", &id, &seed, &style, &swap) != 4) continue;
        play(libs, id, seed, style, swap, log);
    }
    g_game = -1;
    std::fprintf(g_out, "DONE\n");
    std::fflush(g_out);
    std::_Exit(0);    // 不卸载动态库、不跑 bot 的全局析构，直接结束
}

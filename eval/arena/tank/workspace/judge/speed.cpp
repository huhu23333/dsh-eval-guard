// speed.cpp —— 测速：跑两段固定的工作量，报告用了多久。用来确认机器的状态（电源模式、主频、别的任务）有没有变。
//
// 用法：speed [--cpu 逻辑处理器编号] [--repeat 次数]
// 输出一行：
//   SPEED cpu=<编号或 -1> ghz=<实际主频> loop_ms=<空转循环的墙上毫秒> engine_ms=<引擎工作量的墙上毫秒>
//         engine_cpu_ms=<引擎工作量的 CPU 毫秒> per_ms=<每 CPU 毫秒多少个读数单位> source=<这个数怎么来的>
//
// 两段工作量：
//   空转循环：2 亿次一拍一次的加法。次数除以用时就是这个核此刻的实际主频（不受内存和缓存影响）。
//   引擎工作量：用固定的伪随机命令打 400 局不记录的对局（约 12 万次 step）。
// 各重复若干次取最快的一次（最快的一次最不受别的任务打扰）。

#include "../tank.hpp"
#include "cycles.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

double wall_ms() {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

double spin_loop() {
    unsigned long long x = 0;
    const double t0 = wall_ms();
    for (unsigned long long i = 0; i < 200000000ULL; ++i) {
        x += 1;
#if defined(__GNUC__)
        __asm__ volatile("" : "+r"(x));    // 不让编译器把循环算掉；每次迭代一条一拍的加法
#endif
    }
    const double t1 = wall_ms();
    return x ? t1 - t0 : 0;
}

unsigned long long g_sink = 0;

void engine_work() {
    unsigned long long r = 88172645463325252ULL;
    for (int game = 0; game < 400; ++game) {
        tank::State s = tank::make_game(1000 + game);
        while (!s.done) {
            tank::Orders o[2];
            for (int seat = 0; seat < 2; ++seat)
                for (int type = 0; type < 3; ++type) {
                    r ^= r << 13;
                    r ^= r >> 7;
                    r ^= r << 17;
                    tank::Cmd list[tank::N_CMDS];
                    const int n = tank::legal_commands(s, tank::tank_id(seat, type), list);
                    o[seat].cmd[type] = list[(r >> 24) % unsigned(n)];
                }
            tank::step(s, o[0], o[1]);
        }
        g_sink += s.wall_hash + unsigned(s.turn);
    }
}

}  // namespace

int main(int argc, char** argv) {
    int cpu = -1, repeat = 5;
    for (int k = 1; k + 1 < argc; ++k) {
        if (!std::strcmp(argv[k], "--cpu")) cpu = std::atoi(argv[++k]);
        else if (!std::strcmp(argv[k], "--repeat")) repeat = std::atoi(argv[++k]);
    }
#ifdef _WIN32
    if (cpu >= 0 && cpu < 64 && !SetThreadAffinityMask(GetCurrentThread(), DWORD_PTR(1) << cpu)) {
        std::fprintf(stderr, "绑不到逻辑处理器 %d\n", cpu);
        return 2;
    }
#else
    if (cpu >= 0) std::fprintf(stderr, "这个系统上没有实现绑定处理器，忽略 --cpu\n");
#endif
    const cycles::Scale scale = cycles::measure();
    tank::budget_setup(tank::BUDGET_CPU, double(scale.per_ms));
    double best_loop = 1e18, best_wall = 1e18, best_cpu = 1e18;
    for (int k = 0; k < (repeat < 1 ? 1 : repeat); ++k) {
        const double loop = spin_loop();
        if (loop < best_loop) best_loop = loop;
        tank::budget_begin(0);
        const double t0 = wall_ms();
        engine_work();
        const double wall = wall_ms() - t0, used = tank::budget_used();
        if (wall < best_wall) best_wall = wall;
        if (used < best_cpu) best_cpu = used;
    }
    std::printf("SPEED cpu=%d ghz=%.3f loop_ms=%.2f engine_ms=%.2f engine_cpu_ms=%.2f per_ms=%lld source=%s sink=%llu\n", cpu,
                200000000.0 / best_loop / 1e6, best_loop, best_wall, best_cpu, scale.per_ms, scale.source, g_sink % 1000);
    return 0;
}

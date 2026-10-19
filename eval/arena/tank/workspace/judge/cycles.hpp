// cycles.hpp —— 裁判这一侧测定"1 CPU 毫秒是多少个读数单位"。
//
// Windows 上 CPU 用量的读数是周期数（TSC 的计数），每台机器每毫秒多少周期不一样，
// 所以裁判测定一次，在开局时告诉 bot，两边用同一个数。测定的办法，按先后：
//   1. 环境变量 TANK_CYCLES_PER_MS（正式赛想钉死一个数时用）；
//   2. CPUID 的 0x15 号功能给出的 TSC 标称频率（较新的 Intel 处理器有，是精确值，各进程读到的完全相同）；
//   3. 用 rdtsc 对着系统的高精度时钟数 250 毫秒，结果四舍五入到 0.1 MHz（各进程测出来的基本落在同一个数上）。
// 其他系统上 CPU 用量的读数直接是纳秒，固定为 1000000。
#pragma once

#include <chrono>
#include <cstdlib>

#if defined(_WIN32) && (defined(__x86_64__) || defined(__i386__))
#include <cpuid.h>
#include <x86intrin.h>
#endif

namespace cycles {

struct Scale {
    long long per_ms = 1000000;    // 每 CPU 毫秒多少个读数单位
    const char* source = "ns";     // 怎么得到的：env / cpuid / measured / ns
};

inline Scale measure() {
    Scale out;
#if defined(_WIN32)
    if (const char* text = std::getenv("TANK_CYCLES_PER_MS")) {
        const long long v = std::atoll(text);
        if (v > 0) return Scale{v, "env"};
    }
#if defined(__x86_64__) || defined(__i386__)
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (__get_cpuid_max(0, nullptr) >= 0x15 && __get_cpuid(0x15, &a, &b, &c, &d) && a && b && c) {
        const unsigned long long hz = (unsigned long long)c * b / a;
        if (hz > 100000000ULL) return Scale{(long long)(hz / 1000), "cpuid"};
    }
    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();
    const unsigned long long c0 = __rdtsc();
    auto t1 = t0;
    while (std::chrono::duration<double>(t1 - t0).count() < 0.25) t1 = clock::now();
    const unsigned long long c1 = __rdtsc();
    const double per_ms = double(c1 - c0) / std::chrono::duration<double, std::milli>(t1 - t0).count();
    out.per_ms = (long long)(per_ms / 100.0 + 0.5) * 100;    // 0.1 MHz = 每毫秒 100 个周期
    out.source = "measured";
#endif
#endif
    return out;
}

}  // namespace cycles

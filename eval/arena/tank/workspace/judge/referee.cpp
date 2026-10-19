// referee.cpp —— 裁判核心：一个常驻的小程序，替 Python 裁判（engine.py）执行 tank.hpp 里的规则。
//
// Python 那边不含任何规则：开局、检查命令是否合法、结算、列出事件，全部在这里通过 tank.hpp 完成。
// 标准输入一行一条请求，标准输出给回复：
//
//   HELLO                 → OK <引擎版本> <首回合预算> <每回合预算> <首回合墙上上限> <每回合墙上上限>
//                              <每 CPU 毫秒多少个读数单位> <这个数怎么来的> <风格名 × N>
//   NEW <种子> <风格>      → OK，然后是完整局面文本（以 END 行结束）
//   LOAD                  → 后面跟一份完整局面文本（到 END 行）；回 OK 或 ERR <原因>
//   STATE                 → 完整局面文本
//   CHECK <座位> <一行回复> → OK <快速> <火力> <装甲>（并记下，等 STEP 时执行）或 ERR <原因>
//   STEP                  → 用记下的命令结算一回合（没记的座位三辆都不动），回：OK，回合段文本（到 END 行）
//   STEPX                 → 同上，但回：OK，若干行 "EV <事件>"，回合段文本，6 行 "X ..."，最后一行 DONE
//   INFO                  → 6 行 "X ..."，最后一行 DONE
//
//   X <坦克> <还要几回合复活> <还要装填几回合> <场上子弹数> <子弹上限> <下回合能否开炮>
//
// 请求格式不对回一行 ERR，不退出。

#include "../tank.hpp"
#include "cycles.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

void put(const std::string& text) { std::fwrite(text.data(), 1, text.size(), stdout); }

void put_info(const tank::State& s) {
    for (int i = 0; i < 6; ++i)
        std::printf("X %d %d %d %d %d %d\n", i, tank::respawn_in(s, i), tank::reload_left(s, i), tank::bullets_out(s, i),
                    tank::bullet_cap(s, i), tank::can_fire(s, i) ? 1 : 0);
    std::printf("DONE\n");
}

bool read_line(std::string& line) {
    line.clear();
    char buf[1024];
    while (std::fgets(buf, sizeof buf, stdin)) {
        line += buf;
        if (!line.empty() && line.back() == '\n') {
            while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
            return true;
        }
    }
    return !line.empty();
}

}  // namespace

int main() {
    tank::State s = tank::make_game(1);
    tank::Orders pending[2];
    tank::EventLog log;
    std::string line;
    while (read_line(line)) {
        if (line == "HELLO") {
            const cycles::Scale scale = cycles::measure();
            std::printf("OK %s %d %d %d %d %lld %s", tank::ENGINE_VERSION, tank::BUDGET_FIRST_MS, tank::BUDGET_TURN_MS,
                        tank::BUDGET_WALL_FIRST_MS, tank::BUDGET_WALL_TURN_MS, scale.per_ms, scale.source);
            for (const char* name : tank::STYLE_NAMES) std::printf(" %s", name);
            std::printf("\n");
        } else if (line.rfind("NEW ", 0) == 0) {
            unsigned long long seed = 0;
            int style = -1;
            if (std::sscanf(line.c_str() + 4, "%llu %d", &seed, &style) < 1) {
                std::printf("ERR NEW 后面应为 种子 和 风格\n");
            } else {
                s = tank::make_game(seed, style);
                pending[0] = pending[1] = tank::Orders{};
                std::printf("OK\n");
                put(tank::to_text(s));
            }
        } else if (line == "LOAD") {
            std::string text, row;
            while (read_line(row)) {
                text += row;
                text += '\n';
                if (row == "END") break;
            }
            std::string err;
            tank::State loaded;
            if (tank::from_text(text, loaded, &err)) {
                s = loaded;
                pending[0] = pending[1] = tank::Orders{};
                std::printf("OK\n");
            } else {
                std::printf("ERR %s\n", err.c_str());
            }
        } else if (line == "STATE") {
            put(tank::to_text(s));
        } else if (line.rfind("CHECK ", 0) == 0) {
            const char seat = line.size() > 6 ? line[6] : '?';
            std::string err;
            tank::Orders o;
            if ((seat != '0' && seat != '1') || line.size() < 8 || line[7] != ' ') {
                std::printf("ERR CHECK 后面应为 座位 和 一行回复\n");
            } else if (tank::parse_orders(std::string_view(line).substr(8), s, o, &err)) {
                pending[seat - '0'] = o;
                std::printf("OK %s %s %s\n", tank::cmd_text(o.cmd[0]).c_str(), tank::cmd_text(o.cmd[1]).c_str(),
                            tank::cmd_text(o.cmd[2]).c_str());
            } else {
                std::printf("ERR %s\n", err.c_str());
            }
        } else if (line == "STEP" || line == "STEPX") {
            const bool verbose = line == "STEPX";
            tank::step(s, pending[0], pending[1], verbose ? &log : nullptr);
            pending[0] = pending[1] = tank::Orders{};
            std::printf("OK\n");
            if (verbose)
                for (const tank::Event& e : log.list) std::printf("EV %s\n", tank::event_text(e).c_str());
            put(tank::turn_text(s));
            if (verbose) put_info(s);
        } else if (line == "INFO") {
            put_info(s);
        } else {
            std::printf("ERR 不认识的请求\n");
        }
        std::fflush(stdout);
    }
    return 0;
}

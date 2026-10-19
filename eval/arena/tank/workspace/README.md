# 坦克大战

写一个 bot：`submission/bot.cpp`。1 对 1，每方三辆不同车型的坦克守一个基地。完整规则见 `RULES.md`（开头有一屏速览），规则的精确实现是 `tank.hpp`（裁判用的就是这一份，两者不一致时以 `tank.hpp` 为准）。

比赛分上下半场，解压时开始计时。时间规则和考试一样：每个半场 30 分钟，最早交卷时间是第 20 分钟，最晚到 45 分钟强制收卷。最终只看 bot 的强度，提前交卷没有任何好处。上半场结束后所有选手的 bot 互相打一轮，然后你会收到中场包（见"中场"一节），再用下半场继续改进。

**交什么**：只交 `submission/bot.cpp` 这一个文件，每个半场结束时它的内容就是你那个半场的版本；另外用几句话说明你的策略。

## 怎么写 bot

`bot.cpp` 只需要定义两个函数，两个都必须有：

```cpp
#include "tank.hpp"

// 每局开始调用一次。s 是开局局面，me 是你的座位（0 在下方，1 在上方）。
void init(const tank::State& s, int me) {}

// 每回合调用一次，返回三辆车（快速、火力、装甲）这一回合的命令。
tank::Orders decide(const tank::State& s, int me) {
    tank::Orders orders;                                  // 默认三辆都不动
    for (int type = 0; type < 3; ++type) {
        const int i = tank::tank_id(me, type);            // 这辆车的编号
        tank::Cmd list[tank::N_CMDS];
        const int n = tank::legal_commands(s, i, list);   // 这辆车现在有意义的命令，第一条总是"不动"
        orders.cmd[type] = list[n - 1];
    }
    return orders;
}
```

- 不用写任何读输入、写输出的代码。局面直接以 `tank::State` 传进来；想往前推演，把它拷一份再调 `tank::step`。
- 全局变量、静态变量可以跨回合保留。每一局开始时它们都是初始值。
- 调试信息写到标准错误（`fprintf(stderr, ...)` 或 `std::cerr`）。`play` 会显示最后几行，全部内容按回合存在回放里。写到标准输出的内容也会并进调试输出，不会干扰对局。
- 除了 `tank.hpp` 和标准库不能依赖别的文件。想改引擎的行为，就把需要的代码抄进 `bot.cpp` 里改。可以用 `<windows.h>`，放在 `tank.hpp` 前面或后面都行。
- 返回的命令不合法（比如给非快速型下了两步）时，这一回合三辆车都不动，并记一次失败。

### 思考预算

- 首回合（`init` 加第一次 `decide`，含程序启动）2000 CPU 毫秒，之后每回合 50 CPU 毫秒。"CPU 毫秒"是**你的程序自己消耗的 CPU 时间**，被挂起、等输入的时间都不算。
- `tank::budget_left()` 返回本回合还剩多少 CPU 毫秒（浮点数），用它决定什么时候收手：`while (tank::budget_left() > 3) { /* 再搜一层 */ }`。裁判从外面量你的整个进程，比它看到的多零点几毫秒，留 2–3 毫秒余量就够。
- 超出预算的回合三辆车都不动，记一次失败；**连续 3 次失败判负**（超预算和不合法的命令合在一起数）。一回合用掉 150 CPU 毫秒以上就是连续三次。程序崩溃或退出直接判负。
- 可以开线程，但预算按所有线程合计，`budget_left()` 只看得到调用它的那个线程。
- 另有墙上时钟的保险上限：一回合 2 秒（首回合 4 秒）没有回复，记一次失败。

## 编译和对战

不用自己编译：下面的命令遇到 `.cpp` 会自动编译（结果缓存在 `build/`，源码没变不重编，编译出错会显示编译器的报错）。在本目录运行；这是 PowerShell 和命令提示符的写法，在 bash 里把 `.\run.cmd` 换成 `sh run.sh`。

```text
.\run.cmd clock.py
.\run.cmd arena.py check
.\run.cmd arena.py play submission/bot.cpp submission/bot.cpp --seed 7 --out first
.\run.cmd arena.py show replays/first.json 20 --around 2
.\run.cmd arena.py fast submission/bot.cpp submission/bot.cpp --games 200
.\run.cmd arena.py batch submission/bot.cpp submission/bot.cpp --games 20
.\run.cmd arena.py vs submission/bot.cpp submission/bot.cpp --games 200
.\run.cmd arena.py expand replays/first.json
.\run.cmd arena.py html replays/first.json
```

参与对战的可以是 `.cpp` 源码，也可以是编译好的程序；同一个程序可以两边都上。

| 命令 | 做什么 |
|---|---|
| `clock.py` | 看时间：显示形如"上半场 已用 12.3 分钟。最早交卷时间是第 20 分钟（还有 7.7 分钟），30 分钟是本半场的时间，45 分钟强制收卷"；中场包解压后改为显示下半场 |
| `check` | 检查环境：Python、编译器、`submission/bot.cpp` 能否编译并打完一局 |
| `play A B` | 正式模式打一局，A 坐 0 号位、B 坐 1 号位。打印结果、每方的预算用量、故障、调试输出的最后几行；回放存到 `replays/名字.json`，同时生成网页版 `replays/名字.html`。`--seed` 选地图，`--style` 选风格（`auto` 按种子轮换，或 `open`、`brick`、`water`、`steel`） |
| `show 回放 回合` | 在终端里看这一回合结算后的局面：字符画棋盘、每辆车的状态、子弹、命令、事件、调试输出。`--around 2` 连前后各两回合一起看 |
| `fast A B` | 快速模式批量。只支持源码 |
| `batch A B` | 正式模式批量 |
| `vs 新版 旧版` | 比较两个版本。两边都是源码时用快速模式，否则用正式模式；`--mode fast` / `--mode formal` 可以指定 |
| `expand 回放` | 把回放展开成逐回合的完整局面和事件，存成 `回放名.frames.json`，方便自己写脚本分析 |
| `html 回放` | 把已有的回放（比如中场包里的）做成网页，存成 `回放名.html` |

批量命令（`fast`、`batch`、`vs`）每张图换边各打一局：`--games 200` 就是 100 张图，种子从 `--seed`（默认 1）起连续取，`--workers` 指定并行数。**输出的最后一行是一行汇总**（谁对谁、胜-平-负、得分率 ± 误差范围、局数、有无故障），只想要结论就取最后一行。想重看某一局，用 `play` 加同一个种子、按同样的座次再打一遍。

### 正式模式和快速模式

| | 正式模式（`play`、`batch`） | 快速模式（`fast`） |
|---|---|---|
| 怎么跑 | 和正式比赛完全一样：每个 bot 是一个独立的程序 | 两个 bot 装进同一个程序里直接互相调用 |
| 速度 | 简单的 bot 每秒十几局（每个并行位） | 简单的 bot 每秒几百局（每个并行位） |
| 思考预算 | 超了判罚 | 只统计、不判罚 |
| 出错 | 崩溃、超预算、不合法的命令照常判罚并记录 | 不合法的命令照常判罚；任何一方崩溃、卡死、抛异常，整批停下，并报告是谁、哪一局 |
| 调试输出 | 存进回放 | 不显示 |
| 参与者 | 源码或编译好的程序 | 只能是源码 |

### 结果能不能复现

对局里没有随机，所以只看你的 bot 怎么决定停止搜索：

- **不看时间的 bot**（固定深度、固定节点数，或者根本不搜索）：结果完全确定。同一张图、同样的两个程序、同样的座次，快速模式每次打出的都是同一局，并且和正式模式逐回合相同（只要正式模式下没有超预算）；并行多少、机器忙不忙都不影响。这种 bot 放心用快速模式做大批量对比。
- **按 `budget_left()` 停的 bot**：同样的 CPU 时间里能搜多少取决于机器当时的状态，所以同一个版本在单局和批量之间、快速和正式之间、并行少和并行多之间，都可能打出不同的对局。并行越多差得越多。两次对比的差别里会混进这种波动。

**推荐做法：给搜索留一个"固定搜索量"的开关，对比实验时打开，提交时按预算停。** 例如：

```cpp
#define FIXED_NODES 0     // 对比实验：改成固定的节点数（比如 20000）；提交：保持 0，按预算停
static bool keep_searching(long nodes) {
    return FIXED_NODES ? nodes < FIXED_NODES : tank::budget_left() > 3;
}
```

把要比的两个版本各存成一个 `.cpp`，开关设成同一个固定值，用 `vs`（快速模式）比，结果可以重复。定下版本后把开关改回 0 放进 `submission/bot.cpp`，再用正式模式（`play`，或并行数不大的 `batch`）确认没有超预算、没有崩溃；是否接近 50 CPU 毫秒的时限，以单局 `play` 的用量为准。

### `vs` 怎么读

输出形如下面这样。**这里的数字是为了说明格式编的，不是任何真实对局的结果**：

```text
tank-cpp-0.1  快速模式  new 对 old  200 局（100 张图，种子 1–100，每张图换边各打一局）  并行 8  用时 1.2 秒
  new：胜 96  平 20  负 84   得分率 53.0% ± 4.1%（95% 区间，按图计）
  new 坐 0 号位：胜 50 平 9 负 41    坐 1 号位：胜 46 平 11 负 43
  100 张图里有 37 张换边后的两局逐回合完全相同（两个程序在这些图上的表现没有任何差别），能看出差别的有 63 张
  ……（结束方式、双方的用量）
汇总：new 对 old  96-20-84（胜-平-负）  得分率 53.0% ± 4.1%  200 局  故障：无
```

- 得分率：胜算 1、平算 0.5，站在第一个程序（新版）这边。误差范围是 95% 区间，按图计算（一张图上换边的两局算一个样本）。得分率减去误差范围还高于 50%，才能说新版更强；否则加局数再看。
- "完全相同"的图不提供信息：改动很小的两个版本会在很多图上做出完全一样的动作，这样的图上换边的两局是同一局。能看出差别的图很少时，得分率接近 50% 不代表两个版本一样强，换一批种子（`--seed 1001`）或加局数。
- 两个座位的胜负分开列出：如果新版只在一个座位上赢得多，多半是地图或车型排布的缘故，不是版本的差别。

## 可以做和不可以做的

- **本地可以并行跑任意多的对战**：`--workers` 开多大、同时开几个 `arena.py` 命令都可以（并行多了每局变慢，按预算停的 bot 搜得更浅）。
- **中场包里的对手程序可以当作练习对手，也可以当作自动调参的对象**：批量对战、写脚本循环调参数都可以。
- **不得反编译对手的程序。** 它们只用来对战。

## 看回放、找回旧版本

- `play` 生成的 `replays/名字.html`（或 `html` 命令生成的网页）用浏览器打开就能看，不需要联网：带动画的棋盘，两侧是每辆车的状态和命令、事件、思考用量、故障和你的调试输出。空格播放 / 暂停，左右方向键逐回合，`[` `]` 跳到上一个 / 下一个关键事件，按 `H` 看全部快捷键；文件名后面加 `?turn=37` 直接打开到第 37 回合。也可以打开 `viewer.html`，把 `expand` 生成的 `*.frames.json` 拖进去。
- 想用程序分析对局，读 `expand` 生成的 JSON；想知道某一回合的精确数值，用 `show`。
- 版本存档：每次运行 `check`、`play`、`batch`、`fast`、`vs`，参与的每份源码都会按内容存一份到 `history/`（文件名带时间和内容哈希，同样的内容只存一次），并在 `history/log.jsonl` 里追加一行：时间、命令、参与的版本、结果摘要。想找回之前的版本、回头看谁对谁打成什么样，去那里找。请不要删除 `history/`。

## 中场

所有选手的上半场版本互相打一轮之后，你会收到中场包，解压后在 `workspace/halftime/` 里：

- 上半场的成绩。
- 你自己的对局回放（和本地 `play` 存的是同一种格式，可以直接用 `show` 和 `expand`）。
- 其他选手上半场的程序：`halftime/opponents/` 下的可执行文件，没有源码，所以只能用正式模式（`play`、`batch`、`vs --mode formal`）。例如（把文件名换成实际的）：

  ```text
  .\run.cmd arena.py play submission/bot.cpp halftime/opponents/p01.exe --seed 3
  .\run.cmd arena.py batch submission/bot.cpp halftime/opponents/p01.exe --games 20
  ```

中场包解压的那一刻下半场开始计时。

## 文件

| 文件 | 用途 |
|---|---|
| `RULES.md` | 规则 |
| `tank.hpp` | 规则引擎（单个头文件）。你的 bot 直接 `#include` 它：局面结构、精确结算一回合的 `step`、合法命令、地图生成、思考预算查询都在里面 |
| `submission/bot.cpp` | 你的 bot。现在是一个能编译、会合法行动的模板 |
| `baseline.exe` | 基准：一个中等强度的 bot，只有编译好的程序，只能用正式模式和它对战，例如 `arena.py batch submission/bot.cpp baseline.exe` |
| `tank_main.cpp` | 主循环：读局面、调你的函数、写命令、计时。不用改，正式比赛用的是同一份 |
| `arena.py`、`viewer.html` | 本地对战与复盘、网页版回放查看器 |
| `engine.py`、`core.py`、`judge/` | 裁判，与正式比赛相同 |
| `clock.py`、`run.cmd`、`run.sh` | 时钟；找到 Python 并运行脚本的启动器 |
| `build/`、`replays/`、`history/` | 运行后自动生成：编译缓存、回放、你跑过的每个版本的存档 |

正式比赛的编译方式（一般用不到，`arena.py` 会自动编译）：

```text
g++ -std=c++20 -O2 -static -I. submission/bot.cpp tank_main.cpp -o bot.exe
```

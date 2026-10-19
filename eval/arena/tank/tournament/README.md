# 复现正式赛

| 文件 | 内容 |
|---|---|
| `tournament.py`、`bt.py`、`schedule.py` | 这一集的对战平台：冻结赛程、编译、绑核、逐批测速、可中断续跑、Bradley–Terry 评分。和比赛时逐字节相同。它是为 1 对 1 重写的版本，和仓库根目录 `arena/`（第 3 集起的多人平台）不是同一份 |
| `halftime.json`、`final.json`、`progress.json` | 中场赛、终场赛、进步赛的配置。程序路径换成了仓库里的相对路径，`seed` 写的是当时抽到的赛程种子，去掉了本机编译器的路径（不写时先看环境变量 `CXX`，再找 `PATH` 里的 g++；只把 w64devkit 放进仓库 `tools/` 的，先设好 `CXX`，见 `../README.md` 的"马上看 / 马上玩"）。名字（`Astra`、`Opus@1` 等）没有改，赛程按名字排序，改了就不是同一份赛程 |
| `sparring.cpp` | 出题方参照：出题方写的搜索型 bot，成绩表里的 `sparring`。这是比赛时实际编译的文件（源码前面由赛事脚本加了 4 个宏，固定每回合的搜索量，所以它的结果不随机器状态变化）。比赛时不发给选手 |

在本目录运行（终场赛在比赛电脑上约 70 分钟；可以随时中断，再运行同一条命令接着跑）：

```bash
python tournament.py plan final.json        # 冻结赛程，写到 run-final/赛程.json
python tournament.py run  run-final         # 开打
python tournament.py rate run-final         # 出成绩：run-final/成绩.md
```

中场赛、进步赛把 `final` 换成 `halftime`、`progress`。进步赛的进步幅度要和中场赛合并拟合，比赛时由主办方的赛事脚本完成，算法见 `../results/附表.md` 的说明。

需要注意：

- **选手包目录必须是纯英文路径**：`tournament.py` 会拒绝含中文等非 ASCII 字符的 `kit` 路径（Windows 上的 g++ 在某些区域设置下打不开这种路径）。仓库放在中文路径下时，把整个 `tank/` 复制到纯英文路径再跑。
- 地图、座位、对阵可以一模一样地重建：开源前用这三份配置重建赛程，`games_sha` 和原赛程完全相同。
- 逐局结果不一定完全相同。对局本身没有随机，但很多 bot 按 `budget_left()` 决定搜到哪里停，机器状态不同搜得深浅就不同。开源前在比赛电脑上重打终场赛的前 352 局，338 局逐回合相同，347 局胜负相同。总排名应当一致，相邻并列的名次可能互换。
- 默认只用大核、每局两个 bot 各占一个物理核（`pin: big`），同时打几局由本机的核数决定。`python tournament.py cpus final.json` 可以先看绑核方案。用量的上限是 50 CPU 毫秒，机器太忙会让按时间停的 bot 搜得更浅。

# dsh-eval-guard · 评测守护

在 DSH 中启用后，约束**所有用户主对话**：

- 只能读写会话工作区；工作区外（包括 /tmp）不可访问。
- 禁止联网、子代理、Agent Team/跨 agent 通信、读取其它工作区。
- 工具黑名单可配置；未知工具默认放行，但 DSH 内置与 dsh-ops 的工具会做参数路径审计。

## 时间限制

- 每条用户消息发送时，用**当时的** timeLimitEnabled 和 timeLimitMinutes 为本会话创建或清除 deadline。
- 开启限时时：deadline = 当前时刻 + 当时的分钟数；之后修改分钟数、选择任务、关闭或重新打开限时，都**不会**改动已存在的 deadline。
- 关闭限时后再次给已有 deadline 的会话发送消息，会清空该会话自己的 deadline；其它会话不受影响。
- 重新打开限时后，已有会话不会自动恢复 deadline；下一次用户消息才会重新计时。
- 每个会话独立计时；可以同时运行多个不同时长、有的开启限时有的不开启的会话。
- deadline 到点只终止对应会话及其子代理/后台任务，不影响其它会话。
- 关闭「评测守护」总开关会清空全部 deadline 并停止约束；总开关与单轮限时开关是两套状态。

设置页选择任务时会自动把「最长时长 + 限时开关」写成该任务默认值并立即显示，用户可以随后手动修改：

| 任务类型 | 默认限时 |
|---|---|
| 原 5 个 meow-ai-arena 任务 | 90 分钟，默认打开 |
| Commit0 单库 | 480 分钟，默认关闭 |
| SWE-Bench Pro 单任务 | 1440 分钟，默认关闭 |
| FeatureBench 单 feature | 1440 分钟，默认关闭 |
| Neural-CPP 纯 C++ 张量后端 | 1440 分钟，默认关闭 |

## 任务与安装

设置页可安装九个任务包：

- 原 5 个比赛任务：坦克大战、仓储调度、圈地、炸弹人、夺旗。
- 3 个长时单任务：Commit0 单库实现、SWE-Bench Pro 单任务、FeatureBench 单 feature。
- 1 个纯 C++ 项目：Neural-CPP 张量运算与反向传播后端。

Neural-CPP 安装时自动把 workspace.zip 解压到工作区根目录，并在完成后移除 setup.sh；agent 从根目录的 CMakeLists.txt 开始。工作区只包含 TASK.md、CMakeLists.txt 和 include/nn/nn.hpp；实现在 src/ 中从零完成，纯 C++17，无 Python/OpenCL/第三方库。

评测器同时兼容旧安装留下的嵌套布局：若根目录没有 CMakeLists.txt，会自动改用 workspace/CMakeLists.txt，避免已经产出的工作区无法评测。

评测时在临时目录独立构建，然后从 assets/neural-cpp/hidden/ 注入 C++ 测试源码；测试数据由固定种子在进程内生成，不依赖或复制外部数据文件。

结果返回 coreAccuracy（核心 C++ 测试通过率）、matrixAccuracy、逐类型 metrics（f32/int128/MXFP4 的矩阵与反向准确率、平均绝对误差、fwd/bwd 毫秒、fwd/bwd GFLOP/s）、speedGflops，以及按矩阵吞吐计算的 speedGeomeanGflops 和 matrixSizes（f32/i128/MXFP4 均为 256×256×256）。确定性项目使用 4 组独立输入取平均；MXFP4 RNE 和随机舍入分开统计，随机舍入使用 2 组输入×每组 1024 次采样，逐元素比较参考均值/标准差和候选均值/标准差，按 3σ 和 0.1–10× 标准差规则统计正确率。参考值由测试侧独立标量实现先算完，再调用候选实现，参考计算不计入候选耗时。隐藏测试不在工作区，agent 无法读取。开发要求线程数最多 4，评测阶段使用全部逻辑 CPU。

原 5 个任务直接放入打包好的 workspace.zip。后 3 个任务在安装时由 host 侧 setup 脚本按固定 commit clone 仓库、创建 .venv 并安装依赖；安装完成后会清理 `.git`、`setup.sh`、`remove.patch`、`spec.pdf.bz2` 等安装产物，工作区只保留任务本身素材。Commit0 的 `pytest.ini` 会去掉 coverage 参数，保证默认测试命令在沙箱内可运行。agent 运行时仍然禁网，使用工作区内的 .venv 跑测试；agent 可以自行 `git init` 或写自己的脚本。

安装是后台任务：

- 可分别选择 clone 远端、pip 镜像、Git 代理和 pip 代理。
- 进度和日志持久化在 $DSH_HOME/eval-guard/jobs/<job-id>/。
- 离开设置页不会停止；重新进入继续显示；可以取消。
- 安装成功后显示提示词，可点「填入新会话」。

### 已验证的 Git 远端

只有验证过包含任务固定 commit 的远端才会显示在对应任务下：

- Commit0 · tinydb：GitHub。
- SWE-Bench Pro · ansible：GitHub、Gitee。
- FeatureBench · seaborn：GitHub、Gitee、GitCode。

Gitee/GitCode 都使用 git fetch --depth 1 origin <任务 commit> 验证。GitLab 和 ghfast 未通过验证，已移除。另保留「自定义 URL」。

### pip 镜像

提供 PyPI、清华 TUNA、阿里云、腾讯云、北外、上海交大，并保留自定义地址。

### 两个独立代理

- Git 代理：只用于 git clone/fetch。
- pip 代理：只用于 pip install。

地址可以写成 127.0.0.1:7897，会自动补 http:// 前缀。留空会清掉继承来的同名代理变量，不误用宿主机代理。

## 单工作区评测

设置页安装/评测共用一组任务和工作区选择；「评测当前工作区」只评测一个结果，不生成排行。

插件 eval/ 目录内置全部 9 个任务的单结果评测脚本：

- Commit0：单元测试通过率。
- SWE-Bench Pro / FeatureBench：隐藏 F2P/P2P 测试；全部通过才 resolved，同时返回 passRate。
- Neural-CPP：独立编译隐藏 C++ 测试，返回 accuracy 与 matmul speedGflops；数据在测试进程内按固定种子生成。
- Tank / Warehouse / Land / Bomberman / CTF：运行官方引擎/裁判，只返回当前工作区候选者结果，不生成排名。

评测也是后台任务，日志和结果实时显示，持久化在 jobs 目录。

## 任务来源

原 5 个任务来自 **meow-ai-arena**（MIT License，commit 6bd738b，2026-10-16）：

- 第 1 集 · 圈地：<https://github.com/chen-006/meow-ai-arena/tree/main/land>，素材 land/workspace/
- 第 2 集 · 炸弹人：<https://github.com/chen-006/meow-ai-arena/tree/main/bomberman>，素材 bomberman/workspace/
- 第 3 集 · 夺旗：<https://github.com/chen-006/meow-ai-arena/tree/main/ctf>，素材 ctf/workspace/
- 第 5 集 · 仓储调度：<https://github.com/chen-006/meow-ai-arena/tree/main/warehouse>，素材 warehouse/workspace/ 第 1 轮主程包，改编为单人单次 90 分钟
- 第 6 集 · 坦克大战：<https://github.com/chen-006/meow-ai-arena/tree/main/tank>，素材 tank/workspace/

仓库：<https://github.com/chen-006/meow-ai-arena>

长时单任务来源：

- Commit0 单库实现：<https://arxiv.org/abs/2412.01769>，数据集 <https://huggingface.co/datasets/wentingzhao/commit0_combined>，实例 commit-0/tinydb
- SWE-Bench Pro 单任务：<https://arxiv.org/abs/2509.16941>，数据集 <https://huggingface.co/datasets/ScaleAI/SWE-bench_Pro>，实例 ansible vars
- FeatureBench 单 feature：<https://arxiv.org/abs/2602.10975>，数据集 <https://huggingface.co/datasets/LiberCoders/FeatureBench>，实例 seaborn regression

### 远端验证记录

- msiemens/tinydb @ ed761a72：GitHub 通过；Gitee mirrors/tinydb、GitCode gh_mirrors/ti/tinydb、GitLab 均无法 fetch 该 commit，未加入。
- ansible/ansible @ f7234968：GitHub、Gitee mirrors/ansible 通过；GitCode、GitLab 未通过，未加入。
- mwaskom/seaborn @ 7001ebe7：GitHub、Gitee mirrors/seaborn、GitCode gh_mirrors/se/seaborn 通过；GitLab 未通过，未加入。

## 打包适配

本插件打包的 Linux 版 workspace.zip 在原素材基础上做了适配：把 baseline.cpp 编译为 Linux ELF 可执行文件并移出源码，文件名改为 baseline（不带 .exe）；把 clock.py 改为单次 90 分钟版本；README.md 中对应的 baseline 示例同步改名。规则、引擎、本地裁判、模板等其余文件保持原样。安装任务时只把 workspace.zip 放入工作区，不再散装解压。

仓储调度任务包同样只放 workspace.zip；包内 TASK.md 已改写成单人单次 90 分钟版本，保留了原 baseline/、src/、workloads/ 和 tools/。

## 构建

~~~bash
node_modules/.bin/tsdown
node scripts/build-native.mjs
~~~

## 安装到当前 DSH profile

~~~bash
scripts/install.sh web
# 如果脚本找不到 dsh 命令：
DSH_CHECKOUT=/path/to/deepseek-harness scripts/install.sh web
# 然后重启 dsh web
~~~

## 依赖

- Linux + unprivileged user namespaces
- bubblewrap (bwrap)
- gcc（构建 native supervisor 时）
- unzip（安装任务素材时）
- Python 3（长任务 setup 与评测）

## 已知边界

- 只支持 Linux。
- seccomp 用户通知的路径检查与 open 之间存在极小的 TOCTOU 窗口；bwrap 只读/无网络挂载是第二层内核边界。
- 进程审计只覆盖被拦截的文件/网络 syscall；正常 syscall 不记录。
- 设置页使用内联样式，不依赖额外 CSS 构建链。

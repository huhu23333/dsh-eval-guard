# dsh-eval-guard · 评测守护

在 DSH 中启用后，约束**所有用户主对话**：

- 文件类工具（read/write/edit/grep/glob/ops_*/drawio_* 等）做参数路径审计：只允许修改会话工作区；工作区之外允许读取“用户不可写但可读”的文件（系统文件、只读资料），用户可写的数据目录仍不可读写。进程沙箱内 `/tmp` 映射到工作区 `.tmp`；文件工具不会重写路径，读写 `/tmp` 请直接使用工作区内的 `.tmp` 路径。
- 进程类工具（bash / run_code / 后台任务）由 bwrap + seccomp supervisor 二次约束：只读系统视图、工作区可写、私有 /tmp、无网络；进入沙箱前清空继承环境，只回填 PATH/HOME/TMPDIR/XDG/LANG 等非敏感变量，避免宿主 API key / 代理泄漏。文件越界访问会记录并终止进程组；网络 `connect/bind/sendto` 对 AF_INET/AF_INET6 返回 EPERM 并记录，但不杀进程组（`socket()` 创建允许，避免 pip/NSS 等本地检查被误杀），真正的网络边界是 bwrap 的私有 network namespace。
- 禁止联网、子代理、Agent Team/跨 agent 通信，以及可绕过单次评测的持久化工具（goal / schedule / drawio / canvas 等）。用户 fork 出的会话仍按主会话约束；子代理会话本身也同样受工具审计和进程沙箱约束（黑名单只是额外的一层）。
- 工具黑名单可配置；未知工具默认放行。DSH 内置与 dsh-ops 的文件工具会做参数路径审计。

> 完整性修复记录、仍存风险与逐任务覆盖范围见 [ISSUES.md](./ISSUES.md)。

## 时间限制

- 每条用户消息发送时，用**当时的** timeLimitEnabled 和 timeLimitMinutes 为本会话创建或清除 deadline。
- 开启限时时：deadline = 当前时刻 + 当时的分钟数；之后修改分钟数、选择任务、关闭或重新打开限时，都**不会**改动已存在的 deadline。
- 关闭限时后再次给已有 deadline 的会话发送消息，会清空该会话自己的 deadline；其它会话不受影响。
- 重新打开限时后，已有会话不会自动恢复 deadline；下一次用户消息才会重新计时。
- 每个会话独立计时；可以同时运行多个不同时长、有的开启限时有的不开启的会话。
- deadline 到点终止对应会话及其**子代理会话**与**该会话拥有的后台任务**；不影响其它会话。turn 结束后有 5 秒空闲宽限，避免与后台任务的注册竞争。子代理按 `origin/delegationDepth` 识别；用户 fork 会话的 `parentSession` 是 seed 血缘，不会被当作子代理误杀。
- 关闭「评测守护」总开关会清空全部 deadline 并停止约束；总开关与单轮限时开关是两套状态。
- 已知边界：deadline 仅保存在内存，DSH 重启/HMR 重载会丢失；重启后下一条用户消息重新计时。

设置页选择任务时会自动把「最长时长 + 限时开关」写成该任务默认值并立即显示，用户可以随后手动修改；提示词中的分钟数会随当前配置一起变化：

| 任务类型 | 默认限时 |
|---|---|
| 原 5 个 meow-ai-arena 任务 | 90 分钟，默认打开 |
| Commit0 单库 | 480 分钟，默认关闭 |
| SWE-Bench Pro 单任务 | 1440 分钟，默认关闭 |
| FeatureBench 单 feature | 1440 分钟，默认关闭 |
| Neural-CPP 纯 C++ 张量后端 | 1440 分钟，默认关闭 |
| Neural-FP32 纯 C++ 运算与性能 | 1440 分钟，默认关闭 |
| Neural-CIFAR 从零训练图像分类 | 1440 分钟，默认关闭 |
| Neural-Pendulum 从零训练三节摆控制器 | 1440 分钟，默认关闭 |

## 任务与安装

设置页可安装十二个任务包：

- 原 5 个比赛任务：坦克大战、仓储调度、圈地、炸弹人、夺旗。
- 3 个长时单任务：Commit0 单库实现、SWE-Bench Pro 单任务、FeatureBench 单 feature。
- 4 个纯 C++ 项目：Neural-CPP 张量后端、Neural-FP32 运算与性能、Neural-CIFAR 图像分类训练、Neural-Pendulum 三节摆控制器。

Neural-CPP 安装时自动把 workspace.zip 解压到工作区根目录，并在完成后移除 setup.sh；agent 从根目录的 CMakeLists.txt 开始。工作区只包含 TASK.md、CMakeLists.txt 和 include/nn/nn.hpp；实现在 src/ 中从零完成，纯 C++17，无 Python/OpenCL/第三方库。

评测器同时兼容旧安装留下的嵌套布局：若根目录没有 CMakeLists.txt，会自动改用 workspace/CMakeLists.txt，避免已经产出的工作区无法评测。

Neural-FP32 是独立的新任务：固定 fp32，不做完整训练；agent 从零实现 `build/nn_task` CLI，覆盖逐元素、matmul、linear、relu、softmax、layernorm、mse、conv2d、对应反向传播、AdamW 和 SGD momentum。评测侧用 PyTorch 在相同数据和参数上计算参考值，逐操作输出可执行性、是否通过误差阈值、加权平均相对误差、速度，并在最前面给出是否所有操作可执行、最大加权平均相对误差和速度几何平均；不提供统一 score，速度不受误差门控影响。

**评测覆盖**：CLI 评测使用固定形状（逐元素同形状 64×256、matmul 256³、conv2d 单组 stride1/pad1/dil1/groups1 等），不会覆盖广播、groups/stride/dilation/非方形核的所有组合；单次 CLI 调用 60 秒、整轮 30 分钟。速度是“含进程启动与文件 IO 的端到端吞吐”，不是纯 kernel 性能。

Neural-CIFAR 在 Neural-FP32 的基础上要求 agent 用同一套 C++ 训练框架构造并训练 CIFAR-10 图像分类网络。任务不限时，agent 定位为神经网络全栈设计与优化大师，提交前应尽可能优化；速度和未知验证集准确率都是重要指标，会参与世界顶尖水平比拼。安装时下载并准备 50k train split；隐藏 10k test split 由独立评测目录 `dsh-eval-guard/neural-cifar-eval/fetch_cifar.py` 获取并放在该目录的 `hidden/`，不作为工作区文件。评测保留全部原算子指标，并额外用 4 线程和全部逻辑 CPU 分别跑推理，输出验证准确率、FPS、平均单图耗时；4 线程 0.02s/image 只报告，不门控。

**评测口径**：评测只检查 `build/nn_task` 与 `build/infer`（不跑 `build/train`）；FPS/单图耗时包含进程启动与模型加载；验证标签只读入评测进程内存，候选 `infer` 拿到的是独立临时目录中的图像副本，看不到标签。

CIFAR-10 下载缓存固定在 DSH home 下（默认 `~/.dsh/eval-guard/cache/cifar10/`），保存 train/val 的 images/labels。下载使用 `.download.lock` 独占锁，并实时打印进度；锁存在时认为其他 DSH 实例正在下载，任务安装/评测会提示等待或手动删除锁文件。缓存完整且无锁时直接复用。设置页新增「CIFAR download mirror」和「CIFAR download proxy」输入框：mirror 可选 Toronto 官方、Toronto Cave 或自定义 tar.gz URL，proxy 只用于 CIFAR 下载。按任务安装和评测时分别传给 setup/fetch 脚本。

所有任务的后台安装被取消或失败时，会删除本次安装已搬入工作区的文件，并清空工作区。清理只会在安装开始前工作区确实为空时执行，原工作区有内容时绝不触发。Neural-CIFAR 还会删除本次安装拥有的 CIFAR 下载锁，但不会删除其他 DSH 实例的锁。

Neural-Pendulum 要求从零实现纯 C++17、仅 CPU、fp32 的神经网络基础设施，并用它训练控制器驱动预编译三节摆从接近全向下起摆、保持全向上 10 秒。后端评测与 Neural-FP32/Neural-CIFAR 使用同一套算子/阈值（单次调用 60 秒）。算子评测全部通过后才驱动 build/control，在固定初始状态上统计连续全向上时间 t_can。评测脚本在独立目录 `dsh-eval-guard/neural-pendulum-eval/`；宿主侧提交与测试工具为 eval_submit / eval_test，工具的注册依赖会话级 run 绑定。

**开发参考环境**：修正后的 RL 可行性环境（Python + CPU PyTorch，仅开发参考，不进入候选工作区、不是提交依赖）放在 `assets/neural-pendulum/rl-feasibility/`。其 `sim.py` 与评测用 `sim_eval` 使用相同物理和物理关节力矩映射 `(tau1, -tau3, tau3)`；报告、汇总表和曲线在 `RESULTS.md` / `results/` 中。15 组实验确认：full 36D（cos/sin + 手动差分 da/5，H=4）+ base reward + random init 可学；去掉 da 或把 H 缩到 1 会失败；PPO/SAC 都应交付 best checkpoint。

**分数**：eval_test 返回 stage / t_can / completed / error；时间积分 `final_score` 写入宿主侧事件与完成标记（`~/.dsh/eval-guard/neural-pendulum/<workspace>/done`），用于赛后排行，不返回给 agent。分数在**测试请求时刻**切换，当前测试结果也计入本次积分；宿主按事件重新计算最终值，而不是使用“评测完成时刻”的旧值。起算时间取首次提交写入的 meta.json。

评测时在临时目录独立构建，然后从 assets/neural-cpp/hidden/ 注入 C++ 测试源码；隐藏测试在候选构建**开始前**读入内存，构建完成后再落盘编译，测试数据由固定种子在进程内生成，不依赖或复制外部数据文件。

结果返回 coreAccuracy（核心 C++ 测试通过率）、matrixAccuracy、逐类型 metrics（f32/int128/MXFP4 的矩阵与反向准确率、平均绝对误差、fwd/bwd 毫秒、fwd/bwd GFLOP/s）、speedGflops，以及按矩阵吞吐计算的 speedGeomeanGflops 和 matrixSizes（f32/i128/MXFP4 均为 256×256×256）。确定性项目使用 4 组独立输入取平均；MXFP4 RNE 和随机舍入分开统计，随机舍入使用 2 组输入×每组 1024 次采样，逐元素比较参考均值/标准差和候选均值/标准差，按 3σ 和 0.1–10× 标准差规则统计正确率。参考值由测试侧独立标量实现先算完，再调用候选实现，参考计算不计入候选耗时。隐藏测试不在工作区，agent 无法读取。

原 5 个任务直接放入打包好的 workspace.zip。后 3 个任务在安装时由 host 侧 setup 脚本按固定 commit clone 仓库、创建 .venv 并安装依赖；安装完成后会清理 `.git`、`setup.sh`、`remove.patch`、`spec.pdf.bz2` 等安装产物，工作区只保留任务本身素材。Commit0 的 `pytest.ini` 会去掉 coverage 参数，保证默认测试命令在沙箱内可运行。agent 运行时仍然禁网，使用工作区内的 .venv 跑测试；agent 可以自行 `git init` 或写自己的脚本。

**测试完整性**：安装成功时会把 commit0 的 `tests/ + pytest.ini`、SWE-Bench Pro 的 `test/`、FeatureBench 的 `tests/ + conftest.py` 快照到宿主侧（`~/.dsh/eval-guard/task-baseline/<workspace>/`），评测前覆盖回候选副本并删除候选新增的 conftest.py；同时记录 .venv 关键文件（python 指向、pyvenv.cfg、.pth、pytest 包）指纹，指纹不符直接判评测失败。因此“改测试 / 换 python / 改 pytest 包”不再能伪造通过。

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

插件 eval/ 目录内置全部 12 个任务的单结果评测脚本：

- Commit0：单元测试通过率（测试来自安装时快照，评测前恢复）。
- SWE-Bench Pro / FeatureBench：隐藏 F2P/P2P 测试；显式要求 F2P 节点全部通过且 P2P 全部通过才 resolved，skipped 不算通过。
- Neural-CPP：独立编译隐藏 C++ 测试（启动即快照，构建后编译），返回 accuracy、matrixAccuracy、speedGeomean 与 score/resolved。
- Neural-FP32：调用候选 build/nn_task，用 PyTorch 参考值统计每个操作的 MRE 和速度；汇总项在最前面，不输出统一 score。
- Neural-CIFAR：保留 Neural-FP32 的算子评测，加载训练好的模型，在隐藏验证集上分别用 4 线程和全部线程测准确率/FPS；独立脚本在 `dsh-eval-guard/neural-cifar-eval/`。
- Neural-Pendulum：先运行与 Neural-FP32/Neural-CIFAR 相同的 PyTorch 参考算子评测（全部通过才继续），再构建并驱动 build/control，用宿主事件时间积分计分；独立脚本在 `dsh-eval-guard/neural-pendulum-eval/`。
- Tank / Warehouse / Land / Bomberman / CTF：运行官方引擎/裁判，只返回当前工作区候选者结果，不生成排名；使用 quick 档（tank 20 局仅 baseline、warehouse 仅 pub_small、land 仅单挑、bomberman 仅 2 人、ctf 仅 2/3 方），不能代替正式排行。

评测也是后台任务，日志和结果实时显示，持久化在 jobs 目录。评测进程继承的环境变量已收敛为一份白名单，`EVAL_*` 句柄不会再传给候选子进程。

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

评测用的 arena kit 位于 `eval/arena/<task>/workspace`，与 agent 工作区不是同一份快照（kit 保留源码形式的 baseline、历史 bot 与不同的 README/clock.py），只用于提供基准与裁决程序；单结果评测不启用历史对手。

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
- PyTorch + numpy（neural 任务的算子和模型评测；缺少时 neural-fp32/cifar/pendulum 会报明确错误）

## 已知边界

- 只支持 Linux；启用守护前会探测 bwrap 与 unprivileged user namespace，探测失败时设置页显示原因且不生效。
- seccomp 用户通知的路径检查与 open 之间存在极小的 TOCTOU 窗口；bwrap 只读/无网络挂载是第二层内核边界。
- io_uring 被拒绝（以 EPERM 软拒绝，不杀进程）；正常 syscall 不记录，进程审计只覆盖被拦截的文件/网络 syscall。AF_INET/AF_INET6 的连接类 syscall 也是 EPERM 软拒绝；文件策略违规仍会终止进程组。
- 工具路径审计只覆盖已知文件工具（DSH 内置、dsh-ops、drawio）；未知工具默认放行，这是产品选择，不是完整内核边界。
- `forbiddenPaths` 的 `!` 例外只放宽审计；它不会把宿主 home、DSH 状态目录、共享 /tmp 或 workspace 的祖先 ro-bind 进沙箱。系统目录本身已只读可见，通常不需要 `!`。
- 只读文件放行是产品选择：工作区之外“用户不可写但可读”的文件允许读取，因此只读安装的隐藏资产/数据不在 OS 权限层面被隐藏；真正需要隔离的隐藏数据仍需独立用户/容器。
- 进程沙箱会清空继承环境，但 PATH/HOME/TMPDIR/XDG/LANG/TZ/USER 等非敏感变量按白名单回填；run_code 的 PTC 控制通道标记和打包 PTC 的 NODE_OPTIONS 由 supervisor 从自身环境恢复。
- native supervisor 的架构门在源码层支持 x86_64/aarch64；仓库内的预编译二进制只对应构建它的架构，其它架构必须在本机运行 `npm run build:native`。
- 文件工具的路径审计是“先检查后打开”，与进程沙箱的 seccomp TOCTOU 一样存在极小的 symlink 竞态窗口；不要把它当作文件系统的完整边界。
- 评测进程与候选代码仍以同一个 OS 用户运行：安装了恶意 CMakeLists/候选二进制后，理论上仍可读取同用户文件并篡改评测器。生产跑分建议用独立账号或容器执行 eval job；本插件已做的缓解是：环境变量白名单、隐藏数据先读入内存、候选子进程剥掉 EVAL_*、测试基线恢复、venv 指纹校验。
- commit0 没有官方隐藏测试：评分基于“安装时快照恢复的仓库原始测试”；这意味着原始测试本身仍是评分依据。
- 单结果评测是自查工具：quick 档覆盖面小，不能代替正式赛排行；正式负载/隐藏负载需在评测侧另行准备。
- 设置页使用内联样式，不依赖额外 CSS 构建链。

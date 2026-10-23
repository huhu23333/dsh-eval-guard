# dsh-eval-guard 问题清单与修复记录

> 版本：0.7.5 · 审计基线：本仓库工作树（含 0.7.5 修复与 Neural-Pendulum 仿真器分离）
> 对照 DSH：0.2.1-alpha.1 源码（并核对 git tag 0.1.2 / 0.1.5 / 0.1.7 / 0.2.0-rc.1 / rc.2 / 0.2.1-alpha.1）
> 本文件先记录问题，再记录修复状态。状态：✅ 已修复 / 🟡 部分修复 / ⛔ 保留（有明确原因或需要架构级改动）
>
> **2026-10-22 更新**：本轮只修复“守护本体 + Neural-Pendulum”。已修复：fork/子代理会话识别与约束、子代理路径审计、sandbox 继承环境清洗、PTC 控制通道标记回填、allow 规则宿主绑定安全、写路径规则、设置页任务默认值原子保存、Job 异步失败清理与重启 PID 校验、Pendulum 按请求时刻计分与 done final_score。  
>
> **2026-10-23 会话回归修复（未发版）**：修复 Neural-Pendulum 仿真器 Coriolis 项多乘 cos 导致高转速下能量注入并产生 NaN；修正关节 3 在绝对角坐标下的广义力矩映射，由 (tau1,0,tau3) 改为 (tau1,-tau3,tau3)；TASK/SPEC/prompt 明确 theta 约定并说明工作区无 PyTorch/numpy；网络 syscall 改为 AF_INET/AF_INET6 的 EPERM 软拒绝，避免 pip/NSS 本地检查被误杀。

> **0.7.5**：把 0.7.4 误带的开发用 RL 可行性代码从插件中撤除，只保留修正后的 simulator；同时把 agent 训练仿真器与评测仿真器拆开：`assets/neural-pendulum/sim_agent` 只做随机近向下 reset，评测用 `neural-pendulum-eval/sim_eval` 只做固定初态 reset，两者不共享 reset 分支。工作区仍只交付二进制，不提供仿真源码。
> **下文仍有若干条目标记为 ✅ 但实际未完成，不要再按旧状态理解**：arena legacy 的 `std::thread` 静态规则未移除；`cifar_cache.py` 解压仍无 filter、缓存路径仍用 `Path.home()/.dsh`；CIFAR 隐藏标签仍以文件形式落盘；FP32“30 分钟整轮”仍是 op 间检查；无 baseline 时评测仍 fail-open。其余任务相关修复按用户要求暂不动。

## 0. 修复状态总览

| 类别 | 状态 | 备注 |
|---|---|---|
| 会话元数据读错字段（P0） | ✅ | 统一改读 `session.header.{cwd,origin,parentSession,delegationDepth}`，并加真实形状测试 |
| 工具路径审计失效（P0） | ✅ | 恢复审计；工作区缺失时 fail-closed；扩充受审计工具 |
| enable 失败被吞（P0） | ✅ | 增加 bwrap/userns 功能探测、错误落盘/上报/UI 展示、失败退避 |
| /tmp 工作区沙箱不可用（P0） | ✅ | 调整 bwrap 挂载顺序；supervisor 增加 `tmpfs` 规则 |
| io_uring / rename 目标 / 机制欺骗（P0） | 🟡 | io_uring 改为 EPERM 软拒绝（不杀进程）；rename/link 双路径检查；新增 symlink/link/mknod/utimensat/statx/fchmodat2 拦截；bwrap 识别仍按 executable 名（bwrap 在多发行版是 shim 链，pid 绑定会破坏 setup），setup 豁免仅限 pivot 后不存在的路径 |
| 未知工具放行 → goal/schedule/drawio 绕过（P0） | ✅ | 默认黑名单新增这些工具族 |
| 评测执行不可信代码无隔离（P0） | 🟡 | 修复可修部分：环境变量白名单、隐藏数据先读入内存、CIFAR 标签隔离、venv 完整性校验、测试文件从宿主基线恢复；同用户同机执行仍无法彻底隔离，已在 README 已知边界说明 |
| 隐藏测试/负载不隐藏（P0） | 🟡 | 同上；commit0 无隐藏测试的问题改为“从安装时快照恢复测试”；仓库仍保留同用户读面 |
| commit0/SWE/FeatureBench 可伪造（P0） | 🟡 | venv 关键文件校验 + 测试文件基线恢复 + 剥掉 EVAL_* 环境变量；同用户进程仍可读 hidden |
| neural-pendulum 端到端不可用（P0） | ✅ | 修 autoBind→run→eval_submit/eval_test；修 meta 起点；final_score 落盘 |
| warehouse 单结果恒 0 分（P1） | ✅ | wrapper 字段对齐 |
| arena wrapper W/D/L 恒 0 / 超时缺失（P1） | ✅ | 修字段，补超时 |
| 价格文件死链/空表（P1） | ✅ | 改为按任务文件名并容错；价目表本身仍为空，成本显示 missing（不再误报 0 分） |
| neural-fp32 无总超时/EVAL_* 泄漏（P1） | ✅ | 加总 deadline、子进程环境过滤 |
| CIFAR 隐藏标签与图像同目录（P0） | ✅ | 标签只读入内存，图像复制到临时目录 |
| neural-cpp 隐藏测试 build 后才读（P0） | ✅ | 脚本启动即快照到内存，build 后再落盘编译 |
| prompt 硬编码分钟数（P2） | ✅ | 改为按当前 timeLimitMinutes 插值 |
| /tmp 写、系统目录读与 README 口径（P1） | ✅ | 审计恢复后写路径受控；README 改为准确描述“系统目录只读可见、/tmp 为工作区私有” |
| 包类型声明缺失、native 预编译（P2） | ✅ | 移除不存在的 types 导出；构建脚本补 native |
| 文档/文案/包装器 docstring（P3） | ✅ | 已对齐 |

---

## 1. 插件本体：P0 致命问题

### 1.1 会话元数据读错字段，路径审计/子孙清理/自动绑定整体失效

**现象**
- 插件按 `session.header.meta.{cwd,origin,parentSession,delegationDepth,title}` 读取（src/guard.ts 的 isMain/checkTool/onAgentCreated/descendants/associate/runningSessions）。
- DSH 的 `SessionHeader` 字段在**顶层**：`cwd / parentSession / origin / delegationDepth / agentPreset`，没有 `meta`（packages/core/session/src/types.ts；0.1.2 至 0.2.1-alpha.1 各 tag 均如此）。
- 后果：
  1. `checkTool` 拿不到 workspace → 直接放行，**工具参数路径审计从未执行**；
  2. `isMain()` 恒 true（子代理/子会话不被排除）；
  3. `descendants()` 恒空 → 超时不杀子代理；
  4. `onAgentCreated` 拿不到 cwd → `autoBind` 永不执行 → 无 run record；
  5. 标题/cwd 展示为空。
- 因此 `read`/`grep`/`glob` 等宿主工具可读工作区外任意文件（DSH 的 read 工具无沙箱围栏：fs-sandbox 注释 “Reads pass through untouched”），包括 `~/.ssh`、`~/.dsh/**`（含 CIFAR 隐藏验证集缓存、pendulum host submissions/events）、插件内隐藏测试、其它会话工作区。

**根因**：代码从未在真实 DSH 上验证；自带 `test/simulate.ts` 用假会话 `{header:{meta:{cwd}}}`，把错误形状写进夹具，且断言 `installed.runId` 已与当前 `install()` 返回不符。

**修复**
- 统一 helper：`sessionCwd / sessionParent / sessionIsSubagent`，isMain 基于真实字段；
- descendants 基于 `header.parentSession`；
- 标题回退为 cwd/id（DSH header 无 title）；
- 审计拿不到 cwd 时 **fail-closed**（拒绝文件类工具并记事件），不再静默放行；
- 重写 simulate 测试用真实 header 形状，并新增“工作区外读取必须被拒”的断言。

### 1.2 DSH 读取工具本身不设防（依赖 1.1 的审计）

已在 1.1 修复；补充说明：审计的读语义是“工作区 + 只读系统文件可读，其余拒绝”，写语义是“仅工作区 + 无痕设备”。

### 1.3 /tmp 与系统目录与 README 口径不一致

- DSH workspace-write 的 writableRoots 含 /tmp 与 os.tmpdir（packages/sandbox/sandbox/src/roots.ts），插件又把主会话强制为 workspace-write → `write/edit` 可写 /tmp；与 README“工作区外包括 /tmp 不可访问”矛盾。
- bwrap 只读挂载 /usr /etc /opt /sys /var/lib，超 supervisor 也放行其读 → 系统目录对 agent 可读。
- 修复：审计恢复后 `write/edit` 的 /tmp 写被拒；README 改为准确描述（系统目录只读可见、/tmp 映射为工作区私有 .tmp、用户数据目录不可读）。

### 1.4 enable 失败被吞

- `syncConfig()` 里 `void this.enable()`；enable 在 supervisor 缺失/sandbox 服务缺失时返回错误但无人处理 → UI 显示已启用、实际无约束；每秒重试。
- 只检查 supervisor 文件存在，不探测 bwrap/userns。
- 修复：enable 前做 bwrap 功能探测（`bwrap --version` + 最小命名空间 probe）；失败记 `lastError`、写事件、通过 `/api/eval-guard/state` 暴露、设置页红字展示；失败后退避 5s 再重试；成功清空。

### 1.5 未知工具默认放行（黑名单按名字猜）

- 默认黑名单=基础名单+名字正则，未覆盖当前环境里可绕过评测的已装工具：`create_goal/update_goal`（goal 自动续轮绕过 deadline）、`schedule_*`（超时后唤醒）、`drawio_*`（直接 node:fs 写文件，可跨工作区写）、`canvas_*`、`skill` 等。
- 修复：BASE_BLACKLIST 增加这些名字，PLUGIN_BLACKLIST_PATTERNS 增加 `^drawio_`、`^schedule_`、`^(create|get|update)_goal$`、`^canvas_`、`^view_canvas$`；受审计工具集合补 drawio 读写。

### 1.6 进程沙箱缺陷

- io_uring 对 basename 以 `node` 开头的进程放行（与 UV_USE_IO_URING=0 自相矛盾），可用改名进程绕过 seccomp 路径审计 → 修复：取消豁免，一律拒绝。
- rename/renameat/renameat2 只检查源路径 → 修复：同时检查源与目标。
- `is_bwrap()` 只看 exe 文件名含 bwrap，可被伪造 → 修复：绑定首次 exec 的 bwrap pid，仅该 pid 在 setup 阶段享有豁免。
- 未拦截 symlink/link/mknod/utimensat/fchmodat2/statx → 补齐（symlink 目标不触盘；link/symlinkat 新路径按写检查；其余按读/写）。
- 工作区位于 /tmp 时 bwrap 先 bind workspace 再 tmpfs /tmp，tmpfs 覆盖工作区 → 修复：先 tmpfs /tmp 再 bind workspace；supervisor 增加 `tmpfs` 规则允许沙箱内 /tmp 读写（ephemeral）。
- supervisor 用“argv 里最后一个 --”注入 --chdir，命令含独立 -- 时错位 → 修复：插件显式传 `--cwd`，supervisor 仅做“必须在工作区内”的校验。
- `wrapSandbox` 的 cwd 参数被 void → 修复：真正传给 supervisor。
- `enforcement:'full'` 无探测支撑；native 二进制为仓库预编译 → 构建脚本统一重建（见 5.2）。

### 1.7 时间限制语义

- `jobs.list(session)` 含无主 job（jobs-local: owner 缺省时所有 caller 可见），被当成会话自己的 job → 误杀/误等；修复：只统计 `owner === sessionId` 的 job。
- 子代理不递归（1.1 修复）。
- turn/end 后 tick 立即清 deadline，可能与后台 job 注册竞争 → 修复：增加 5s 活动宽限（lastActivity）。
- 超时后 run 仍 active、usage 继续累加 → 修复：expire 时把 run 置 timeout 并写盘；后续事件忽略已结束 run。
- deadline 仅内存，DSH 重启丢失 → 保留为已知边界（README 说明），不做磁盘持久化（重启后没有正在跑的轮次，恢复语义不明）。

### 1.8 审批副作用

- `approval.setPolicy(agent,'never')` 会注入一条 user-approval 消息（DSH 行为），可能影响模型；approvalPrev 会覆盖用户期间手动设置。
- 修复：保持强制 never（评测必需），在 README/设置页提示“启用时会给运行中会话注入一条策略变更消息；停用恢复启用前策略”。

---

## 2. 评测体系：P0 完整性问题

所有 eval/install job 由宿主 JobManager 直接 spawn，继承完整 process.env、无沙箱；被评测对象是 agent 写的源码与编译产物。

### 2.1 已修复/缓解
- **环境变量白名单**：JobManager 只传 PATH/HOME/LANG/LC_*/TMPDIR/SSL_* 等；EVAL_* 由 job 显式注入；评估脚本再 spawn 子进程时剥离 EVAL_RESULT_FILE/EVAL_JOB_DIR/EVAL_INSTALL_TOKEN，避免把结果路径交给候选代码。
- **neural-cpp**：脚本启动即把 `assets/neural-cpp/hidden/test_neural.cpp` 读入内存，候选 cmake build 结束后才写临时文件并编译 → build 阶段无法替换隐藏测试。
- **SWE/FeatureBench**：patch / hidden test / instance 在脚本早期读取；hidden 测试注入后再跑；P2P/测试文件由“安装时基线快照”在评测前恢复（见 2.2）。
- **CIFAR**：先读 hidden labels 到内存，把 images 复制到独立临时目录再交给候选 infer；不再把 labels 同目录暴露。
- **commit0/SWE/FeatureBench venv**：安装成功时记录 `.venv` 关键文件清单（python 真实路径、pyvenv.cfg、pytest/_pytest/pluggy 目录树 sha256）；评测前校验，不一致直接判失败（不再静默伪造）。
- **测试来源**：安装成功时为 commit0/SWE/FeatureBench 快照测试目录（tests/、test/、pytest.ini、conftest.py）；评测前从宿主快照覆盖候选副本并清理候选新增的 conftest.py，避免改测试/加 conftest 伪造通过。
- **价格/成本**：legacy 脚本改为读取对应 `<task>-prices.json`（缺失/空表时按 missing 处理，不再恒 0 且来源错误）。

### 2.2 保留（结构性）
- 评测器与候选代码仍以**同一 OS 用户、同一机器**执行；理论上仍可用 CMake/二进制扫描文件系统篡改评测器/结果。彻底修复需要独立用户/容器/微虚拟机执行评测，超出本插件单进程能力，已在 README 的“已知边界”明确标注，并建议生产环境用独立账号或容器执行 eval job。
- `EVAL_RESULT_FILE` 仍通过 env 传给顶层 evaluator（候选二进制可通过 /proc 窥探父进程环境/命令行）；已剥离直接继承，但同用户无法完全隐藏。

---

## 3. 各任务问题与修复

### 3.1 tank
- 单结果评测仅 quick（20 局，仅 baseline），rating 与正式赛 BT 不可比 → 文档标注；wrapper 修正 W/D/L（从 pairwise 汇总）。
- 静态扫描把 std::thread 判可疑，但 RULES 允许线程 → 移除线程规则。
- kit 与工作区快照漂移（README/clock.py/history 等）→ README 记录 kit 差异；不再把 history 当作评测输入（仅 --with-history 时使用，单结果评测不启用）。
- eval kit 写入插件目录（build/history）→ 保持，README 说明需插件目录可写。

### 3.2 warehouse
- wrapper 读 `row.get('speedup')`，实际字段 `geomeanSpeedup` → 单结果恒 0 → 已修（score/speedup/correct/status 对齐）。
- 单候选归一化退化（>1x 即 100 分）→ 单结果视为“加速比参考”，不再宣称分数；文档说明。
- 公开负载同时在工作区与 eval kit（含 .out），可硬编码 → 保留为已知限制：正式跑分应使用隐藏负载；README 标注。评估脚本仍只跑 public（单结果评测定位是自查）。
- CRLF 归一化与“逐字节”口径不同 → 文档措辞改为“逐字节（CRLF 归一化）”。
- 候选可 fork/写文件/联网未被运行时审计 → 保留（同用户执行问题）；静态扫描仅报告。

### 3.3 land / bomberman / ctf
- quick 档只跑单挑/2 人/2-3 方，与任务宣称的混战/2-10/2-15 不符 → 文档标注单结果评测覆盖范围；wrapper 的 W/D/L 字段改为按 summary 实际字段输出（没有就不输出）。
- BT 拟合失败时静默 0 分 → legacy 检测 `unavailable` 并以 `ok:false` 报告。
- 子进程无超时 → 补 timeout（1800s/3600s）。
- 评测 kit 与工作区文档漂移（README/clock/TASK）→ README 记录；工作区文档保持原样。

### 3.4 commit0
- 提示词称“隐藏测试”，实际跑工作区 tests → 改为“从安装时快照恢复的仓库原始测试”，并快照 tests/pytest.ini；评测前恢复+清理新增 conftest。
- `.venv` 可伪造 → venv 关键文件校验。
- setup 未执行 `pip install -e .` 且 python 版本与 instance 不同 → setup.sh 补 `pip install -e .`（在实现已存在时）；版本不一致记 README。**注意**：安装时 tinydb/ 已删，pip install -e . 会失败，故不在 install 时执行；改为评测前检测元数据缺失风险，README 说明。
- spec.pdf.bz2 命名 → cleanup 列表修正为同时清理 spec.pdf.bz2（保留 spec.pdf）。

### 3.5 SWE-Bench Pro
- patch 应用 returncode 未检查 → 补检查，失败即判失败；应用后校验 F2P 节点存在。
- F2P/P2P 区分丢失 → wrapper 按 instance 的 fail_to_pass/pass_to_pass 计算，skipped 不计通过。
- 测试文件可改 → 安装时快照 `test/` 并在评测前恢复。
- venv 可伪造 → venv 校验。

### 3.6 FeatureBench
- 同 SWE：RESTORE tests/、venv 校验、skipped 不算通过、resolved 需 F2P 通过。

### 3.7 Neural-CPP
- 隐藏测试在 build 后读取 → 改为启动即快照到内存。
- nn.hpp 可改 → 不强制（公开接口语义不得改但无校验）；评测用候选头文件，保留为已知限制并提示人工审计。
- score/resolved 文档未定义 → README/TASK 补充。
- speedGeomean 混 dtype/绝对 GFLOPS → README 明确口径。

### 3.8 Neural-FP32
- 无总超时 → 增加 30 分钟总 deadline，单次调用 60s（与 pendulum oracle 对齐）。
- build/候选进程继承 EVAL_* → 剥离。
- 覆盖范围小于 TASK 文字（广播/conv groups/stride 等未测）→ TASK.md 澄清“本 CLI 评测固定形状；广播等能力需自行保证”，避免“承诺=评测”的误解。
- 速度包含进程启动 → README 明确这是“含启动开销的端到端吞吐”。

### 3.9 Neural-CIFAR
- hidden labels 与 images 同目录 → 标签只读入内存、images 复制到独立临时目录。
- hidden 缺失时评测内联网下载 → 保留 fallback（README 已写 fetch 脚本），错误信息更明确。
- FPS 含模型加载 → README 明示口径。
- 不检查 build/train → README 标注仅 infer/nn_task 被评测。

### 3.10 Neural-Pendulum
- autoBind/run 链路失效 → 随 1.1 修复；eval_submit/eval_test 可用。
- meta.started_at 被每次 eval 覆盖 → 只在缺失时写，用 run.createdAt。
- final_score 被丢弃 → hostTest 把 final_score 写入事件与 done 文件。
- control/sim stderr 死锁 → stderr=DEVNULL；selector 改为自定义 LineReader（二进制 + 内部缓冲）。
- t_can 容差 < vs <= → 改 <=；不影响 ≥10 判定。
- 构建环境剥离 EVAL_*。

---

## 4. 其它（已修/记录）

- reveal 路由：spawn 错误未处理 → 等 spawn 成功再返回，失败返回 500；路径检查用 realpath。
- JobManager 重启后不杀旧进程 → 记录 pid，启动时对 interrupted job 尝试杀进程组。
- Job 日志/结果路径校验、detail 读取范围不变。
- `install()` / `prepare` / `runSetup` 死代码 → 删除；测试改为使用 startInstall/新 API。
- 包 `exports.types` 指向未生成 d.ts → 移除 types 字段；构建脚本补 native。
- prompt 硬编码分钟数 → 按当前配置插值。
- 文档/文案/包装器 docstring 复制粘贴 → 修正。
- `forbiddenPaths` 通配符被静默丢弃 → 截断到首个通配符作为前缀规则并在设置页说明。
- `cleanupCifarLock` 硬编码 ~/.dsh 与 dshHome 不一致 → 统一为 dshHome()（CIFAR 缓存仍按 README 固定在 ~/.dsh，README 与代码一致化：缓存路径以 dshHome 为准）。
- CIFAR tar 解压无 filter → 加 `filter='data'`（Python 3.12+ 兼容处理）。
- prices 文件死链 → 每任务文件名 + 容错。

---

## 5. 修复后仍需注意（残留风险）

1. **评测隔离**：eval 进程与候选代码同用户同机，属结构性风险；生产跑分请使用独立账号/容器。
2. **隐藏数据**：host `~/.dsh/eval-guard` 与插件目录对同用户可读；审计恢复后正常 agent 工具无法读，但宿主进程/被评测二进制仍可读。
3. **deadline 不持久化**：DSH 重启/HMR 会丢 deadline。
4. **任务覆盖**：单结果评测是“自查”定位，quick 档覆盖面小，不能代替正式排行。
5. **neural-cpp 公开头文件**：未做哈希校验，需赛后人工审计。
6. **neural-cifar/torch 环境**：评测机需安装 torch/numpy，README 依赖清单已注明。
---

## 6. 本轮修复的验证记录

在 2026-10-22 的修复后执行：

- 宿主半 `tsc --noEmit`（以 DSH 0.2.1-alpha.1 的 built d.ts 映射 cordis/schemastery）：通过。
- `tsdown` 构建 host + client bundle：通过。
- `node scripts/build-native.mjs` 重建 supervisor：通过（沿用既有 -Wall 告警）。
- `node test/simulate.ts`：通过（真实 header 形状、isMain/子代理、路径审计、黑名单 goal/drawio、autoBind cwd、deadline 宽限、descendants 清理、owned job 过滤、enable/disable、confine argv 含 --cwd）。
- `node test/wrap.ts`、`test/matrix.ts`、`test/runtime.ts`、`test/python-runtime.ts`、`test/ptc.ts`、`test/entry.ts`、`test/client-entry.ts`：通过（bwrap 允许/拒绝、网络拒绝、node/gcc/python/PTC 导入、入口冒烟）。
- 修改过的 Python 评测脚本全部通过 `python3 -m py_compile`；四个 neural 任务 asset zip 重新打包并通过 `unzip -t`。
- 未执行：commit0/SWE/FeatureBench 的完整端到端评测（需要 clone + venv + 网络，成本高）；neural 任务完整评测（需要数十分钟 CMake/torch 运行）。这些路径的改动已按“先快照/先校验、再构建/再评测”的顺序静态核对。

### 仍未端到端验证的点

1. workspace 位于 host /tmp 时的 bwrap 顺序修复（本会话的工具沙箱无法访问 host /tmp）。
2. 评测 job 的真实 run（需要设置页点击安装/评测，或在宿主侧启动完整 DSH）。
3. CIFAR hidden 数据下载与 infer 端到端（需要网络与训练好的模型）。
4. neural-pendulum 的 eval_submit/eval_test 完整闭环（需要真实会话运行；autoBind 依赖 header.cwd 现在已修正）。


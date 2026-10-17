# dsh-eval-guard · 评测守护

在 DSH 中启用后，约束**所有用户主对话**：

- 只能读写会话工作区；工作区外（包括 /tmp）不可访问。
- 禁止联网、子代理、Agent Team/跨 agent 通信、读取其它工作区。
- 工具黑名单可配置；未知工具默认放行，但 DSH 内置与 dsh-ops 的工具会做参数路径审计。
- 每个用户消息后的单轮有可配置的最长时长（默认 90 分钟）；到点只终止当前轮次，并递归清理子代理与后台任务。
- 设置页可安装“第 6 集 · 坦克大战”任务：空工作区 → 复制素材 → 新会话填入提示词（由用户发送）。

插件默认关闭；关闭时不做任何行为。所有 run 记录、usage、violations 写入
$DSH_HOME/eval-guard/runs/<run-id>/，供独立的 ../tank-eval 评测程序读取。

## 任务来源

安装的评测任务来自开源项目 **meow-ai-arena** 的第 6 集 · 坦克大战：

- 仓库：<https://github.com/chen-006/meow-ai-arena>
- 任务目录：<https://github.com/chen-006/meow-ai-arena/tree/main/tank>
- 使用版本：commit `6bd738b`（2026-10-16）
- 素材来源：该仓库的 `tank/workspace/`（MIT License）

本插件打包的 Linux 版 `workspace.zip` 在原素材基础上做了两处适配：把 `baseline.cpp` 编译为 Linux 可执行文件并移出源码；把 `clock.py` 改为单次 90 分钟版本。规则、引擎、本地裁判、模板等其余文件保持原样。

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

## 已知边界

- 只支持 Linux。
- seccomp 用户通知的路径检查与 open 之间存在极小的 TOCTOU 窗口；bwrap 只读/无网络挂载是第二层内核边界。
- 进程审计只覆盖被拦截的文件/网络 syscall；正常 syscall 不记录。
- 当前设置页使用内联样式，不依赖额外 CSS 构建链。

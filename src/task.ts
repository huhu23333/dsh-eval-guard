/**
 * Task packs: packaged workspace.zip install, original prompt construction, and
 * the run record the evaluators later read.
 * @module dsh-eval-guard/task
 */
import fs from 'node:fs'
import path from 'node:path'
import { execFileSync, spawn } from 'node:child_process'
import { createHash } from 'node:crypto'
import {
  TANK_TASK, TANK_TASK_LABEL, WAREHOUSE_TASK, WAREHOUSE_TASK_LABEL,
  LAND_TASK, LAND_TASK_LABEL, BOMBERMAN_TASK, BOMBERMAN_TASK_LABEL, CTF_TASK, CTF_TASK_LABEL,
  COMMIT0_TASK, COMMIT0_TASK_LABEL, SWE_BENCH_PRO_TASK, SWE_BENCH_PRO_TASK_LABEL,
  FEATUREBENCH_TASK, FEATUREBENCH_TASK_LABEL, NEURAL_CPP_TASK, NEURAL_CPP_TASK_LABEL,
  NEURAL_FP32_TASK, NEURAL_FP32_TASK_LABEL, NEURAL_CIFAR_TASK, NEURAL_CIFAR_TASK_LABEL,
  NEURAL_PENDULUM_TASK, NEURAL_PENDULUM_TASK_LABEL,
  PIP_MIRRORS, CIFAR_MIRRORS,
  type CheckResult, type RunRecord, type TaskInfo,
} from './shared.ts'
import type { Guard, TaskDefinition } from './guard.ts'
import { dshHome, guardRoot, packageRoot } from './sandbox.ts'
import { baseEnvironment, JobManager } from './jobs.ts'
import type { JobInfo } from './shared.ts'

/** Tasks offered by the installer. */
export const TASKS: readonly TaskInfo[] = [
  { id: TANK_TASK, label: TANK_TASK_LABEL, description: '1v1 坦克大战 bot；解压 workspace.zip，提交 workspace/submission/bot.cpp。', timeLimitMinutes: 90, timeLimitEnabled: true, needsSetup: false },
  { id: WAREHOUSE_TASK, label: WAREHOUSE_TASK_LABEL, description: '单人单次仓储调度优化；解压 workspace.zip，提交 workspace/src/。', timeLimitMinutes: 90, timeLimitEnabled: true, needsSetup: false },
  { id: LAND_TASK, label: LAND_TASK_LABEL, description: '圈地 1v1/4 人混战 bot；解压 workspace.zip，提交 workspace/submission/bot.cpp。', timeLimitMinutes: 90, timeLimitEnabled: true, needsSetup: false },
  { id: BOMBERMAN_TASK, label: BOMBERMAN_TASK_LABEL, description: '炸弹人 2–10 人 bot；解压 workspace.zip，提交 workspace/submission/bot.cpp。', timeLimitMinutes: 90, timeLimitEnabled: true, needsSetup: false },
  { id: CTF_TASK, label: CTF_TASK_LABEL, description: '夺旗 2–15 人 bot；解压 workspace.zip，提交 workspace/submission/bot.cpp。', timeLimitMinutes: 90, timeLimitEnabled: true, needsSetup: false },
  { id: COMMIT0_TASK, label: COMMIT0_TASK_LABEL, description: 'Commit0 单库实现：按规格从零实现一个 Python 库，单元测试评分。', timeLimitMinutes: 480, timeLimitEnabled: false, needsSetup: true, repoUrl: 'https://github.com/msiemens/tinydb', remotes: [{ id: 'github', label: 'GitHub', url: 'https://github.com/msiemens/tinydb.git' }] },
  { id: SWE_BENCH_PRO_TASK, label: SWE_BENCH_PRO_TASK_LABEL, description: 'SWE-Bench Pro 单任务：在真实仓库中修复一个多文件 issue，隐藏测试评分。', timeLimitMinutes: 1440, timeLimitEnabled: false, needsSetup: true, repoUrl: 'https://github.com/ansible/ansible', remotes: [{ id: 'github', label: 'GitHub', url: 'https://github.com/ansible/ansible.git' }, { id: 'gitee', label: 'Gitee 镜像', url: 'https://gitee.com/mirrors/ansible.git' }] },
  { id: FEATUREBENCH_TASK, label: FEATUREBENCH_TASK_LABEL, description: 'FeatureBench 单 feature：实现一个跨文件功能，隐藏测试评分。', timeLimitMinutes: 1440, timeLimitEnabled: false, needsSetup: true, repoUrl: 'https://github.com/mwaskom/seaborn', remotes: [{ id: 'github', label: 'GitHub', url: 'https://github.com/mwaskom/seaborn.git' }, { id: 'gitee', label: 'Gitee 镜像', url: 'https://gitee.com/mirrors/seaborn.git' }, { id: 'gitcode', label: 'GitCode 镜像', url: 'https://gitcode.com/gh_mirrors/se/seaborn.git' }] },
  { id: NEURAL_CPP_TASK, label: NEURAL_CPP_TASK_LABEL, description: '从零实现纯 C++ 张量运算与反向传播后端；隐藏测试统计正确性与速度。', timeLimitMinutes: 1440, timeLimitEnabled: false, needsSetup: true },
  { id: NEURAL_FP32_TASK, label: NEURAL_FP32_TASK_LABEL, description: '从零实现纯 C++ FP32 神经网络算子、反向传播与优化器 CLI；逐操作统计误差和速度。', timeLimitMinutes: 1440, timeLimitEnabled: false, needsSetup: true },
  { id: NEURAL_CIFAR_TASK, label: NEURAL_CIFAR_TASK_LABEL, description: '从零实现纯 C++ 训练框架，构造并训练 CIFAR-10 图像分类网络；保留算子指标并统计验证集准确率与多线程推理性能。', timeLimitMinutes: 1440, timeLimitEnabled: false, needsSetup: true },
  { id: NEURAL_PENDULUM_TASK, label: NEURAL_PENDULUM_TASK_LABEL, description: '从零实现纯 C++17 神经网络基础设施（算子数值以评测侧 PyTorch 参考校验），用三个关节角度控制预编译三节摆起摆并稳定保持；评测随机隐藏回合，统计成功率。', timeLimitMinutes: 1440, timeLimitEnabled: false, needsSetup: true },
]

/** The tank task definition (original prompt; only halftime mechanics removed). */
export const TANK_DEFINITION: TaskDefinition = {
  id: TANK_TASK,
  label: TANK_TASK_LABEL,
  description: TASKS[0]!.description,
  asset: path.join(packageRoot(), 'assets', 'tank', 'workspace.zip'),
  timeLimitMinutes: 90,
  timeLimitEnabled: true,
  needsSetup: false,
  prompt({ minutes }) {
    return [
      '按照要求编写尽可能强的坦克大战 bot。你的对手是其他顶尖 AI 模型写的 bot，它们可能主动设计战术。基准是一个中等强度、思路常规的 bot，它在正式比赛中是 0 分线；战胜基准不代表足够强，正式对手远强于基准。正式赛是 1 对 1 循环赛，用 Bradley-Terry 模型拟合实力值。',
      '',
      '解压 workspace.zip，阅读 README.md 和 RULES.md，然后开始。',
      '',
      '比赛是连续 ' + minutes + ' 分钟的单次任务：',
      '- 你有 ' + minutes + ' 分钟修改 bot，从对话开始计时；到 ' + minutes + ' 分钟强制停止；',
      '- 最终只看 bot 的强度，提前交卷没有任何好处；觉得没什么可改了，先换一个思路再试；',
      '- 到点时 workspace/submission/bot.cpp 是什么就交什么：不要让它长时间停在编译不过的状态；',
      '- 允许使用本地工具、对战和复盘；禁止联网，禁止求助其他模型（包括启动子代理：subagent、Task / Agent 工具、后台并行代理之类），除主办方发给你的材料外禁止查看其他选手的资料；',
      '- 这台机器上同时运行着其他程序：只能结束你自己启动的进程（按进程号），不要按名字批量结束进程；',
      '- 只能读写当前工作区内的文件，包括 /tmp 在内的其他位置都不能访问；',
      '- clock.py 从解压那一刻开始计算，实际计时从对话开始计算，它只能作为参考，请尽快解压以减小误差；',
      '- 不要向我提问，也不要等待我确认。',
      '',
      '完成后，用几句话简述你的策略。',
    ].join('\n')
  },
}

/** The warehouse single-run optimization task (original main-programmer prompt, minimal changes). */
export const WAREHOUSE_DEFINITION: TaskDefinition = {
  id: WAREHOUSE_TASK,
  label: WAREHOUSE_TASK_LABEL,
  description: TASKS[1]!.description,
  asset: path.join(packageRoot(), 'assets', 'warehouse', 'workspace.zip'),
  timeLimitMinutes: 90,
  timeLimitEnabled: true,
  needsSetup: false,
  prompt({ minutes }) {
    return [
      '这是一场 AI 编程赛。你接手了一个代码混乱的老系统：src/ 里是一个仓储机器人调度仿真系统，功能正确，但运行很慢。你的任务：在输出完全不变的前提下，让它尽可能快。',
      '',
      '当前目录里有 workspace.zip。请先自行解压，进入 workspace 文件夹，阅读 TASK.md，然后完全按照 TASK.md 的要求独立完成任务。',
      '',
      '要点（详见 TASK.md）：',
      '- 只能修改 src/；编译命令固定为 g++ -O2 -std=c++20 src/*.cpp，只用 C++ 标准库；',
      '- 对任何合法输入（以 tools/check_input.py --full 为准），输出都必须与基线逐字节相同；SPEC.md 是过时的旧文档，一切以基线代码的实际行为为准；',
      '- 本次是连续 ' + minutes + ' 分钟的单次任务；clock.py 从解压那一刻开始计算，实际计时从对话开始计算，它只能作为参考，请尽快解压；',
      '- 只能读写当前文件夹里的文件；可以使用本环境的所有工具，但不能联网搜索；',
      '- 不要向我提问，也不要等待我确认，直到完成为止。',
      '',
      '完成后请告诉我：src 文件夹的完整路径，并用几句话总结你做了什么。',
    ].join('\n')
  },
}

/** Land bot task: original participant prompt, unchanged. */
export const LAND_DEFINITION: TaskDefinition = {
  id: LAND_TASK,
  label: LAND_TASK_LABEL,
  description: TASKS[2]!.description,
  asset: path.join(packageRoot(), 'assets', 'land', 'workspace.zip'),
  timeLimitMinutes: 90,
  timeLimitEnabled: true,
  needsSetup: false,
  prompt({ minutes }) {
    const suggested = Math.min(60, minutes)
    return [
      '这是一场正式的 AI 编程对抗赛，你的对手是其他顶尖 AI 模型写的 bot，它们会主动切断你、抢你的地。比赛结果会公开排名，请拿出你的最高水平。',
      '',
      '请解压当前目录的 workspace.zip，进入 workspace 文件夹，按 TASK.md 的要求编写一个尽可能强的圈地对战 bot，保存为 workspace/submission/bot.cpp。时间预算约 ' + suggested + ' 分钟，请自己把握进度，做完就可以结束；超过 ' + minutes + ' 分钟会被强制停止。只能读写当前文件夹里的文件，不能联网搜索，不要向我提问。',
    ].join('\n')
  },
}

/** Bomberman bot task: original participant prompt, unchanged. */
export const BOMBERMAN_DEFINITION: TaskDefinition = {
  id: BOMBERMAN_TASK,
  label: BOMBERMAN_TASK_LABEL,
  description: TASKS[3]!.description,
  asset: path.join(packageRoot(), 'assets', 'bomberman', 'workspace.zip'),
  timeLimitMinutes: 90,
  timeLimitEnabled: true,
  needsSetup: false,
  prompt({ minutes }) {
    const suggested = Math.min(60, minutes)
    return [
      '按照要求编写尽可能强bot，你的对手是全球其他顶尖ai，可能主动设计战术。基准只是很弱的bot，它在正式比赛为0分线。正式赛会对战多轮，用Bradley-Terry模型拟合实力值。',
      '解压 workspace.zip，阅读 README.md 和 RULES.md，编写支持2–10人的炸弹人 bot。尽量' + suggested + '分钟完成，' + minutes + '分钟必须交卷。允许本地工具、对战和复盘，禁止联网、调用其他AI或查看其他选手资料。提交 submission/bot.cpp，简述策略。',
    ].join('\n')
  },
}

/** CTF bot task: original participant prompt, unchanged. */
export const CTF_DEFINITION: TaskDefinition = {
  id: CTF_TASK,
  label: CTF_TASK_LABEL,
  description: TASKS[4]!.description,
  asset: path.join(packageRoot(), 'assets', 'ctf', 'workspace.zip'),
  timeLimitMinutes: 90,
  timeLimitEnabled: true,
  needsSetup: false,
  prompt({ minutes }) {
    const suggested = Math.min(60, minutes)
    return [
      '按照要求编写尽可能强的 bot，你的对手是全球其他顶尖 AI，可能主动设计战术。基准是一个中等强度、思路常规的 bot，它在正式比赛是 0 分线；战胜基准不代表足够强，正式对手远强于基准。正式赛会对战多轮，用 Bradley-Terry 模型拟合实力值。',
      '解压 workspace.zip，阅读 README.md 和 RULES.md，编写支持 2–15 方的夺旗 bot。你有 ' + suggested + ' 分钟，只看最终 bot 的强度，提前交卷不加分，请充分利用时间持续改进；可以适当超时，但到 ' + minutes + ' 分钟会强制交卷。允许本地工具、对战和复盘，禁止联网、调用其他 AI 或查看其他选手资料。提交 workspace/submission/bot.cpp，简述策略。',
    ].join('\n')
  },
}

/** Commit0 single-library task. */
export const COMMIT0_DEFINITION: TaskDefinition = {
  id: COMMIT0_TASK,
  label: COMMIT0_TASK_LABEL,
  description: TASKS[5]!.description,
  asset: path.join(packageRoot(), 'assets', 'commit0', 'workspace.zip'),
  testMaterial: ['tests', 'pytest.ini'],
  timeLimitMinutes: 480,
  timeLimitEnabled: false,
  needsSetup: true,
  repoUrl: 'https://github.com/msiemens/tinydb',
  remotes: [{ id: 'github', label: 'GitHub', url: 'https://github.com/msiemens/tinydb.git' }],
  prompt() {
    return [
      '这是 Commit0 的一个单库实现任务：从零实现一个 Python 库，使其通过仓库自带的完整单元测试。',
      '',
      '当前 workspace 已经由安装器准备好：包含该库的测试、规格文档和项目配置，但库的实现源码已被移除。请阅读 TASK.md 和规格文档，自己实现库，直到单元测试全部通过。',
      '',
      '要求：',
      '- 只能修改当前工作区内的文件；工作区外不可访问，包括 /tmp；',
      '- 禁止联网、禁止调用其他 AI 或子代理；',
      '- 使用工作区里的 .venv 运行测试，例如 .venv/bin/python -m pytest tests；',
      '- 不要删除、修改或伪造测试；评分以隐藏的测试执行为准；',
      '- 完成后用几句话总结实现思路。',
    ].join('\n')
  },
}

/** SWE-Bench Pro single-task task. */
export const SWE_BENCH_PRO_DEFINITION: TaskDefinition = {
  id: SWE_BENCH_PRO_TASK,
  label: SWE_BENCH_PRO_TASK_LABEL,
  description: TASKS[6]!.description,
  asset: path.join(packageRoot(), 'assets', 'swe-bench-pro', 'workspace.zip'),
  testMaterial: ['test', 'pytest.ini', 'conftest.py'],
  timeLimitMinutes: 1440,
  timeLimitEnabled: false,
  needsSetup: true,
  repoUrl: 'https://github.com/ansible/ansible',
  remotes: [{ id: 'github', label: 'GitHub', url: 'https://github.com/ansible/ansible.git' }, { id: 'gitee', label: 'Gitee 镜像', url: 'https://gitee.com/mirrors/ansible.git' }],
  prompt() {
    return [
      '这是 SWE-Bench Pro 的一个单任务：在真实的 Python 仓库中完成一个多文件 issue 修复。',
      '',
      '当前 workspace 已经由安装器准备好：包含目标仓库在指定 commit 的源码和依赖环境。请阅读 TASK.md 中的 issue 描述与要求，修改源码完成修复。',
      '',
      '要求：',
      '- 只能修改当前工作区内的文件；工作区外不可访问，包括 /tmp；',
      '- 禁止联网、禁止调用其他 AI 或子代理；',
      '- 使用工作区里的 .venv 运行测试，例如 .venv/bin/python -m pytest <测试文件>；',
      '- 不要删除、修改或伪造测试；评分会在你的代码上应用隐藏测试；',
      '- 完成后用几句话总结修改。',
    ].join('\n')
  },
}

/** FeatureBench single-feature task. */
export const FEATUREBENCH_DEFINITION: TaskDefinition = {
  id: FEATUREBENCH_TASK,
  label: FEATUREBENCH_TASK_LABEL,
  description: TASKS[7]!.description,
  asset: path.join(packageRoot(), 'assets', 'featurebench', 'workspace.zip'),
  testMaterial: ['tests', 'conftest.py'],
  timeLimitMinutes: 1440,
  timeLimitEnabled: false,
  needsSetup: true,
  repoUrl: 'https://github.com/mwaskom/seaborn',
  remotes: [{ id: 'github', label: 'GitHub', url: 'https://github.com/mwaskom/seaborn.git' }, { id: 'gitee', label: 'Gitee 镜像', url: 'https://gitee.com/mirrors/seaborn.git' }, { id: 'gitcode', label: 'GitCode 镜像', url: 'https://gitcode.com/gh_mirrors/se/seaborn.git' }],
  prompt() {
    return [
      '这是 FeatureBench 的一个单 feature 任务：在真实 Python 仓库中实现一个跨文件功能，并保持已有功能不退化。',
      '',
      '当前 workspace 已经由安装器准备好：包含目标仓库源码和依赖环境。请阅读 TASK.md 中的功能需求，修改源码完成实现。',
      '',
      '要求：',
      '- 只能修改当前工作区内的文件；工作区外不可访问，包括 /tmp；',
      '- 禁止联网、禁止调用其他 AI 或子代理；',
      '- 使用工作区里的 .venv 运行测试；',
      '- 不要删除、修改或伪造测试；评分会在你的代码上运行隐藏测试和回归测试；',
      '- 完成后用几句话总结实现思路。',
    ].join('\n')
  },
}

/** Pure C++ tensor backend task. */
export const NEURAL_CPP_DEFINITION: TaskDefinition = {
  id: NEURAL_CPP_TASK,
  label: NEURAL_CPP_TASK_LABEL,
  description: TASKS[8]!.description,
  asset: path.join(packageRoot(), 'assets', 'neural-cpp', 'workspace.zip'),
  timeLimitMinutes: 1440,
  timeLimitEnabled: false,
  needsSetup: true,
  prompt() {
    return [
      '这是一个纯 C++ 张量运算后端任务：从零实现 DType、Tensor、视图/切片、逐元素与矩阵运算、自动求导和线程池；不提供 Python 或 OpenCL 支持。',
      '',
      '当前 workspace 已经由安装器准备好：包含 TASK.md、CMakeLists.txt 和 include/nn/nn.hpp。请阅读 TASK.md 和公开头文件，按接口约定实现 src/ 下的静态库，并用 CMake 构建。',
      '',
      '要求：',
      '- 仅使用 Linux + GCC + C++17 标准库；不允许引入第三方依赖；',
      '- 开发/自测阶段线程数最多 4；评测阶段会使用全部逻辑 CPU；',
      '- 不得修改 include/nn/nn.hpp 的公开接口语义；不要伪造测试或结果；',
      '- 隐藏测试会独立编译，数据由固定种子在测试时生成；',
      '- 完成后用几句话总结实现了哪些部分和性能优化。',
    ].join('\n')
  },
}

/** FP32 operator/performance task. */
export const NEURAL_FP32_DEFINITION: TaskDefinition = {
  id: NEURAL_FP32_TASK,
  label: NEURAL_FP32_TASK_LABEL,
  description: TASKS[9]!.description,
  asset: path.join(packageRoot(), 'assets', 'neural-fp32', 'workspace.zip'),
  timeLimitMinutes: 1440,
  timeLimitEnabled: false,
  needsSetup: true,
  prompt() {
    return [
      '这是一个从零实现的纯 C++ FP32 神经网络算子与性能任务：实现 forward、反向传播、损失和优化器更新，并提供一个固定 CLI 供评测调用；不提供 Python 或 OpenCL 支持。',
      '',
      '当前 workspace 已经由安装器准备好：包含 TASK.md 和 CMakeLists.txt。请阅读 TASK.md 的 CLI 协议，实现 src/ 下的程序，构建出 build/nn_task。',
      '',
      '要求：',
      '- 仅使用 Linux + GCC + C++17 标准库；不允许引入第三方依赖；',
      '- 所有输入输出均为 little-endian float32；',
      '- 开发/自测阶段线程数最多 4；评测阶段会使用全部逻辑 CPU；',
      '- 数值参考由评测侧 PyTorch 在相同数据上计算，不要伪造结果；',
      '- 评测会独立统计每个操作的可执行性、加权平均相对误差和速度；',
      '- 完成后用几句话总结实现了哪些操作和性能优化。',
    ].join('\n')
  },
}

/** CIFAR-10 training task. */
export const NEURAL_CIFAR_DEFINITION: TaskDefinition = {
  id: NEURAL_CIFAR_TASK,
  label: NEURAL_CIFAR_TASK_LABEL,
  description: TASKS[10]!.description,
  asset: path.join(packageRoot(), 'assets', 'neural-cifar', 'workspace.zip'),
  timeLimitMinutes: 1440,
  timeLimitEnabled: false,
  needsSetup: true,
  prompt() {
    return [
      '你是一名世界顶尖的神经网络全栈设计与优化工程师，也是一名以“榨干每一分合法算力”为信条的竞赛选手。任务：从零实现纯 C++17、仅 CPU 的 FP32 训练框架（算子、反向、优化器、固定 CLI），并用同一框架训练 CIFAR-10 图像分类网络。评测会分别报告隐藏验证集准确率、推理性能、以及保留的算子正确性/误差/速度；不提供统一总分，各项权重未知——因此不要把任何一项做成短板。',
      '',
      '请先完整阅读 TASK.md 与 CLI 协议，它是权威规则；在规则模糊处按本提示的保守口径执行，并把理解写入 NOTES.md。硬约束：仅 Linux + GCC + C++17 标准库，固定 fp32，无 Python/OpenCL/CUDA/BLAS/Eigen/第三方库；必须产出 build/nn_task、build/train、build/infer 并留下可加载的 model/；工作区提供 50k train split，评测使用独立隐藏验证集。线程：由你发起的所有运行（开发、自测、算子基准、训练）一律最多 4 个 worker；`infer --threads N` 必须能正确处理不同线程数，但本地自测固定 4；把分块/归约逻辑与线程数解耦，用纯函数单元测试覆盖不同分块的正确性。算子速度不是准确率的竞争者而是共同乘数：更快的 matmul/conv 意味着同样时间能跑更多 epoch，也直接抬高推理 FPS；但任何速度优化都不能牺牲正确性。',
      '',
      '你只需要把全部精力放在把结果做到最好：穷尽优化、持续实验、自我批判。评测由独立数据和外部约束执行，不要让对边界的担心分散注意力；把能复现的结果、命令和文件留在工作区即可。',
      '',
      '工作方式：',
      '0) 先必要算子 + 便宜完整基线：尽快实现让 CIFAR 训练跑通所需的最小算子/反向/优化器/线程池与三个 CLI，短训出 model/，端到端跑通 train→infer，拿到第一个本地 holdout 准确率与单图耗时。先保证可交付，再谈优化。',
      '1) 补全算子正确性（昂贵搜索前必须过门）：用独立 double 标量参考、有限差分梯度检查，以及边界形状/广播/分组/步长/空洞/非方形核/softmax 数值稳定/AdamW step 偏置/N=1/H/W<kernel/fp32 写出等用例，逐算子记录误差到 NOTES.md；每次改 conv/matmul/线程池后重测受影响算子与端到端推理。',
      '2) 持续探索：不断提出实质性不同的架构/训练假设并做可比较的 pilot，一次只改一个主变量；固定可复现的分层 holdout（种子写死），holdout 仅用于评估，不参与训练、增强或归一化统计；把假设、唯一改动、命令、种子、划分、准确率、耗时、算子指标、结论记入 RESULTS.md 排名表。候选扩大训练前先看 pilot 信号；胜出方向继续做单变量消融，最终选择尽量在另一个独立划分/种子上复核。时间无限，持续寻找更高价值的方向；不要跑一个配置就收。',
      '3) 深挖：学习率/调度/weight decay/增强/标签平滑/EMA/网络结构/推理优化（BN 融合、卷积实现、多线程分块、内存复用）；TTA 与集成只在实测准确率收益值得其 FPS 代价时采用；使用 BatchNorm 必须把 running mean/var 固化进 model/ 并在 infer 使用。任何提升若接近噪声，都要结合 5k holdout 抽样误差、随机种子波动和独立复核划分综合判断，不能当成硬阈值；给出准确率-速度 Pareto 权衡再选配置。',
      '4) 锁定与重训：冻结配置后用全部 50k 重训最终权重（自测 holdout 回归训练集）；反复在同一 holdout 上挑选会产生选择性偏差，报告保守期望而非最好一次；CIFAR-10 测试集约 3.3% 与训练集近重复，自测可能偏乐观，按保守口径。长训练必须 checkpoint + resume。',
      '5) 交付审计：清空/新建 build 目录后 cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j4，确认三个二进制从零构建；跑自己的全操作自测（覆盖协议与边界）；用真实图像字节跑 infer（4 线程），核对输出为 N 个 uint8；确认无 NaN/崩溃、无绝对路径、不依赖开发进程或临时构建产物。保留已验证的最佳模型，退步立即回退。',
      '',
      '时间无限，持续优化，不要因为基线跑通就结束；始终保证工作区处于可评测状态：三个 CLI 可构建、model/ 可加载、退步可回滚。能跑通或超过朴素基线不代表够强，正式对手远强于此。',
      '',
      '自我批判：假设自测数字被高估了 0.5%，找出高估来源（holdout 被反复选择、增强/统计泄漏、数据顺序、随机性、口径不一致、单次运气），用不同种子/划分/更严口径复核；主动红队实现（极端与边界形状、大/小数值、eps/溢出、groups/dilation/stride 组合、线程数与确定性、长训练内存与数值稳定性）。',
      '',
      '诚实汇报：完成后给出网络结构与训练方法、自测划分与实测结果、推理 FPS 与单图耗时、算子自测结果与已知风险、留下的命令与文件清单。所有数字必须能由留下的命令复现。',
      '',
      '不要向我提问，也不要等待确认，独立完成。',
    ].join('\n')
  },
}

/** Neural-Pendulum swing-up task. */
export const NEURAL_PENDULUM_DEFINITION: TaskDefinition = {
  id: NEURAL_PENDULUM_TASK,
  label: NEURAL_PENDULUM_TASK_LABEL,
  description: TASKS[11]!.description,
  asset: path.join(packageRoot(), 'assets', 'neural-pendulum', 'workspace.zip'),
  timeLimitMinutes: 1440,
  timeLimitEnabled: false,
  needsSetup: true,
  prompt() {
    return [
      '你是一名控制与神经网络工程师。任务：从零实现纯 C++17、仅 CPU、fp32 的神经网络基础设施，并用它训练一个控制器，控制预编译的三节摆从接近全向下起摆，使三根杆全部进入竖直向上容差并尽可能长时间保持。',
      '',
      '请先完整阅读 TASK.md；它是权威规则。硬约束：仅 C++17 标准库、仅 CPU、fp32；不使用 Python/OpenCL/CUDA/BLAS/Eigen/第三方库；必须从提交源码构建出 build/nn_task 与 build/control。',
      '',
      '环境细节：joint1 与 joint3 驱动，joint2 被动；控制频率 100 Hz；每回合最多 3000 步（30 s），达到上限后继续 step 返回 {"error":"episode_finished"}，必须 reset。theta=0 表示竖直向下、theta=pi 表示竖直向上；观测 a_i=wrap(theta_i-pi)，即三个关节角度（弧度，不除 pi）。环境不返回 reward/loss/cost/done；不提供杆长、质量、惯量、角速度或 tip。训练与评测的每个回合都会随机化物理参数、初始状态和观测噪声；评测在 100 个隐藏随机回合上统计成功率，单回合连续全向上保持 10 s 记成功。',
      '',
      '优化目标：从接近全向下起摆，使三根杆全部进入 0.05 rad 的竖直向上容差并保持；评测指标为 100 个隐藏随机回合上的成功率（每回合连续保持 10 s 记成功），成功率 >= 90% 即完成。损失函数、奖励、优化器、网络结构和训练方法由你自行设计，不规定任何训练方案。',
      '',
      '该任务已由内部基线验证：100 回合成功率 >= 0.90 是可达的，不要怀疑可行性。在 eval_test 返回 stage=completed 之前，绝对不允许停止、总结或放弃；不要因为成功率接近 0.90、遇到平台期或短期没有提升就收尾。只要没有完成，就必须继续训练、改进、提交和测试，直到完成或外部强制停止。',
      '',
      '最终提交的 build/control 必须由神经网络输出层直接产生两个力矩；最终控制程序中不得包含任何显式控制律（PID/LQR/MPC/滑模/自抗扰等），也不得在神经网络之外根据状态计算或修正力矩。',
      '',
      '不得对已编译文件进行任何形式的逆向、反汇编、strings/objdump/readelf/gdb 分析，也不得读取 /proc 或其它进程内存。环境信息只能来自 TASK.md/SPEC.md 和正常使用 tools/sim 的 reset/step 输入输出；违规者成绩无效。',
      '',
      '后台任务管理：长时间实验必须使用宿主管理的 run_in_background，禁止 nohup、setsid、disown 或尾随 & 脱管。启动任何长任务前先 job_list；同时最多保留 2 个后台任务；同一实验不要重复启动；读完日志且不再需要后立即 job_kill 或用受控方式清理。每约 5 分钟，下一次工具结果会附带当前 running 后台任务和本工作区 OS 进程提醒；看到提醒必须立即处理，不要等用户提醒。开始新方向、提交或最终回答前，必须清理所有不用的 job、训练进程和 tools/sim，确认没有残留。',
      '',
      '如果当前方向失败，必须记录失败原因、清理无用后台任务，并换一个实质性不同的方向继续；不要重复同一配置，也不要因为一次失败就断定任务不可做。只有 stage=completed 或外部手动停止才是结束条件。',
      '',
      '注意：评测侧用 PyTorch 计算算子参考值，但工作区/候选运行时不提供 PyTorch 或 numpy，也没有网络，无法 pip 安装；开发阶段请用 C++ 或 Python 标准库/独立 double 实现做数值对照。',
      '',
      '评测会从干净副本重新构建提交源码，先运行与 Neural-FP32/Neural-CIFAR 相同的算子数值评测：评测侧用 PyTorch 在相同数据上计算参考值，逐操作比较加权平均相对误差并对照阈值，任一算子不可执行、崩溃、超时、输出缺失或误差超阈值即判为基础设施失败；然后在 100 个随机隐藏回合上驱动 build/control，统计成功率（单回合连续全向上保持 10 s 记成功）。不要把任何未提交的二进制、临时构建产物或外部进程当作交付依赖。',
      '使用宿主工具 eval_submit 提交当前源码快照，使用 eval_test 请求宿主评测最新提交。提交快照、事件、测试结果和最终分数全部保存在工作区之外的宿主目录；不要尝试读取或修改这些数据，也不要依赖任何工作区内的测试结果文件。最终评测使用最新提交快照，按宿主事件时间积分计算分数。',
      '',
      '工作方式：先完成最小可构建的基础设施和 control 协议；再持续做控制/训练实验并记录可复现结果；最终保证工作区从零构建、build/nn_task 算子数值评测通过、control 可被评测驱动，并留下 NOTES.md/RESULTS.md。',
      '',
      '不要向我提问，也不要等待确认，独立完成。',
    ].join('\n')
  },
}

/** All task definitions keyed by id. */
export const TASK_DEFINITIONS: Readonly<Record<string, TaskDefinition>> = {
  [TANK_TASK]: TANK_DEFINITION,
  [WAREHOUSE_TASK]: WAREHOUSE_DEFINITION,
  [LAND_TASK]: LAND_DEFINITION,
  [BOMBERMAN_TASK]: BOMBERMAN_DEFINITION,
  [CTF_TASK]: CTF_DEFINITION,
  [COMMIT0_TASK]: COMMIT0_DEFINITION,
  [SWE_BENCH_PRO_TASK]: SWE_BENCH_PRO_DEFINITION,
  [FEATUREBENCH_TASK]: FEATUREBENCH_DEFINITION,
  [NEURAL_CPP_TASK]: NEURAL_CPP_DEFINITION,
  [NEURAL_FP32_TASK]: NEURAL_FP32_DEFINITION,
  [NEURAL_CIFAR_TASK]: NEURAL_CIFAR_DEFINITION,
  [NEURAL_PENDULUM_TASK]: NEURAL_PENDULUM_DEFINITION,
}

/** SHA-256 of one file. */
function sha256File(file: string): string {
  const hash = createHash('sha256')
  hash.update(fs.readFileSync(file))
  return hash.digest('hex')
}

/** Order-stable SHA-256 of a directory tree (relative paths + contents). */
function sha256Tree(dir: string): string {
  const hash = createHash('sha256')
  const walk = (current: string): void => {
    for (const entry of fs.readdirSync(current, { withFileTypes: true }).sort((a, b) => a.name.localeCompare(b.name))) {
      const full = path.join(current, entry.name)
      hash.update(path.relative(dir, full))
      if (entry.isDirectory()) walk(full)
      else if (entry.isFile()) hash.update(fs.readFileSync(full))
      else if (entry.isSymbolicLink()) hash.update('link:' + fs.readlinkSync(full))
    }
  }
  walk(dir)
  return hash.digest('hex')
}

/** The venv site-packages directories that exist under one workspace venv. */
function sitePackageDirs(venv: string): string[] {
  const out: string[] = []
  for (const lib of ['lib', 'lib64']) {
    const base = path.join(venv, lib)
    let entries: fs.Dirent[] = []
    try { entries = fs.readdirSync(base, { withFileTypes: true }) } catch { continue }
    for (const entry of entries) {
      if (!entry.isDirectory() || !entry.name.startsWith('python')) continue
      const site = path.join(base, entry.name, 'site-packages')
      if (fs.existsSync(site)) out.push(site)
    }
  }
  return out
}

/** Install, check, and prompt services for the task packs. */
export interface SetupOptions {
  cloneSource?: string
  customCloneUrl?: string
  pipIndex?: string
  customPipUrl?: string
  gitProxyUrl?: string
  pipProxyUrl?: string
  cifarProxyUrl?: string
  cifarMirror?: string
  customCifarUrl?: string
}

export class TaskService {
  private readonly guard: Guard
  private readonly registry: { get(id: string): { path: string; title?: string } | undefined }
  readonly jobs: JobManager

  constructor(guard: Guard, registry: { get(id: string): { path: string; title?: string } | undefined }) {
    this.guard = guard
    this.registry = registry
    this.jobs = new JobManager()
  }


  /** Host-side directory for one workspace's authoritative task state. */
  private hostDir(workspaceId: string): string {
    return path.join(guardRoot(), 'neural-pendulum', workspaceId.replace(/[^A-Za-z0-9_.-]/g, '_'))
  }

  private hostEvents(workspaceId: string): string {
    return path.join(this.hostDir(workspaceId), 'events.jsonl')
  }

  private appendHostEvent(workspaceId: string, event: Record<string, unknown>): void {
    const dir = this.hostDir(workspaceId)
    fs.mkdirSync(dir, { recursive: true })
    fs.appendFileSync(this.hostEvents(workspaceId), JSON.stringify(event) + '\n', 'utf8')
  }

  private hostLatestSubmission(workspaceId: string): { id: string; path: string } | undefined {
    const root = path.join(this.hostDir(workspaceId), 'submissions')
    if (!fs.existsSync(root)) return undefined
    const dirs = fs.readdirSync(root, { withFileTypes: true }).filter(entry => entry.isDirectory())
    if (dirs.length === 0) return undefined
    const id = dirs.map(entry => entry.name).sort((a, b) => {
      const am = fs.statSync(path.join(root, a)).mtimeMs
      const bm = fs.statSync(path.join(root, b)).mtimeMs
      return bm - am
    })[0]!
    return { id, path: path.join(root, id) }
  }

  private copySnapshot(source: string, destination: string): number {
    const excluded = new Set([
      'build', '.git', '.eval', '.submissions', 'node_modules', '__pycache__', '.venv',
      // Sandbox-private runtime dirs are host state, not submitted source.
      '.tmp', '.home', '.cache', '.config', '.local', 'data', 'work',
    ])
    let count = 0
    const walk = (dir: string): void => {
      for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
        if (excluded.has(entry.name)) continue
        const from = path.join(dir, entry.name)
        const to = path.join(destination, path.relative(source, from))
        if (entry.isDirectory()) {
          fs.mkdirSync(to, { recursive: true })
          walk(from)
        } else if (entry.isFile()) {
          fs.mkdirSync(path.dirname(to), { recursive: true })
          fs.copyFileSync(from, to)
          count++
        }
      }
    }
    fs.mkdirSync(destination, { recursive: true })
    walk(source)
    return count
  }

  /** Host-side submit: snapshot the workspace into the run directory. */
  hostSubmit(run: RunRecord): { ok: boolean; submission?: string; files?: number; error?: string } {
    const done = path.join(this.hostDir(run.workspaceId), 'done')
    if (fs.existsSync(done)) return { ok: false, error: 'task already completed' }
    const id = Date.now().toString(36) + '-' + Math.random().toString(36).slice(2, 8)
    const metaPath = path.join(this.hostDir(run.workspaceId), 'meta.json')
    if (!fs.existsSync(metaPath)) {
      fs.mkdirSync(this.hostDir(run.workspaceId), { recursive: true })
      fs.writeFileSync(metaPath, JSON.stringify({ started_at: run.createdAt / 1000, t_tgt: 10 }), 'utf8')
    }
    const destination = path.join(this.hostDir(run.workspaceId), 'submissions', id)
    if (fs.existsSync(destination)) return { ok: false, error: 'submission id collision' }
    const files = this.copySnapshot(run.workspacePath, destination)
    const hash = createHash('sha256')
    const hashDir = (dir: string): void => {
      for (const entry of fs.readdirSync(dir, { withFileTypes: true }).sort((a, b) => a.name.localeCompare(b.name))) {
        const full = path.join(dir, entry.name)
        if (entry.isDirectory()) hashDir(full)
        else if (entry.isFile()) {
          hash.update(path.relative(destination, full))
          hash.update(fs.readFileSync(full))
        }
      }
    }
    hashDir(destination)
    this.appendHostEvent(run.workspaceId, {
      event: 'submit', time: Date.now() / 1000, submission_id: id,
      source_hash: 'sha256:' + hash.digest('hex'), files,
    })
    return { ok: true, submission: id, files }
  }

  /** Map an evaluator result that has no explicit stage (older evaluator builds). */
  private inferStage(value: Record<string, unknown>, error: string): string {
    if (value.infra_ok === true) return value.success === true ? 'completed' : 'scoring'
    const text = error.toLowerCase()
    if (text.includes('cmake') || text.includes('build')) return 'compile'
    if (text.includes('infrastructure') || text.includes('operator') || text.includes('mre')) return 'infra'
    if (text.includes('controller') || text.includes('torque')) return 'controller'
    return 'simulation'
  }

  /** Evaluation success-rate target used by the neural-pendulum scoring integral. */
  private static readonly FINAL_SUCCESS_TARGET = 0.9

  /** One test event's instantaneous score, mirroring the task specification. */
  private static eventScore(event: Record<string, unknown>): number {
    const stage = String(event['stage'] ?? '')
    if (stage !== 'scoring' && stage !== 'completed' && event['valid'] !== true) return -1
    const rate = Number(event['success_rate'])
    if (Number.isFinite(rate)) {
      return (Math.min(Math.max(rate, 0), TaskService.FINAL_SUCCESS_TARGET)
        - TaskService.FINAL_SUCCESS_TARGET) / TaskService.FINAL_SUCCESS_TARGET
    }
    // Backward-compatible fallback for old single-episode events.
    const value = Number(event['t_can'] ?? 0)
    if (!Number.isFinite(value)) return -1
    return (Math.min(Math.max(value, 0), 10) - 10) / 10
  }

  /**
   * Host-side authoritative final score: integral of the per-test score over
   * time, applying every test event at its REQUEST time and stopping at the
   * first completed request. Mirrors neural-pendulum-eval/run_eval.py, but the
   * host owns it because only the host knows the request timestamp and the
   * current test's outcome before the next test arrives.
   */
  private static finalScoreFromEvents(
    startedAt: number,
    events: readonly Record<string, unknown>[],
    now: number,
  ): number {
    const tests = events.filter(item => (item['event'] ?? item['type']) === 'test')
    if (tests.length === 0) return -1 * Math.max(0, now - startedAt)
    let doneAt: number | undefined
    let previous = startedAt
    let current = -1
    let total = 0
    for (const item of tests) {
      const raw = Number(item['time'] ?? item['at'] ?? previous)
      const at = Number.isFinite(raw) ? Math.max(raw, previous) : previous
      total += current * (at - previous)
      current = TaskService.eventScore(item)
      previous = at
      if (item['completed'] === true) {
        doneAt = at
        break
      }
    }
    const end = doneAt ?? now
    total += current * Math.max(0, end - previous)
    return total
  }

  /** Read host test events; malformed lines are ignored. */
  private readHostEvents(workspaceId: string): Record<string, unknown>[] {
    const file = this.hostEvents(workspaceId)
    if (!fs.existsSync(file)) return []
    const out: Record<string, unknown>[] = []
    for (const line of fs.readFileSync(file, 'utf8').split('\n')) {
      if (line.trim() === '') continue
      try {
        const value = JSON.parse(line) as unknown
        if (value !== null && typeof value === 'object' && !Array.isArray(value)) out.push(value as Record<string, unknown>)
      } catch { /* skip corrupt line */ }
    }
    return out
  }

  /** The task start time recorded at first submission, falling back to the run creation time. */
  /** Whether this run has reached the completed state. */
  isCompleted(run: RunRecord): boolean {
    try {
      return fs.existsSync(path.join(this.hostDir(run.workspaceId), 'done'))
    } catch {
      return false
    }
  }

  /** The latest host test event, if any. */
  lastTest(run: RunRecord): Record<string, unknown> | undefined {
    try {
      const file = this.hostEvents(run.workspaceId)
      if (!fs.existsSync(file)) return undefined
      let last: Record<string, unknown> | undefined
      for (const line of fs.readFileSync(file, 'utf8').split('\n')) {
        if (line.trim() === '') continue
        try {
          const event = JSON.parse(line) as Record<string, unknown>
          if (event['event'] === 'test') last = event
        } catch { /* ignore malformed history */ }
      }
      return last
    } catch {
      return undefined
    }
  }

  private hostStartedAt(workspaceId: string, run: RunRecord): number {
    try {
      const meta = JSON.parse(fs.readFileSync(path.join(this.hostDir(workspaceId), 'meta.json'), 'utf8')) as { started_at?: unknown }
      const value = Number(meta.started_at)
      if (Number.isFinite(value) && value > 0) return value
    } catch { /* fall through */ }
    return run.createdAt / 1000
  }

  /** @param startedAtSeconds - task start in epoch seconds (from host meta). */
  private async runHostEval(workspaceId: string, startedAtSeconds: number): Promise<Record<string, unknown>> {
    const latest = this.hostLatestSubmission(workspaceId)
    if (latest === undefined) return { stage: 'no_submission', t_can: 0, completed: false, error: '', final_score: 0 }
    const temp = fs.mkdtempSync(path.join(guardRoot(), 'pend-eval-'))
    try {
      const submissions = path.join(temp, '.eval', 'submissions')
      fs.mkdirSync(submissions, { recursive: true })
      fs.cpSync(latest.path, path.join(submissions, latest.id), { recursive: true })
      const events = this.hostEvents(workspaceId)
      fs.writeFileSync(path.join(temp, '.eval', 'events.jsonl'), fs.existsSync(events) ? fs.readFileSync(events) : '', 'utf8')
      // The time integral starts at the first submission, not at each evaluation.
      fs.writeFileSync(path.join(temp, '.eval', 'meta.json'), JSON.stringify({ started_at: startedAtSeconds, t_tgt: 10 }), 'utf8')
      const resultFile = path.join(temp, 'result.json')
      const script = path.join(packageRoot(), 'neural-pendulum-eval', 'run_eval.py')
      const env = { ...baseEnvironment(), EVAL_RESULT_FILE: resultFile }
      // detached: true gives the evaluator its own process group so a timeout
      // can take its build/control/simulator descendants down with it.
      const child = spawn('python3', ['-X', 'utf8', script, '--workspace', temp], { env, detached: true })
      let stderr = ''
      child.stderr?.on('data', chunk => { stderr += String(chunk) })
      let timedOut = false
      await new Promise<void>((resolve, reject) => {
        const timer = setTimeout(() => {
          timedOut = true
          try { if (child.pid !== undefined) process.kill(-child.pid, 'SIGKILL') } catch { /* already gone */ }
          try { child.kill('SIGKILL') } catch { /* already gone */ }
          resolve()
        }, 20 * 60_000)
        child.on('error', error => { clearTimeout(timer); reject(error) })
        child.on('close', () => { clearTimeout(timer); resolve() })
      })
      if (timedOut) return { stage: 'infra', t_can: 0, completed: false, error: 'evaluation timed out after 20 minutes', final_score: 0 }
      if (!fs.existsSync(resultFile)) return { stage: 'infra', t_can: 0, completed: false, error: stderr || 'evaluator failed', final_score: 0 }
      const value = JSON.parse(fs.readFileSync(resultFile, 'utf8')) as Record<string, unknown>
      const error = String(value.error ?? '')
      const rawStage = String(value.stage ?? '')
      const stage = rawStage !== '' ? rawStage : this.inferStage(value, error)
      const tCan = Number(value.t_can ?? 0)
      const successRate = Number(value.success_rate ?? 0)
      const finalScore = Number(value.final_score ?? 0)
      return {
        stage,
        t_can: Number.isFinite(tCan) ? tCan : 0,
        success_rate: Number.isFinite(successRate) ? successRate : 0,
        episodes: Number(value.episodes ?? 0),
        completed: stage === 'completed' && Number.isFinite(successRate)
          && successRate >= TaskService.FINAL_SUCCESS_TARGET,
        error,
        final_score: Number.isFinite(finalScore) ? finalScore : 0,
      }
    } finally {
      fs.rmSync(temp, { recursive: true, force: true })
    }
  }

  /** Host-side test: evaluate the latest host submission and write the event. */
  async hostTest(run: RunRecord): Promise<Record<string, unknown>> {
    const done = path.join(this.hostDir(run.workspaceId), 'done')
    if (fs.existsSync(done)) return { ok: false, stage: 'completed', error: 'task already completed' }
    // The score changes at the REQUEST instant, not when the (possibly
    // minutes-long) evaluation finishes.
    const requestedAt = Date.now() / 1000
    const latest = this.hostLatestSubmission(run.workspaceId)
    if (latest === undefined) {
      this.appendHostEvent(run.workspaceId, { event: 'test', time: requestedAt, submission_id: '',
        stage: 'no_submission', t_can: 0, success_rate: 0, episodes: 0,
        completed: false, error: '' })
      return { ok: true, stage: 'no_submission', t_can: 0, completed: false, error: '' }
    }
    const startedAt = this.hostStartedAt(run.workspaceId, run)
    const value = await this.runHostEval(run.workspaceId, startedAt)
    const stage = String(value.stage ?? 'simulation')
    const tCan = Number(value.t_can ?? 0)
    const successRate = Number(value.success_rate ?? 0)
    const episodes = Number(value.episodes ?? 0)
    const completed = stage === 'completed' && Number.isFinite(successRate)
      && successRate >= TaskService.FINAL_SUCCESS_TARGET
    const event: Record<string, unknown> = {
      event: 'test', time: requestedAt, submission_id: latest.id,
      stage, t_can: tCan, success_rate: Number.isFinite(successRate) ? successRate : 0,
      episodes: Number.isFinite(episodes) ? episodes : 0,
      completed, error: String(value.error ?? ''),
    }
    // The evaluator only sees the events written before this test, so its
    // final_score omits the current outcome. Recompute the integral with the
    // current test event included; this is the value the ranking stores.
    const finalScore = TaskService.finalScoreFromEvents(
      startedAt,
      [...this.readHostEvents(run.workspaceId), event],
      Date.now() / 1000,
    )
    event['final_score'] = finalScore
    this.appendHostEvent(run.workspaceId, event)
    if (completed) {
      fs.writeFileSync(done, JSON.stringify({ at: requestedAt, stage, t_can: tCan,
        success_rate: Number.isFinite(successRate) ? successRate : 0,
        episodes: Number.isFinite(episodes) ? episodes : 0, final_score: finalScore }) + '\n', 'utf8')
    }
    return { ok: true, stage, t_can: tCan, success_rate: Number.isFinite(successRate) ? successRate : 0,
      episodes: Number.isFinite(episodes) ? episodes : 0,
      completed, error: String(value.error ?? '') }
  }

  /** Build a temporary workspace containing the host-side authoritative state. */
  stageHostWorkspace(workspaceId: string): string | undefined {
    // Only the latest snapshot is staged: copying every submission makes the
    // evaluator's mtime-based "latest" pick ambiguous (cpSync does not
    // preserve timestamps).
    const latest = this.hostLatestSubmission(workspaceId)
    if (latest === undefined) return undefined
    const temp = fs.mkdtempSync(path.join(guardRoot(), 'pend-eval-view-'))
    const state = path.join(temp, '.eval')
    fs.mkdirSync(path.join(state, 'submissions'), { recursive: true })
    fs.cpSync(latest.path, path.join(state, 'submissions', latest.id), { recursive: true })
    const events = this.hostEvents(workspaceId)
    fs.writeFileSync(path.join(state, 'events.jsonl'), fs.existsSync(events) ? fs.readFileSync(events) : '', 'utf8')
    const meta = path.join(this.hostDir(workspaceId), 'meta.json')
    let fallbackStart = Date.now() / 1000
    try {
      for (const line of (fs.existsSync(events) ? fs.readFileSync(events, 'utf8') : '').split('\n')) {
        if (line.trim() === '') continue
        const time = Number((JSON.parse(line) as { time?: number }).time)
        if (Number.isFinite(time) && time > 0) { fallbackStart = time; break }
      }
    } catch { /* keep now */ }
    fs.writeFileSync(path.join(state, 'meta.json'), fs.existsSync(meta) ? fs.readFileSync(meta) : JSON.stringify({ started_at: fallbackStart, t_tgt: 10 }), 'utf8')
    return temp
  }


  /** Bind every main conversation to the configured active task, no install needed. */
  autoBind(agent: { id: string }, cwd: string): boolean {
    if (!this.guard.active) return false
    const taskId = this.guard.configuration.taskId
    if (taskId === '') return false
    const definition = TASK_DEFINITIONS[taskId]
    if (definition === undefined) return false
    const list = (this.registry as unknown as { list?: () => { id: string; path: string }[] }).list?.() ?? []
    const matched = list.find(workspace => path.resolve(workspace.path) === path.resolve(cwd))
    const workspaceId = matched?.id ?? 'path-' + createHash('sha1').update(cwd).digest('hex').slice(0, 16)
    const runId = this.newRunId()
    const prompt = this.promptNotice(this.guard.promptFor(cwd, this.materialZip(taskId), definition))
    const run: RunRecord = {
      runId, task: definition.id, workspaceId, workspacePath: cwd,
      sessionId: null, createdAt: Date.now(), startedAt: null, endedAt: null,
      status: 'prepared', prompt,
      timeLimitMinutes: this.guard.configuration.timeLimitMinutes,
    }
    this.guard.registerRun(run)
    this.guard.attachRun(agent as never, run)
    return true
  }

  /** The clone URL for a task, honouring the selected source. */
  cloneUrl(definition: TaskDefinition, options: SetupOptions): string {
    const repo = definition.repoUrl ?? ''
    const source = options.cloneSource ?? 'github'
    if (source === 'custom' && options.customCloneUrl !== undefined && options.customCloneUrl.trim() !== '') {
      return options.customCloneUrl.trim().replace(/\/$/, '') + '/' + repo.replace(/^https?:\/\/github\.com\//, '')
    }
    const remote = definition.remotes?.find(item => item.id === source)
    if (remote !== undefined) return remote.url
    return definition.remotes?.[0]?.url ?? repo
  }

  /** Normalize a user-entered proxy address, or return an empty string. */
  proxyValue(raw: string | undefined): string {
    const value = (raw ?? '').trim()
    if (value === '') return ''
    return /^[a-z][a-z0-9+.-]*:\/\//i.test(value) ? value : 'http://' + value
  }

  /** The CIFAR-10 binary archive URL for the selected mirror. */
  cifarUrl(options: SetupOptions): string {
    const mirror = options.cifarMirror ?? 'toronto'
    if (mirror === 'custom') return (options.customCifarUrl ?? '').trim() || CIFAR_MIRRORS[0]!.url
    return CIFAR_MIRRORS.find(item => item.id === mirror)?.url ?? CIFAR_MIRRORS[0]!.url
  }

  /** The pip index URL for a task, honouring the selected mirror. */
  pipIndexUrl(options: SetupOptions): string {
    const index = options.pipIndex ?? 'pypi'
    if (index === 'custom') return (options.customPipUrl ?? '').trim() || 'https://pypi.org/simple'
    return PIP_MIRRORS.find(item => item.id === index)?.url ?? 'https://pypi.org/simple'
  }

  /** Host-side pristine material for one workspace (tests, venv manifest). */
  baselineDir(workspaceId: string): string {
    return path.join(guardRoot(), 'task-baseline', workspaceId.replace(/[^A-Za-z0-9_.-]/g, '_'))
  }

  /**
   * Snapshot the test material a benchmark task ships with, so an agent cannot
   * pass by editing or weakening the tests. Runs once after a successful
   * install; the evaluator restores it before scoring.
   */
  private snapshotTestMaterial(definition: TaskDefinition, workspaceId: string, workspacePath: string): void {
    const names = definition.testMaterial ?? []
    if (names.length === 0) return
    const target = this.baselineDir(workspaceId)
    fs.mkdirSync(target, { recursive: true })
    for (const name of names) {
      const source = path.join(workspacePath, name)
      if (!fs.existsSync(source)) continue
      const destination = path.join(target, name)
      fs.rmSync(destination, { recursive: true, force: true })
      fs.cpSync(source, destination, { recursive: true, dereference: false })
    }
    fs.writeFileSync(path.join(target, 'manifest.json'), JSON.stringify({ task: definition.id, names }, null, 2) + '\n', 'utf8')
  }

  /**
   * Record a fingerprint of the venv's control plane (python target, pyvenv.cfg,
   * .pth files, pytest packages). The evaluator refuses to trust a venv whose
   * fingerprint no longer matches, which closes the "replace bin/python or
   * patch pytest" forgery path.
   */
  private snapshotVenv(workspaceId: string, workspacePath: string): void {
    const venv = path.join(workspacePath, '.venv')
    if (!fs.existsSync(venv)) return
    const target = this.baselineDir(workspaceId)
    fs.mkdirSync(target, { recursive: true })
    const manifest: { python?: string; files: Record<string, string>; pytest?: string } = { files: {} }
    const python = path.join(venv, 'bin', 'python')
    try { manifest.python = fs.realpathSync.native(python) } catch { /* missing */ }
    const cfg = path.join(venv, 'pyvenv.cfg')
    if (fs.existsSync(cfg)) manifest.files['pyvenv.cfg'] = sha256File(cfg)
    for (const site of sitePackageDirs(venv)) {
      for (const entry of fs.readdirSync(site, { withFileTypes: true })) {
        if (!entry.isFile() || !entry.name.endsWith('.pth')) continue
        const file = path.join(site, entry.name)
        manifest.files[path.relative(venv, file)] = sha256File(file)
      }
      const pytest = path.join(site, 'pytest')
      if (fs.existsSync(pytest)) manifest.pytest = sha256Tree(pytest)
    }
    fs.writeFileSync(path.join(target, 'venv.json'), JSON.stringify(manifest, null, 2) + '\n', 'utf8')
  }

  /** Check one workspace before install. */
  check(workspaceId: string): CheckResult {
    const workspace = this.registry.get(workspaceId)
    if (workspace === undefined) return { ok: false, workspaceId, path: '', empty: false, entries: [], error: '找不到这个工作区。' }
    let entries: string[] = []
    try {
      entries = fs.readdirSync(workspace.path)
    } catch (error: unknown) {
      return { ok: false, workspaceId, path: workspace.path, empty: false, entries: [], error: '读取工作区失败：' + (error instanceof Error ? error.message : String(error)) }
    }
    return { ok: true, workspaceId, path: workspace.path, empty: entries.length === 0, entries }
  }

  /** A known task definition, or undefined. */
  definition(taskId: string): TaskDefinition | undefined {
    return TASK_DEFINITIONS[taskId]
  }

  /** Extract a setup task's workspace.zip into the workspace root. */
  private extractMaterial(workspacePath: string): void {
    const zip = path.join(workspacePath, 'workspace.zip')
    if (!fs.existsSync(zip)) throw new Error('workspace.zip is missing')
    const temp = fs.mkdtempSync(path.join(guardRoot(), 'extract-'))
    try {
      execFileSync('unzip', ['-q', zip, '-d', temp], { stdio: 'pipe' })
      const inner = path.join(temp, 'workspace')
      const source = fs.existsSync(inner) ? inner : temp
      for (const entry of fs.readdirSync(source)) {
        const destination = path.join(workspacePath, entry)
        if (fs.existsSync(destination)) fs.rmSync(destination, { recursive: true, force: true })
        fs.renameSync(path.join(source, entry), destination)
      }
      fs.rmSync(zip, { force: true })
    } finally {
      fs.rmSync(temp, { recursive: true, force: true })
    }
  }

  /** Remove install-only artifacts so they are not part of the agent workspace. */
  private cleanupWorkspace(workspacePath: string): void {
    for (const name of ['.git', '.repo', 'setup.sh', 'remove.patch', 'spec.pdf.bz2', 'cifar_cache.py']) {
      // setup.sh removes spec.pdf.bz2 itself when present; keep spec.pdf as task material.
      try { fs.rmSync(path.join(workspacePath, name), { recursive: true, force: true }) } catch { /* best effort */ }
    }
  }

  /** Remove every entry created by an install that never reached success. */
  private wipeInstallWorkspace(workspacePath: string): void {
    try {
      for (const entry of fs.readdirSync(workspacePath)) {
        fs.rmSync(path.join(workspacePath, entry), { recursive: true, force: true })
      }
    } catch { /* best effort */ }
  }

  /** Remove the shared CIFAR lock only when it belongs to this install. */
  private cleanupCifarLock(token: string): void {
    const lock = path.join(dshHome(), 'eval-guard', 'cache', 'cifar10', '.download.lock')
    try {
      if (!fs.existsSync(lock)) return
      const body = fs.readFileSync(lock, 'utf8')
      if (body.includes('token=' + token)) fs.rmSync(lock, { force: true })
    } catch { /* best effort */ }
  }

  /** Start a non-blocking install. The settings page polls the returned job. */
  startInstall(workspaceId: string, taskId: string, options: SetupOptions = {}): { ok: boolean; jobId?: string; error?: string } {
    const definition = TASK_DEFINITIONS[taskId]
    if (definition === undefined) return { ok: false, error: '未知任务：' + taskId }
    const checked = this.check(workspaceId)
    if (!checked.ok) return { ok: false, error: checked.error }
    if (!checked.empty) return { ok: false, error: '工作区不是空的；未执行任何操作。' }
    if (!this.guard.active) return { ok: false, error: '请先启用评测守护，再安装评测任务。' }
    const initiallyEmpty = checked.empty
    const installToken = 'install-' + Date.now().toString(36) + '-' + Math.random().toString(36).slice(2, 10)
    const cleanupInstall = (): void => {
      // A successful install can make the workspace non-empty; only the
      // snapshot taken before install decides whether cleanup is allowed.
      if (!initiallyEmpty) return
      this.wipeInstallWorkspace(checked.path)
      this.cleanupCifarLock(installToken)
    }
    try {
      fs.copyFileSync(this.materialZip(taskId), path.join(checked.path, 'workspace.zip'))
      if (definition.needsSetup) this.extractMaterial(checked.path)
    } catch (error: unknown) {
      cleanupInstall()
      return { ok: false, error: '复制素材失败：' + (error instanceof Error ? error.message : String(error)) }
    }
    const env: Record<string, string> = {
      EVAL_INSTALL_TOKEN: installToken,
      EVAL_CLONE_URL: this.cloneUrl(definition, options),
      EVAL_PIP_INDEX_URL: this.pipIndexUrl(options),
      GIT_TERMINAL_PROMPT: '0',
      EVAL_GIT_PROXY: this.proxyValue(options.gitProxyUrl),
      EVAL_PIP_PROXY: this.proxyValue(options.pipProxyUrl),
      EVAL_CIFAR_PROXY: this.proxyValue(options.cifarProxyUrl),
      EVAL_CIFAR_URL: this.cifarUrl(options),
    }
    const command = definition.needsSetup ? ['bash', 'setup.sh'] : ['bash', '-c', 'true']
    const job = this.jobs.start({
      kind: 'install', task: taskId, workspaceId, command, cwd: checked.path, env,
      cleanupWorkspace: checked.path,
      cifarToken: installToken,
      onCancel: cleanupInstall,
      onFailure: cleanupInstall,
      onSuccess: () => {
        if (definition.needsSetup) this.cleanupWorkspace(checked.path)
        try {
          this.snapshotTestMaterial(definition, workspaceId, checked.path)
          this.snapshotVenv(workspaceId, checked.path)
        } catch { /* pristine snapshots are best effort; the evaluator reports a missing baseline */ }
        const prompt = this.promptNotice(this.guard.promptFor(checked.path, this.materialZip(taskId), definition))
        return { prompt }
      },
    })
    if (job.status === 'failed') cleanupInstall()
    return { ok: true, jobId: job.id }
  }

  /** Start a single-workspace evaluation job. */
  startEval(workspaceId: string, taskId: string): { ok: boolean; jobId?: string; error?: string } {
    const definition = TASK_DEFINITIONS[taskId]
    if (definition === undefined) return { ok: false, error: '未知任务：' + taskId }
    const checked = this.check(workspaceId)
    if (!checked.ok) return { ok: false, error: checked.error }
    if (!this.guard.active) return { ok: false, error: '请先启用评测守护。' }
    const script = path.join(packageRoot(), 'eval', taskId + '.py')
    if (!fs.existsSync(script)) return { ok: false, error: '评测脚本不存在：' + script }
    const hostView = taskId === NEURAL_PENDULUM_TASK ? this.stageHostWorkspace(workspaceId) : undefined
    const evalWorkspace = hostView ?? checked.path
    const cleanupHostView = (): void => {
      if (hostView === undefined) return
      try { fs.rmSync(hostView, { recursive: true, force: true }) } catch { /* best effort */ }
    }
    const baseline = this.baselineDir(workspaceId)
    const baselineEnv: Record<string, string> = fs.existsSync(baseline) ? { EVAL_BASELINE_DIR: baseline } : {}
    const job = this.jobs.start({
      kind: 'eval', task: taskId, workspaceId,
      command: ['python3', '-X', 'utf8', script, '--workspace', evalWorkspace],
      cwd: packageRoot(),
      env: {
        PYTHONPATH: '', PYTHONHOME: '',
        EVAL_CIFAR_PROXY: this.proxyValue(this.guard.configuration.cifarProxyUrl),
        EVAL_CIFAR_URL: this.cifarUrl(this.guard.configuration),
        ...baselineEnv,
      },
      onSuccess: cleanupHostView,
      onCancel: cleanupHostView,
      onFailure: cleanupHostView,
    })
    if (job.status === 'failed') {
      cleanupHostView()
      return { ok: false, error: job.error ?? '评测启动失败' }
    }
    return { ok: true, jobId: job.id }
  }

  /** Add the optional shared-machine notice to a task prompt. */
  private promptNotice(prompt: string): string {
    if (!this.guard.configuration.parallelEval) return prompt
    return prompt + '\n\n' +
      '注意：当前机器可能同时运行其他 agent 的评测/训练任务。运行环境的 CPU、内存、IO 与可用并发会因其他 agent 的行为而波动；请自行适应这种波动，不要假设环境稳定，也不要依赖固定墙钟时间或固定可用资源来判断进度。'
  }

  /** Copy the packaged workspace.zip to a stable material path for install/manual use. */
  materialZip(taskId: string = TANK_TASK): string {
    const definition = TASK_DEFINITIONS[taskId]
    if (definition === undefined) throw new Error('unknown task: ' + taskId)
    const dir = path.join(guardRoot(), 'material', taskId)
    fs.mkdirSync(dir, { recursive: true })
    const target = path.join(dir, 'workspace.zip')
    const source = definition.asset
    if (!fs.existsSync(source)) throw new Error('task asset is missing: ' + source)
    let stale = true
    try {
      stale = !fs.existsSync(target) || fs.statSync(target).size !== fs.statSync(source).size
    } catch {
      stale = true
    }
    if (stale) fs.copyFileSync(source, target)
    return target
  }

  /** The prompt for manual copying, without installing anything. */
  prompt(workspaceId: string, taskId: string = TANK_TASK): string {
    const definition = TASK_DEFINITIONS[taskId]
    if (definition === undefined) return ''
    const workspace = this.registry.get(workspaceId)
    return this.promptNotice(this.guard.promptFor(workspace?.path ?? '', this.materialZip(taskId), definition))
  }

  /** Filesystem paths for the manual channel. */
  manualPaths(workspaceId: string, taskId: string = TANK_TASK): { source: string; prompt: string } {
    const definition = TASK_DEFINITIONS[taskId]
    if (definition === undefined) return { source: '', prompt: '' }
    const workspace = this.registry.get(workspaceId)
    return {
      source: this.materialZip(taskId),
      prompt: this.promptNotice(this.guard.promptFor(workspace?.path ?? '', this.materialZip(taskId), definition)),
    }
  }

  /** A unique run id. */
  private newRunId(): string {
    const stamp = new Date().toISOString().replace(/[-:.TZ]/g, '').slice(0, 14)
    return stamp + '-' + Math.random().toString(16).slice(2, 8)
  }
}

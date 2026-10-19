/**
 * Task packs: packaged workspace.zip install, original prompt construction, and
 * the run record the evaluators later read.
 * @module dsh-eval-guard/task
 */
import fs from 'node:fs'
import path from 'node:path'
import { execFileSync } from 'node:child_process'
import {
  TANK_TASK, TANK_TASK_LABEL, WAREHOUSE_TASK, WAREHOUSE_TASK_LABEL,
  LAND_TASK, LAND_TASK_LABEL, BOMBERMAN_TASK, BOMBERMAN_TASK_LABEL, CTF_TASK, CTF_TASK_LABEL,
  COMMIT0_TASK, COMMIT0_TASK_LABEL, SWE_BENCH_PRO_TASK, SWE_BENCH_PRO_TASK_LABEL,
  FEATUREBENCH_TASK, FEATUREBENCH_TASK_LABEL, NEURAL_CPP_TASK, NEURAL_CPP_TASK_LABEL,
  NEURAL_FP32_TASK, NEURAL_FP32_TASK_LABEL, PIP_MIRRORS,
  type CheckResult, type InstallResult, type RunRecord, type TaskInfo,
} from './shared.ts'
import type { Guard, TaskDefinition } from './guard.ts'
import { guardRoot, packageRoot } from './sandbox.ts'
import { JobManager } from './jobs.ts'
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
  prompt() {
    return [
      '按照要求编写尽可能强的坦克大战 bot。你的对手是其他顶尖 AI 模型写的 bot，它们可能主动设计战术。基准是一个中等强度、思路常规的 bot，它在正式比赛中是 0 分线；战胜基准不代表足够强，正式对手远强于基准。正式赛是 1 对 1 循环赛，用 Bradley-Terry 模型拟合实力值。',
      '',
      '解压 workspace.zip，阅读 README.md 和 RULES.md，然后开始。',
      '',
      '比赛是连续 90 分钟的单次任务：',
      '- 你有 90 分钟修改 bot，从对话开始计时；到 90 分钟强制停止；',
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
  prompt() {
    return [
      '这是一场 AI 编程赛。你接手了一个代码混乱的老系统：src/ 里是一个仓储机器人调度仿真系统，功能正确，但运行很慢。你的任务：在输出完全不变的前提下，让它尽可能快。',
      '',
      '当前目录里有 workspace.zip。请先自行解压，进入 workspace 文件夹，阅读 TASK.md，然后完全按照 TASK.md 的要求独立完成任务。',
      '',
      '要点（详见 TASK.md）：',
      '- 只能修改 src/；编译命令固定为 g++ -O2 -std=c++20 src/*.cpp，只用 C++ 标准库；',
      '- 对任何合法输入（以 tools/check_input.py --full 为准），输出都必须与基线逐字节相同；SPEC.md 是过时的旧文档，一切以基线代码的实际行为为准；',
      '- 本次是连续 90 分钟的单次任务；clock.py 从解压那一刻开始计算，实际计时从对话开始计算，它只能作为参考，请尽快解压；',
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
  prompt() {
    return [
      '这是一场正式的 AI 编程对抗赛，你的对手是其他顶尖 AI 模型写的 bot，它们会主动切断你、抢你的地。比赛结果会公开排名，请拿出你的最高水平。',
      '',
      '请解压当前目录的 workspace.zip，进入 workspace 文件夹，按 TASK.md 的要求编写一个尽可能强的圈地对战 bot，保存为 workspace/submission/bot.cpp。时间预算约 60 分钟，请自己把握进度，做完就可以结束；超过 90 分钟会被强制停止。只能读写当前文件夹里的文件，不能联网搜索，不要向我提问。',
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
  prompt() {
    return [
      '按照要求编写尽可能强bot，你的对手是全球其他顶尖ai，可能主动设计战术。基准只是很弱的bot，它在正式比赛为0分线。正式赛会对战多轮，用Bradley-Terry模型拟合实力值。',
      '解压 workspace.zip，阅读 README.md 和 RULES.md，编写支持2–10人的炸弹人 bot。尽量60分钟完成，90分钟必须交卷。允许本地工具、对战和复盘，禁止联网、调用其他AI或查看其他选手资料。提交 submission/bot.cpp，简述策略。',
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
  prompt() {
    return [
      '按照要求编写尽可能强的 bot，你的对手是全球其他顶尖 AI，可能主动设计战术。基准是一个中等强度、思路常规的 bot，它在正式比赛是 0 分线；战胜基准不代表足够强，正式对手远强于基准。正式赛会对战多轮，用 Bradley-Terry 模型拟合实力值。',
      '解压 workspace.zip，阅读 README.md 和 RULES.md，编写支持 2–15 方的夺旗 bot。你有 60 分钟，只看最终 bot 的强度，提前交卷不加分，请充分利用时间持续改进；可以适当超时，但到 90 分钟会强制交卷。允许本地工具、对战和复盘，禁止联网、调用其他 AI 或查看其他选手资料。提交 workspace/submission/bot.cpp，简述策略。',
    ].join('\n')
  },
}

/** Run the task's host-side setup script with network access. */
function runSetup(workspacePath: string): void {
  execFileSync('bash', ['setup.sh'], {
    cwd: workspacePath,
    stdio: 'inherit',
    env: { ...process.env, GIT_TERMINAL_PROMPT: '0' },
  })
}

/** Commit0 single-library task. */
export const COMMIT0_DEFINITION: TaskDefinition = {
  id: COMMIT0_TASK,
  label: COMMIT0_TASK_LABEL,
  description: TASKS[5]!.description,
  asset: path.join(packageRoot(), 'assets', 'commit0', 'workspace.zip'),
  prepare: runSetup,
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
  prepare: runSetup,
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
  prepare: runSetup,
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
}

/** Install, check, and prompt services for the task packs. */
export interface SetupOptions {
  cloneSource?: string
  customCloneUrl?: string
  pipIndex?: string
  customPipUrl?: string
  gitProxyUrl?: string
  pipProxyUrl?: string
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

  /** The pip index URL for a task, honouring the selected mirror. */
  pipIndexUrl(options: SetupOptions): string {
    const index = options.pipIndex ?? 'pypi'
    if (index === 'custom') return (options.customPipUrl ?? '').trim() || 'https://pypi.org/simple'
    return PIP_MIRRORS.find(item => item.id === index)?.url ?? 'https://pypi.org/simple'
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
    for (const name of ['.git', '.repo', 'setup.sh', 'remove.patch', 'spec.pdf.bz2']) {
      try { fs.rmSync(path.join(workspacePath, name), { recursive: true, force: true }) } catch { /* best effort */ }
    }
  }

  /** Start a non-blocking install. The settings page polls the returned job. */
  startInstall(workspaceId: string, taskId: string, options: SetupOptions = {}): { ok: boolean; jobId?: string; error?: string } {
    const definition = TASK_DEFINITIONS[taskId]
    if (definition === undefined) return { ok: false, error: '未知任务：' + taskId }
    const checked = this.check(workspaceId)
    if (!checked.ok) return { ok: false, error: checked.error }
    if (!checked.empty) return { ok: false, error: '工作区不是空的；未执行任何操作。' }
    if (!this.guard.active) return { ok: false, error: '请先启用评测守护，再安装评测任务。' }
    try {
      fs.copyFileSync(this.materialZip(taskId), path.join(checked.path, 'workspace.zip'))
      if (definition.needsSetup) this.extractMaterial(checked.path)
    } catch (error: unknown) {
      return { ok: false, error: '复制素材失败：' + (error instanceof Error ? error.message : String(error)) }
    }
    const env: Record<string, string> = {
      EVAL_CLONE_URL: this.cloneUrl(definition, options),
      EVAL_PIP_INDEX_URL: this.pipIndexUrl(options),
      GIT_TERMINAL_PROMPT: '0',
      EVAL_GIT_PROXY: this.proxyValue(options.gitProxyUrl),
      EVAL_PIP_PROXY: this.proxyValue(options.pipProxyUrl),
    }
    const command = definition.needsSetup ? ['bash', 'setup.sh'] : ['bash', '-c', 'true']
    const job = this.jobs.start({
      kind: 'install', task: taskId, workspaceId, command, cwd: checked.path, env,
      onSuccess: () => {
        if (definition.needsSetup) this.cleanupWorkspace(checked.path)
        const runId = this.newRunId()
        const prompt = this.guard.promptFor(checked.path, this.materialZip(taskId), definition)
        const run: RunRecord = {
          runId, task: definition.id, workspaceId, workspacePath: checked.path,
          sessionId: null, createdAt: Date.now(), startedAt: null, endedAt: null,
          status: 'prepared', prompt,
          timeLimitMinutes: this.guard.configuration.timeLimitMinutes,
        }
        this.guard.registerRun(run)
        return { runId, prompt }
      },
    })
    return { ok: true, jobId: job.id }
  }

  /** Start a single-workspace evaluation job. */
  startEval(workspaceId: string, taskId: string): { ok: boolean; jobId?: string; error?: string } {
    const definition = TASK_DEFINITIONS[taskId]
    if (definition === undefined) return { ok: false, error: '未知任务：' + taskId }
    const checked = this.check(workspaceId)
    if (!checked.ok) return { ok: false, error: checked.error }
    const script = path.join(packageRoot(), 'eval', taskId + '.py')
    if (!fs.existsSync(script)) return { ok: false, error: '评测脚本不存在：' + script }
    const job = this.jobs.start({
      kind: 'eval', task: taskId, workspaceId,
      command: ['python3', '-X', 'utf8', script, '--workspace', checked.path],
      cwd: packageRoot(),
      env: { PYTHONPATH: '', PYTHONHOME: '' },
    })
    return { ok: true, jobId: job.id }
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

  /** Install the selected task pack into an empty workspace. */
  install(workspaceId: string, taskId: string = TANK_TASK, promptOverride?: string): InstallResult {
    const definition = TASK_DEFINITIONS[taskId]
    if (definition === undefined) return { ok: false, workspaceId, error: '未知任务：' + taskId }
    const checked = this.check(workspaceId)
    if (!checked.ok) return { ok: false, error: checked.error, workspaceId }
    if (!checked.empty) {
      return { ok: false, workspaceId, workspacePath: checked.path, error: '工作区不是空的；未执行任何操作。请换一个空工作区，或按手动通道处理。' }
    }
    if (!this.guard.active) {
      return { ok: false, workspaceId, workspacePath: checked.path, error: '请先启用评测守护，再安装评测任务。' }
    }
    try {
      fs.copyFileSync(this.materialZip(taskId), path.join(checked.path, 'workspace.zip'))
      if (definition.needsSetup) this.extractMaterial(checked.path)
      if (definition.prepare !== undefined) definition.prepare(checked.path)
      if (definition.needsSetup) this.cleanupWorkspace(checked.path)
    } catch (error: unknown) {
      return { ok: false, workspaceId, workspacePath: checked.path, error: '任务准备失败：' + (error instanceof Error ? error.message : String(error)) }
    }
    const runId = this.newRunId()
    const prompt = promptOverride ?? this.guard.promptFor(checked.path, this.materialZip(taskId), definition)
    const run: RunRecord = {
      runId,
      task: definition.id,
      workspaceId,
      workspacePath: checked.path,
      sessionId: null,
      createdAt: Date.now(),
      startedAt: null,
      endedAt: null,
      status: 'prepared',
      prompt,
      timeLimitMinutes: this.guard.configuration.timeLimitMinutes,
    }
    this.guard.registerRun(run)
    return { ok: true, runId, workspaceId, workspacePath: checked.path, prompt }
  }

  /** The prompt for manual copying, without installing anything. */
  prompt(workspaceId: string, taskId: string = TANK_TASK): string {
    const definition = TASK_DEFINITIONS[taskId]
    if (definition === undefined) return ''
    const workspace = this.registry.get(workspaceId)
    return this.guard.promptFor(workspace?.path ?? '', this.materialZip(taskId), definition)
  }

  /** Filesystem paths for the manual channel. */
  manualPaths(workspaceId: string, taskId: string = TANK_TASK): { source: string; prompt: string } {
    const definition = TASK_DEFINITIONS[taskId]
    if (definition === undefined) return { source: '', prompt: '' }
    const workspace = this.registry.get(workspaceId)
    return {
      source: this.materialZip(taskId),
      prompt: this.guard.promptFor(workspace?.path ?? '', this.materialZip(taskId), definition),
    }
  }

  /** A unique run id. */
  private newRunId(): string {
    const stamp = new Date().toISOString().replace(/[-:.TZ]/g, '').slice(0, 14)
    return stamp + '-' + Math.random().toString(16).slice(2, 8)
  }
}

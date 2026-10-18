/**
 * Task packs: packaged workspace.zip install, original prompt construction, and
 * the run record the evaluators later read.
 * @module dsh-eval-guard/task
 */
import fs from 'node:fs'
import path from 'node:path'
import {
  TANK_TASK, TANK_TASK_LABEL, WAREHOUSE_TASK, WAREHOUSE_TASK_LABEL,
  LAND_TASK, LAND_TASK_LABEL, BOMBERMAN_TASK, BOMBERMAN_TASK_LABEL, CTF_TASK, CTF_TASK_LABEL,
  type CheckResult, type InstallResult, type RunRecord, type TaskInfo,
} from './shared.ts'
import type { Guard, TaskDefinition } from './guard.ts'
import { guardRoot, packageRoot } from './sandbox.ts'

/** Tasks offered by the installer. */
export const TASKS: readonly TaskInfo[] = [
  { id: TANK_TASK, label: TANK_TASK_LABEL, description: '1v1 坦克大战 bot；解压 workspace.zip，提交 workspace/submission/bot.cpp。' },
  { id: WAREHOUSE_TASK, label: WAREHOUSE_TASK_LABEL, description: '单人单次仓储调度优化；解压 workspace.zip，提交 workspace/src/。' },
  { id: LAND_TASK, label: LAND_TASK_LABEL, description: '圈地 1v1/4 人混战 bot；解压 workspace.zip，提交 workspace/submission/bot.cpp。' },
  { id: BOMBERMAN_TASK, label: BOMBERMAN_TASK_LABEL, description: '炸弹人 2–10 人 bot；解压 workspace.zip，提交 workspace/submission/bot.cpp。' },
  { id: CTF_TASK, label: CTF_TASK_LABEL, description: '夺旗 2–15 人 bot；解压 workspace.zip，提交 workspace/submission/bot.cpp。' },
]

/** The tank task definition (original prompt; only halftime mechanics removed). */
export const TANK_DEFINITION: TaskDefinition = {
  id: TANK_TASK,
  label: TANK_TASK_LABEL,
  description: TASKS[0]!.description,
  asset: path.join(packageRoot(), 'assets', 'tank', 'workspace.zip'),
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
  prompt() {
    return [
      '按照要求编写尽可能强的 bot，你的对手是全球其他顶尖 AI，可能主动设计战术。基准是一个中等强度、思路常规的 bot，它在正式比赛是 0 分线；战胜基准不代表足够强，正式对手远强于基准。正式赛会对战多轮，用 Bradley-Terry 模型拟合实力值。',
      '解压 workspace.zip，阅读 README.md 和 RULES.md，编写支持 2–15 方的夺旗 bot。你有 60 分钟，只看最终 bot 的强度，提前交卷不加分，请充分利用时间持续改进；可以适当超时，但到 90 分钟会强制交卷。允许本地工具、对战和复盘，禁止联网、调用其他 AI 或查看其他选手资料。提交 workspace/submission/bot.cpp，简述策略。',
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
}

/** Install, check, and prompt services for the task packs. */
export class TaskService {
  private readonly guard: Guard
  private readonly registry: { get(id: string): { path: string; title?: string } | undefined }

  constructor(guard: Guard, registry: { get(id: string): { path: string; title?: string } | undefined }) {
    this.guard = guard
    this.registry = registry
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
    } catch (error: unknown) {
      return { ok: false, workspaceId, workspacePath: checked.path, error: '复制素材失败：' + (error instanceof Error ? error.message : String(error)) }
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

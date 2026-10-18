/**
 * The tank task pack: packaged workspace.zip install, prompt construction, and the one
 * run record the evaluator later reads.
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
export const TASKS: readonly TaskInfo[] = [{
  id: TANK_TASK,
  label: TANK_TASK_LABEL,
  description: '1v1 坦克大战 bot：解压 workspace.zip 后编写 workspace/submission/bot.cpp，评测时与基准及其它提交打循环赛。',
}, {
  id: WAREHOUSE_TASK,
  label: WAREHOUSE_TASK_LABEL,
  description: '单人单次仓储调度优化：解压 workspace.zip 后改写 workspace/src/，在输出逐字节不变的前提下尽量提速。',
}, {
  id: LAND_TASK,
  label: LAND_TASK_LABEL,
  description: '圈地 1v1/4 人混战 bot：解压 workspace.zip 后编写 workspace/submission/bot.cpp。',
}, {
  id: BOMBERMAN_TASK,
  label: BOMBERMAN_TASK_LABEL,
  description: '炸弹人 2–10 人 bot：解压 workspace.zip 后编写 workspace/submission/bot.cpp。',
}, {
  id: CTF_TASK,
  label: CTF_TASK_LABEL,
  description: '夺旗 2–15 人 bot：解压 workspace.zip 后编写 workspace/submission/bot.cpp。',
}]

/** Format a blacklist for the prompt. */
function blacklistBlock(blacklist: readonly string[]): string {
  if (blacklist.length === 0) return '（当前没有需要特别列出的工具。）'
  return blacklist.join('\n')
}

/** Wording shared by every task: difficulty, strong-agent comparison, no early submit. */
const COMPETITION_LINES = [
  '## 竞争强度',
  '',
  '- 这个任务并不像看起来那么简单，不要低估隐藏边界、正确性细节和对手强度。',
  '- 你的结果会与 meow-ai-arena 中其他强 agent（如 GPT-6 Astra、Opus、Kimi K3 等）的提交在同一口径下比较，而不只是和基准 bot 比较；请按能进入前列的标准要求自己。',
  '- 不要过早交卷：在时间上限内尽量多验证、迭代和完善，只有确实完成时才结束。',
]

/** The tank task definition. */
export const TANK_DEFINITION: TaskDefinition = {
  id: TANK_TASK,
  label: TANK_TASK_LABEL,
  description: TASKS[0]!.description,
  asset: path.join(packageRoot(), 'assets', 'tank', 'workspace.zip'),
  prompt(blacklist, options) {
    return [
      '这是一场用于评测 AI agent 能力的坦克大战编程任务。你需要在当前工作区里独立完成它。',
      '',
      '## 环境与限制',
      '',
      '- 你只能读写当前工作区内的文件；工作区以外的任何位置都不能访问，包括 /tmp。',
      '- 禁止联网、禁止调用其他 AI 或子代理、禁止与其他 agent 通信或读取其他工作区。',
      '- 评测守护会阻止并记录违规行为；违规可能导致本次评测无效。',
      '- 任务必须自主完成：不要向用户提问、不要请求审批、不要等待任何用户输入。',
      '',
      '## 交付',
      '',
      '- 解压工作区里的 workspace.zip，把最终 bot 保存到解压后的 workspace/submission/bot.cpp（只需要这一个文件）。',
      '- 最后用几句话说明你的策略。',
      '',
      '## 时间规则',
      '',
      '- 本次是**连续 ' + String(options.minutes ?? 90) + ' 分钟的单次任务**，从对话开始时开始计时。',
      '- 交付前检查并关闭所有后台任务；到点会被强制终止。',
      '- clock.py 从解压那一刻开始计算，而实际计时从对话开始计算；它**只能作为参考**，请尽快解压以减小误差。实际时间以评测守护为准。',
      '',
      '## 本次评测禁止调用的工具',
      '',
      blacklistBlock(blacklist),
      '',
      '## 工作区内容',
      '',
      '工作区里有一个 workspace.zip；解压后进入 workspace/ 目录，先读 README.md 和 RULES.md，规则引擎是 tank.hpp。',
      '本地工具（arena.py、check、play、batch、vs 等）和起步模板都在解压后的 workspace/ 里，可直接使用。',
      '',
      ...COMPETITION_LINES,
      '',
      '开始吧。',
    ].join('\n')
  },
}

/** The warehouse single-run optimization task. */
export const WAREHOUSE_DEFINITION: TaskDefinition = {
  id: WAREHOUSE_TASK,
  label: WAREHOUSE_TASK_LABEL,
  description: TASKS[1]!.description,
  asset: path.join(packageRoot(), 'assets', 'warehouse', 'workspace.zip'),
  prompt(blacklist, options) {
    return [
      '这是一场用于评测 AI agent 能力的仓储调度优化任务。你需要在当前工作区里独立完成它。',
      '',
      '## 环境与限制',
      '',
      '- 你只能读写当前工作区内的文件；工作区以外的任何位置都不能访问，包括 /tmp。',
      '- 禁止联网、禁止调用其他 AI 或子代理、禁止与其他 agent 通信或读取其他工作区。',
      '- 评测守护会阻止并记录违规行为；违规可能导致本次评测无效。',
      '- 任务必须自主完成：不要向用户提问、不要请求审批、不要等待任何用户输入。',
      '',
      '## 交付',
      '',
      '- 解压工作区里的 workspace.zip，进入解压后的 workspace/ 目录；只能修改 workspace/src/ 下的文件。',
      '- 结束时 workspace/src/ 整个目录就是提交；不要修改 baseline/、workloads/、tools/。',
      '- 最后用几句话说明你做了什么优化、如何验证输出不变。',
      '',
      '## 时间规则',
      '',
      '- 本次是**连续 ' + String(options.minutes ?? 90) + ' 分钟的单次任务**，从对话开始时开始计时。',
      '- 交付前检查并关闭所有后台任务；到点会被强制终止，以当时的 src/ 为准。',
      '- clock.py 从解压那一刻开始计算，而实际计时从对话开始计算；它**只能作为参考**，请尽快解压以减小误差。实际时间以评测守护为准。',
      '',
      '## 本次评测禁止调用的工具',
      '',
      blacklistBlock(blacklist),
      '',
      '## 工作区内容',
      '',
      '解压后 workspace/ 里有 TASK.md、SPEC.md、baseline/、src/、workloads/public/ 和 tools/。',
      '先读 TASK.md、baseline 代码和 SPEC.md；SPEC.md 是旧文档，与基线代码不一致时以基线代码的行为为准。',
      '编译命令固定为 g++ -O2 -std=c++20 src/*.cpp；输出必须与 baseline 逐字节相同，包括所有能通过 tools/check_input.py --full 的输入。',
      '用 python3 tools/bench.py 检查正确性和耗时，用 tools/gen.py 生成更多输入和基线对拍。',
      '',
      ...COMPETITION_LINES,
      '',
      '开始吧。',
    ].join('\n')
  },
}

/** Shared prompt for the three bot-writing tasks. */
function botTaskPrompt(
  intro: string,
  submit: string,
  workspaceLines: readonly string[],
  blacklist: readonly string[],
  options: { source: string; workspacePath: string; minutes: number },
): string {
  return [
    intro,
    '',
    '## 环境与限制',
    '',
    '- 你只能读写当前工作区内的文件；工作区以外的任何位置都不能访问，包括 /tmp。',
    '- 禁止联网、禁止调用其他 AI 或子代理、禁止与其他 agent 通信或读取其他工作区。',
    '- 评测守护会阻止并记录违规行为；违规可能导致本次评测无效。',
    '- 任务必须自主完成：不要向用户提问、不要请求审批、不要等待任何用户输入。',
    '',
    '## 交付',
    '',
    submit,
    '- 最后用几句话说明你的策略。',
    '',
    '## 时间规则',
    '',
    '- 本次是**连续 ' + String(options.minutes ?? 90) + ' 分钟的单次任务**，从对话开始时开始计时。',
    '- 交付前检查并关闭所有后台任务；到点会被强制终止。',
    '- clock.py 从解压那一刻开始计算，而实际计时从对话开始计算；它**只能作为参考**，请尽快解压以减小误差。实际时间以评测守护为准。',
    '',
    '## 本次评测禁止调用的工具',
    '',
    blacklistBlock(blacklist),
    '',
    '## 工作区内容',
    '',
    ...workspaceLines,
    '',
    ...COMPETITION_LINES,
    '',
    '开始吧。',
  ].join('\n')
}

/** Land (territory) bot task. */
export const LAND_DEFINITION: TaskDefinition = {
  id: LAND_TASK,
  label: LAND_TASK_LABEL,
  description: TASKS[2]!.description,
  asset: path.join(packageRoot(), 'assets', 'land', 'workspace.zip'),
  prompt(blacklist, options) {
    return botTaskPrompt(
      '这是一场用于评测 AI agent 能力的圈地对战 bot 编程任务。你需要在当前工作区里独立完成它。',
      '- 解压工作区里的 workspace.zip，进入 workspace/；从 starter/bot.cpp 起步，最终把单文件程序保存到 workspace/submission/bot.cpp。',
      [
        '解压后 workspace/ 里有 RULES.md、TASK.md、engine/、anchors/、starter/ 和 submission/。',
        '先读 RULES.md、TASK.md、engine/game.py 和 starter/bot.cpp；规则以 engine/game.py 为准。',
        '本地裁判：python3 engine/referee.py submission/bot.cpp anchors/greedy.cpp --games 10（单挑），也可加多个 bot 打混战。',
        '只能提交一个 bot.cpp，编译参数固定为 g++ -O2 -std=c++20，单文件、只用 C++ 标准库。',
      ],
      blacklist, options,
    )
  },
}

/** Bomberman bot task. */
export const BOMBERMAN_DEFINITION: TaskDefinition = {
  id: BOMBERMAN_TASK,
  label: BOMBERMAN_TASK_LABEL,
  description: TASKS[3]!.description,
  asset: path.join(packageRoot(), 'assets', 'bomberman', 'workspace.zip'),
  prompt(blacklist, options) {
    return botTaskPrompt(
      '这是一场用于评测 AI agent 能力的炸弹人对战 bot 编程任务。你需要在当前工作区里独立完成它。',
      '- 解压工作区里的 workspace.zip，进入 workspace/；最终把单文件程序保存到 workspace/submission/bot.cpp。',
      [
        '解压后 workspace/ 里有 README.md、RULES.md、engine/、baseline.cpp 和 submission/。',
        '先读 README.md、RULES.md 和 engine/engine.py；baseline.cpp 是公开练习对手，可以编译、对战和复盘。',
        '同一程序要支持 2–10 人；编译参数固定为 g++ -O2 -std=c++20，单文件、只用 C++ 标准库。',
        '用 engine/arena.py 或 README 里的本地命令做对战和回放验证。',
      ],
      blacklist, options,
    )
  },
}

/** CTF (capture the flag) bot task. */
export const CTF_DEFINITION: TaskDefinition = {
  id: CTF_TASK,
  label: CTF_TASK_LABEL,
  description: TASKS[4]!.description,
  asset: path.join(packageRoot(), 'assets', 'ctf', 'workspace.zip'),
  prompt(blacklist, options) {
    return botTaskPrompt(
      '这是一场用于评测 AI agent 能力的夺旗对战 bot 编程任务。你需要在当前工作区里独立完成它。',
      '- 解压工作区里的 workspace.zip，进入 workspace/；最终把单文件程序保存到 workspace/submission/bot.cpp。',
      [
        '解压后 workspace/ 里有 README.md、RULES.md、arena.py、engine.py、core.py、baseline.cpp 和 submission/bot.cpp 模板。',
        '先读 README.md、RULES.md 和 submission/bot.cpp；一个程序统一控制己方 3 个角色，支持 2–15 个阵营。',
        '本地对战：python3 arena.py play submission/bot.cpp baseline --seed 7，或 arena.py batch；baseline.cpp 是公开练习对手。',
        '编译参数固定为 g++ -O2 -std=c++20，单文件、只用 C++ 标准库。',
      ],
      blacklist, options,
    )
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

/** Install, check, and prompt services for the task pack. */
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
      return {
        ok: false, workspaceId, path: workspace.path, empty: false, entries: [],
        error: '读取工作区失败：' + (error instanceof Error ? error.message : String(error)),
      }
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
      return {
        ok: false, workspaceId, workspacePath: checked.path,
        error: '工作区不是空的；未执行任何操作。请换一个空工作区，或按手动通道处理。',
      }
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

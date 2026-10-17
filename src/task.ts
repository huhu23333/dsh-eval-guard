/**
 * The tank task pack: workspace extraction, prompt construction, and the one
 * run record the evaluator later reads.
 * @module dsh-eval-guard/task
 */
import fs from 'node:fs'
import path from 'node:path'
import { execFileSync } from 'node:child_process'
import { TANK_TASK, TANK_TASK_LABEL, type CheckResult, type InstallResult, type RunRecord, type TaskInfo } from './shared.ts'
import type { Guard, TaskDefinition } from './guard.ts'
import { guardRoot, packageRoot } from './sandbox.ts'

/** The only task offered today. */
export const TASKS: readonly TaskInfo[] = [{
  id: TANK_TASK,
  label: TANK_TASK_LABEL,
  description: '1v1 坦克大战 bot：编写 submission/bot.cpp，评测时与基准及其它提交打循环赛。',
}]

/** Format a blacklist for the prompt. */
function blacklistBlock(blacklist: readonly string[]): string {
  if (blacklist.length === 0) return '（当前没有需要特别列出的工具。）'
  return blacklist.join('\n')
}

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
      '- 把最终 bot 保存到工作区内的 submission/bot.cpp（只需要这一个文件）。',
      '- 交付前检查并关闭所有后台任务；最多 ' + String(options.minutes ?? 90) + ' 分钟，到点会被强制终止。',
      '- 最后用几句话说明你的策略。',
      '',
      '## 本次评测禁止调用的工具',
      '',
      blacklistBlock(blacklist),
      '',
      '## 工作区内容',
      '',
      '工作区根目录就是选手包：先读 README.md 和 RULES.md，规则引擎是 tank.hpp。',
      '本地工具（arena.py、check、play、batch、vs 等）和起步模板都在工作区里，可直接使用。',
      'README.md 里提到的上下半场属于原始赛制；clock.py 只是参考，本次是单次任务，以本提示词的时间规则为准。',
      '',
      '开始吧。',
    ].join('\n')
  },
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

  /** Extract the task pack to a material directory the manual channel can reveal. */
  materialDir(): string {
    const dir = path.join(guardRoot(), 'material', 'tank')
    const marker = path.join(guardRoot(), 'material', '.tank-complete')
    if (fs.existsSync(marker) && fs.existsSync(path.join(dir, 'RULES.md'))) return dir
    fs.rmSync(dir, { recursive: true, force: true })
    fs.mkdirSync(dir, { recursive: true })
    fs.mkdirSync(path.dirname(marker), { recursive: true })
    this.extract(TANK_DEFINITION.asset, dir)
    fs.writeFileSync(marker, 'ok\n', 'utf8')
    return dir
  }

  /** Install the task pack into an empty workspace. */
  install(workspaceId: string, promptOverride?: string): InstallResult {
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
      this.extract(TANK_DEFINITION.asset, checked.path)
    } catch (error: unknown) {
      return { ok: false, workspaceId, workspacePath: checked.path, error: '复制素材失败：' + (error instanceof Error ? error.message : String(error)) }
    }
    const runId = this.newRunId()
    const prompt = promptOverride ?? this.guard.promptFor(checked.path, this.materialDir())
    const run: RunRecord = {
      runId,
      task: TANK_TASK,
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
  prompt(workspaceId: string): string {
    const workspace = this.registry.get(workspaceId)
    return this.guard.promptFor(workspace?.path ?? '', this.materialDir())
  }

  /** Filesystem paths for the manual channel. */
  manualPaths(workspaceId: string): { source: string; prompt: string } {
    const workspace = this.registry.get(workspaceId)
    return {
      source: this.materialDir(),
      prompt: this.guard.promptFor(workspace?.path ?? '', this.materialDir()),
    }
  }

  /** Extract a workspace.zip into a destination, stripping the workspace/ prefix. */
  private extract(zip: string, destination: string): void {
    if (!fs.existsSync(zip)) throw new Error('task asset is missing: ' + zip)
    fs.mkdirSync(path.join(guardRoot(), 'tmp'), { recursive: true })
    const temporary = fs.mkdtempSync(path.join(guardRoot(), 'tmp', 'extract-'))
    try {
      execFileSync('unzip', ['-q', '-o', zip, '-d', temporary], { stdio: 'ignore' })
      const inner = path.join(temporary, 'workspace')
      if (!fs.existsSync(inner)) throw new Error('task asset has no workspace/ root')
      fs.cpSync(inner, destination, { recursive: true })
    } finally {
      fs.rmSync(temporary, { recursive: true, force: true })
    }
  }

  /** A unique run id. */
  private newRunId(): string {
    const stamp = new Date().toISOString().replace(/[-:.TZ]/g, '').slice(0, 14)
    return stamp + '-' + Math.random().toString(16).slice(2, 8)
  }
}

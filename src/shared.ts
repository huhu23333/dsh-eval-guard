/**
 * Wire types shared by the host and browser halves. Keep this module import-free:
 * the client bundle includes it without pulling host code.
 * @module dsh-eval-guard/shared
 */

/** Settings namespace; also the Loader row id in cordis.patch.yml. */
export const SETTINGS_NS = 'eval-guard'

/** Route paths owned by this plugin. */
export const STATE_ROUTE = '/api/eval-guard/state'
export const CHECK_ROUTE = '/api/eval-guard/check'
export const INSTALL_ROUTE = '/api/eval-guard/install'
export const PROMPT_ROUTE = '/api/eval-guard/prompt'
export const EVENTS_ROUTE = '/api/eval-guard/events'
export const REVEAL_ROUTE = '/api/eval-guard/reveal'
export const MANUAL_ROUTE = '/api/eval-guard/manual'

/** Supported task ids. */
export const TANK_TASK = 'tank'
export const TANK_TASK_LABEL = '第 6 集 · 坦克大战'
export const WAREHOUSE_TASK = 'warehouse'
export const WAREHOUSE_TASK_LABEL = '第 5 集 · 仓储调度'
export const LAND_TASK = 'land'
export const LAND_TASK_LABEL = '第 1 集 · 圈地'
export const BOMBERMAN_TASK = 'bomberman'
export const BOMBERMAN_TASK_LABEL = '第 2 集 · 炸弹人'
export const CTF_TASK = 'ctf'
export const CTF_TASK_LABEL = '第 3 集 · 夺旗'

/** One task offered by the installer. */
export interface TaskInfo {
  id: string
  label: string
  description: string
}

/** A live conversation the enable-time warning has to mention. */
export interface RunningSession {
  sessionId: string
  title: string
  cwd: string
  running: boolean
  jobs: number
}

/** One warning surfaced to the browser. */
export interface GuardEvent {
  seq: number
  at: number
  sessionId?: string
  runId?: string
  kind: 'tool-blocked' | 'path-denied' | 'process-denied' | 'wait-blocked' | 'timeout' | 'notice'
  tool?: string
  detail: string
}

/** Host answer for the enable pre-check. */
export interface GuardState {
  enabled: boolean
  running: RunningSession[]
  tasks: TaskInfo[]
  defaultBlacklist: string
  generatedDefault: boolean
  workspaceRoot: string
  runsRoot: string
}

/** Result of checking one workspace before install. */
export interface CheckResult {
  ok: boolean
  workspaceId: string
  path: string
  empty: boolean
  entries: string[]
  error?: string
}

/** Result of installing the task pack. */
export interface InstallResult {
  ok: boolean
  error?: string
  runId?: string
  workspaceId?: string
  workspacePath?: string
  prompt?: string
}

/** Manual-channel material locations. */
export interface ManualPaths {
  source: string
  prompt: string
}

/** Usage record written next to one run. */
export interface RunUsage {
  inputTokens: number
  outputTokens: number
  cacheReadTokens: number
  cacheWriteTokens: number
  reasoningTokens: number
  toolCalls: number
  turns: number
  steps: number
  wallTimeMs: number
  startedAt: number | null
  updatedAt: number
}

/** Metadata for one guarded task run. */
export interface RunRecord {
  runId: string
  task: string
  workspaceId: string
  workspacePath: string
  sessionId: string | null
  createdAt: number
  startedAt: number | null
  endedAt: number | null
  status: 'prepared' | 'active' | 'timeout' | 'ended'
  prompt: string
  timeLimitMinutes: number
  provider?: string
  model?: string
  title?: string
}

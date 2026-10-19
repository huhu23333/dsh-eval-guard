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
export const JOBS_ROUTE = '/api/eval-guard/jobs'
export const JOB_ROUTE = '/api/eval-guard/job'
export const JOB_CANCEL_ROUTE = '/api/eval-guard/job/cancel'
export const EVAL_ROUTE = '/api/eval-guard/evaluate'

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
export const COMMIT0_TASK = 'commit0'
export const COMMIT0_TASK_LABEL = 'Commit0 · 单库实现'
export const SWE_BENCH_PRO_TASK = 'swe-bench-pro'
export const SWE_BENCH_PRO_TASK_LABEL = 'SWE-Bench Pro · 单任务'
export const FEATUREBENCH_TASK = 'featurebench'
export const FEATUREBENCH_TASK_LABEL = 'FeatureBench · 单 feature'
export const NEURAL_CPP_TASK = 'neural-cpp'
export const NEURAL_CPP_TASK_LABEL = 'Neural-CPP · 纯 C++ 张量后端'

/** One validated clone remote for a task. */
export interface TaskRemote {
  id: string
  label: string
  url: string
}

/** Common pip indexes offered by the settings page. */
export const PIP_MIRRORS: readonly { id: string; label: string; url: string }[] = [
  { id: 'pypi', label: 'PyPI', url: 'https://pypi.org/simple' },
  { id: 'tsinghua', label: '清华 TUNA', url: 'https://pypi.tuna.tsinghua.edu.cn/simple' },
  { id: 'aliyun', label: '阿里云', url: 'https://mirrors.aliyun.com/pypi/simple' },
  { id: 'tencent', label: '腾讯云', url: 'https://mirrors.cloud.tencent.com/pypi/simple' },
  { id: 'bfsu', label: '北外', url: 'https://mirrors.bfsu.edu.cn/pypi/web/simple' },
  { id: 'sjtu', label: '上海交大', url: 'https://mirror.sjtu.edu.cn/pypi/web/simple' },
]

/** One task offered by the installer. */
export interface TaskInfo {
  id: string
  label: string
  description: string
  /** Default wall-clock limit applied when the user selects this task. */
  timeLimitMinutes: number
  /** Default time-limit switch applied when the user selects this task. */
  timeLimitEnabled: boolean
  /** Whether installation needs a host-side clone / venv setup. */
  needsSetup: boolean
  /** Canonical upstream repository; used to build mirror URLs. */
  repoUrl?: string
  /** Clone remotes verified to contain the exact pinned commit. */
  remotes?: TaskRemote[]
}

export type JobKind = 'install' | 'eval'
export type JobStatus = 'queued' | 'running' | 'succeeded' | 'failed' | 'cancelled' | 'interrupted'

/** Summary of one background setup or evaluation job. */
export interface JobInfo {
  id: string
  kind: JobKind
  task: string
  workspaceId: string
  status: JobStatus
  createdAt: number
  startedAt: number | null
  endedAt: number | null
  updatedAt: number
  error?: string
}

/** Full job state, including a bounded log and optional result. */
export interface JobDetail extends JobInfo {
  log: string
  prompt?: string
  runId?: string
  result?: unknown
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
  pipMirrors?: { id: string; label: string; url: string }[]
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

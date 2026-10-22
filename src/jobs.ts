/** Persistent background jobs for task setup and single-result evaluation. */
import fs from 'node:fs'
import path from 'node:path'
import { spawn, type ChildProcess } from 'node:child_process'
import { guardRoot } from './sandbox.ts'
import type { JobDetail, JobInfo, JobKind, JobStatus } from './shared.ts'

interface StartOptions {
  kind: JobKind
  task: string
  workspaceId: string
  command: string[]
  cwd: string
  env?: Record<string, string>
  /** Install-only: workspace that must be wiped if the host restarts mid-install. */
  cleanupWorkspace?: string
  /** Install-only: CIFAR download-lock token owned by this install. */
  cifarToken?: string
  onSuccess?: (job: JobDetail) => { prompt?: string; runId?: string; result?: unknown } | void
  onCancel?: (job: JobDetail) => void
  onFailure?: (job: JobDetail) => void
}

interface Stored extends JobInfo {
  command: string[]
  cwd: string
  logFile: string
  resultFile: string
  prompt?: string
  runId?: string
  result?: unknown
  pid?: number
  /** Linux /proc/<pid>/stat starttime, used to avoid killing a reused PID after a restart. */
  pidStartTime?: string
  cleanupWorkspace?: string
  cifarToken?: string
}

/**
 * The only host environment variables a setup/evaluation child inherits.
 *
 * Evaluation children execute agent-authored build scripts and binaries, so
 * the ambient DSH environment (credentials, session secrets, proxy settings)
 * must not leak into them. Everything the task needs is passed explicitly via
 * the EVAL_* variables in StartOptions.env.
 */
export function baseEnvironment(source: NodeJS.ProcessEnv = process.env): Record<string, string> {
  const keep = [
    'PATH', 'HOME', 'USER', 'LOGNAME', 'SHELL', 'LANG', 'LANGUAGE', 'LC_ALL', 'LC_CTYPE',
    'LC_MESSAGES', 'TZ', 'TERM', 'TMPDIR', 'TMP', 'TEMP', 'SSL_CERT_FILE', 'SSL_CERT_DIR',
    'GIT_SSL_CAINFO', 'REQUESTS_CA_BUNDLE', 'CURL_CA_BUNDLE', 'NODE_EXTRA_CA_CERTS',
  ]
  const env: Record<string, string> = {}
  for (const key of keep) {
    const value = source[key]
    if (value !== undefined) env[key] = value
  }
  return env
}

function now(): number { return Date.now() }
function newId(): string { return new Date().toISOString().replace(/[-:.TZ]/g, '').slice(0, 14) + '-' + Math.random().toString(16).slice(2, 8) }

/** Jobs live under the guard root and survive settings unmounts and page reloads. */
export class JobManager {
  private readonly root: string
  private readonly jobs = new Map<string, Stored>()
  private readonly children = new Map<string, ChildProcess>()
  private readonly cancelHandlers = new Map<string, (job: JobDetail) => void>()

  constructor() {
    this.root = path.join(guardRoot(), 'jobs')
    fs.mkdirSync(this.root, { recursive: true })
    for (const name of fs.readdirSync(this.root)) {
      const file = path.join(this.root, name, 'job.json')
      if (!fs.existsSync(file)) continue
      try {
        const stored = JSON.parse(fs.readFileSync(file, 'utf8')) as Stored
        if (stored.status === 'running' || stored.status === 'queued') {
          // The host restarted while a child was live. The job can no longer
          // be observed, so kill its process group instead of leaving an
          // orphaned (and unguarded) setup/evaluation process behind. Verify
          // the PID still belongs to that incarnation: a restart can easily
          // have reused the number.
          if (typeof stored.pid === 'number' && stored.pid > 0 && this.stillOurProcess(stored)) {
            try { process.kill(-stored.pid, 'SIGTERM') } catch {
              try { process.kill(stored.pid, 'SIGTERM') } catch { /* already gone */ }
            }
          }
          stored.status = 'interrupted'
          stored.error = '服务重启，任务已中断'
          stored.endedAt = now()
          this.write(stored)
          // A restart can leave a half-installed workspace behind, which then
          // blocks re-install with "workspace not empty". startInstall only
          // ever runs in an initially empty workspace, so wiping the recorded
          // install directory is safe.
          if (stored.kind === 'install' && stored.cleanupWorkspace !== undefined) {
            this.wipeInstallWorkspace(stored.cleanupWorkspace)
            if (stored.cifarToken !== undefined) this.cleanupCifarLock(stored.cifarToken)
          }
        }
        this.jobs.set(stored.id, stored)
      } catch { /* ignore corrupt job */ }
    }
  }

  /** Remove every top-level entry an interrupted install could have created. */
  private wipeInstallWorkspace(workspacePath: string): void {
    try {
      for (const entry of fs.readdirSync(workspacePath)) {
        fs.rmSync(path.join(workspacePath, entry), { recursive: true, force: true })
      }
    } catch { /* best effort */ }
  }

  /** Remove the CIFAR lock only when it carries this install's token. */
  private cleanupCifarLock(token: string): void {
    try {
      const lock = path.join(guardRoot(), 'cache', 'cifar10', '.download.lock')
      if (fs.existsSync(lock) && fs.readFileSync(lock, 'utf8').includes('token=' + token)) fs.rmSync(lock, { force: true })
    } catch { /* best effort */ }
  }

  /** Read the Linux process start time from /proc/<pid>/stat (field 22). */
  private processStartTime(pid: number): string | undefined {
    try {
      const stat = fs.readFileSync('/proc/' + String(pid) + '/stat', 'utf8')
      const end = stat.lastIndexOf(')')
      if (end < 0) return undefined
      const fields = stat.slice(end + 2).trim().split(/\s+/)
      return fields[19]
    } catch {
      return undefined
    }
  }

  /** Whether a persisted PID still names the process this job started. */
  private stillOurProcess(stored: Stored): boolean {
    if (stored.pidStartTime !== undefined && this.processStartTime(stored.pid!) !== stored.pidStartTime) return false
    if (stored.pidStartTime !== undefined) return true
    // Older job records predate pidStartTime; fall back to argv[0] matching.
    try {
      const argv = fs.readFileSync('/proc/' + String(stored.pid) + '/cmdline', 'utf8').split('\0').filter(Boolean)
      if (argv.length === 0) return false
      return path.basename(argv[0]!) === path.basename(stored.command[0] ?? '')
    } catch {
      return false
    }
  }

  private dir(idValue: string): string { return path.join(this.root, idValue) }
  private write(job: Stored): void {
    fs.mkdirSync(this.dir(job.id), { recursive: true })
    fs.writeFileSync(path.join(this.dir(job.id), 'job.json'), JSON.stringify(job, undefined, 2))
  }

  /** Start one child process and stream its output to a persistent log. */
  start(options: StartOptions): JobInfo {
    const jobId = newId()
    const directory = this.dir(jobId)
    fs.mkdirSync(directory, { recursive: true })
    const logFile = path.join(directory, 'log.txt')
    fs.writeFileSync(logFile, '')
    const job: Stored = {
      id: jobId, kind: options.kind, task: options.task, workspaceId: options.workspaceId,
      status: 'running', createdAt: now(), startedAt: now(), endedAt: null, updatedAt: now(),
      command: options.command, cwd: options.cwd, logFile, resultFile: path.join(directory, 'result.json'),
      ...options.cleanupWorkspace === undefined ? {} : { cleanupWorkspace: options.cleanupWorkspace },
      ...options.cifarToken === undefined ? {} : { cifarToken: options.cifarToken },
    }
    this.jobs.set(jobId, job)
    this.write(job)
    let child: ChildProcess
    try {
      child = spawn(options.command[0]!, options.command.slice(1), {
        cwd: options.cwd,
        env: {
          ...baseEnvironment(),
          ...(options.env ?? {}),
          PYTHONUNBUFFERED: '1',
          EVAL_JOB_DIR: directory,
          EVAL_RESULT_FILE: job.resultFile,
        },
        detached: true,
        stdio: ['ignore', 'pipe', 'pipe'],
      })
    } catch (error) {
      job.status = 'failed'
      job.error = error instanceof Error ? error.message : String(error)
      job.endedAt = now(); job.updatedAt = now(); this.write(job)
      return this.info(job)
    }
    this.children.set(jobId, child)
    if (child.pid !== undefined) {
      job.pid = child.pid
      job.pidStartTime = this.processStartTime(child.pid)
      this.write(job)
    }
    if (options.onCancel !== undefined) this.cancelHandlers.set(jobId, options.onCancel)
    const append = (chunk: Buffer): void => {
      try { fs.appendFileSync(logFile, chunk) } catch { /* log write must not crash the host */ }
    }
    child.stdout?.on('data', (chunk: Buffer) => append(chunk))
    child.stderr?.on('data', (chunk: Buffer) => append(chunk))
    let failureNotified = false
    const fail = (error: string): void => {
      if (job.status !== 'running' && job.status !== 'queued') return
      job.status = 'failed'
      job.error = error
      job.endedAt = now(); job.updatedAt = now()
      this.write(job)
      if (failureNotified) return
      failureNotified = true
      try { options.onFailure?.(this.detail(jobId)) } catch { /* failure cleanup is best effort */ }
    }
    child.on('error', (error: Error) => {
      this.children.delete(jobId)
      this.cancelHandlers.delete(jobId)
      fail(error.message)
    })
    child.on('exit', (code, signal) => {
      this.children.delete(jobId)
      this.cancelHandlers.delete(jobId)
      if (job.status === 'cancelled' || job.status === 'failed') return
      if (code === 0) {
        try {
          const extra = options.onSuccess?.(this.detail(jobId))
          if (extra !== undefined) {
            if (extra.prompt !== undefined) job.prompt = extra.prompt
            if (extra.runId !== undefined) job.runId = extra.runId
            if (extra.result !== undefined) {
              job.result = extra.result
              fs.writeFileSync(job.resultFile, JSON.stringify(extra.result, undefined, 2))
            }
          }
          if (fs.existsSync(job.resultFile)) {
            try { job.result = JSON.parse(fs.readFileSync(job.resultFile, 'utf8')) } catch { /* result is optional */ }
          }
          job.status = 'succeeded'
        } catch (error) {
          job.status = 'failed'
          job.error = error instanceof Error ? error.message : String(error)
          if (!failureNotified) {
            failureNotified = true
            try { options.onFailure?.(this.detail(jobId)) } catch { /* failure cleanup is best effort */ }
          }
        }
      } else {
        fail(signal !== null ? '进程被信号 ' + signal + ' 终止' : '退出码 ' + String(code))
      }
      job.endedAt = now(); job.updatedAt = now(); this.write(job)
    })
    return this.info(job)
  }

  /** Cancel a running job and its process group. */
  cancel(jobId: string): JobInfo | undefined {
    const job = this.jobs.get(jobId)
    if (job === undefined) return undefined
    if (job.status !== 'running' && job.status !== 'queued') return this.info(job)
    job.status = 'cancelled'; job.endedAt = now(); job.updatedAt = now(); job.error = '用户取消'; this.write(job)
    const child = this.children.get(jobId)
    const handler = this.cancelHandlers.get(jobId)
    this.cancelHandlers.delete(jobId)
    let cleaned = false
    const cleanup = (): void => {
      if (cleaned) return
      cleaned = true
      if (handler !== undefined) {
        try { handler(this.detail(jobId)) } catch { /* cancellation cleanup is best effort */ }
      }
    }
    if (child?.pid !== undefined) {
      child.once('exit', cleanup)
      try { process.kill(-child.pid, 'SIGTERM') } catch { try { child.kill('SIGTERM') } catch { /* gone */ } }
      // If a child ignores SIGTERM, force the process group down and still
      // perform the workspace cleanup instead of leaving a partial install.
      setTimeout(() => {
        if (cleaned) return
        try { process.kill(-child.pid!, 'SIGKILL') } catch { try { child.kill('SIGKILL') } catch { /* gone */ } }
        cleanup()
      }, 2000).unref?.()
    } else {
      cleanup()
    }
    return this.info(job)
  }

  /** Current state, including a bounded log tail and optional result. */
  get(jobId: string): JobDetail | undefined {
    if (!this.jobs.has(jobId)) return undefined
    return this.detail(jobId)
  }

  /** Recent jobs, optionally filtered by workspace and task. */
  list(workspaceId?: string, task?: string): JobInfo[] {
    return [...this.jobs.values()]
      .filter(job => workspaceId === undefined || job.workspaceId === workspaceId)
      .filter(job => task === undefined || job.task === task)
      .sort((a, b) => b.createdAt - a.createdAt)
      .slice(0, 20)
      .map(job => this.info(job))
  }

  private info(job: Stored): JobInfo {
    return {
      id: job.id, kind: job.kind, task: job.task, workspaceId: job.workspaceId,
      status: job.status, createdAt: job.createdAt, startedAt: job.startedAt,
      endedAt: job.endedAt, updatedAt: job.updatedAt, error: job.error,
    }
  }

  private detail(jobId: string): JobDetail {
    const job = this.jobs.get(jobId)!
    let log = ''
    try { log = fs.readFileSync(job.logFile, 'utf8').slice(-16000) } catch { /* no log yet */ }
    let result: unknown
    if (job.result !== undefined) result = job.result
    else if (fs.existsSync(job.resultFile)) {
      try { result = JSON.parse(fs.readFileSync(job.resultFile, 'utf8')) } catch { /* invalid result */ }
    }
    return { ...this.info(job), log, prompt: job.prompt, runId: job.runId, result }
  }
}

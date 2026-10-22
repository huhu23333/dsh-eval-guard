/**
 * Host-side evaluation guard: main-conversation detection, sandbox hook,
 * tool policy, wall-clock limit, run records, and audit events.
 * @module dsh-eval-guard/guard
 */
import fs from 'node:fs'
import path from 'node:path'
import type { Context } from '@deepseek-ai/cordis'
import {
  EVENTS_ROUTE, type GuardEvent, type RunRecord, type RunUsage, type RunningSession,
} from './shared.ts'
import { type Config, resolveConfig } from './config.ts'
import {
  auditToolCall, generateDefaultBlacklist, isAuditedTool, isBaseBlacklistName, parseBlacklist,
  parsePathRules, WAIT_TOOLS, type PathRule,
} from './policy.ts'
import { dshHome, guardRoot, probeConfinement, runtimeFiles, supervisorPath, wrapSandbox } from './sandbox.ts'

/** Minimal Agent shape this plugin consumes. */
interface AgentLike {
  id: string
  status: string
  options?: { provider?: string; model?: string }
  session: SessionLike
  cancel(cause: { kind: 'hook'; reason: string }, options?: { keepInbox?: boolean }): void
}

/**
 * The durable session header fields this plugin uses. DSH keeps them at the
 * TOP LEVEL of `session.header` (cwd/parentSession/origin/delegationDepth);
 * there is no `header.meta` wrapper.
 */
interface SessionHeaderLike {
  cwd?: string
  origin?: 'subagent'
  parentSession?: string
  delegationDepth?: number
  /** Optional display title supplied by newer callers; not part of DSH's header. */
  title?: string
}

/** Minimal Session shape this plugin consumes. */
interface SessionLike {
  id: string
  header?: SessionHeaderLike
}

interface JobViewLike { id: string; status: string; owner?: string }

interface Services {
  sessions: { get(id: string): SessionLike | undefined; list(): SessionLike[] }
  agents: { list(): AgentLike[]; get(id: string): AgentLike | undefined }
  tools: {
    schemas(scope?: object): { name: string }[]
    guard(guard: (exec: { name: string; arguments: unknown; agent?: AgentLike }) => string | undefined): () => void
  }
  sandbox?: { confine(argv: readonly string[], policy: Record<string, unknown>, signal?: AbortSignal): Promise<unknown> }
  sandboxPolicy?: { resolve(request?: Record<string, unknown>): Record<string, unknown> }
  approval?: {
    effectivePolicy(session: SessionLike): 'ask' | 'never'
    setPolicy(agent: AgentLike, policy: 'ask' | 'never'): void
  }
  jobs?: {
    list(caller?: string): JobViewLike[]
    kill(id: string, caller?: string, reason?: string): string
  }
  workspaceRegistry?: { get(id: string): { path: string; title?: string } | undefined }
  on(event: string, listener: (...args: any[]) => void): () => void
}

/** The cwd a session is confined to, or undefined when the header has none. */
export function sessionCwd(session: SessionLike | undefined): string | undefined {
  const cwd = session?.header?.cwd
  return typeof cwd === 'string' && cwd !== '' ? cwd : undefined
}

/**
 * Whether a session is a subagent child.
 *
 * DSH uses `origin: 'subagent'` and/or `delegationDepth > 0` for subagents.
 * `parentSession` alone is NOT a subagent marker: a user-visible fork of a
 * normal conversation also sets `parentSession` (seed lineage), and treating
 * that as a subagent would leave the whole fork unguarded.
 */
export function sessionIsSubagent(session: SessionLike | undefined): boolean {
  const header = session?.header
  if (header === undefined) return false
  if (header.origin === 'subagent') return true
  return typeof header.delegationDepth === 'number' && header.delegationDepth > 0
}

/** The session this one was forked/delegated from, if any. */
function sessionParent(session: SessionLike | undefined): string | undefined {
  const parent = session?.header?.parentSession
  return typeof parent === 'string' && parent !== '' ? parent : undefined
}

/** Display title for the enable warning; DSH's header has none, so fall back. */
function sessionTitle(session: SessionLike | undefined): string | undefined {
  const title = session?.header?.title
  if (typeof title === 'string' && title !== '') return title
  const cwd = sessionCwd(session)
  return cwd === undefined ? undefined : path.basename(cwd)
}

/** How long an idle session keeps its deadline alive before it is settled. */
const IDLE_GRACE_MS = 5_000

/** One installed task definition. */
export interface TaskDefinition {
  id: string
  label: string
  description: string
  asset: string
  /** Defaults applied by the settings page when this task is selected. */
  timeLimitMinutes: number
  timeLimitEnabled: boolean
  needsSetup: boolean
  repoUrl?: string
  remotes?: import('./shared.ts').TaskRemote[]
  /** Workspace-relative test material snapshotted at install and restored at evaluation. */
  testMaterial?: readonly string[]
  /** Build the initial prompt for `minutes` (the live wall-clock limit). */
  prompt: (options: { minutes: number }) => string
}

/** Event ring + run audit writer. */
export class Guard {
  private readonly ctx: Context
  private readonly services: Services
  private rawConfig: any
  private enabled = false
  private readonly events: GuardEvent[] = []
  private seq = 0
  private interval: ReturnType<typeof setInterval> | undefined
  private origConfine: ((...args: any[]) => Promise<unknown>) | undefined
  private origResolve: ((...args: any[]) => Record<string, unknown>) | undefined
  private sandboxObject: any
  private policyObject: any
  private readonly approvalPrev = new Map<string, 'ask' | 'never'>()
  private readonly deadlines = new Map<string, number>()
  private readonly startedAt = new Map<string, number>()
  private readonly lastActivity = new Map<string, number>()
  private readonly runs = new Map<string, RunRecord>()
  private readonly sessionRuns = new Map<string, string>()
  private readonly offsets = new Map<string, number>()
  private readonly knownAgents = new Set<string>()
  private toolCache: string[] = []
  private readonly disposers: (() => void)[] = []
  private readonly task: TaskDefinition
  private lastError: string | undefined
  private enableRetryAt = 0
  /** Optional hook invoked when any run binds to an agent. */
  onRunAssociated: ((agent: AgentLike, run: RunRecord) => void) | undefined
  /** Optional automatic task binding for every main conversation. */
  onAutoRun: ((agent: AgentLike, cwd: string) => boolean) | undefined

  /** @param ctx - host context; @param raw - live config; @param task - the one task pack. */
  constructor(ctx: Context, services: Services, raw: Partial<Config>, task: TaskDefinition) {
    this.ctx = ctx
    this.services = services
    this.rawConfig = raw
    this.task = task
    fs.mkdirSync(guardRoot(), { recursive: true })
    this.refreshTools()
    this.disposers.push(this.services.on('tools/change', () => this.refreshTools()))
    this.disposers.push(this.services.on('agent/created', (payload: { agent?: AgentLike }) => {
      if (payload.agent !== undefined) this.onAgentCreated(payload.agent)
    }))
    this.disposers.push(this.services.on('agent/disposed', (payload: { agent?: AgentLike }) => {
      if (payload.agent !== undefined) this.onAgentDisposed(payload.agent)
    }))
    this.disposers.push(this.services.on('session/event', (session: SessionLike, event: { type?: string; data?: any }) => {
      this.onSessionEvent(session, event)
    }))
    this.disposers.push(this.services.tools.guard(exec => this.checkTool(exec)))
    // The API is registered in apply(); this timer only advances wall-clock work.
    this.interval = setInterval(() => { this.tick() }, 1000)
    if (this.interval !== undefined && typeof (this.interval as any).unref === 'function') (this.interval as any).unref()
  }

  /** Release listeners and the timer. */
  dispose(): void {
    this.disable()
    if (this.interval !== undefined) clearInterval(this.interval)
    this.interval = undefined
    for (const dispose of this.disposers.splice(0)) {
      try { dispose() } catch { /* already gone */ }
    }
  }

  /** Resolve the live volatile configuration document. */
  get configuration(): Config {
    const source = (this.rawConfig ?? {}) as Record<string, any>
    const read = (key: string): unknown => {
      const value = source[key]
      return value !== null && typeof value === 'object' && typeof value.get === 'function' ? value.get() : value
    }
    return resolveConfig({
      enabled: read('enabled') as boolean,
      timeLimitEnabled: read('timeLimitEnabled') as boolean,
      timeLimitMinutes: read('timeLimitMinutes') as number,
      forbiddenPaths: read('forbiddenPaths') as string,
      toolBlacklistUseDefault: read('toolBlacklistUseDefault') as boolean,
      toolBlacklist: read('toolBlacklist') as string,
      cloneSource: read('cloneSource') as string,
      customCloneUrl: read('customCloneUrl') as string,
      pipIndex: read('pipIndex') as string,
      customPipUrl: read('customPipUrl') as string,
      gitProxyUrl: read('gitProxyUrl') as string,
      pipProxyUrl: read('pipProxyUrl') as string,
      cifarProxyUrl: read('cifarProxyUrl') as string,
      cifarMirror: read('cifarMirror') as string,
      customCifarUrl: read('customCifarUrl') as string,
      parallelEval: read('parallelEval') as boolean,
      taskId: read('taskId') as string,
    })
  }

  /** Apply a new configuration document. */
  update(raw: Partial<Config>): void {
    this.rawConfig = raw
    this.syncConfig()
  }

  /** Reconcile the master switch and time limit with the live document. */
  private syncConfig(): void {
    if (!this.rawConfig) return
    const next = this.configuration
    const wasEnabled = this.enabled
    if (!wasEnabled && next.enabled) {
      if (Date.now() < this.enableRetryAt) return
      void this.enable().catch(error => {
        this.failEnable(error instanceof Error ? error.message : String(error))
      })
    } else if (wasEnabled && !next.enabled) {
      this.disable()
    }
  }

  /** Whether the guard is active. */
  get active(): boolean { return this.enabled }

  /** Why the guard refused to enable, if it did. */
  get lastFailure(): string | undefined { return this.lastError }

  /** Installed tool names currently visible in this deployment. */
  private refreshTools(): void {
    try {
      this.toolCache = this.services.tools.schemas().map(tool => tool.name).sort()
    } catch {
      this.toolCache = []
    }
  }

  /** Tools present right now. */
  installedTools(): string[] { return [...this.toolCache] }

  /** The default blacklist for this installation. */
  defaultBlacklist(): string { return generateDefaultBlacklist(this.toolCache) }

  /** The effective blacklist document. */
  blacklistText(): string {
    const config = this.configuration
    return config.toolBlacklistUseDefault ? this.defaultBlacklist() : config.toolBlacklist
  }

  /** Tool names in the effective blacklist. */
  blacklist(): Set<string> { return new Set(parseBlacklist(this.blacklistText())) }

  /** Ordered path rules from configuration. */
  pathRules(): PathRule[] { return parsePathRules(this.configuration.forbiddenPaths) }

  /** Jobs owned by one session (unowned/global jobs must never be attributed). */
  private ownJobs(sessionId: string): JobViewLike[] {
    return (this.services.jobs?.list(sessionId) ?? [])
      .filter(job => job.owner === sessionId)
      .filter(job => job.status === 'running' || job.status === 'stopping')
  }

  /** Live main conversations for the enable warning. */
  runningSessions(): RunningSession[] {
    const out: RunningSession[] = []
    for (const agent of this.services.agents.list()) {
      if (!this.isMain(agent.session)) continue
      const jobs = this.ownJobs(agent.id)
      const running = agent.status === 'running'
      if (!running && jobs.length === 0) continue
      out.push({
        sessionId: agent.id,
        title: sessionTitle(agent.session) ?? agent.id,
        cwd: sessionCwd(agent.session) ?? '',
        running,
        jobs: jobs.length,
      })
    }
    return out
  }

  /** Whether a session is a user conversation (not a subagent/plugin child). */
  isMain(session: SessionLike | undefined): boolean {
    return session !== undefined && !sessionIsSubagent(session)
  }

  /** Approvals are auto-rejected while the guard is active. */
  private forceApproval(session: SessionLike): void {
    const approval = this.services.approval
    if (approval === undefined) return
    const agent = this.services.agents.get(session.id)
    if (agent === undefined) return
    try {
      if (!this.approvalPrev.has(session.id)) this.approvalPrev.set(session.id, approval.effectivePolicy(session))
      approval.setPolicy(agent, 'never')
    } catch {
      // A session can disappear between list() and setPolicy(); do not break enable.
    }
  }

  /** Restore the policy that existed before the guard touched this session. */
  private restoreApproval(sessionId: string): void {
    const previous = this.approvalPrev.get(sessionId)
    if (previous === undefined) return
    this.approvalPrev.delete(sessionId)
    const agent = this.services.agents.get(sessionId)
    const approval = this.services.approval
    if (agent !== undefined && approval !== undefined) approval.setPolicy(agent, previous)
  }

  /** Record an enable-time refusal and back off before retrying. */
  private failEnable(error: string): { ok: false; error: string } {
    this.lastError = error
    this.enableRetryAt = Date.now() + 5_000
    this.record({ kind: 'notice', detail: '评测守护启用失败：' + error })
    try { this.ctx.logger.warn('eval-guard: %s', error) } catch { /* logging is optional */ }
    return { ok: false, error }
  }

  /** Turn the guard on. */
  async enable(): Promise<{ ok: boolean; error?: string }> {
    if (this.enabled) return { ok: true }
    const sandbox = this.services.sandbox
    const policy = this.services.sandboxPolicy
    if (sandbox === undefined || policy === undefined) {
      return this.failEnable('sandbox service is unavailable; refusing to enable without process confinement')
    }
    if (!fs.existsSync(supervisorPath())) {
      return this.failEnable('native supervisor is not built; run npm run build:native')
    }
    try {
      fs.accessSync(supervisorPath(), fs.constants.X_OK)
    } catch {
      return this.failEnable('native supervisor is not executable; run npm run build:native')
    }
    const probe = probeConfinement()
    if (probe !== undefined) return this.failEnable(probe)
    this.sandboxObject = sandbox
    this.policyObject = policy
    this.origConfine = sandbox.confine.bind(sandbox)
    this.origResolve = policy.resolve.bind(policy)
    const guard = this
    sandbox.confine = async function confineHook(argv: readonly string[], executionPolicy: Record<string, unknown>, signal?: AbortSignal): Promise<unknown> {
      if (!guard.enabled) return guard.origConfine!(argv, executionPolicy, signal)
      const sessionId = typeof executionPolicy['sessionId'] === 'string' ? executionPolicy['sessionId'] : undefined
      const session = sessionId === undefined ? undefined : guard.services.sessions.get(sessionId)
      // Agentless calls have no session to scope; everything tied to a session,
      // including subagent sessions, is confined.
      if (session === undefined) return guard.origConfine!(argv, executionPolicy, signal)
      const workspace = sessionCwd(session)
      if (workspace === undefined) throw new Error('dsh-eval-guard: session has no workspace')
      const audit = guard.auditFileFor(session.id)
      // Do not pin cwd to the workspace here: the supervisor prefers its own
      // inherited cwd when it lies inside the workspace, so bash's explicit
      // workdir is preserved. Outside cwd falls back to the workspace.
      const wrap = wrapSandbox(argv, workspace, '', guard.pathRules(), audit)
      return {
        argv: wrap.argv,
        enforcement: 'full',
        denialSignatures: ['operation not permitted', 'permission denied', 'erofs'],
        runnerFailureRules: [{ fatalSignatures: ['dsh-eval-supervisor:'] }],
      }
    }
    policy.resolve = function resolveHook(request: Record<string, unknown> = {}): Record<string, unknown> {
      const resolved = guard.origResolve!(request)
      if (!guard.enabled) return resolved
      const session = request['session'] as SessionLike | undefined
      if (session === undefined) return resolved
      return { ...resolved, mode: 'workspace-write' }
    }
    try {
      for (const agent of this.services.agents.list()) {
        if (this.isMain(agent.session)) this.forceApproval(agent.session)
      }
      try {
        const runtime = runtimeFiles()
        if (runtime.ptcProcess === undefined || runtime.ptcControl === undefined) {
          this.record({ kind: 'notice', detail: '未找到 run_code 运行时（PTC）文件；沙箱内 run_code 可能不可用。' })
        }
      } catch {
        this.record({ kind: 'notice', detail: '无法读取 run_code 运行时（PTC）文件；沙箱内 run_code 可能不可用。' })
      }
      this.enabled = true
      this.lastError = undefined
      this.enableRetryAt = 0
      this.record({ kind: 'notice', detail: '评测守护已启用。' })
      return { ok: true }
    } catch (error) {
      this.enabled = false
      this.restoreHooks()
      return this.failEnable(error instanceof Error ? error.message : String(error))
    }
  }

  /** Restore the original sandbox/policy methods (idempotent). */
  private restoreHooks(): void {
    if (this.sandboxObject !== undefined && this.origConfine !== undefined) this.sandboxObject.confine = this.origConfine
    if (this.policyObject !== undefined && this.origResolve !== undefined) this.policyObject.resolve = this.origResolve
    this.sandboxObject = undefined
    this.policyObject = undefined
    this.origConfine = undefined
    this.origResolve = undefined
  }

  /** Turn the guard off without touching already-running confined processes. */
  disable(): void {
    this.enableRetryAt = 0
    if (!this.enabled) return
    this.enabled = false
    this.restoreHooks()
    for (const sessionId of [...this.approvalPrev.keys()]) {
      try { this.restoreApproval(sessionId) } catch { /* a disposed agent has no policy to restore */ }
    }
    this.deadlines.clear()
    this.lastActivity.clear()
    this.record({ kind: 'notice', detail: '评测守护已停用。' })
  }

  /** Set the deadline for one session. */
  private arm(sessionId: string): void {
    const config = this.configuration
    if (!config.timeLimitEnabled) return
    this.deadlines.set(sessionId, Date.now() + config.timeLimitMinutes * 60_000)
    this.lastActivity.set(sessionId, Date.now())
  }

  /** A new main agent appeared. */
  private onAgentCreated(agent: AgentLike): void {
    if (!this.isMain(agent.session)) return
    this.knownAgents.add(agent.id)
    if (this.enabled) this.forceApproval(agent.session)
    const cwd = sessionCwd(agent.session)
    if (cwd !== undefined && this.enabled) {
      // resume/clear/compact re-emit agent/created for the same session. An
      // active run already bound to this session must not be replaced (that
      // would reset usage and the run's start time); an ended/timeout run is
      // allowed to be rebound by a fresh turn.
      const existingRunId = this.sessionRuns.get(agent.id)
      const existing = existingRunId === undefined ? undefined : this.runs.get(existingRunId)
      if (existing !== undefined && (existing.status === 'active' || existing.status === 'prepared')) return
      try { this.onAutoRun?.(agent, cwd) } catch { /* optional auto-task binding */ }
    }
  }

  /** An agent went away. */
  private onAgentDisposed(agent: AgentLike): void {
    try { this.restoreApproval(agent.id) } catch { /* disposed agents may have no restorable policy */ }
    this.deadlines.delete(agent.id)
    this.startedAt.delete(agent.id)
    this.lastActivity.delete(agent.id)
    const runId = this.sessionRuns.get(agent.id)
    if (runId !== undefined) {
      const run = this.runs.get(runId)
      if (run !== undefined && run.status === 'active') {
        run.endedAt = Date.now()
        run.status = 'ended'
        this.writeRun(run)
      }
    }
  }

  /** Bind an already-registered run to an agent. */
  attachRun(agent: AgentLike, run: RunRecord): void {
    this.runs.set(run.runId, run)
    this.associate(run.runId, agent)
  }

  /** Associate one run with the new session. */
  private associate(runId: string, agent: AgentLike): void {
    const run = this.runs.get(runId)
    if (run === undefined) return
    run.sessionId = agent.id
    run.startedAt = Date.now()
    run.status = 'active'
    run.provider = agent.options?.provider
    run.model = agent.options?.model
    run.title = sessionTitle(agent.session)
    this.sessionRuns.set(agent.id, runId)
    this.writeRun(run)
    this.writeUsage(runId, this.zeroUsage(run.startedAt))
    try { this.onRunAssociated?.(agent, run) } catch { /* task tools are optional */ }
  }

  /** Session event intake. */
  private onSessionEvent(session: SessionLike, event: { type?: string; data?: any }): void {
    if (!this.enabled || session === undefined || !this.isMain(session)) return
    this.lastActivity.set(session.id, Date.now())
    const runId = this.sessionRuns.get(session.id)
    const run = runId === undefined ? undefined : this.runs.get(runId)
    const activeRun = run !== undefined && run.status === 'active'
    if (event.type === 'user/message') {
      const source = event.data?.source
      if (source?.kind === 'user') {
        this.startedAt.set(session.id, Date.now())
        if (this.configuration.timeLimitEnabled) {
          this.arm(session.id)
        } else {
          // This session's new message was sent while the limit is off: it is
          // now unlimited, without touching any other session's deadline.
          this.deadlines.delete(session.id)
        }
      }
    } else if (event.type === 'assistant/message') {
      const usage = event.data?.usage
      if (usage !== undefined && runId !== undefined && activeRun) this.addUsage(runId, usage)
    } else if (event.type === 'tool/call') {
      if (runId !== undefined && activeRun) this.addToolCall(runId)
    } else if (event.type === 'turn/end') {
      if (runId !== undefined && activeRun) this.addTurn(runId)
      this.maybeComplete(session.id)
    } else if (event.type === 'turn/start') {
      if (run !== undefined && run.startedAt === null) run.startedAt = Date.now()
    }
  }

  /** Clear a turn deadline once no work remains for that session (with grace). */
  private maybeComplete(sessionId: string): void {
    const agent = this.services.agents.get(sessionId)
    if (agent?.status === 'running' || this.ownJobs(sessionId).length > 0) return
    const last = this.lastActivity.get(sessionId) ?? 0
    if (Date.now() - last < IDLE_GRACE_MS) return
    this.deadlines.delete(sessionId)
  }

  /** One-second clock: expire deadlines and tail supervisor audits. */
  private tick(): void {
    try {
      this.syncConfig()
      if (!this.enabled) return
      const now = Date.now()
      for (const [sessionId, deadline] of [...this.deadlines]) {
        if (deadline > now) {
          // A finished turn with no owned or descendant work no longer needs a deadline.
          const agent = this.services.agents.get(sessionId)
          const ownJobs = this.ownJobs(sessionId).length > 0
          const last = this.lastActivity.get(sessionId) ?? 0
          const descendantRunning = this.descendants(sessionId).some(id => {
            const child = this.services.agents.get(id)
            if (child?.status === 'running') return true
            return this.ownJobs(id).length > 0
          })
          if (agent?.status !== 'running' && !ownJobs && !descendantRunning && now - last >= IDLE_GRACE_MS) {
            this.deadlines.delete(sessionId)
          }
          continue
        }
        this.deadlines.delete(sessionId)
        this.expire(sessionId)
      }
      this.tailAudits()
      this.cleanupTemp()
    } catch (error) {
      try { this.ctx.logger.warn('eval-guard: tick failed: %s', error instanceof Error ? error.message : String(error)) } catch { /* logging is optional */ }
    }
  }

  /** Kill one session's turn, descendants, and jobs. */
  private expire(sessionId: string): void {
    const reason = '评测时间上限已到，已强制终止当前轮次。'
    const targets = [sessionId, ...this.descendants(sessionId)]
    for (const id of targets) {
      const agent = this.services.agents.get(id)
      if (agent !== undefined && agent.status === 'running') {
        try { agent.cancel({ kind: 'hook', reason }) } catch { /* already gone */ }
      }
      for (const job of this.ownJobs(id)) {
        try { this.services.jobs?.kill(job.id, id, '评测时间上限') } catch { /* already gone */ }
      }
      this.deadlines.delete(id)
      this.lastActivity.delete(id)
    }
    const runId = this.sessionRuns.get(sessionId)
    if (runId !== undefined) {
      const run = this.runs.get(runId)
      if (run !== undefined) {
        run.status = 'timeout'
        run.endedAt = Date.now()
        this.writeRun(run)
        this.record({ kind: 'timeout', runId, sessionId, detail: reason })
        return
      }
    }
    this.record({ kind: 'timeout', sessionId, detail: reason })
  }

  /** Direct and transitive subagent sessions of one main session. */
  private descendants(sessionId: string): string[] {
    const children = new Map<string, string[]>()
    for (const agent of this.services.agents.list()) {
      // parentSession also records fork lineage for user-visible sessions; only
      // origin/delegationDepth identify a real subagent child.
      if (!sessionIsSubagent(agent.session)) continue
      const parent = sessionParent(agent.session)
      if (parent === undefined) continue
      const list = children.get(parent) ?? []
      list.push(agent.id)
      children.set(parent, list)
    }
    const out: string[] = []
    const queue = [...(children.get(sessionId) ?? [])]
    while (queue.length > 0) {
      const id = queue.shift() as string
      if (out.includes(id)) continue
      out.push(id)
      queue.push(...(children.get(id) ?? []))
    }
    return out
  }

  /** Tool guard body. */
  private checkTool(exec: { name: string; arguments: unknown; agent?: AgentLike }): string | undefined {
    if (!this.enabled) return undefined
    const agent = exec.agent
    if (agent === undefined) return undefined
    const name = String(exec.name)
    if (WAIT_TOOLS.includes(name)) {
      this.record({ kind: 'wait-blocked', sessionId: agent.id, tool: name, detail: '评测任务不允许等待用户输入。' })
      return '评测任务要求自主完成，不能等待用户输入；请自行做出决定。'
    }
    if (this.blacklist().has(name)) {
      this.record({ kind: 'tool-blocked', sessionId: agent.id, tool: name, detail: '该工具在评测黑名单中。' })
      return '该工具在本次评测的黑名单中，调用已被阻止。'
    }
    const workspace = sessionCwd(agent.session)
    if (workspace === undefined) {
      if (!isAuditedTool(name)) return undefined
      this.record({ kind: 'path-denied', sessionId: agent.id, tool: name, detail: '会话没有工作区，无法审计路径。' })
      return '评测守护：无法确定会话工作区，文件类工具已被阻止。'
    }
    const decision = auditToolCall(name, exec.arguments, workspace, this.pathRules())
    if (!decision.allow) {
      this.record({
        kind: 'path-denied', sessionId: agent.id, tool: name,
        detail: (decision.write ? '禁止写入 ' : '禁止读取 ') + (decision.path ?? ''),
      })
      return decision.reason ?? '该路径不在允许范围内。'
    }
    return undefined
  }

  /** Audit path for one session: the run's file when active, else a loose audit file. */
  private auditFileFor(sessionId: string): string {
    const runId = this.sessionRuns.get(sessionId)
    if (runId !== undefined) return path.join(guardRoot(), 'runs', runId, 'process-audit.jsonl')
    return path.join(guardRoot(), 'loose', sessionId, 'process-audit.jsonl')
  }

  /** Path of a run's violation ledger. */
  private violationFile(runId: string | undefined, sessionId?: string): string {
    if (runId !== undefined) {
      const dir = path.join(guardRoot(), 'runs', runId)
      fs.mkdirSync(dir, { recursive: true })
      return path.join(dir, 'violations.jsonl')
    }
    const dir = path.join(guardRoot(), 'loose', sessionId ?? 'unknown')
    fs.mkdirSync(dir, { recursive: true })
    return path.join(dir, 'violations.jsonl')
  }

  /** Record one warning in memory and on disk. */
  private record(event: Omit<GuardEvent, 'seq' | 'at'>): void {
    const runId = event.runId ?? (event.sessionId === undefined ? undefined : this.sessionRuns.get(event.sessionId))
    const full: GuardEvent = { ...event, seq: ++this.seq, at: Date.now(), runId }
    this.events.push(full)
    if (this.events.length > 300) this.events.shift()
    try {
      const file = this.violationFile(runId, event.sessionId)
      fs.appendFileSync(file, JSON.stringify(full) + '\n', 'utf8')
    } catch {
      // Auditing must never break the tool path.
    }
  }

  /** Recent events for the polling client. */
  recent(since: number): GuardEvent[] {
    return this.events.filter(event => event.seq > since)
  }

  /** Tail supervisor audit files and convert new lines to guard events. */
  private tailAudits(): void {
    const files: string[] = []
    const runsRoot = path.join(guardRoot(), 'runs')
    try {
      for (const entry of fs.readdirSync(runsRoot, { withFileTypes: true })) {
        if (!entry.isDirectory()) continue
        const file = path.join(runsRoot, entry.name, 'process-audit.jsonl')
        if (fs.existsSync(file)) files.push(file)
      }
    } catch { /* no runs yet */ }
    const looseRoot = path.join(guardRoot(), 'loose')
    try {
      for (const entry of fs.readdirSync(looseRoot, { withFileTypes: true })) {
        if (!entry.isDirectory()) continue
        const file = path.join(looseRoot, entry.name, 'process-audit.jsonl')
        if (fs.existsSync(file)) files.push(file)
      }
    } catch { /* no loose audits */ }
    for (const file of files) {
      const offset = this.offsets.get(file) ?? 0
      let size = 0
      try { size = fs.statSync(file).size } catch { continue }
      if (size <= offset) continue
      try {
        const fd = fs.openSync(file, 'r')
        const length = size - offset
        const buffer = Buffer.alloc(length)
        fs.readSync(fd, buffer, 0, length, offset)
        fs.closeSync(fd)
        this.offsets.set(file, size)
        for (const raw of buffer.toString('utf8').split('\n')) {
          if (raw.trim() === '') continue
          try {
            const parsed = JSON.parse(raw) as { pid?: number; syscall?: string; path?: string; access?: string }
            const runId = file.includes('/runs/') ? path.basename(path.dirname(file)) : undefined
            this.record({
              kind: 'process-denied',
              runId,
              detail: '进程尝试' + (parsed.access === 'write' ? '写入 ' : '读取 ') + (parsed.path ?? '') +
                '（' + (parsed.syscall ?? 'syscall') + '），已终止进程组。',
            })
          } catch { /* partial line */ }
        }
      } catch {
        // audit tailing is best effort
      }
    }
  }

  /** Remove old sandbox policy files. */
  private cleanupTemp(): void {
    const now = Date.now()
    const tmp = path.join(guardRoot(), 'tmp')
    try {
      for (const entry of fs.readdirSync(tmp)) {
        const file = path.join(tmp, entry)
        try {
          if (now - fs.statSync(file).mtimeMs > 3_600_000) fs.unlinkSync(file)
        } catch { /* raced */ }
      }
    } catch { /* no temp dir yet */ }
  }

  /** Zero usage record. */
  private zeroUsage(startedAt: number | null): RunUsage {
    return {
      inputTokens: 0, outputTokens: 0, cacheReadTokens: 0, cacheWriteTokens: 0,
      reasoningTokens: 0, toolCalls: 0, turns: 0, steps: 0, wallTimeMs: 0,
      startedAt, updatedAt: Date.now(),
    }
  }

  /** Read one run's usage. */
  private readUsage(runId: string): RunUsage {
    const file = path.join(guardRoot(), 'runs', runId, 'usage.json')
    try {
      return JSON.parse(fs.readFileSync(file, 'utf8')) as RunUsage
    } catch {
      const run = this.runs.get(runId)
      return this.zeroUsage(run?.startedAt ?? null)
    }
  }

  /** Persist one run's usage. */
  private writeUsage(runId: string, usage: RunUsage): void {
    const dir = path.join(guardRoot(), 'runs', runId)
    fs.mkdirSync(dir, { recursive: true })
    usage.updatedAt = Date.now()
    const started = usage.startedAt ?? this.runs.get(runId)?.startedAt ?? null
    usage.wallTimeMs = started === null ? 0 : Date.now() - started
    fs.writeFileSync(path.join(dir, 'usage.json'), JSON.stringify(usage, null, 2) + '\n', 'utf8')
  }

  /** Add one model usage record. */
  private addUsage(runId: string, usage: { inputTokens?: number; outputTokens?: number; cacheReadTokens?: number; cacheWriteTokens?: number; reasoningTokens?: number }): void {
    const current = this.readUsage(runId)
    current.inputTokens += usage.inputTokens ?? 0
    current.outputTokens += usage.outputTokens ?? 0
    current.cacheReadTokens += usage.cacheReadTokens ?? 0
    current.cacheWriteTokens += usage.cacheWriteTokens ?? 0
    current.reasoningTokens += usage.reasoningTokens ?? 0
    current.steps += 1
    this.writeUsage(runId, current)
  }

  /** Count one tool call. */
  private addToolCall(runId: string): void {
    const usage = this.readUsage(runId)
    usage.toolCalls += 1
    this.writeUsage(runId, usage)
  }

  /** Count one completed turn. */
  private addTurn(runId: string): void {
    const usage = this.readUsage(runId)
    usage.turns += 1
    this.writeUsage(runId, usage)
  }

  /** Write one run record. */
  private writeRun(run: RunRecord): void {
    const dir = path.join(guardRoot(), 'runs', run.runId)
    fs.mkdirSync(dir, { recursive: true })
    fs.writeFileSync(path.join(dir, 'run.json'), JSON.stringify(run, null, 2) + '\n', 'utf8')
  }

  /** Register a freshly created run (called by the task installer). */
  registerRun(run: RunRecord): void {
    this.runs.set(run.runId, run)
    this.writeRun(run)
    this.writeUsage(run.runId, this.zeroUsage(null))
    fs.writeFileSync(path.join(guardRoot(), 'runs', run.runId, 'violations.jsonl'), '', 'utf8')
  }

  /** Resolve the registered run bound to one agent session. */
  runForSession(sessionId: string | undefined): RunRecord | undefined {
    if (sessionId === undefined) return undefined
    const runId = this.sessionRuns.get(sessionId)
    return runId === undefined ? undefined : this.runs.get(runId)
  }

  /** Build the prompt for one task with only installed tools listed. */
  promptFor(workspacePath: string, source: string, task: TaskDefinition = this.task): string {
    void workspacePath
    void source
    return task.prompt({ minutes: this.configuration.timeLimitMinutes })
  }

  /** The task definition. */
  get definition(): TaskDefinition { return this.task }

  /** Route constant kept close to the service that owns it. */
  get eventsRoute(): string { return EVENTS_ROUTE }
}

/** DSH home re-export for routes. */
export { dshHome }

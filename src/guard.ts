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
  auditToolCall, generateDefaultBlacklist, isBaseBlacklistName, parseBlacklist, parsePathRules, WAIT_TOOLS, type PathRule,
} from './policy.ts'
import { dshHome, guardRoot, supervisorPath, wrapSandbox } from './sandbox.ts'

/** Minimal Agent shape this plugin consumes. */
interface AgentLike {
  id: string
  status: string
  options?: { provider?: string; model?: string }
  session: SessionLike
  cancel(cause: { kind: 'hook'; reason: string }, options?: { keepInbox?: boolean }): void
}

/** Minimal Session shape this plugin consumes. */
interface SessionLike {
  id: string
  header?: { meta?: { cwd?: string; origin?: string; parentSession?: string; delegationDepth?: number; title?: string } }
  append?(type: string, data: unknown): void
}

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
    list(caller?: string): { id: string; status: string; owner?: string }[]
    kill(id: string, caller?: string, reason?: string): string
  }
  workspaceRegistry?: { get(id: string): { path: string; title?: string } | undefined }
  on(event: string, listener: (...args: any[]) => void): () => void
}

/** One installed task definition. */
export interface TaskDefinition {
  id: string
  label: string
  description: string
  asset: string
  prompt: (blacklist: readonly string[], options: { source: string; workspacePath: string; minutes: number }) => string
}

/** Resolve a workspace path from a workspace id. */
function mainOf(guard: Guard): void {
  void guard
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
  private readonly runs = new Map<string, RunRecord>()
  private readonly sessionRuns = new Map<string, string>()
  private readonly pending = new Map<string, { runId: string; expiresAt: number }>()
  private readonly offsets = new Map<string, number>()
  private readonly knownAgents = new Set<string>()
  private toolCache: string[] = []
  private readonly disposers: (() => void)[] = []
  private readonly task: TaskDefinition

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
    if (!wasEnabled && next.enabled) void this.enable()
    else if (wasEnabled && !next.enabled) this.disable()
    else if (this.enabled && !next.timeLimitEnabled) this.deadlines.clear()
    else if (this.enabled && next.timeLimitEnabled) this.rearmRunning()
  }

  /** Whether the guard is active. */
  get active(): boolean { return this.enabled }

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

  /** Live conversations for the enable warning. */
  runningSessions(): RunningSession[] {
    const out: RunningSession[] = []
    for (const agent of this.services.agents.list()) {
      const jobs = this.services.jobs?.list(agent.id) ?? []
      const running = agent.status === 'running'
      const jobCount = jobs.filter(job => job.status === 'running' || job.status === 'stopping').length
      if (!running && jobCount === 0) continue
      out.push({
        sessionId: agent.id,
        title: (agent.session.header?.meta?.title as string | undefined) ?? agent.id,
        cwd: agent.session.header?.meta?.cwd ?? '',
        running,
        jobs: jobCount,
      })
    }
    return out
  }

  /** Whether a session is a user main conversation (not a subagent or plugin child). */
  isMain(session: SessionLike | undefined): boolean {
    if (session === undefined) return false
    const meta = session.header?.meta
    if (meta?.origin === 'subagent') return false
    if (meta?.parentSession !== undefined || meta?.delegationDepth !== undefined) return false
    return true
  }

  /** Approvals are auto-rejected while the guard is active. */
  private forceApproval(session: SessionLike): void {
    const approval = this.services.approval
    if (approval === undefined) return
    const agent = this.services.agents.get(session.id)
    if (agent === undefined) return
    if (!this.approvalPrev.has(session.id)) this.approvalPrev.set(session.id, approval.effectivePolicy(session))
    approval.setPolicy(agent, 'never')
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

  /** Turn the guard on. */
  async enable(): Promise<{ ok: boolean; error?: string }> {
    if (this.enabled) return { ok: true }
    const sandbox = this.services.sandbox
    const policy = this.services.sandboxPolicy
    if (sandbox === undefined || policy === undefined) {
      return { ok: false, error: 'sandbox service is unavailable; refusing to enable without process confinement' }
    }
    if (!fs.existsSync(supervisorPath())) {
      return { ok: false, error: 'native supervisor is not built; run npm run build:native' }
    }
    this.sandboxObject = sandbox
    this.policyObject = policy
    this.origConfine = sandbox.confine.bind(sandbox)
    this.origResolve = policy.resolve.bind(policy)
    const guard = this
    sandbox.confine = async function confineHook(argv: readonly string[], executionPolicy: Record<string, unknown>, signal?: AbortSignal): Promise<unknown> {
      if (!guard.enabled) return guard.origConfine!(argv, executionPolicy, signal)
      const sessionId = typeof executionPolicy['sessionId'] === 'string' ? executionPolicy['sessionId'] : undefined
      const session = sessionId === undefined ? undefined : guard.services.sessions.get(sessionId)
      if (session === undefined || !guard.isMain(session)) return guard.origConfine!(argv, executionPolicy, signal)
      const workspace = typeof executionPolicy['workspaceRoot'] === 'string' ? executionPolicy['workspaceRoot'] : session.header?.meta?.cwd
      if (workspace === undefined || workspace === '') throw new Error('dsh-eval-guard: session has no workspace')
      const audit = guard.auditFileFor(session.id)
      const wrap = wrapSandbox(argv, workspace, workspace, guard.pathRules(), audit)
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
      if (session === undefined || !guard.isMain(session)) return resolved
      return { ...resolved, mode: 'workspace-write' }
    }
    this.enabled = true
    this.rearmRunning()
    for (const agent of this.services.agents.list()) {
      if (this.isMain(agent.session)) this.forceApproval(agent.session)
    }
    this.record({ kind: 'notice', detail: '评测守护已启用。' })
    return { ok: true }
  }

  /** Turn the guard off without touching already-running confined processes. */
  disable(): void {
    if (!this.enabled) return
    this.enabled = false
    if (this.sandboxObject !== undefined && this.origConfine !== undefined) this.sandboxObject.confine = this.origConfine
    if (this.policyObject !== undefined && this.origResolve !== undefined) this.policyObject.resolve = this.origResolve
    for (const sessionId of [...this.approvalPrev.keys()]) this.restoreApproval(sessionId)
    this.deadlines.clear()
    this.record({ kind: 'notice', detail: '评测守护已停用。' })
  }

  /** Re-arm deadlines for running main conversations after an enable/config change. */
  private rearmRunning(): void {
    if (!this.enabled || !this.configuration.timeLimitEnabled) return
    for (const agent of this.services.agents.list()) {
      if (!this.isMain(agent.session)) continue
      const jobs = (this.services.jobs?.list(agent.id) ?? []).filter(job => job.status === 'running' || job.status === 'stopping')
      if (agent.status === 'running' || jobs.length > 0) {
        this.arm(agent.session.id)
      }
    }
  }

  /** Set the deadline for one session. */
  private arm(sessionId: string): void {
    const config = this.configuration
    if (!config.timeLimitEnabled) return
    this.deadlines.set(sessionId, Date.now() + config.timeLimitMinutes * 60_000)
  }

  /** A new main agent appeared. */
  private onAgentCreated(agent: AgentLike): void {
    if (!this.isMain(agent.session)) return
    this.knownAgents.add(agent.id)
    if (this.enabled) this.forceApproval(agent.session)
    const cwd = agent.session.header?.meta?.cwd
    if (cwd !== undefined) {
      const pending = this.pending.get(cwd)
      if (pending !== undefined && pending.expiresAt > Date.now()) {
        this.pending.delete(cwd)
        this.associate(pending.runId, agent)
      }
    }
  }

  /** An agent went away. */
  private onAgentDisposed(agent: AgentLike): void {
    this.restoreApproval(agent.id)
    this.deadlines.delete(agent.id)
    this.startedAt.delete(agent.id)
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

  /** Associate one pending run with the new session. */
  private associate(runId: string, agent: AgentLike): void {
    const run = this.runs.get(runId)
    if (run === undefined) return
    run.sessionId = agent.id
    run.startedAt = Date.now()
    run.status = 'active'
    run.provider = agent.options?.provider
    run.model = agent.options?.model
    run.title = agent.session.header?.meta?.title
    this.sessionRuns.set(agent.id, runId)
    this.writeRun(run)
    this.writeUsage(runId, this.zeroUsage(run.startedAt))
  }

  /** Session event intake. */
  private onSessionEvent(session: SessionLike, event: { type?: string; data?: any }): void {
    if (!this.enabled || session === undefined || !this.isMain(session)) return
    const runId = this.sessionRuns.get(session.id)
    const run = runId === undefined ? undefined : this.runs.get(runId)
    if (event.type === 'user/message') {
      const source = event.data?.source
      if (source?.kind === 'user') {
        this.startedAt.set(session.id, Date.now())
        this.arm(session.id)
      }
    } else if (event.type === 'assistant/message') {
      const usage = event.data?.usage
      if (usage !== undefined && runId !== undefined) this.addUsage(runId, usage)
    } else if (event.type === 'tool/call') {
      if (runId !== undefined) this.addToolCall(runId)
    } else if (event.type === 'turn/end') {
      if (runId !== undefined) this.addTurn(runId)
      this.maybeComplete(session.id)
    } else if (event.type === 'turn/start') {
      if (run !== undefined && run.startedAt === null) run.startedAt = Date.now()
    }
  }

  /** Clear a turn deadline once no work remains for that session. */
  private maybeComplete(sessionId: string): void {
    const agent = this.services.agents.get(sessionId)
    const jobs = (this.services.jobs?.list(sessionId) ?? []).filter(job => job.status === 'running' || job.status === 'stopping')
    if (agent?.status === 'running' || jobs.length > 0) return
    this.deadlines.delete(sessionId)
  }

  /** One-second clock: expire deadlines and tail supervisor audits. */
  private tick(): void {
    this.syncConfig()
    if (!this.enabled) return
    const now = Date.now()
    for (const [sessionId, deadline] of [...this.deadlines]) {
      if (deadline > now) {
        // A finished turn with no owned or descendant work no longer needs a deadline.
        const agent = this.services.agents.get(sessionId)
        const ownJobs = (this.services.jobs?.list(sessionId) ?? [])
          .some(job => job.status === 'running' || job.status === 'stopping')
        const descendantRunning = this.descendants(sessionId).some(id => {
          const child = this.services.agents.get(id)
          if (child?.status === 'running') return true
          return (this.services.jobs?.list(id) ?? []).some(job => job.status === 'running' || job.status === 'stopping')
        })
        if (agent?.status !== 'running' && !ownJobs && !descendantRunning) this.deadlines.delete(sessionId)
        continue
      }
      this.deadlines.delete(sessionId)
      this.expire(sessionId)
    }
    this.tailAudits()
    this.cleanupTemp()
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
      for (const job of this.services.jobs?.list(id) ?? []) {
        if (job.status === 'running' || job.status === 'stopping') {
          try { this.services.jobs?.kill(job.id, id, '评测时间上限') } catch { /* already gone */ }
        }
      }
    }
    const runId = this.sessionRuns.get(sessionId)
    if (runId !== undefined) {
      const run = this.runs.get(runId)
      if (run !== undefined) {
        run.status = 'timeout'
        run.endedAt = Date.now()
        this.writeRun(run)
        this.record({ kind: 'timeout', runId, sessionId, detail: reason })
      }
    } else {
      this.record({ kind: 'timeout', sessionId, detail: reason })
    }
  }

  /** Direct and transitive subagent sessions of one main session. */
  private descendants(sessionId: string): string[] {
    const all = this.services.agents.list()
    const children = new Map<string, string[]>()
    for (const agent of all) {
      const parent = agent.session.header?.meta?.parentSession
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
    if (agent === undefined || !this.isMain(agent.session)) return undefined
    const name = String(exec.name)
    if (WAIT_TOOLS.includes(name)) {
      this.record({ kind: 'wait-blocked', sessionId: agent.id, tool: name, detail: '评测任务不允许等待用户输入。' })
      return '评测任务要求自主完成，不能等待用户输入；请自行做出决定。'
    }
    if (this.blacklist().has(name)) {
      this.record({ kind: 'tool-blocked', sessionId: agent.id, tool: name, detail: '该工具在评测黑名单中。' })
      return '该工具在本次评测的黑名单中，调用已被阻止。'
    }
    const workspace = agent.session.header?.meta?.cwd
    if (workspace === undefined || workspace === '') return undefined
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

  /** Remove old sandbox policy files and expired pending associations. */
  private cleanupTemp(): void {
    const now = Date.now()
    for (const [workspace, pending] of [...this.pending]) {
      if (pending.expiresAt <= now) this.pending.delete(workspace)
    }
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
    this.pending.set(run.workspacePath, { runId: run.runId, expiresAt: Date.now() + 6 * 60 * 60_000 })
  }

  /** Build the prompt for one task with only installed tools listed. */
  promptFor(workspacePath: string, source: string): string {
    const present = new Set(this.toolCache)
    const blacklist = parseBlacklist(this.blacklistText()).filter(name => isBaseBlacklistName(name) || present.has(name))
    return this.task.prompt(blacklist, { source, workspacePath, minutes: this.configuration.timeLimitMinutes })
  }

  /** The task definition. */
  get definition(): TaskDefinition { return this.task }

  /** Route constant kept close to the service that owns it. */
  get eventsRoute(): string { return EVENTS_ROUTE }
}

/** DSH home re-export for routes. */
export { dshHome }

/**
 * Per-run agent guards:
 *  - periodic background-job reminder attached to the next tool result;
 *  - turn-stopping continuation while the evaluation is incomplete;
 *  - a short strategy reminder after a failed eval_test.
 *
 * Auditor/agent-facing text never lists allowed implementation techniques.
 */
import fs from 'node:fs'
import path from 'node:path'
import { randomUUID } from 'node:crypto'
import type { Guard } from './guard.ts'
import type { TaskService } from './task.ts'
import { NEURAL_PENDULUM_TASK, type RunRecord } from './shared.ts'

type AnyAgent = {
  id: string
  session?: { id?: string }
  inbox?: { nextTurn?: readonly any[]; nextStep?: readonly any[] }
  steer?: (message: any) => void
  followup?: (message: any) => void
  cancel?: (cause: any, options?: any) => void
  whenIdle?: () => Promise<void>
}

type Services = {
  jobs?: { list(caller?: string): any[] }
  /** Optional host event bus; used to notice a real user abort between guard resets. */
  on?: (event: string, listener: (...args: any[]) => void) => void
}

type SessionState = {
  lastJobReminder: number
  evalFailures: number
  ownMessageIds: Set<string>
  /** Time of the last tool result, used to tell a text-only final from a real stop. */
  lastToolAt: number
  /** Time of the last steer/follow-up; a newer tool result means the agent actually worked. */
  lastSteerAt: number
  /** Consecutive same-turn text-only resets; capped so a refusing model cannot spin forever. */
  resets: number
  /** A cancel/reopen is in flight. */
  recovering: boolean
  /** Set when the user aborted a turn; suppresses an already-scheduled reopen. */
  userStopped: boolean
}

function sessionIdOf(agent: AnyAgent): string {
  return String(agent.session?.id ?? agent.id)
}

function makeNotice(text: string): any {
  return {
    id: randomUUID(),
    role: 'user',
    content: [{ type: 'text', text }],
    source: { kind: 'user' },
  }
}

function ageMinutes(startedAt: unknown): string {
  const value = Number(startedAt)
  if (!Number.isFinite(value) || value <= 0) return '?'
  const minutes = Math.max(0, Math.floor((Date.now() - value) / 60_000))
  return minutes < 60 ? minutes + 'm' : Math.floor(minutes / 60) + 'h' + String(minutes % 60).padStart(2, '0')
}

function activeJobs(services: Services, sessionId: string): any[] {
  try {
    const list = services.jobs?.list(sessionId) ?? []
    return list.filter(job => job?.status === 'running' || job?.status === 'stopping')
  } catch {
    return []
  }
}

function workspaceProcesses(workspace: string): Array<{ pid: number; cmd: string }> {
  let root: string
  try {
    root = fs.realpathSync(workspace)
  } catch {
    root = path.resolve(workspace)
  }
  const out: Array<{ pid: number; cmd: string }> = []
  let entries: string[] = []
  try {
    entries = fs.readdirSync('/proc')
  } catch {
    return out
  }
  for (const name of entries) {
    if (!/^[0-9]+$/.test(name)) continue
    const pid = Number(name)
    if (pid === process.pid) continue
    let cwd = ''
    try {
      cwd = fs.realpathSync('/proc/' + name + '/cwd')
    } catch {
      continue
    }
    if (cwd !== root && !cwd.startsWith(root + path.sep)) continue
    let cmd = ''
    try {
      cmd = fs.readFileSync('/proc/' + name + '/cmdline', 'utf8').split('\u0000').filter(Boolean).join(' ')
    } catch {
      continue
    }
    if (cmd === '') continue
    out.push({ pid, cmd })
    if (out.length >= 12) break
  }
  return out
}

function jobReminderText(services: Services, sessionId: string, run: RunRecord): string | undefined {
  const jobs = activeJobs(services, sessionId)
  const procs = workspaceProcesses(run.workspacePath)
  if (jobs.length === 0 && procs.length === 0) return undefined
  const lines: string[] = ['[eval-guard: background jobs]']
  if (jobs.length > 0) {
    lines.push('Host-managed running jobs: ' + String(jobs.length))
    for (const job of jobs.slice(0, 8)) {
      lines.push('- ' + String(job.id) + ' ' + String(job.kind ?? '') + ' ' + ageMinutes(job.startedAt) +
        ' — ' + String(job.label ?? '').split('\n')[0].slice(0, 180))
    }
  }
  if (procs.length > 0) {
    lines.push('OS processes with cwd in this workspace: ' + String(procs.length))
    for (const proc of procs.slice(0, 8)) {
      lines.push('- pid ' + String(proc.pid) + ' — ' + proc.cmd.slice(0, 220))
    }
  }
  lines.push('Kill unused jobs/processes now. Do not launch a duplicate trainer. Use managed run_in_background; never nohup/setsid/&. Keep at most 2 long-running jobs.')
  return lines.join('\n')
}

function failureText(last: Record<string, unknown> | undefined, failures: number): string {
  const rate = last?.['success_rate']
  const stage = String(last?.['stage'] ?? 'unknown')
  const prefix = '[eval-guard: evaluation not complete]'
  const status = rate === undefined ? 'stage=' + stage : 'stage=' + stage + ', success_rate=' + String(rate)
  let text = prefix + ' ' + status + '. Do not stop, summarize, or finalize. Clean unused background jobs, then continue with a materially different approach. Only stage=completed (success_rate >= 0.90) or an external manual stop ends the task.'
  if (failures >= 2) {
    text += ' This is the ' + String(failures) + 'th consecutive failed/low evaluation. Do not repeat the same configuration; change the hypothesis or method substantially.'
  }
  return text
}

function actionText(last: Record<string, unknown> | undefined, jobs: number): string {
  const rate = last?.['success_rate']
  const stage = String(last?.['stage'] ?? 'unknown')
  const status = rate === undefined ? 'stage=' + stage : 'stage=' + stage + ', success_rate=' + String(rate)
  const jobHint = jobs > 0 ? ' Clean the ' + String(jobs) + ' running host job(s) you no longer need first.' : ''
  return '[eval-guard: action required] Evaluation is not complete (' + status + '). '
    + 'Do not write a status report or final answer. Your next response MUST contain at least one tool call '
    + 'that makes progress: run an experiment, inspect a result, submit, or run eval_test.' + jobHint
    + ' A prose-only reply will not move the task forward. Only stage=completed (success_rate >= 0.90) or an external manual stop ends the task.'
}

/** Install the per-run reminders once for all agents; each listener filters by run. */
export function installAgentRunGuards(ctx: any, services: Services, guard: Guard, tasks: TaskService): void {
  const states = new Map<string, SessionState>()
  const stateFor = (sessionId: string): SessionState => {
    let state = states.get(sessionId)
    if (state === undefined) {
      state = { lastJobReminder: 0, evalFailures: 0, ownMessageIds: new Set(), lastToolAt: 0, lastSteerAt: 0, resets: 0, recovering: false, userStopped: false }
      states.set(sessionId, state)
    }
    return state
  }
  services.on?.('session/event', (session: { id?: string }, event: { type?: string; data?: any }) => {
    if (session?.id === undefined || event?.type !== 'turn/end') return
    const reason = event.data?.reason
    if (reason?.kind === 'aborted' && reason?.reason?.kind === 'user') {
      stateFor(String(session.id)).userStopped = true
    }
  })

  const runFor = (agent: AnyAgent | undefined): RunRecord | undefined => {
    if (agent === undefined) return undefined
    try {
      return guard.runForSession(sessionIdOf(agent))
    } catch {
      return undefined
    }
  }
  const isPendulumActive = (run: RunRecord | undefined): run is RunRecord => {
    return run !== undefined && run.task === NEURAL_PENDULUM_TASK && run.status === 'active' && !tasks.isCompleted(run)
  }

  // A real user message starts a new attempt and resets the no-tool recovery budget.
  ctx.on('agent/pre-step', ({ agent, messages }: { agent: AnyAgent; messages?: readonly any[] }, next: () => Promise<any>) => {
    if (agent !== undefined && messages !== undefined) {
      const state = stateFor(sessionIdOf(agent))
      for (const message of messages) {
        if (message?.source?.kind === 'user' && !state.ownMessageIds.has(String(message?.id))) {
          state.lastSteerAt = 0
          state.resets = 0
          state.userStopped = false
        }
      }
    }
    return next()
  })

  ctx.on('tools/post-execute', async (exec: any, result: any, next: any) => {
    const downstream = await next()
    const run = runFor(exec.agent)
    if (!isPendulumActive(run)) return downstream
    const sessionId = sessionIdOf(exec.agent)
    const state = stateFor(sessionId)
    const notes: string[] = []
    const cfg = guard.configuration
    const interval = Number(cfg.jobReminderMinutes ?? 5) * 60_000
    const now = Date.now()
    state.lastToolAt = now
    // A real tool result after a continuation/reset means the model is working again.
    if (state.lastSteerAt > 0 && state.lastToolAt > state.lastSteerAt) state.resets = 0
    if (interval > 0 && now - state.lastJobReminder >= interval) {
      state.lastJobReminder = now
      const reminder = jobReminderText(services, sessionId, run)
      if (reminder !== undefined) notes.push(reminder)
    }
    if (exec.name === 'eval_test') {
      const value = result?.value as any
      const rate = typeof value?.success_rate === 'number' ? value.success_rate : undefined
      const stage = String(value?.stage ?? '')
      if (rate !== undefined && rate < 0.9) {
        state.evalFailures += 1
        notes.push(failureText(tasks.lastTest(run), state.evalFailures))
      } else if (stage === 'completed' || (rate !== undefined && rate >= 0.9)) {
        state.evalFailures = 0
      }
    }
    if (notes.length === 0) return downstream
    const contexts = notes.map(makeNotice)
    for (const message of contexts) state.ownMessageIds.add(String(message.id))
    if (downstream?.kind === 'block') {
      return { kind: 'block', feedback: downstream.feedback, additionalContexts: [...contexts, ...(downstream.additionalContexts ?? [])] }
    }
    return { ...downstream, additionalContexts: [...contexts, ...(downstream?.additionalContexts ?? [])] }
  })

  ctx.on('agent/turn-stopping', ({ agent, signal }: { agent: AnyAgent; signal: AbortSignal }) => {
    if (signal.aborted) return
    const run = runFor(agent)
    if (!isPendulumActive(run)) return
    const sessionId = sessionIdOf(agent)
    const state = stateFor(sessionId)
    if (state.recovering) return
    // A real user message / explicit stop must pass through. Our own notices are
    // user-role messages too; ignore those by id.
    const pending = [...(agent.inbox?.nextTurn ?? []), ...(agent.inbox?.nextStep ?? [])]
      .some(message => message?.source?.kind === 'user' && !state.ownMessageIds.has(String(message?.id)))
    if (pending) return
    if (typeof agent.steer !== 'function') return
    const now = Date.now()
    const last = tasks.lastTest(run)
    const jobs = activeJobs(services, sessionId).length
    // The model already had one continuation after its last tool call and
    // produced another text-only final. Do not inject the same message forever:
    // close this turn and start one fresh turn with an action-only prompt.
    if (state.lastSteerAt > 0 && state.lastToolAt < state.lastSteerAt) {
      if (state.resets >= 2) {
        // The model refuses to call tools even after two fresh turns. Stop the
        // turn without a final answer; the user can resume manually. This is
        // deliberately bounded rather than an endless same-turn steer loop.
        try { agent.cancel?.({ kind: 'hook', reason: 'eval-guard: incomplete run keeps producing text-only steps' }) } catch { /* gone */ }
        return
      }
      state.resets += 1
      state.recovering = true
      try {
        agent.cancel?.({ kind: 'hook', reason: 'eval-guard: incomplete run produced no tool call; reopening turn' })
      } catch {
        state.recovering = false
        return
      }
      const prompt = actionText(last, jobs) + ' This is a fresh turn; call a tool in your first response.'
      const message = makeNotice(prompt)
      state.ownMessageIds.add(String(message.id))
      void (agent.whenIdle?.() ?? Promise.resolve()).then(() => {
        state.recovering = false
        // A real user message reset lastSteerAt; do not override a manual turn.
        if (state.lastSteerAt === 0 || state.userStopped) return
        if (!isPendulumActive(run) || typeof agent.followup !== 'function') return
        state.lastSteerAt = Date.now()
        agent.followup(message)
      }).catch(() => { state.recovering = false })
      return
    }
    // New tool work (or the first incomplete final): ask once for action.
    state.lastSteerAt = now
    const message = makeNotice(actionText(last, jobs))
    state.ownMessageIds.add(String(message.id))
    agent.steer(message)
  })
}

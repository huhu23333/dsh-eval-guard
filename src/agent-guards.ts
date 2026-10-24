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
}

type Services = { jobs?: { list(caller?: string): any[] } }

type SessionState = {
  lastJobReminder: number
  evalFailures: number
  ownMessageIds: Set<string>
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

/** Install the per-run reminders once for all agents; each listener filters by run. */
export function installAgentRunGuards(ctx: any, services: Services, guard: Guard, tasks: TaskService): void {
  const states = new Map<string, SessionState>()
  const stateFor = (sessionId: string): SessionState => {
    let state = states.get(sessionId)
    if (state === undefined) {
      state = { lastJobReminder: 0, evalFailures: 0, ownMessageIds: new Set() }
      states.set(sessionId, state)
    }
    return state
  }
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
    // A real user message / explicit stop must pass through. Our own notices are
    // user-role messages too; ignore those by id.
    const pending = [...(agent.inbox?.nextTurn ?? []), ...(agent.inbox?.nextStep ?? [])]
      .some(message => message?.source?.kind === 'user' && !state.ownMessageIds.has(String(message?.id)))
    if (pending) return
    if (typeof agent.steer !== 'function') return
    const last = tasks.lastTest(run)
    const jobs = activeJobs(services, sessionId).length
    const procs = workspaceProcesses(run.workspacePath).length
    const text = failureText(last, state.evalFailures) +
      (jobs + procs > 0 ? '\nThere are ' + String(jobs) + ' host jobs and ' + String(procs) + ' workspace processes running; clean the unused ones first.' : '')
    const message = makeNotice(text)
    state.ownMessageIds.add(String(message.id))
    agent.steer(message)
  })
}

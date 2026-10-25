/**
 * Unit tests for the per-run agent guards (no DSH host required).
 */
import assert from 'node:assert/strict'
import { installAgentRunGuards } from '../src/agent-guards.ts'
import { NEURAL_PENDULUM_TASK } from '../src/shared.ts'

function harness(opts: { completed?: boolean; pendingUser?: boolean; aborted?: boolean } = {}) {
  const handlers: Record<string, any> = {}
  const ctx = { on: (name: string, fn: any) => { handlers[name] = fn } }
  const run = {
    task: NEURAL_PENDULUM_TASK,
    status: 'active',
    workspacePath: process.cwd(),
  }
  const listeners: Record<string, any> = {}
  const services = {
    jobs: { list: () => [{ id: 'bash-x', kind: 'bash', label: 'trainer', status: 'running', startedAt: Date.now() - 60_000 }] },
    on: (name: string, fn: any) => { listeners[name] = fn },
  }
  const guard = { configuration: { jobReminderMinutes: 5 }, runForSession: () => run }
  const tasks = { isCompleted: () => opts.completed === true, lastTest: () => ({ success_rate: 0, stage: 'scoring' }) }
  const steered: any[] = []
  const followed: any[] = []
  const cancelled: any[] = []
  const agent = {
    id: 's1', session: { id: 's1' },
    inbox: { nextTurn: opts.pendingUser ? [{ id: 'user-1', source: { kind: 'user' } }] : [], nextStep: [] },
    steer: (message: any) => { steered.push(message) },
    followup: (message: any) => { followed.push(message) },
    cancel: (cause: any) => { cancelled.push(cause) },
    whenIdle: async () => {},
  }
  installAgentRunGuards(ctx as any, services as any, guard as any, tasks as any)
  const userAbort = () => listeners['session/event']?.({ id: 's1' }, { type: 'turn/end', data: { reason: { kind: 'aborted', reason: { kind: 'user' } } } })
  return { handlers, agent, steered, followed, cancelled, userAbort }
}

const post = async (h: ReturnType<typeof harness>, name = 'bash', value: any = undefined) => {
  const downstream = { kind: 'accept' as const }
  const out = await h.handlers['tools/post-execute'](
    { agent: h.agent, name, arguments: {} },
    { value },
    async () => downstream,
  )
  return out
}

// A due reminder is attached to the next tool result.
{
  const h = harness()
  const out = await post(h)
  assert.equal(out.additionalContexts.length, 1)
  assert.match(out.additionalContexts[0].content[0].text, /background jobs/i)
  assert.match(out.additionalContexts[0].content[0].text, /bash-x/)
}

// The second tool result before 5 minutes has no reminder.
{
  const h = harness()
  await post(h)
  const out = await post(h)
  assert.equal(out.additionalContexts, undefined)
}

// A failed eval produces a continuation note, and does not stop the turn.
{
  const h = harness()
  await post(h)
  const out = await post(h, 'eval_test', { success_rate: 0, stage: 'scoring' })
  assert.equal(out.additionalContexts.length, 1)
  assert.match(out.additionalContexts[0].content[0].text, /evaluation not complete/i)
}

// Incomplete turn is steered.
{
  const h = harness()
  h.handlers['agent/turn-stopping']({ agent: h.agent, signal: { aborted: false } })
  assert.equal(h.steered.length, 1)
  assert.match(h.steered[0].content[0].text, /Do not write a status report/i)
}

// Completed task is not steered.
{
  const h = harness({ completed: true })
  h.handlers['agent/turn-stopping']({ agent: h.agent, signal: { aborted: false } })
  assert.equal(h.steered.length, 0)
}

// A pending real user message is a manual stop/new instruction: no steer.
{
  const h = harness({ pendingUser: true })
  h.handlers['agent/turn-stopping']({ agent: h.agent, signal: { aborted: false } })
  assert.equal(h.steered.length, 0)
}

// An aborted signal (user stop / time limit) is not steered.
{
  const h = harness()
  h.handlers['agent/turn-stopping']({ agent: h.agent, signal: { aborted: true } })
  assert.equal(h.steered.length, 0)
}

// A text-only final that ignores the first steer resets the turn instead of
// injecting the same message again.
{
  const h = harness()
  h.handlers['agent/turn-stopping']({ agent: h.agent, signal: { aborted: false } })
  h.handlers['agent/turn-stopping']({ agent: h.agent, signal: { aborted: false } })
  await new Promise(resolve => setTimeout(resolve, 0))
  assert.equal(h.steered.length, 1)
  assert.equal(h.cancelled.length, 1)
  assert.equal(h.followed.length, 1)
  assert.match(h.followed[0].content[0].text, /fresh turn/i)
}

// A user abort after the guard scheduled a reopen suppresses the reopen.
{
  const h = harness()
  h.handlers['agent/turn-stopping']({ agent: h.agent, signal: { aborted: false } })
  h.handlers['agent/turn-stopping']({ agent: h.agent, signal: { aborted: false } })
  h.userAbort()
  await new Promise(resolve => setTimeout(resolve, 0))
  assert.equal(h.cancelled.length, 1)
  assert.equal(h.followed.length, 0)
}

// Repeated text-only turns are bounded: after two resets the turn is cancelled
// without another follow-up, so a refusing model cannot spin forever.
{
  const h = harness()
  for (let i = 0; i < 4; i += 1) {
    h.handlers['agent/turn-stopping']({ agent: h.agent, signal: { aborted: false } })
    await new Promise(resolve => setTimeout(resolve, 0))
  }
  assert.equal(h.steered.length, 1)
  assert.equal(h.cancelled.length, 3)
  assert.equal(h.followed.length, 2)
}

console.log('agent guards: OK')

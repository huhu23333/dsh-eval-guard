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
  const services = { jobs: { list: () => [{ id: 'bash-x', kind: 'bash', label: 'trainer', status: 'running', startedAt: Date.now() - 60_000 }] } }
  const guard = { configuration: { jobReminderMinutes: 5 }, runForSession: () => run }
  const tasks = { isCompleted: () => opts.completed === true, lastTest: () => ({ success_rate: 0, stage: 'scoring' }) }
  const steered: any[] = []
  const agent = {
    id: 's1', session: { id: 's1' },
    inbox: { nextTurn: opts.pendingUser ? [{ id: 'user-1', source: { kind: 'user' } }] : [], nextStep: [] },
    steer: (message: any) => { steered.push(message) },
  }
  installAgentRunGuards(ctx as any, services as any, guard as any, tasks as any)
  return { handlers, agent, steered }
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
  assert.match(h.steered[0].content[0].text, /Do not stop/i)
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

console.log('agent guards: OK')

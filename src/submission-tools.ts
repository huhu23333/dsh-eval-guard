/**
 * Host-side submission and test tools, installed only into the scoped tool
 * registry of the agent that owns a Neural-Pendulum run.  The agent never sees
 * a writable event/submission directory.
 */
import type { Guard } from './guard.ts'
import type { TaskService } from './task.ts'
import { NEURAL_PENDULUM_TASK } from './shared.ts'

interface ToolService {
  register(definition: unknown): () => void
}

interface ToolRunContext {
  agent?: { id?: string }
}

interface AgentWithTools {
  id: string
  ctx?: { tools?: ToolService }
}

const OUTPUT_SCHEMA = {
  type: 'object',
  additionalProperties: true,
  properties: {
    ok: { type: 'boolean' },
    submission: { type: 'string' },
    files: { type: 'number' },
    stage: { type: 'string' },
    t_can: { type: 'number' },
    completed: { type: 'boolean' },
    error: { type: 'string' },
  },
  required: ['ok'],
}

function render(_args: unknown, value: unknown): Array<{ type: 'text'; text: string }> {
  return [{ type: 'text', text: JSON.stringify(value) }]
}

function resolve(guard: Guard, exec: ToolRunContext) {
  if (!guard.active) throw new Error('eval guard is disabled')
  const run = guard.runForSession(exec.agent?.id)
  if (run === undefined) throw new Error('eval task run is not active for this session')
  if (run.task !== NEURAL_PENDULUM_TASK) throw new Error('this tool is only available for the neural-pendulum task')
  return run
}

/** Install the task tools into one agent's own scoped registry. */
export function registerAgentSubmissionTools(agent: AgentWithTools, guard: Guard, tasks: TaskService): void {
  const tools = agent.ctx?.tools
  if (tools === undefined) return
  const submit = {
    name: 'eval_submit',
    description: 'Submit the current workspace source as a new immutable snapshot for the active Neural-Pendulum run. Submission itself does not change the score.',
    parameters: { type: 'object', properties: {} },
    output: { schema: OUTPUT_SCHEMA, render },
    timeoutMs: 60_000,
    execute: async (_args: unknown, exec: ToolRunContext) => tasks.hostSubmit(resolve(guard, exec)),
  }
  const test = {
    name: 'eval_test',
    description: 'Run a host-side evaluation of the latest submitted snapshot and return stage, t_can, completed and error. Only successful tests update the score.',
    parameters: { type: 'object', properties: {} },
    output: { schema: OUTPUT_SCHEMA, render },
    timeoutMs: 15 * 60_000,
    execute: async (_args: unknown, exec: ToolRunContext) => tasks.hostTest(resolve(guard, exec)),
  }
  tools.register(submit)
  tools.register(test)
}

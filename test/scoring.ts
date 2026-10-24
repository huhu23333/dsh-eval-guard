/** Host-side neural-pendulum final-score integral tests. */
import assert from 'node:assert/strict'
import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'

const home = fs.mkdtempSync(path.join(os.tmpdir(), 'eval-guard-score-'))
process.env.DSH_HOME = home
const { TaskService } = await import('../src/task.ts')
const score = (TaskService as any).finalScoreFromEvents as
  (startedAt: number, events: readonly Record<string, unknown>[], now: number) => number

// No test: -1 per second.
assert.equal(score(100, [], 150), -50)

// A scoring test changes the score at its request instant; completion stops the integral.
const events: Record<string, unknown>[] = [
  { event: 'test', time: 110, stage: 'scoring', t_can: 5, completed: false },
  { event: 'test', time: 120, stage: 'completed', t_can: 10, completed: true },
]
assert.equal(score(100, events, 500), -15)

// Invalid tests contribute -1 until the next test; t_can=0 is also -1.
assert.equal(score(100, [
  { event: 'test', time: 110, stage: 'infra', t_can: 0, completed: false },
  { event: 'test', time: 120, stage: 'scoring', t_can: 0, completed: false },
], 130), -30)

// success_rate scoring uses the 0.90 target.
assert.equal(score(100, [
  { event: 'test', time: 110, stage: 'scoring', success_rate: 0.9, completed: false },
  { event: 'test', time: 120, stage: 'completed', success_rate: 0.9, completed: true },
], 500), -10)

fs.rmSync(home, { recursive: true, force: true })
console.log('scoring test: OK')

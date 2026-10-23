/**
 * Neural-Pendulum simulator regression tests.
 *
 * 1. Stability: the agent-facing random-reset simulator must not produce
 *    NaN/Inf under plausible extreme control sequences.
 * 2. Separation: the agent simulator is random-reset only; the evaluator
 *    simulator is fixed-reset only.
 * 3. Torque mapping: joint 3 is an internal motor, so its generalized force in
 *    absolute coordinates is (tau1, -tau3, tau3), not (tau1, 0, tau3).
 */
import assert from 'node:assert/strict'
import { spawn } from 'node:child_process'
import { createInterface } from 'node:readline'
import path from 'node:path'

const root = path.join(import.meta.dirname, '..', 'assets', 'neural-pendulum')
const simAgent = path.join(root, 'sim_agent')
const simEval = path.join(root, 'sim_eval')

function drive(sim: string, args: string[], script: (send: (value: object) => void, next: () => Promise<string>) => Promise<void>): Promise<void> {
  return new Promise((resolve, reject) => {
    const child = spawn(sim, args, { stdio: ['pipe', 'pipe', 'ignore'] })
    const rl = createInterface({ input: child.stdout })
    const queue: string[] = []
    let wake: (() => void) | undefined
    rl.on('line', line => { queue.push(line); wake?.(); wake = undefined })
    const next = async (): Promise<string> => {
      while (queue.length === 0) await new Promise<void>(done => { wake = done })
      return queue.shift() as string
    }
    const send = (value: object): void => { child.stdin.write(JSON.stringify(value) + '\n') }
    ;(async () => {
      await script(send, next)
      child.stdin.end()
      child.kill()
      resolve()
    })().catch(reject)
  })
}

const patterns: { name: string; torque: (step: number) => [number, number] }[] = [
  { name: 'zero', torque: () => [0, 0] },
  { name: 'constant-left', torque: () => [1, 0] },
  { name: 'constant-both', torque: () => [1, 1] },
  { name: 'constant-joint3', torque: () => [0, 1] },
  { name: 'sine', torque: (step) => [Math.sin(step * 0.05), Math.cos(step * 0.07)] },
  { name: 'chaotic-blocks', torque: (step) => {
    const seed = Math.floor(step / 10)
    const a = ((seed * 1103515245 + 12345) % 2000) / 1000 - 1
    const b = ((seed * 214013 + 2531011) % 2000) / 1000 - 1
    return [a, b]
  } },
]

// Agent simulator: random near-down reset; no fixed evaluation state.
for (const pattern of patterns) {
  await drive(simAgent, ['--seed', '0'], async (send, next) => {
    send({ cmd: 'reset' })
    await next()
    for (let step = 0; step < 3000; step++) {
      send({ cmd: 'step', torque: pattern.torque(step) })
      const line = await next()
      assert.ok(!/nan|inf/i.test(line), pattern.name + ' produced non-finite obs at step ' + String(step) + ': ' + line)
    }
  })
}

// The evaluator simulator must always reset to the fixed initial state.
const fixedReset = [-3.12859, 3.13359, -3.12059]
await drive(simEval, [], async (send, next) => {
  send({ cmd: 'reset' })
  const first = await next()
  const firstObs = (JSON.parse(first) as { obs: number[] }).obs
  for (let i = 0; i < 3; i++) {
    assert.ok(Math.abs(firstObs[i] - fixedReset[i]) < 2e-4,
      'eval simulator reset is not the fixed evaluation state: obs[' + String(i) + ']=' + String(firstObs[i]))
  }

  // Fixed initial state; one control step. The expected value is computed from
  // the independent Lagrangian equations with generalized torque
  // (tau1, -tau3, tau3). A regression to (tau1, 0, tau3) changes these numbers.
  const expectedFirstStep = [-3.1189, 3.10593, -3.08296]
  send({ cmd: 'step', torque: [0, 1] })
  const line = await next()
  const obs = (JSON.parse(line) as { obs: number[] }).obs
  assert.equal(obs.length, 3)
  for (let i = 0; i < 3; i++) {
    assert.ok(Math.abs(obs[i] - expectedFirstStep[i]) < 2e-4,
      'torque mapping regression: obs[' + String(i) + ']=' + String(obs[i]) + ' expected ' + String(expectedFirstStep[i]))
  }
})

console.log('pendulum simulator stability, split, and torque mapping: OK')

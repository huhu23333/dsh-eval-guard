/**
 * Neural-Pendulum Tier A simulator tests.
 *
 * 1. Stability: the agent-facing simulator must not produce NaN/Inf under
 *    plausible extreme control sequences.
 * 2. Randomization: every reset (agent and evaluator) samples a fresh hidden
 *    plant/initial state/noise level, so consecutive resets differ.
 * 3. Evaluator-only true state: sim_eval reports the noise-free state used for
 *    the success check while the controller only gets the noisy obs.
 */
import assert from 'node:assert/strict'
import { spawn } from 'node:child_process'
import { createInterface } from 'node:readline'
import path from 'node:path'

const root = path.join(import.meta.dirname, '..', 'assets', 'neural-pendulum')
const simAgent = path.join(root, 'sim_agent')
const simEval = path.join(root, 'sim_eval')

function drive(sim, args, script) {
  return new Promise((resolve, reject) => {
    const child = spawn(sim, args, { stdio: ['pipe', 'pipe', 'ignore'] })
    const rl = createInterface({ input: child.stdout })
    const queue = []
    let wake
    rl.on('line', line => { queue.push(line); wake?.(); wake = undefined })
    const next = async () => {
      while (queue.length === 0) await new Promise(done => { wake = done })
      return queue.shift()
    }
    const send = value => { child.stdin.write(JSON.stringify(value) + '\n') }
    ;(async () => {
      await script(send, next)
      child.stdin.end()
      child.kill()
      resolve()
    })().catch(reject)
  })
}

const patterns = [
  { name: 'zero', torque: () => [0, 0] },
  { name: 'constant-left', torque: () => [1, 0] },
  { name: 'constant-both', torque: () => [1, 1] },
  { name: 'constant-joint3', torque: () => [0, 1] },
  { name: 'sine', torque: step => [Math.sin(step * 0.05), Math.cos(step * 0.07)] },
  { name: 'chaotic-blocks', torque: step => {
    const seed = Math.floor(step / 10)
    const a = ((seed * 1103515245 + 12345) % 2000) / 1000 - 1
    const b = ((seed * 214013 + 2531011) % 2000) / 1000 - 1
    return [a, b]
  } },
]

for (const pattern of patterns) {
  await drive(simAgent, ['--seed', '0'], async (send, next) => {
    send({ cmd: 'reset' })
    await next()
    for (let step = 0; step < 3000; step++) {
      send({ cmd: 'step', torque: pattern.torque(step) })
      const line = await next()
      assert.ok(!/nan|inf/i.test(line), pattern.name + ' produced non-finite obs at ' + String(step) + ': ' + line)
    }
  })
}

await drive(simAgent, ['--seed', '7'], async (send, next) => {
  send({ cmd: 'reset' })
  const a = JSON.parse(await next())
  send({ cmd: 'reset' })
  const b = JSON.parse(await next())
  assert.ok(Array.isArray(a.obs) && a.obs.length === 3)
  assert.ok(Array.isArray(b.obs) && b.obs.length === 3)
  assert.notDeepEqual(a.obs, b.obs)
  assert.ok(a.state === undefined, 'agent simulator must not expose true state')
})

await drive(simEval, [], async (send, next) => {
  const states = []
  for (let ep = 0; ep < 3; ep++) {
    send({ cmd: 'reset' })
    const reply = JSON.parse(await next())
    assert.ok(Array.isArray(reply.obs) && reply.obs.length === 3)
    assert.ok(Array.isArray(reply.state) && reply.state.length === 3)
    for (const value of reply.obs.concat(reply.state)) assert.ok(Number.isFinite(value))
    for (const value of reply.state) assert.ok(Math.abs(Math.abs(value) - Math.PI) < 0.35)
    states.push(reply.state)
  }
  assert.notDeepEqual(states[0], states[1])
  assert.notDeepEqual(states[1], states[2])

  send({ cmd: 'step', torque: [0, 1] })
  const stepped = JSON.parse(await next())
  assert.ok(Array.isArray(stepped.state) && stepped.state.length === 3)
  for (const value of stepped.state.concat(stepped.obs)) assert.ok(Number.isFinite(value))
})

console.log('pendulum Tier A simulator stability, randomization, and split: OK')

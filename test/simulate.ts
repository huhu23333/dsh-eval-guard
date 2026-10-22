/**
 * Simulated host test: exercises config resolution, path audit, blacklist,
 * session classification, descendants cleanup, auto binding, and the
 * enable/disable sandbox hooks against the REAL DSH session header shape.
 */
import assert from 'node:assert/strict'
import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import { Guard } from '../src/guard.ts'
import { TANK_DEFINITION } from '../src/task.ts'
import { generateDefaultBlacklist, parsePathRules, auditToolCall } from '../src/policy.ts'
import { wrapSandbox, supervisorPath, guardRoot } from '../src/sandbox.ts'

const sandboxRoot = fs.mkdtempSync(path.join(import.meta.dirname, '..', '.simulate-test-'))
process.env.DSH_HOME = path.join(sandboxRoot, 'dsh-home')
const workspace = path.join(sandboxRoot, 'workspace')
fs.mkdirSync(workspace, { recursive: true })
fs.writeFileSync(path.join(workspace, 'inside.txt'), 'ok\n')
// A path outside the workspace that exists and is readable.
const outside = path.join(sandboxRoot, 'outside.txt')
fs.writeFileSync(outside, 'secret\n')
fs.chmodSync(outside, 0o444) // read-only system-like file: readable, not writable

const listeners = new Map<string, ((...args: any[]) => void)[]>()
const guardCallbacks: ((exec: any) => string | undefined)[] = []
/** Real DSH shape: cwd/origin/parentSession/delegationDepth are TOP-LEVEL header fields. */
const mainSession: any = { id: 'm1', header: { cwd: workspace } }
const childSession: any = { id: 'c1', header: { cwd: workspace, parentSession: 'm1', delegationDepth: 1 } }
const subagentSession: any = { id: 's1', header: { cwd: workspace, origin: 'subagent' } }
// A user-visible fork: parentSession is seed lineage, NOT a subagent marker.
const forkSession: any = { id: 'f1', header: { cwd: workspace, parentSession: 'm1' } }
const agents: any[] = []
const jobKills: string[] = []
const sandbox: any = { confine: async (argv: string[]) => ({ argv }) }
const sandboxPolicy: any = { resolve: () => ({ mode: 'danger-full-access', workspaceRoot: workspace }) }
const approvalPrev: string[] = []
const approval: any = { effectivePolicy: () => 'ask', setPolicy: (_agent: any, policy: string) => approvalPrev.push(policy) }
const jobs: any = {
  list: (owner?: string) => [
    { id: 'owned-' + String(owner), status: 'running', owner },
    { id: 'unowned', status: 'running' },
  ],
  kill: (id: string) => { jobKills.push(id); return 'requested' },
}
const emptyWorkspace = path.join(sandboxRoot, 'empty')
fs.mkdirSync(emptyWorkspace, { recursive: true })
const registry = {
  get: (id: string) => id === 'ws1' ? { path: workspace, title: 'sim' } : id === 'empty' ? { path: emptyWorkspace, title: 'empty' } : undefined,
  list: () => [{ id: 'ws1', path: workspace, title: 'sim' }, { id: 'empty', path: emptyWorkspace, title: 'empty' }],
}
const services: any = {
  sessions: { get: (id: string) => id === 'm1' ? mainSession : agents.find(agent => agent.id === id)?.session, list: () => agents.map(agent => agent.session) },
  agents: { list: () => agents, get: (id: string) => agents.find(agent => agent.id === id) },
  tools: {
    schemas: () => ['read', 'write', 'bash', 'web_fetch', 'subagent', 'create_goal', 'drawio_edit'].map(name => ({ name })),
    guard: (callback: (exec: any) => string | undefined) => { guardCallbacks.push(callback); return () => undefined },
  },
  sandbox,
  sandboxPolicy,
  approval,
  jobs,
  workspaceRegistry: registry,
  on: (event: string, listener: (...args: any[]) => void) => {
    const list = listeners.get(event) ?? []
    list.push(listener)
    listeners.set(event, list)
    return () => undefined
  },
}
const rawConfig: any = {
  enabled: { get: () => true },
  timeLimitEnabled: { get: () => true },
  timeLimitMinutes: { get: () => 45 },
  forbiddenPaths: { get: () => '' },
  toolBlacklistUseDefault: { get: () => true },
  toolBlacklist: { get: () => '' },
}
const guard = new Guard({ logger: { warn: () => undefined, info: () => undefined } } as any, services, rawConfig, TANK_DEFINITION)

// --- enable first: the tool guard only inspects while the guard is active ----
const enabled = await guard.enable()
assert.equal(enabled.ok, true, enabled.error)
assert.equal(guard.active, true)
assert.ok(fs.existsSync(supervisorPath()))
const confined: any = await sandbox.confine(['/bin/true'], { sessionId: 'm1', workspaceRoot: workspace, mode: 'workspace-write' })
assert.equal(Array.isArray(confined.argv), true)
assert.match(confined.argv[0], /dsh-eval-supervisor$/)
assert.ok(!confined.argv.includes('--cwd'))
assert.ok(confined.argv.includes(workspace))
assert.ok(confined.argv.includes('bwrap'))
assert.ok(confined.argv.includes('--clearenv'))
assert.equal(sandboxPolicy.resolve({ session: mainSession }).mode, 'workspace-write')
assert.equal(sandboxPolicy.resolve({}).mode, 'danger-full-access')

// --- config / session shape -------------------------------------------------
assert.equal(guard.configuration.timeLimitMinutes, 45)
assert.equal(guard.isMain(mainSession), true)
assert.equal(guard.isMain(forkSession), true)
assert.equal(guard.isMain(childSession), false)
assert.equal(guard.isMain(subagentSession), false)
assert.equal(guard.isMain(undefined), false)
assert.match(guard.defaultBlacklist(), /web_fetch/)
assert.match(guard.defaultBlacklist(), /create_goal/)
assert.match(guard.defaultBlacklist(), /drawio_edit/)

// Prompt must follow the live minute setting instead of hard-coded 90.
const prompt = guard.promptFor(workspace, '')
assert.match(prompt, /45 分钟/)
assert.doesNotMatch(prompt, /90 分钟/)

// --- path rules / audit -----------------------------------------------------
const rules = parsePathRules('/home\n!/home/ok\n/var/*\n')
assert.equal(rules.length, 3)
assert.equal(rules[2]!.path, '/var')
assert.equal(auditToolCall('read', { path: workspace + '/inside.txt' }, workspace, []).allow, true)
assert.equal(auditToolCall('write', { path: workspace + '/x' }, workspace, []).allow, true)
assert.equal(auditToolCall('read', { path: outside }, workspace, []).allow, true) // read-only file outside workspace is allowed by design
const writable = path.join(sandboxRoot, 'writable.txt')
fs.writeFileSync(writable, 'x')
assert.equal(auditToolCall('write', { path: writable }, workspace, []).allow, false)
// Ordered rules can narrow writes inside the workspace, but cannot widen them outside it.
const denyRules = parsePathRules(workspace + '/blocked\n')
assert.equal(auditToolCall('write', { path: workspace + '/blocked/x' }, workspace, denyRules).allow, false)
assert.equal(auditToolCall('write', { path: writable }, workspace, parsePathRules('!' + writable + '\n')).allow, false)
// Content that merely contains a path-looking key is not a path argument.
assert.equal(auditToolCall('write', { path: workspace + '/x', content: '{"path":"/etc/passwd"}' }, workspace, []).allow, true)

// --- tool guard -------------------------------------------------------------
const toolGuard = guardCallbacks[0]!
const mainAgent = { id: 'm1', status: 'idle', session: mainSession, options: { provider: 'p', model: 'm' }, cancel: () => undefined }
agents.push(mainAgent)
assert.match(String(toolGuard({ name: 'web_fetch', arguments: {}, agent: mainAgent })), /黑名单/)
assert.match(String(toolGuard({ name: 'create_goal', arguments: {}, agent: mainAgent })), /黑名单/)
assert.match(String(toolGuard({ name: 'drawio_edit', arguments: { path: outside }, agent: mainAgent })), /黑名单/)
assert.match(String(toolGuard({ name: 'ask_user_question', arguments: {}, agent: mainAgent })), /自主完成/)
assert.match(String(toolGuard({ name: 'read', arguments: { path: writable }, agent: mainAgent })), /阻止/)
assert.equal(toolGuard({ name: 'read', arguments: { path: workspace + '/inside.txt' }, agent: mainAgent }), undefined)
// Subagent sessions are confined too: their tool calls must pass the same audit.
const subAgent = { id: 's1', status: 'running', session: subagentSession, options: {}, cancel: () => undefined }
agents.push(subAgent)
assert.match(String(toolGuard({ name: 'read', arguments: { path: writable }, agent: subAgent })), /阻止/)
assert.equal(sandboxPolicy.resolve({ session: subagentSession }).mode, 'workspace-write')

// --- auto binding sees the real cwd ----------------------------------------
let boundCwd = ''
guard.onAutoRun = (_agent, cwd) => { boundCwd = cwd; return true }
for (const listener of listeners.get('agent/created') ?? []) listener({ agent: mainAgent })
assert.equal(boundCwd, workspace)

// --- deadline: arm on user message, grace on turn/end ----------------------
const deadlines = (guard as any).deadlines as Map<string, number>
const lastActivity = (guard as any).lastActivity as Map<string, number>
for (const listener of listeners.get('session/event') ?? []) {
  listener(mainSession, { type: 'user/message', data: { source: { kind: 'user' } } })
}
assert.equal(deadlines.has('m1'), true)
for (const listener of listeners.get('session/event') ?? []) {
  listener(mainSession, { type: 'turn/end', data: { reason: { kind: 'completed' } } })
}
assert.equal(deadlines.has('m1'), true) // within the idle grace window
const originalJobsList = jobs.list
jobs.list = () => [] // no work left: only the grace window keeps the deadline alive
lastActivity.set('m1', Date.now() - 60_000)
;(guard as any).maybeComplete('m1')
assert.equal(deadlines.has('m1'), false)
jobs.list = originalJobsList

// --- descendants + owned jobs are cleaned on timeout ----------------------
const childAgent = { id: 'c1', status: 'running', session: childSession, options: {}, cancel: () => undefined }
const forkAgent = { id: 'f1', status: 'running', session: forkSession, options: {}, cancel: () => undefined }
agents.push(childAgent, forkAgent)
const cancels: string[] = []
;(mainAgent as any).cancel = () => { cancels.push('m1') }
;(childAgent as any).cancel = () => { cancels.push('c1') }
;(forkAgent as any).cancel = () => { cancels.push('f1') }
;(mainAgent as any).status = 'running'
deadlines.set('m1', Date.now() - 1)
;(guard as any).tick()
assert.deepEqual(cancels.sort(), ['c1', 'm1']) // fork f1 is not a subagent descendant
assert.ok(jobKills.includes('owned-m1'))
assert.ok(jobKills.includes('owned-c1'))
assert.ok(!jobKills.includes('owner-f1'))
assert.ok(!jobKills.includes('unowned')) // global jobs must not be killed

// --- task install path (JobManager + material copy + prompt) ---------------
const { TaskService } = await import('../src/task.ts')
const tasks = new TaskService(guard as any, registry as any)
const install = tasks.startInstall('empty', 'tank')
assert.equal(install.ok, true, install.error)
assert.ok(install.jobId !== undefined)
const job = await (async () => {
  for (let attempt = 0; attempt < 100; attempt += 1) {
    const detail = tasks.jobs.get(install.jobId!)
    if (detail !== undefined && detail.status !== 'running' && detail.status !== 'queued') return detail
    await new Promise(resolve => setTimeout(resolve, 100))
  }
  return undefined
})()
assert.ok(job !== undefined)
assert.equal(job.status, 'succeeded', job.error)
assert.ok(fs.existsSync(path.join(emptyWorkspace, 'workspace.zip')))
assert.match(job.prompt ?? '', /45 分钟/)

// A non-empty workspace is refused and left untouched.
const refused = tasks.startInstall('ws1', 'tank')
assert.equal(refused.ok, false)
assert.match(refused.error ?? '', /不是空的/)

// Pristine test/venv snapshots used by the benchmark evaluators.
const { TASK_DEFINITIONS } = await import('../src/task.ts')
const fakeWs = path.join(sandboxRoot, 'fake-ws')
fs.mkdirSync(path.join(fakeWs, 'tests'), { recursive: true })
fs.writeFileSync(path.join(fakeWs, 'tests', 'test_a.py'), 'assert 1\n')
fs.writeFileSync(path.join(fakeWs, 'pytest.ini'), '[pytest]\n')
const fakeSite = path.join(fakeWs, '.venv', 'lib', 'python3.10', 'site-packages')
fs.mkdirSync(path.join(fakeSite, 'pytest'), { recursive: true })
fs.writeFileSync(path.join(fakeSite, 'pytest', '__init__.py'), 'v1\n')
fs.writeFileSync(path.join(fakeSite, 'a.pth'), 'x\n')
fs.mkdirSync(path.join(fakeWs, '.venv', 'bin'), { recursive: true })
fs.symlinkSync(process.execPath, path.join(fakeWs, '.venv', 'bin', 'python'))
fs.writeFileSync(path.join(fakeWs, '.venv', 'pyvenv.cfg'), 'home = x\n')
;(tasks as any).snapshotTestMaterial(TASK_DEFINITIONS['commit0'], 'fake', fakeWs)
;(tasks as any).snapshotVenv('fake', fakeWs)
const baselineDir = (tasks as any).baselineDir('fake')
assert.ok(fs.existsSync(path.join(baselineDir, 'tests', 'test_a.py')))
assert.ok(fs.existsSync(path.join(baselineDir, 'pytest.ini')))
const venvManifest = JSON.parse(fs.readFileSync(path.join(baselineDir, 'venv.json'), 'utf8'))
assert.equal(typeof venvManifest.pytest, 'string')
assert.equal(venvManifest.files['pyvenv.cfg'].length, 64)

// --- disable restores the untouched services --------------------------------
for (const listener of listeners.get('agent/disposed') ?? []) listener({ agent: childAgent })
for (const listener of listeners.get('agent/disposed') ?? []) listener({ agent: mainAgent })

guard.disable()
assert.equal(guard.active, false)
assert.equal(sandboxPolicy.resolve({ session: mainSession }).mode, 'danger-full-access')
assert.ok(approvalPrev.includes('never'))

guard.dispose()
fs.rmSync(sandboxRoot, { recursive: true, force: true })
console.log('simulated host test: OK')

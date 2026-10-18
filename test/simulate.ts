/**
 * Simulated host test: no DSH process, no agent tokens. Exercises config
 * resolution, path audit, default blacklist, task installation, sandbox
 * wrapping, and the enable/disable service hook.
 */
import assert from 'node:assert/strict'
import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import { Guard } from '../src/guard.ts'
import { TaskService } from '../src/task.ts'
import { TANK_DEFINITION } from '../src/task.ts'
import { generateDefaultBlacklist, parsePathRules, auditToolCall, resolveCheckPath } from '../src/policy.ts'
import { wrapSandbox, supervisorPath, guardRoot } from '../src/sandbox.ts'

const sandboxRoot = fs.mkdtempSync(path.join(os.tmpdir(), 'eval-guard-sim-'))
const workspace = path.join(sandboxRoot, 'workspace')
const workspace2 = path.join(sandboxRoot, 'workspace2')
const workspace3 = path.join(sandboxRoot, 'workspace3')
const workspace4 = path.join(sandboxRoot, 'workspace4')
const workspace5 = path.join(sandboxRoot, 'workspace5')
for (const directory of [workspace, workspace2, workspace3, workspace4, workspace5]) fs.mkdirSync(directory, { recursive: true })
fs.writeFileSync(path.join(workspace, 'inside.txt'), 'ok\n')

const listeners = new Map<string, ((...args: any[]) => void)[]>()
const fakeSession: any = { id: 'm1', header: { meta: { cwd: workspace } } }
const guardCallbacks: ((exec: any) => string | undefined)[] = []
const registry = {
  get(id: string) {
    if (id === 'ws1') return { path: workspace, title: 'sim' }
    if (id === 'ws2') return { path: workspace2, title: 'sim2' }
    if (id === 'ws3') return { path: workspace3, title: 'sim3' }
    if (id === 'ws4') return { path: workspace4, title: 'sim4' }
    if (id === 'ws5') return { path: workspace5, title: 'sim5' }
    return undefined
  },
  list() { return [{ path: workspace, title: 'sim' }, { path: workspace2, title: 'sim2' }, { path: workspace3, title: 'sim3' }, { path: workspace4, title: 'sim4' }, { path: workspace5, title: 'sim5' }] },
}
const agents: any[] = []
const sandbox: any = { confine: async (argv: string[]) => ({ argv }) }
const sandboxPolicy: any = { resolve: () => ({ mode: 'danger-full-access', workspaceRoot: workspace }) }
const approvalPrev: string[] = []
const approval: any = { effectivePolicy: () => 'ask', setPolicy: (_agent: any, policy: string) => approvalPrev.push(policy) }
const jobs: any = { list: () => [], kill: () => 'requested' }
const services: any = {
  sessions: { get: (id: string) => id === 'm1' ? fakeSession : agents.find(agent => agent.id === id)?.session, list: () => agents.map(agent => agent.session) },
  agents: { list: () => agents, get: (id: string) => agents.find(agent => agent.id === id) },
  tools: {
    schemas: () => ['read', 'write', 'bash', 'web_fetch', 'subagent', 'ops_bash', 'generate_image', 'terminal_open'].map(name => ({ name })),
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
  timeLimitMinutes: { get: () => 90 },
  forbiddenPaths: { get: () => '' },
  toolBlacklistUseDefault: { get: () => true },
  toolBlacklist: { get: () => '' },
}
const guard = new Guard({} as any, services, rawConfig, TANK_DEFINITION)
assert.equal(guard.configuration.timeLimitMinutes, 90)
assert.equal(guard.isMain({ id: 'm', header: { meta: {} } }), true)
assert.equal(guard.isMain({ id: 's', header: { meta: { origin: 'subagent' } } }), false)
assert.match(guard.defaultBlacklist(), /web_fetch/)
assert.match(guard.defaultBlacklist(), /generate_image/)
assert.match(guard.defaultBlacklist(), /# --- 已安装插件工具 ---/)

const rules = parsePathRules('/home\n!/home/ok\n')
assert.equal(rules.length, 2)
const readDecision = auditToolCall('read', { path: '/etc/passwd' }, workspace, [])
assert.equal(readDecision.allow, true)
const denyHome = auditToolCall('read', { path: path.join(os.homedir(), '.bashrc') }, workspace, [])
assert.equal(denyHome.allow, false)
const writeOutside = auditToolCall('write', { path: '/etc/hosts' }, workspace, [])
assert.equal(writeOutside.allow, false)
const inside = auditToolCall('write', { path: path.join(workspace, 'x') }, workspace, [])
assert.equal(inside.allow, true)
assert.equal(resolveCheckPath(path.join(workspace, 'x'), workspace).startsWith(workspace), true)

fs.rmSync(path.join(workspace, 'inside.txt'))
fs.rmSync(path.join(workspace, 'x'), { force: true })
const early = await guard.enable()
assert.equal(early.ok, true)
const tasks = new TaskService(guard, registry)
const checked = tasks.check('ws1')
assert.equal(checked.empty, true)
const installed = tasks.install('ws1')
assert.equal(installed.ok, true)
assert.ok(installed.runId)
assert.ok(fs.existsSync(path.join(workspace, 'workspace.zip')))
assert.match(installed.prompt ?? '', /submission\/bot\.cpp/)
assert.ok(fs.existsSync(path.join(guardRoot(), 'runs', installed.runId as string, 'run.json')))
const warehouse = tasks.install('ws2', 'warehouse')
assert.equal(warehouse.ok, true)
assert.ok(fs.existsSync(path.join(workspace2, 'workspace.zip')))
assert.match(warehouse.prompt ?? '', /仓储机器人调度/)
assert.match(warehouse.prompt ?? '', /src\//)
assert.ok(fs.existsSync(path.join(guardRoot(), 'runs', warehouse.runId as string, 'run.json')))
for (const [task, workspaceId] of [['land', 'ws3'], ['bomberman', 'ws4'], ['ctf', 'ws5']] as const) {
  const installedTask = tasks.install(workspaceId, task)
  assert.equal(installedTask.ok, true)
  assert.ok(fs.existsSync(path.join(registry.get(workspaceId)!.path, 'workspace.zip')))
  assert.match(installedTask.prompt ?? '', /workspace\.zip/)
}

const enabled = await guard.enable()
assert.equal(enabled.ok, true)
assert.notEqual(sandbox.confine, undefined)
const confined = await sandbox.confine(['/bin/true'], { sessionId: 'm1', workspaceRoot: workspace, mode: 'workspace-write' })
assert.equal(Array.isArray(confined.argv), true)
assert.match(confined.argv[0], /dsh-eval-supervisor$/)
assert.ok(confined.argv.includes('bwrap'))
const resolved = sandboxPolicy.resolve({ session: fakeSession })
assert.equal(resolved.mode, 'workspace-write')
assert.ok(fs.existsSync(supervisorPath()))

const agent = { id: 'm1', status: 'idle', session: fakeSession, options: { provider: 'p', model: 'm' } }
agents.push(agent)
for (const listener of listeners.get('agent/created') ?? []) listener({ agent })
const runPath = path.join(guardRoot(), 'runs', installed.runId as string, 'run.json')
assert.equal(JSON.parse(fs.readFileSync(runPath, 'utf8')).sessionId, 'm1')
for (const listener of listeners.get('session/event') ?? []) {
  listener(fakeSession, { type: 'user/message', data: { source: { kind: 'user' } } })
  listener(fakeSession, { type: 'assistant/message', data: { usage: { inputTokens: 10, outputTokens: 2 } } })
  listener(fakeSession, { type: 'tool/call', data: {} })
  listener(fakeSession, { type: 'turn/end', data: { reason: { kind: 'completed' } } })
}
const usage = JSON.parse(fs.readFileSync(path.join(guardRoot(), 'runs', installed.runId as string, 'usage.json'), 'utf8'))
assert.equal(usage.inputTokens, 10)
assert.equal(usage.outputTokens, 2)
assert.equal(usage.toolCalls, 1)
assert.equal(usage.turns, 1)

const toolGuard = guardCallbacks[0]
assert.ok(toolGuard)
assert.match(String(toolGuard!({ name: 'web_fetch', arguments: {}, agent })), /黑名单/)
assert.match(String(toolGuard!({ name: 'ask_user_question', arguments: {}, agent })), /自主完成/)
assert.match(String(toolGuard!({ name: 'read', arguments: { path: path.join(os.homedir(), '.bashrc') }, agent })), /阻止/)
assert.equal(toolGuard!({ name: 'read', arguments: { path: path.join(workspace, 'inside.txt') }, agent }), undefined)

guard.disable()
assert.equal(sandboxPolicy.resolve({ session: fakeSession }).mode, 'danger-full-access')
assert.ok(approvalPrev.includes('never'))
guard.dispose()
fs.rmSync(sandboxRoot, { recursive: true, force: true })
console.log('simulated host test: OK')

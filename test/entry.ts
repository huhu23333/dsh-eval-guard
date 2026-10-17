import assert from 'node:assert/strict'
import { apply, Config } from '../lib/index.js'
function volatile(value) { return { get: () => value } }
const services = {
  sessions: { get: () => undefined, list: () => [] },
  agents: { list: () => [], get: () => undefined },
  tools: { schemas: () => [{ name: 'read' }, { name: 'web_fetch' }], guard: () => () => undefined },
  sandbox: { confine: async (argv) => ({ argv }) },
  sandboxPolicy: { resolve: () => ({ mode: 'workspace-full-access', workspaceRoot: '/tmp' }) },
  workspaceRegistry: { get: () => undefined, list: () => [] },
  approval: { effectivePolicy: () => 'ask', setPolicy: () => undefined },
  jobs: { list: () => [], kill: () => 'requested' },
}
let applyFn
const child = {
  get: key => services[key],
  on: () => () => undefined,
  effect: fn => { const dispose = fn(); return dispose },
  inject: (deps, cb) => { if (deps.includes('settings')) cb({ get: () => ({ configure: () => () => undefined }), effect: () => undefined }); },
}
const ctx = {
  inject: (deps, cb) => cb(child),
  effect: fn => fn(),
  logger: { warn: () => undefined, info: () => undefined },
  fiber: {},
}
apply(ctx, {
  enabled: volatile(false), timeLimitEnabled: volatile(true), timeLimitMinutes: volatile(90),
  forbiddenPaths: volatile(''), toolBlacklistUseDefault: volatile(true), toolBlacklist: volatile(''),
})
assert.ok(Config)
console.log('entry smoke: OK')

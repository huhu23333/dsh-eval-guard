import { createRequire } from 'node:module'
import assert from 'node:assert/strict'
const require = createRequire(import.meta.url)
let captured
globalThis.window = { __ModuleLoader__: { load: value => { captured = value } } }
await import('../lib/client/index.js')
assert.ok(captured)
const modules = { react: require('react'), 'react/jsx-runtime': require('react/jsx-runtime') }
const exportsObject = captured.factory(spec => { if (!(spec in modules)) throw new Error('missing ' + spec); return modules[spec] })
assert.ok(typeof exportsObject.apply === 'function')
const disposers = []
const t = key => key
const ctx = {
  effect: fn => { const d = fn(); disposers.push(d) },
  locale: { register: () => () => undefined, bind: () => t },
  configForms: { get: () => ({ getSnapshot: () => ({ status: 'ready', value: {}, revision: 1, writable: true }), subscribe: () => () => undefined, mutate: async () => true }) },
  slots: { inject: (name, cb) => cb(), register: (options, Component) => { assert.ok(typeof Component === 'function'); return () => undefined } },
  uiWorkspace: { startSession: () => undefined },
  get: key => key === 'workspaces' ? { list: { getSnapshot: () => ({ items: [{ workspaceId: 'w', title: 'W' }] }) } } : undefined,
}
exportsObject.apply(ctx)
for (const dispose of disposers) { if (typeof dispose === 'function') dispose() }
console.log('client apply smoke: OK')

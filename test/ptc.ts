import { spawnSync } from 'node:child_process'
import fs from 'node:fs'
import path from 'node:path'
import { wrapSandbox, runtimeFiles } from '../src/sandbox.ts'
const runtime = runtimeFiles()
console.log('runtime', JSON.stringify({ node: runtime.node, process: runtime.ptcProcess, control: runtime.ptcControl, dirs: runtime.ptcDirs }, null, 1))
if (runtime.ptcProcess === undefined || runtime.ptcControl === undefined) throw new Error('PTC sources not located')
const root = fs.mkdtempSync(path.join(import.meta.dirname, '..', '.ptc-test-'))
try {
  const source = 'const a = await import(' + JSON.stringify('file://' + runtime.ptcControl) + ');' +
    'const b = await import(' + JSON.stringify('file://' + runtime.ptcProcess) + ');' +
    'console.log("ptc-import-ok", typeof a.openInheritedControlChannel, typeof b.runNodeMain);'
  const wrap = wrapSandbox([process.execPath, '--input-type=module', '--eval', source], root, root, [], path.join(root, 'audit.jsonl'))
  const result = spawnSync(wrap.argv[0], wrap.argv.slice(1), { cwd: root, encoding: 'utf8', timeout: 60000 })
  console.log('status', result.status, 'signal', result.signal)
  console.log((result.stdout || '').slice(0, 1000))
  console.log((result.stderr || '').slice(0, 1000))
} finally {
  fs.rmSync(root, { recursive: true, force: true })
}

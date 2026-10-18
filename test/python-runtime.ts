import { spawnSync } from 'node:child_process'
import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import { wrapSandbox } from '../src/sandbox.ts'

const base = fs.mkdtempSync(path.join(import.meta.dirname, '..', '.python-test-'))
const parent = path.join(base, 'ws')
const workspace = path.join(parent, '3')
fs.mkdirSync(path.join(parent, 'lib', 'python3.10'), { recursive: true })
fs.writeFileSync(path.join(parent, 'lib', 'python3.10', 'os.py'), 'stdlib\n')
fs.mkdirSync(workspace, { recursive: true })
fs.writeFileSync(path.join(workspace, 'inside.txt'), 'ok\n')
try {
  const command = 'cat inside.txt; cat ../lib/python3.10/os.py; python3 -c "print(2+2)"'
  const wrap = wrapSandbox(['/bin/sh', '-c', command], workspace, workspace, [], path.join(base, 'audit.jsonl'))
  const result = spawnSync(wrap.argv[0] as string, wrap.argv.slice(1), { cwd: workspace, encoding: 'utf8', timeout: 30000 })
  console.log('status', result.status)
  console.log('stdout', result.stdout)
  console.log('stderr', result.stderr)
  if (result.status !== 0 || !(result.stdout || '').includes('stdlib') || !(result.stdout || '').includes('4')) process.exitCode = 1
} finally {
  fs.rmSync(base, { recursive: true, force: true })
}

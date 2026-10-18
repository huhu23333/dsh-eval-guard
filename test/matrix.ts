import { spawnSync } from 'node:child_process'
import fs from 'node:fs'
import path from 'node:path'
import { wrapSandbox } from '../src/sandbox.ts'

const root = fs.mkdtempSync(path.join(import.meta.dirname, '..', '.matrix-test-'))
fs.writeFileSync(path.join(root, 'inside.txt'), 'hello\n')
fs.writeFileSync(path.join(root, 'workspace.zip'), 'not-a-real-zip\n')
const audit = path.join(root, 'audit.jsonl')
function run(command: string) {
  const wrap = wrapSandbox(['/bin/sh', '-c', command], root, root, [], audit)
  return spawnSync(wrap.argv[0] as string, wrap.argv.slice(1), { cwd: root, encoding: 'utf8', timeout: 30000 })
}
const allowed = [
  'ls -la',
  'cat inside.txt',
  'find . -maxdepth 2 -type f',
  'grep -R hello .',
  'python3 -c "import platform; print(platform.system())"',
  'python3 -c "import json; print(json.dumps({}))"',
  'mkdir -p sub/dir',
  'cp inside.txt sub/copy',
  'mv sub/copy sub/moved',
  'rm sub/moved',
  'sha256sum inside.txt',
  'echo tmp > /tmp/t && cat /tmp/t',
  'printf "#include <cstdio>\nint main(){return 0;}\n" > sub/t.cpp && g++ -O2 -std=c++20 sub/t.cpp -o sub/t && ./sub/t && echo gcc-ok',
  'tar -cf sub/a.tar inside.txt && tar -tf sub/a.tar',
]
const blocked = [
  'cat ' + path.join(process.env.HOME || '/root', '.bashrc'),
  'python3 -c "import socket; socket.socket(socket.AF_INET, socket.SOCK_STREAM)"',
  'echo x > ' + path.join(path.dirname(root), 'outside.txt'),
]
let failed = 0
for (const command of allowed) {
  const result = run(command)
  if (result.status !== 0) {
    failed++
    console.log('ALLOWED-FAILED', command, result.status, (result.stdout || '') + (result.stderr || ''))
  } else {
    console.log('allowed-ok', command.slice(0, 48))
  }
}
for (const command of blocked) {
  const result = run(command)
  if (result.status === 0) {
    failed++
    console.log('BLOCKED-ALLOWED', command)
  } else {
    console.log('blocked-ok', command.slice(0, 48), result.status)
  }
}
fs.rmSync(root, { recursive: true, force: true })
if (failed > 0) process.exitCode = 1

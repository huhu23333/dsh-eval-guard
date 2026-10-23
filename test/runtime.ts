import { spawnSync } from 'node:child_process'
import fs from 'node:fs'
import path from 'node:path'
import { wrapSandbox } from '../src/sandbox.ts'
const root = fs.mkdtempSync(path.join(import.meta.dirname, '..', '.runtime-test-'))
fs.writeFileSync(path.join(root, 'a.cpp'), '#include <cstdio>\nint main(){printf("hi\\n");}\n')
const run = (argv, label) => {
  const wrap = wrapSandbox(argv, root, root, [], path.join(root, label + '-audit.jsonl'))
  const result = spawnSync(wrap.argv[0], wrap.argv.slice(1), { cwd: root, encoding: 'utf8', timeout: 60000 })
  console.log(label, 'status', result.status, 'signal', result.signal)
  console.log((result.stdout || '').slice(0, 500))
  console.log((result.stderr || '').slice(0, 500))
}
try {
  run([process.execPath, '-e', 'console.log("node-ok", process.version)'], 'node')
  run(['/usr/bin/g++', '-O2', 'a.cpp', '-o', 'a.out'], 'gcc')
  run([path.join(root, 'a.out')], 'run')
  run(['/usr/bin/python3', '-c', 'import socket\ns=socket.socket()\ns.connect(("1.1.1.1", 80))\nprint("SHOULD_NOT")'], 'net')
} finally {
  fs.rmSync(root, { recursive: true, force: true })
}

import { spawnSync } from 'node:child_process'
import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import { wrapSandbox } from '../src/sandbox.ts'
const root = fs.mkdtempSync(path.join(import.meta.dirname, '..', '.wrap-test-'))
fs.writeFileSync(path.join(root, 'inside.txt'), 'ok\n')
try {
  const wrap = wrapSandbox(['/bin/sh', '-c', 'cat inside.txt; ls /usr/bin/g++ >/dev/null && echo usr-ok; echo x > /tmp/t && echo tmp-ok; cat ' + path.join(os.homedir(), '.bashrc') + ' 2>&1 || true'], root, root, [], path.join(root, 'audit.jsonl'))
  const result = spawnSync(wrap.argv[0] as string, wrap.argv.slice(1), { cwd: root, encoding: 'utf8', timeout: 30000 })
  console.log('status', result.status, 'signal', result.signal)
  console.log('stdout', (result.stdout || '').slice(0, 500))
  console.log('stderr', (result.stderr || '').slice(0, 500))
  console.log('tmp exists', fs.existsSync(path.join(root, '.tmp', 't')))
  console.log('audit', fs.readFileSync(wrap.auditFile, 'utf8').slice(0, 500))
} finally {
  fs.rmSync(root, { recursive: true, force: true })
}

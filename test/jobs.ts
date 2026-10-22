/** JobManager failure-cleanup test. */
import assert from 'node:assert/strict'
import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'

const home = fs.mkdtempSync(path.join(os.tmpdir(), 'eval-guard-jobs-'))
process.env.DSH_HOME = home
const { JobManager } = await import('../src/jobs.ts')
const jobs = new JobManager()
let failed = 0
const info = jobs.start({
  kind: 'install',
  task: 'tank',
  workspaceId: 'w',
  command: ['/definitely-not-a-real-executable-dsh-eval-guard'],
  cwd: home,
  onFailure: () => { failed += 1 },
})
await new Promise(resolve => setTimeout(resolve, 300))
assert.equal(failed, 1)
assert.equal(jobs.get(info.id)?.status, 'failed')

// An interrupted install recorded on disk must now be cleaned on startup.
const partial = path.join(home, 'partial-workspace')
fs.mkdirSync(partial, { recursive: true })
fs.writeFileSync(path.join(partial, 'workspace.zip'), 'half\n')
const jobDir = path.join(home, 'eval-guard', 'jobs', 'restart-1')
fs.mkdirSync(jobDir, { recursive: true })
fs.writeFileSync(path.join(jobDir, 'job.json'), JSON.stringify({
  id: 'restart-1', kind: 'install', task: 'tank', workspaceId: 'w',
  status: 'running', createdAt: 1, startedAt: 1, endedAt: null, updatedAt: 1,
  command: ['bash', '-c', 'true'], cwd: partial,
  logFile: path.join(jobDir, 'log.txt'), resultFile: path.join(jobDir, 'result.json'),
  cleanupWorkspace: partial,
}))
const restarted = new JobManager()
assert.equal(restarted.get('restart-1')?.status, 'interrupted')
assert.equal(fs.readdirSync(partial).length, 0)

fs.rmSync(home, { recursive: true, force: true })
console.log('jobs test: OK')

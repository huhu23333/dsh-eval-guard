/**
 * Strict sandbox profile: bwrap with a system-only read view, the workspace
 * writable, the workspace .tmp mounted at /tmp, no host /tmp, no network, and
 * a host-side seccomp supervisor that denies out-of-view file/network effects.
 * @module dsh-eval-guard/sandbox
 */
import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import { createRequire } from 'node:module'
import { fileURLToPath } from 'node:url'
import type { PathRule } from './policy.ts'

/** Package root (the folder containing package.json). */
export function packageRoot(): string {
  return fileURLToPath(new URL('..', import.meta.url))
}

/** Path of the compiled native supervisor. */
export function supervisorPath(): string {
  return path.join(packageRoot(), 'native', 'dsh-eval-supervisor')
}

/** Launcher-provided facts; nothing here is a fixed install path. */
let runtimeHome: string | undefined
let runtimeProfileDir: string | undefined

/** Record the launcher's profile/home paths. */
export function configureRuntimeContext(next: { home?: string; profileDir?: string }): void {
  if (next.home !== undefined && next.home !== '') runtimeHome = next.home
  if (next.profileDir !== undefined && next.profileDir !== '') runtimeProfileDir = next.profileDir
}

/** The DSH home: launcher profile context, then $DSH_HOME, then ~/.dsh. */
export function dshHome(): string {
  if (runtimeHome !== undefined) return runtimeHome
  return process.env.DSH_HOME && process.env.DSH_HOME !== ''
    ? process.env.DSH_HOME
    : path.join(os.homedir(), '.dsh')
}

/** Candidate node_modules directories, launcher profile first and then profiles/*. */
function profileNodeModules(): string[] {
  const out: string[] = []
  if (runtimeProfileDir !== undefined) out.push(path.join(runtimeProfileDir, 'node_modules'))
  const profiles = path.join(dshHome(), 'profiles')
  try {
    for (const entry of fs.readdirSync(profiles, { withFileTypes: true })) {
      if (entry.isDirectory()) out.push(path.join(profiles, entry.name, 'node_modules'))
    }
  } catch {
    // No profile tree (custom launcher); the caller falls back to its own paths.
  }
  return out
}

/** Root for run records, audits, and sandbox policy files. */
export function guardRoot(): string {
  return path.join(dshHome(), 'eval-guard')
}

/** Ensure a directory exists. */
function ensureDir(dir: string): void {
  fs.mkdirSync(dir, { recursive: true })
}

/** Host-side system directories mounted read-only into the sandbox. */
const SYSTEM_READ_DIRS: readonly string[] = [
  '/usr', '/bin', '/sbin', '/lib', '/lib32', '/lib64', '/libx32', '/etc', '/opt', '/sys', '/var/lib',
]

/** Ancestors of a path, outermost first. */
export function ancestorsOf(target: string): string[] {
  const out: string[] = []
  let current = path.dirname(target)
  for (;;) {
    out.unshift(current)
    const parent = path.dirname(current)
    if (parent === current) break
    current = parent
  }
  return out
}

/** Resolve one package directory from the host launch world. */
function packageDir(name: string): string | undefined {
  const candidates: string[] = []
  try {
    const require = createRequire(path.join(process.cwd(), 'noop.js'))
    candidates.push(path.dirname(require.resolve(name + '/package.json')))
  } catch {
    // fall through to source-checkout guesses
  }
  if (name === '@deepseek-ai/dsh-ptc-runtime-node') {
    candidates.push(path.join(process.cwd(), 'packages', 'ptc-runtime', 'ptc-runtime-node'))
  }
  if (name === '@deepseek-ai/dsh-subprocess') {
    candidates.push(path.join(process.cwd(), 'packages', 'subprocess', 'subprocess'))
  }
  for (const modules of profileNodeModules()) candidates.push(path.join(modules, name))
  for (const candidate of candidates) {
    try {
      if (fs.statSync(path.join(candidate, 'package.json')).isFile()) return fs.realpathSync.native(candidate)
    } catch {
      // keep looking
    }
  }
  return undefined
}

/** The PTC bootstrap files in whichever source/built mode this DSH runs. */
function findPtcFiles(): { process?: string; control?: string; dirs: string[] } {
  const dirs = new Set<string>()
  const ptc = packageDir('@deepseek-ai/dsh-ptc-runtime-node')
  const subprocess = packageDir('@deepseek-ai/dsh-subprocess')
  let processFile: string | undefined
  let controlFile: string | undefined
  if (ptc !== undefined) {
    for (const relative of [
      path.join('src', 'process.ts'), path.join('src', 'process.js'),
      path.join('lib', 'process.js'), 'process.js',
    ]) {
      const candidate = path.join(ptc, relative)
      if (fs.existsSync(candidate)) { processFile = fs.realpathSync.native(candidate); break }
    }
    const src = path.join(ptc, 'src')
    if (fs.existsSync(src)) dirs.add(src)
    const lib = path.join(ptc, 'lib')
    if (fs.existsSync(lib)) dirs.add(lib)
  }
  if (subprocess !== undefined) {
    for (const relative of [path.join('src', 'control.ts'), path.join('src', 'control.js'), path.join('lib', 'control.js')]) {
      const candidate = path.join(subprocess, relative)
      if (fs.existsSync(candidate)) { controlFile = fs.realpathSync.native(candidate); break }
    }
    const src = path.join(subprocess, 'src')
    if (fs.existsSync(src)) dirs.add(src)
    const lib = path.join(subprocess, 'lib')
    if (fs.existsSync(lib)) dirs.add(lib)
  }
  return { process: processFile, control: controlFile, dirs: [...dirs] }
}

/** One prepared sandbox invocation. */
export interface SandboxWrap {
  argv: string[]
  policyFile: string
  auditFile: string
}

/** Runtime paths that must stay readable even though they live under a writable home. */
export interface RuntimeFiles {
  node: string
  ptcProcess?: string
  ptcControl?: string
  ptcDirs: string[]
}

/** Resolve the runtime files the PTC execution world needs. */
export function runtimeFiles(): RuntimeFiles {
  const node = fs.realpathSync.native(process.execPath)
  const ptc = findPtcFiles()
  return { node, ptcProcess: ptc.process, ptcControl: ptc.control, ptcDirs: ptc.dirs }
}

/** Escape one policy line; policy paths never contain newlines. */
function line(kind: string, value: string): string {
  return kind + ' ' + value
}

/**
 * Build the supervisor policy file for one workspace.
 * The policy mirrors the bwrap view: system reads, workspace read/write,
 * exact ancestors for traversal, workspace .tmp mapped at /tmp, user rules.
 */
function writePolicy(workspace: string, rules: readonly PathRule[]): string {
  const dir = path.join(guardRoot(), 'tmp')
  ensureDir(dir)
  const file = path.join(dir, 'policy-' + process.pid + '-' + Date.now() + '-' + Math.random().toString(16).slice(2) + '.txt')
  const runtime = runtimeFiles()
  const lines: string[] = []
  lines.push(line('workspace', workspace))
  for (const system of SYSTEM_READ_DIRS) {
    try {
      if (fs.statSync(system).isDirectory()) {
        lines.push(line('read', system))
        for (const ancestor of ancestorsOf(system)) lines.push(line('exact', ancestor))
      }
    } catch {
      // absent on this host
    }
  }
  lines.push(line('read', '/proc'))
  lines.push(line('exact', '/'))
  for (const ancestor of ancestorsOf(workspace)) lines.push(line('exact', ancestor))
  for (const ancestor of ancestorsOf(runtime.node)) lines.push(line('exact', ancestor))
  for (const file of [runtime.ptcProcess, runtime.ptcControl]) {
    if (file === undefined) continue
    for (const ancestor of ancestorsOf(file)) lines.push(line('exact', ancestor))
  }
  const manifests = new Set<string>()
  for (const dir of runtime.ptcDirs) {
    lines.push(line('read', dir))
    let current = dir
    for (let depth = 0; depth < 6; depth++) {
      const manifest = path.join(current, 'package.json')
      if (fs.existsSync(manifest)) manifests.add(fs.realpathSync.native(manifest))
      const parent = path.dirname(current)
      if (parent === current) break
      current = parent
    }
  }
  for (const manifest of manifests) lines.push(line('read', manifest))
  for (const dev of ['/dev', '/dev/null', '/dev/zero', '/dev/full', '/dev/random', '/dev/urandom', '/dev/tty', '/dev/pts']) {
    lines.push(line('dev', dev))
  }
  // Runtime files the PTC child must read. The supervisor binary itself is
  // read only when the runtime inspects its own process image; it carries no task data.
  const supervisor = supervisorPath()
  if (fs.existsSync(supervisor)) {
    for (const ancestor of ancestorsOf(supervisor)) lines.push(line('exact', ancestor))
    lines.push(line('read', supervisor))
  }
  lines.push(line('read', runtime.node))
  for (const file of [runtime.ptcProcess, runtime.ptcControl]) if (file !== undefined) lines.push(line('read', file))
  // User ordered rules apply to reads; later rules win.
  for (const rule of rules) lines.push(line('rule', (rule.allow ? 'allow ' : 'deny ') + rule.path))
  // A workspace under the host /tmp cannot also bind its .tmp over /tmp:
  // that bind would shadow the workspace itself. The supervisor is told not to
  // translate /tmp in that layout.
  const underHostTmp = workspace === '/tmp' || workspace.startsWith('/tmp/')
  if (!underHostTmp) lines.push(line('tmpbind', path.join(workspace, '.tmp')))
  fs.writeFileSync(file, lines.join('\n') + '\n', { encoding: 'utf8', mode: 0o600 })
  return file
}

/** Build the bwrap profile arguments for one workspace. */
function bwrapArgs(workspace: string, cwd: string, rules: readonly PathRule[], runtime: RuntimeFiles): string[] {
  const args: string[] = ['--unshare-all', '--die-with-parent', '--new-session']
  for (const system of SYSTEM_READ_DIRS) {
    try {
      if (fs.statSync(system).isDirectory()) args.push('--ro-bind', system, system)
    } catch {
      // absent
    }
  }
  args.push('--proc', '/proc', '--dev', '/dev')
  args.push('--bind', workspace, workspace)
  const tmp = path.join(workspace, '.tmp')
  ensureDir(tmp)
  const underHostTmp = workspace === '/tmp' || workspace.startsWith('/tmp/')
  let tmpdirForChild = '/tmp'
  if (underHostTmp) {
    args.push('--tmpfs', '/tmp')
    tmpdirForChild = tmp
  } else {
    args.push('--bind', tmp, '/tmp')
  }
  const supervisorFile = supervisorPath()
  if (fs.existsSync(supervisorFile)) args.push('--ro-bind', supervisorFile, supervisorFile)
  args.push('--ro-bind', runtime.node, runtime.node)
  const manifests = new Set<string>()
  for (const dir of runtime.ptcDirs) {
    args.push('--ro-bind', dir, dir)
    let current = dir
    for (let depth = 0; depth < 6; depth++) {
      const manifest = path.join(current, 'package.json')
      if (fs.existsSync(manifest)) manifests.add(fs.realpathSync.native(manifest))
      const parent = path.dirname(current)
      if (parent === current) break
      current = parent
    }
  }
  for (const manifest of manifests) args.push('--ro-bind', manifest, manifest)
  for (const file of [runtime.ptcProcess, runtime.ptcControl]) if (file !== undefined) args.push('--ro-bind', file, file)
  // Read-only exceptions the user asked for ('!' lines).
  for (const rule of rules) {
    if (!rule.allow) continue
    try {
      const real = fs.realpathSync.native(rule.path)
      if (real !== workspace && !real.startsWith(workspace + '/')) args.push('--ro-bind', real, real)
    } catch {
      // absent exception path is ignored
    }
  }
  const home = path.join(workspace, '.home')
  const cache = path.join(workspace, '.cache')
  const config = path.join(workspace, '.config')
  const data = path.join(workspace, '.local', 'share')
  for (const dir of [home, cache, config, data]) ensureDir(dir)
  for (const name of [
    'LD_LIBRARY_PATH', 'PYTHONPATH', 'MVCAM_SDK_PATH', 'AMENT_PREFIX_PATH', 'COLCON_PREFIX_PATH',
    'CMAKE_PREFIX_PATH', 'ROS_DISTRO', 'ROS_VERSION', 'ROS_PYTHON_VERSION', 'NODE_OPTIONS',
    'NODE_PATH', 'PYTHONSTARTUP', 'PYTHONHOME',
  ]) args.push('--unsetenv', name)
  args.push('--setenv', 'PATH', '/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin')
  args.push('--setenv', 'HOME', home)
  args.push('--setenv', 'TMPDIR', tmpdirForChild)
  args.push('--setenv', 'XDG_CACHE_HOME', cache)
  args.push('--setenv', 'XDG_CONFIG_HOME', config)
  args.push('--setenv', 'XDG_DATA_HOME', data)
  args.push('--setenv', 'LANG', 'C.UTF-8')
  args.push('--setenv', 'LC_ALL', 'C.UTF-8')
  args.push('--setenv', 'TERM', 'dumb')
  // Node/libuv opportunistically uses io_uring; the supervisor rejects it so a
  // program cannot bypass syscall-level path checks. Keep libuv on epoll.
  args.push('--setenv', 'UV_USE_IO_URING', '0')
  return args
}

/**
 * Wrap one argv in the strict sandbox.
 * @param argv - the exact command the consumer was about to spawn.
 * @param workspace - session cwd.
 * @param cwd - working directory the consumer will use (same as workspace in practice).
 * @param rules - ordered forbidden-path rules.
 * @param auditFile - host-side audit path the supervisor appends to.
 */
export function wrapSandbox(
  argv: readonly string[],
  workspace: string,
  cwd: string,
  rules: readonly PathRule[],
  auditFile: string,
): SandboxWrap {
  void cwd
  const supervisor = supervisorPath()
  if (!fs.existsSync(supervisor)) throw new Error('dsh-eval-guard: native supervisor is missing; refusing to run unconfined')
  ensureDir(guardRoot())
  ensureDir(path.dirname(auditFile))
  ensureDir(path.join(workspace, '.tmp'))
  ensureDir(path.join(workspace, '.home'))
  ensureDir(path.join(workspace, '.cache'))
  ensureDir(path.join(workspace, '.config'))
  ensureDir(path.join(workspace, '.local', 'share'))
  const policyFile = writePolicy(workspace, rules)
  const runtime = runtimeFiles()
  const args = bwrapArgs(workspace, cwd, rules, runtime)
  return {
    argv: [supervisor, '--policy', policyFile, '--audit', auditFile, '--', 'bwrap', ...args, '--', ...argv],
    policyFile,
    auditFile,
  }
}

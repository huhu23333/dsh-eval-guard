/**
 * Strict sandbox profile: bwrap with a system-only read view, the workspace
 * writable, the workspace .tmp mounted at /tmp, no host /tmp, no network, and
 * a host-side seccomp supervisor that denies out-of-view file/network effects.
 * @module dsh-eval-guard/sandbox
 */
import { spawnSync } from 'node:child_process'
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

/**
 * Probe the two host capabilities the guard depends on: a usable bwrap binary
 * and unprivileged user namespaces. Returns a human-readable failure, or
 * undefined when confinement is expected to work.
 */
export function probeConfinement(): string | undefined {
  const version = spawnSync('bwrap', ['--version'], { timeout: 2_000, encoding: 'utf8' })
  if (version.error !== undefined || version.status !== 0) {
    return 'bubblewrap (bwrap) is unavailable; refusing to enable without process confinement'
  }
  const probe = spawnSync('bwrap', [
    '--unshare-all', '--die-with-parent',
    '--ro-bind', '/', '/', '--dev', '/dev', '--proc', '/proc',
    '--', '/bin/true',
  ], { timeout: 5_000, encoding: 'utf8' })
  if (probe.error !== undefined || probe.status !== 0) {
    return 'bubblewrap cannot create an unprivileged user namespace on this host; refusing to enable without process confinement'
  }
  return undefined
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

/**
 * Individual read-only system files commonly needed by benign inspection tools
 * such as uptime/who. Keep this list narrow; do not expose /run as a directory.
 */
const SYSTEM_READ_FILES: readonly string[] = ['/run/utmp', '/run/utmpx', '/var/log/wtmp']

/** Host environment variables that are safe to expose to a confined command. */
const SAFE_ENV_KEYS: readonly string[] = [
  'USER', 'LOGNAME', 'SHELL', 'TZ',
  'SSL_CERT_FILE', 'SSL_CERT_DIR', 'REQUESTS_CA_BUNDLE', 'CURL_CA_BUNDLE',
  'NODE_EXTRA_CA_CERTS', 'GIT_SSL_CAINFO',
]

/** Whether `root` is `target` or an ancestor of it. */
function containsPath(root: string, target: string): boolean {
  const relative = path.relative(root, target)
  return relative === '' || (!relative.startsWith('..') && !path.isAbsolute(relative))
}

/** Python stdlib/venv directories that may live next to a workspace. */
function pythonRuntimeDirs(workspace: string): string[] {
  const dirs = new Set<string>()
  for (const ancestor of [workspace, ...ancestorsOf(workspace)]) {
    for (const lib of ['lib', 'lib64']) {
      const base = path.join(ancestor, lib)
      try {
        for (const entry of fs.readdirSync(base, { withFileTypes: true })) {
          if (entry.isDirectory() && entry.name.startsWith('python')) {
            const real = fs.realpathSync.native(path.join(base, entry.name))
            if (!SYSTEM_READ_DIRS.some(system => real === system || real.startsWith(system + '/'))) dirs.add(real)
          }
        }
      } catch {
        // no Python tree at this level
      }
    }
  }
  return [...dirs]
}

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
/** Likely roots for the running DSH installation (app, checkout, or profile). */
function runtimeRoots(): string[] {
  const roots = new Set<string>()
  const starts: string[] = [fileURLToPath(new URL('..', import.meta.url)), process.cwd()]
  if (process.argv[1] !== undefined) starts.push(path.dirname(process.argv[1]))
  if (process.env.DSH_CHECKOUT !== undefined && process.env.DSH_CHECKOUT !== '') starts.push(process.env.DSH_CHECKOUT)
  for (const start of starts) {
    let current = path.resolve(start)
    for (let depth = 0; depth < 8; depth += 1) {
      roots.add(current)
      const parent = path.dirname(current)
      if (parent === current) break
      current = parent
    }
  }
  return [...roots]
}

/** Checkout-relative guesses for DSH source layouts. */
const CHECKOUT_PATHS: Record<string, readonly string[]> = {
  '@deepseek-ai/dsh-ptc-runtime-node': ['packages/ptc-runtime/ptc-runtime-node'],
  '@deepseek-ai/dsh-subprocess': ['packages/subprocess/subprocess'],
}

function packageDir(name: string): string | undefined {
  const candidates: string[] = []
  const roots = runtimeRoots()
  for (const root of roots) {
    try {
      const require = createRequire(path.join(root, 'noop.js'))
      candidates.push(path.dirname(require.resolve(name + '/package.json')))
    } catch {
      // keep looking
    }
    candidates.push(path.join(root, 'node_modules', name))
    for (const relative of CHECKOUT_PATHS[name] ?? []) candidates.push(path.join(root, relative))
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
  for (const file of SYSTEM_READ_FILES) {
    // Allow the path even when it is absent on this host: a missing file should
    // produce ENOENT, not a policy violation that kills the whole command group.
    lines.push(line('read', file))
    for (const ancestor of ancestorsOf(file)) lines.push(line('exact', ancestor))
  }
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
  for (const dir of pythonRuntimeDirs(workspace)) {
    lines.push(line('read', dir))
    for (const ancestor of ancestorsOf(dir)) lines.push(line('exact', ancestor))
  }
  // User ordered rules apply to reads; later rules win.
  for (const rule of rules) lines.push(line('rule', (rule.allow ? 'allow ' : 'deny ') + rule.path))
  // A workspace under the host /tmp cannot also bind its .tmp over /tmp:
  // that bind would shadow the workspace itself. The supervisor is told not to
  // translate /tmp in that layout.
  const underHostTmp = workspace === '/tmp' || workspace.startsWith('/tmp/')
  if (!underHostTmp) {
    lines.push(line('tmpbind', path.join(workspace, '.tmp')))
  } else {
    // The sandbox mounts a private tmpfs at /tmp (see bwrapArgs); the
    // supervisor must treat that ephemeral tmpfs as writable, while the host
    // /tmp never appears in the mount view at all.
    lines.push(line('tmpfs', '/tmp'))
  }
  fs.writeFileSync(file, lines.join('\n') + '\n', { encoding: 'utf8', mode: 0o600 })
  return file
}

/** Build the bwrap profile arguments for one workspace. */
function bwrapArgs(workspace: string, rules: readonly PathRule[], runtime: RuntimeFiles): string[] {
  const args: string[] = ['--unshare-all', '--die-with-parent', '--new-session', '--as-pid-1']
  for (const system of SYSTEM_READ_DIRS) {
    try {
      if (fs.statSync(system).isDirectory()) args.push('--ro-bind', system, system)
    } catch {
      // absent
    }
  }
  args.push('--proc', '/proc', '--dev', '/dev')
  const systemFiles = SYSTEM_READ_FILES.filter(file => {
    try { return fs.statSync(file).isFile() } catch { return false }
  })
  if (systemFiles.some(file => file.startsWith('/run/'))) args.push('--dir', '/run')
  if (systemFiles.some(file => file.startsWith('/var/log/'))) {
    args.push('--dir', '/var')
    args.push('--dir', '/var/log')
  }
  for (const file of systemFiles) args.push('--ro-bind', file, file)
  const tmp = path.join(workspace, '.tmp')
  ensureDir(tmp)
  const underHostTmp = workspace === '/tmp' || workspace.startsWith('/tmp/')
  let tmpdirForChild = '/tmp'
  if (underHostTmp) {
    // A workspace that lives under the host /tmp must have the private tmpfs
    // mounted BEFORE its own bind: mounting it afterwards would shadow the
    // workspace subtree with an empty tmpfs.
    args.push('--tmpfs', '/tmp')
    tmpdirForChild = tmp
  }
  args.push('--bind', workspace, workspace)
  if (!underHostTmp) args.push('--bind', tmp, '/tmp')
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
  for (const dir of pythonRuntimeDirs(workspace)) args.push('--ro-bind', dir, dir)
  // Read-only exceptions the user asked for ('!' lines).
  //
  // An exception is an audit rule first; it must not turn into a host mount
  // that exposes the user's home, the DSH state directory, the shared /tmp, or
  // an ancestor of the writable workspace. System directories are already
  // mounted read-only above.
  for (const rule of rules) {
    if (!rule.allow) continue
    try {
      const real = fs.realpathSync.native(rule.path)
      if (containsPath(real, workspace) || containsPath(workspace, real)) continue
      if (containsPath(real, guardRoot()) || containsPath(guardRoot(), real)) continue
      if (containsPath(real, dshHome()) || containsPath(dshHome(), real)) continue
      if (real === '/tmp' || real.startsWith('/tmp/')) continue
      if (SYSTEM_READ_DIRS.some(system => real === system || real.startsWith(system + '/'))) continue
      args.push('--ro-bind', real, real)
    } catch {
      // absent exception path is ignored
    }
  }
  const home = path.join(workspace, '.home')
  const cache = path.join(workspace, '.cache')
  const config = path.join(workspace, '.config')
  const data = path.join(workspace, '.local', 'share')
  for (const dir of [home, cache, config, data]) ensureDir(dir)
  // The host environment carries credentials and operator state that confined
  // candidate/agent code must not see. Clear it completely, then add back only
  // locale/path/terminal plumbing.
  args.push('--clearenv')
  for (const name of SAFE_ENV_KEYS) {
    const value = process.env[name]
    if (value !== undefined && value !== '') args.push('--setenv', name, value)
  }
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
  // pip's post-command update check opens an AF_INET socket even for
  // `pip --version`, which would otherwise trip the no-network kill path.
  args.push('--setenv', 'PIP_DISABLE_PIP_VERSION_CHECK', '1')
  args.push('--setenv', 'PIP_NO_INPUT', '1')
  return args
}

/**
 * Wrap one argv in the strict sandbox.
 * @param argv - the exact command the consumer was about to spawn.
 * @param workspace - session cwd.
 * @param cwd - explicit working directory override; the empty string lets the
 *   supervisor use its inherited cwd when that cwd lies inside the workspace.
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
  const args = bwrapArgs(workspace, rules, runtime)
  // The supervisor runs with the consumer's cwd. When no explicit cwd is
  // requested it uses its inherited cwd when that cwd is inside the workspace,
  // which preserves the bash tool's workdir; otherwise it falls back to the
  // workspace. Scanning argv for a later '--' broke commands that carry one of
  // their own (for example `bash -- -c ...`).
  const cwdArgs = cwd === '' ? [] : ['--cwd', cwd]
  return {
    argv: [supervisor, '--policy', policyFile, '--audit', auditFile, ...cwdArgs, '--', 'bwrap', ...args, '--', ...argv],
    policyFile,
    auditFile,
  }
}

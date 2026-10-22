/**
 * Tool blacklist, path rules, and tool-argument path auditing.
 *
 * The blacklist is a user document: one tool name per line, '#' comments.
 * The default document is generated from the tools actually installed, so a
 * profile that lacks a plugin simply never mentions that plugin's tools.
 * @module dsh-eval-guard/policy
 */
import fs from 'node:fs'
import path from 'node:path'

/**
 * DSH-first default section. Only names that exist in the registry are surfaced
 * in prompts; the enforcement set keeps every name so a tool installed later
 * cannot slip through with an empty user document.
 *
 * The last block covers tools that can otherwise escape a one-shot evaluation:
 * goal rounds and schedules keep working after the wall-clock deadline, and the
 * drawio/canvas tools write host files directly (bypassing the fs sandbox) and
 * can cross workspace boundaries.
 */
const BASE_BLACKLIST: readonly string[] = [
  'web_search',
  'web_fetch',
  'subagent',
  'subagent_fork',
  'subagent_role',
  'close_subagent',
  'list_subagent_models',
  'spawn_teammate',
  'retire_teammate',
  'send_message',
  'list_agents',
  'interrupt_agent',
  'wait_agent',
  'team_task_create',
  'team_task_get',
  'team_task_list',
  'team_task_update',
  'session_search',
  'session_event_read',
  'session_event_search',
  'session_event_trace',
  'session_trace',
  'terminal_open',
  'terminal_send',
  'terminal_read',
  'terminal_close',
  'terminal_list',
  'terminal_signal',
  'pwsh',
  'workflow',
  'ralph',
  'cordis_inspect_list',
  'cordis_inspect_query',
  'exit_plan_mode',
  'ask_user_question',
  // Goal / schedule persistence outlives one turn and one deadline.
  'create_goal',
  'get_goal',
  'update_goal',
  'schedule_create',
  'schedule_list',
  'schedule_update',
  'schedule_delete',
  // Host-side file writers that do not go through the fs sandbox.
  'drawio_edit',
  'drawio_render',
  'drawio_template',
  'drawio_validate',
  'canvas_state',
  'view_canvas',
]

/** Third-party tools that reach an external service, subagent, or agent channel. */
const PLUGIN_BLACKLIST_PATTERNS: readonly RegExp[] = [
  /^generate_images?$/,
  /^edit_image$/,
  /^find_dsh_plugins$/,
  /^dshmarket/i,
  /^orchestrate$/,
  /^out[sS]ourc/,
  /^image[-_]gen/i,
  /^web_/,
  /^browser_/,
  /^computer_/,
  /^ssh_/,
  /^e2b_/,
  /^mcp_/,
  /^drawio_/,
  /^schedule_/,
  /^(?:create|get|update)_goal$/,
  /^canvas_/,
  /^view_canvas$/,
]

/** Parse one blacklist document into ordered tool names (comments removed). */
export function parseBlacklist(text: string): string[] {
  const names: string[] = []
  const seen = new Set<string>()
  for (const raw of text.split(/\r?\n/)) {
    const line = raw.trim()
    if (line === '' || line.startsWith('#')) continue
    const name = line.split(/\s+/)[0] ?? ''
    if (name === '' || seen.has(name)) continue
    seen.add(name)
    names.push(name)
  }
  return names
}

/** Build the default blacklist document from the tools that exist right now. */
export function generateDefaultBlacklist(installed: readonly string[]): string {
  const present = new Set(installed)
  const base = [...BASE_BLACKLIST]
  const baseSet = new Set(BASE_BLACKLIST)
  const plugin: string[] = []
  for (const name of [...installed].sort()) {
    if (baseSet.has(name)) continue
    if (!PLUGIN_BLACKLIST_PATTERNS.some(pattern => pattern.test(name))) continue
    plugin.push(name)
  }
  const lines = ['# dsh 本体', ...base]
  lines.push('# --- 已安装插件工具 ---')
  lines.push(...plugin)
  return lines.join('\n')
}

/** Whether a name belongs to the always-present DSH base section. */
export function isBaseBlacklistName(name: string): boolean {
  return BASE_BLACKLIST.includes(name)
}

/** One ordered forbidden-path rule; 'allow' is a '!' exception. */
export interface PathRule {
  allow: boolean
  path: string
}

/**
 * Parse the forbidden-paths document. Invalid lines are ignored.
 *
 * A line containing a glob character is treated as the prefix before the first
 * wildcard, so a line like /home/<anything>/secret means "everything under
 * /home"; this is deliberately conservative (never narrower than written).
 */
export function parsePathRules(text: string): PathRule[] {
  const rules: PathRule[] = []
  for (const raw of text.split(/\r?\n/)) {
    let line = raw.trim()
    if (line === '' || line.startsWith('#')) continue
    let allow = false
    if (line.startsWith('!')) {
      allow = true
      line = line.slice(1).trim()
    }
    if (!line.startsWith('/')) continue
    const wildcard = line.search(/[*?]/)
    if (wildcard >= 0) line = line.slice(0, wildcard)
    if (line.length > 1 && line.endsWith('/')) line = line.slice(0, -1)
    if (line === '') line = '/'
    rules.push({ allow, path: path.normalize(line) })
  }
  return rules
}

/** Whether a candidate is exactly the prefix or lives under it. */
export function under(candidate: string, prefix: string): boolean {
  if (prefix === '/') return candidate.startsWith('/')
  if (candidate === prefix) return true
  return candidate.startsWith(prefix.endsWith('/') ? prefix : prefix + '/')
}

/** Resolve a possibly-relative path, following existing ancestors for missing tails. */
export function resolveCheckPath(target: string, cwd: string): string {
  const absolute = path.isAbsolute(target) ? path.normalize(target) : path.resolve(cwd, target)
  try {
    return fs.realpathSync.native(absolute)
  } catch {
    const tail: string[] = []
    let current = absolute
    for (;;) {
      const parent = path.dirname(current)
      if (parent === current) return absolute
      tail.unshift(path.basename(current))
      try {
        const real = fs.realpathSync.native(parent)
        return path.join(real, ...tail)
      } catch {
        current = parent
      }
    }
  }
}

/** Files that may be written without leaving a durable trace. */
export function isNoTraceDevice(target: string): boolean {
  return target === '/dev/null' || target === '/dev/zero' || target === '/dev/full' ||
    target === '/dev/random' || target === '/dev/urandom' || target === '/dev/tty' ||
    target.startsWith('/dev/pts/') || target === '/dev/stdin' || target === '/dev/stdout' ||
    target === '/dev/stderr'
}

/** Base read permission before the user's ordered rules. */
function baseReadAllowed(real: string, workspace: string): boolean {
  if (under(real, workspace)) return true
  if (isNoTraceDevice(real)) return true
  try {
    fs.accessSync(real, fs.constants.W_OK)
  } catch {
    try {
      fs.accessSync(real, fs.constants.R_OK)
      return true
    } catch {
      return false
    }
  }
  return false
}

/** Apply ordered user rules (later rules win) to a base read decision. */
export function applyRules(real: string, rules: readonly PathRule[], base: boolean): boolean {
  let allowed = base
  for (const rule of rules) {
    if (under(real, rule.path)) allowed = rule.allow
  }
  return allowed
}

/** Decide one absolute path for read or write access. */
export function checkPath(real: string, workspace: string, write: boolean, rules: readonly PathRule[]): boolean {
  const inWorkspace = under(real, workspace)
  if (!write) {
    if (inWorkspace) return true
    if (isNoTraceDevice(real)) return true
    return applyRules(real, rules, baseReadAllowed(real, workspace))
  }
  // Writes stay inside the workspace (or a no-trace device). Ordered rules can
  // narrow that set, but an allow rule must never widen it beyond the workspace.
  const base = inWorkspace || isNoTraceDevice(real)
  if (!base) return false
  return applyRules(real, rules, base) && base
}

/** Keys whose value is task content, not another path to audit. */
const CONTENT_KEYS = new Set([
  'content', 'old_string', 'new_string', 'text', 'code', 'prompt',
  'description', 'justification', 'new_source', 'file_text',
])

/** Path-bearing argument keys audited for DSH-built-in and installed tools. */
const PATH_KEYS = new Set([
  'path', 'file', 'file_path', 'filePath', 'files', 'dir', 'cwd', 'root', 'source',
  'destination', 'dest', 'target', 'input', 'output', 'script_path',
  'source_path', 'source_paths', 'file_paths', 'paths', 'input_path', 'output_path',
  'image', 'images', 'out',
])

/** Tools whose path arguments are read-only accesses. */
const READ_TOOLS = new Set([
  'read', 'read_image', 'glob', 'grep', 'present', 'load_workspace_dependencies',
  'ops_inspect_local_file', 'ops_grep', 'ops_glob',
  'drawio_validate',
])

/** Tools whose path arguments are mutations. */
const WRITE_TOOLS = new Set([
  'write', 'edit', 'str_replace_editor', 'ops_replace',
  'drawio_edit', 'drawio_render', 'drawio_template',
])

/** Tools we deliberately do not inspect: the process sandbox owns their effects. */
const PROCESS_TOOLS = new Set([
  'bash', 'ops_bash', 'ops_run', 'ops_run_background', 'run_code', 'job_output',
  'job_list', 'job_kill', 'ops_job_output', 'ops_job_list', 'ops_job_kill',
])

/** Whether one tool name takes file paths this module knows how to audit. */
export function isAuditedTool(name: string): boolean {
  return READ_TOOLS.has(name) || WRITE_TOOLS.has(name)
}

/** Collect every path-looking string under a known path key. */
export function collectPathArgs(value: unknown, out: string[] = []): string[] {
  if (Array.isArray(value)) {
    for (const item of value) collectPathArgs(item, out)
    return out
  }
  if (value !== null && typeof value === 'object') {
    for (const [key, child] of Object.entries(value as Record<string, unknown>)) {
      if (PATH_KEYS.has(key)) {
        if (typeof child === 'string') out.push(child)
        else if (Array.isArray(child)) {
          for (const item of child) {
            if (typeof item === 'string') out.push(item)
            else collectPathArgs(item, out)
          }
        } else collectPathArgs(child, out)
      } else if (!CONTENT_KEYS.has(key)) {
        // File contents are data: a JSON body containing a "path" key must not
        // be mistaken for a second path argument.
        collectPathArgs(child, out)
      }
    }
  }
  return out
}

/** Result of auditing one tool call. */
export interface AuditDecision {
  allow: boolean
  tool: string
  path?: string
  write: boolean
  reason?: string
}

/** Audit the path arguments of one DSH/ops tool call. Non-targeted tools pass. */
export function auditToolCall(
  name: string,
  args: unknown,
  workspace: string,
  rules: readonly PathRule[],
): AuditDecision {
  if (PROCESS_TOOLS.has(name)) return { allow: true, tool: name, write: false }
  if (!isAuditedTool(name)) return { allow: true, tool: name, write: false }
  const argsRecord = args !== null && typeof args === 'object' && !Array.isArray(args)
    ? args as Record<string, unknown>
    : undefined
  // str_replace_editor's view command is a read; treating it as a write blocks
  // inspecting read-only system files outside the workspace.
  const write = name === 'str_replace_editor' && argsRecord?.['command'] === 'view'
    ? false
    : WRITE_TOOLS.has(name)
  const paths = collectPathArgs(args)
  for (const target of paths) {
    const real = resolveCheckPath(target, workspace)
    if (!checkPath(real, workspace, write, rules)) {
      return {
        allow: false,
        tool: name,
        path: real,
        write,
        reason: write
          ? '只允许修改当前工作区；该写入已被阻止。'
          : '只允许读取当前工作区与系统只读文件；该读取已被阻止。',
      }
    }
  }
  return { allow: true, tool: name, write }
}

/** The ask-user tool never waits during an evaluation. */
export const WAIT_TOOLS: readonly string[] = ['ask_user_question']

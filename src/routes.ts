/**
 * Browser-facing routes: enable pre-check, workspace check, task install,
 * manual-channel paths, and the warning event feed.
 * @module dsh-eval-guard/routes
 */
import fs from 'node:fs'
import path from 'node:path'
import { spawn } from 'node:child_process'
import type { Context } from '@deepseek-ai/cordis'
import {
  CHECK_ROUTE, EVENTS_ROUTE, INSTALL_ROUTE, MANUAL_ROUTE, PROMPT_ROUTE, REVEAL_ROUTE,
  STATE_ROUTE, TANK_TASK, JOBS_ROUTE, JOB_ROUTE, JOB_CANCEL_ROUTE, EVAL_ROUTE,
  type CheckResult, type GuardState, type ManualPaths, type JobDetail, PIP_MIRRORS,
} from './shared.ts'
import type { Guard } from './guard.ts'
import { TASKS, type TaskService } from './task.ts'
import { dshHome, guardRoot } from './sandbox.ts'

/** Minimal Workspace shape read from the registry. */
interface WorkspaceLike { path: string }

interface FetchRouteRegistrar {
  register(route: {
    path: string
    methods: readonly ('GET' | 'HEAD' | 'POST')[]
    requestBody: 'buffered' | 'streaming'
    fetch: (request: Request) => Promise<Response>
  }): () => Promise<void>
}

/** One JSON reply. */
function json(value: unknown, status = 200): Response {
  return new Response(JSON.stringify(value), {
    status,
    headers: { 'content-type': 'application/json', 'cache-control': 'no-store' },
  })
}

/** Read a query parameter. */
function query(request: Request, key: string): string {
  return new URL(request.url).searchParams.get(key) ?? ''
}

/** Whether one path is inside another. */
function under(target: string, root: string): boolean {
  const relative = path.relative(root, target)
  return relative === '' || (!relative.startsWith('..') && !path.isAbsolute(relative))
}

/** Resolve symlinks where possible; keep the lexical path when it does not exist. */
function realOrSelf(target: string): string {
  try { return fs.realpathSync.native(target) } catch { return path.resolve(target) }
}

/**
 * Register every route this plugin owns.
 * @param ctx - host context.
 * @param guard - the guard instance.
 * @param tasks - task service.
 */
export function registerRoutes(ctx: Context, guard: Guard, tasks: TaskService): void {
  ctx.inject(['connection'], (child) => {
    const connection = child.get('connection') as { fetch?: FetchRouteRegistrar } | undefined
    const register = connection?.fetch?.register?.bind(connection.fetch)
    if (register === undefined) {
      ctx.logger.warn('eval-guard: connection.fetch is unavailable; the Settings page cannot reach the host')
      return
    }
    const add = (route: Parameters<FetchRouteRegistrar['register']>[0]): void => {
      child.effect(() => register(route), 'eval-guard: route ' + route.path)
    }
    add({
      path: STATE_ROUTE, methods: ['GET'], requestBody: 'buffered',
      fetch: async (): Promise<Response> => {
        const state: GuardState = {
          enabled: guard.active,
          ...guard.lastFailure === undefined ? {} : { error: guard.lastFailure },
          running: guard.runningSessions(),
          tasks: [...TASKS],
          defaultBlacklist: guard.defaultBlacklist(),
          generatedDefault: guard.configuration.toolBlacklistUseDefault,
          workspaceRoot: dshHome(),
          runsRoot: path.join(guardRoot(), 'runs'),
          pipMirrors: [...PIP_MIRRORS],
        }
        return json(state)
      },
    })
    add({
      path: CHECK_ROUTE, methods: ['GET'], requestBody: 'buffered',
      fetch: async (request: Request): Promise<Response> => {
        const workspaceId = query(request, 'workspaceId')
        if (workspaceId === '') return json({ error: 'workspaceId is required' }, 400)
        return json(tasks.check(workspaceId))
      },
    })
    add({
      path: INSTALL_ROUTE, methods: ['POST'], requestBody: 'buffered',
      fetch: async (request: Request): Promise<Response> => {
        let body: { workspaceId?: unknown; task?: unknown; cloneSource?: unknown; customCloneUrl?: unknown; pipIndex?: unknown; customPipUrl?: unknown; gitProxyUrl?: unknown; pipProxyUrl?: unknown; cifarProxyUrl?: unknown; cifarMirror?: unknown; customCifarUrl?: unknown } = {}
        try { body = await request.json() as typeof body } catch { return json({ error: 'invalid body' }, 400) }
        const workspaceId = typeof body.workspaceId === 'string' ? body.workspaceId : ''
        const taskId = typeof body.task === 'string' && body.task !== '' ? body.task : TANK_TASK
        if (workspaceId === '') return json({ error: 'workspaceId is required' }, 400)
        const config = guard.configuration
        return json(tasks.startInstall(workspaceId, taskId, {
          cloneSource: typeof body.cloneSource === 'string' ? body.cloneSource : config.cloneSource,
          customCloneUrl: typeof body.customCloneUrl === 'string' ? body.customCloneUrl : config.customCloneUrl,
          pipIndex: typeof body.pipIndex === 'string' ? body.pipIndex : config.pipIndex,
          customPipUrl: typeof body.customPipUrl === 'string' ? body.customPipUrl : config.customPipUrl,
          gitProxyUrl: typeof body.gitProxyUrl === 'string' ? body.gitProxyUrl : config.gitProxyUrl,
          pipProxyUrl: typeof body.pipProxyUrl === 'string' ? body.pipProxyUrl : config.pipProxyUrl,
          cifarProxyUrl: typeof body.cifarProxyUrl === 'string' ? body.cifarProxyUrl : config.cifarProxyUrl,
          cifarMirror: typeof body.cifarMirror === 'string' ? body.cifarMirror : config.cifarMirror,
          customCifarUrl: typeof body.customCifarUrl === 'string' ? body.customCifarUrl : config.customCifarUrl,
        }))
      },
    })
    add({
      path: EVAL_ROUTE, methods: ['POST'], requestBody: 'buffered',
      fetch: async (request: Request): Promise<Response> => {
        let body: { workspaceId?: unknown; task?: unknown } = {}
        try { body = await request.json() as typeof body } catch { return json({ error: 'invalid body' }, 400) }
        const workspaceId = typeof body.workspaceId === 'string' ? body.workspaceId : ''
        const taskId = typeof body.task === 'string' && body.task !== '' ? body.task : TANK_TASK
        if (workspaceId === '') return json({ error: 'workspaceId is required' }, 400)
        return json(tasks.startEval(workspaceId, taskId))
      },
    })
    add({
      path: JOBS_ROUTE, methods: ['GET'], requestBody: 'buffered',
      fetch: async (request: Request): Promise<Response> => {
        const workspaceId = query(request, 'workspaceId')
        const taskId = query(request, 'task')
        return json(tasks.jobs.list(workspaceId === '' ? undefined : workspaceId, taskId === '' ? undefined : taskId))
      },
    })
    add({
      path: JOB_ROUTE, methods: ['GET'], requestBody: 'buffered',
      fetch: async (request: Request): Promise<Response> => {
        const jobId = query(request, 'id')
        const job: JobDetail | undefined = tasks.jobs.get(jobId)
        return job === undefined ? json({ error: 'job not found' }, 404) : json(job)
      },
    })
    add({
      path: JOB_CANCEL_ROUTE, methods: ['POST'], requestBody: 'buffered',
      fetch: async (request: Request): Promise<Response> => {
        let body: { id?: unknown } = {}
        try { body = await request.json() as typeof body } catch { return json({ error: 'invalid body' }, 400) }
        const jobId = typeof body.id === 'string' ? body.id : ''
        if (jobId === '') return json({ error: 'id is required' }, 400)
        const job = tasks.jobs.cancel(jobId)
        return job === undefined ? json({ error: 'job not found' }, 404) : json(job)
      },
    })
    add({
      path: PROMPT_ROUTE, methods: ['GET'], requestBody: 'buffered',
      fetch: async (request: Request): Promise<Response> => {
        const workspaceId = query(request, 'workspaceId')
        const taskId = query(request, 'task') || TANK_TASK
        if (tasks.definition(taskId) === undefined) return json({ error: 'unknown task' }, 400)
        return json({ prompt: tasks.prompt(workspaceId, taskId) })
      },
    })
    add({
      path: MANUAL_ROUTE, methods: ['GET'], requestBody: 'buffered',
      fetch: async (request: Request): Promise<Response> => {
        const workspaceId = query(request, 'workspaceId')
        const taskId = query(request, 'task') || TANK_TASK
        if (tasks.definition(taskId) === undefined) return json({ error: 'unknown task' }, 400)
        const paths: ManualPaths = tasks.manualPaths(workspaceId, taskId)
        return json(paths)
      },
    })
    add({
      path: EVENTS_ROUTE, methods: ['GET'], requestBody: 'buffered',
      fetch: async (request: Request): Promise<Response> => {
        const since = Number(query(request, 'since'))
        return json({ events: guard.recent(Number.isFinite(since) ? since : 0) })
      },
    })
    add({
      path: REVEAL_ROUTE, methods: ['POST'], requestBody: 'buffered',
      fetch: async (request: Request): Promise<Response> => {
        let body: { path?: unknown } = {}
        try { body = await request.json() as typeof body } catch { return json({ error: 'invalid body' }, 400) }
        const target = typeof body.path === 'string' ? path.resolve(body.path) : ''
        if (target === '') return json({ error: 'path is required' }, 400)
        if (!fs.existsSync(target)) return json({ error: 'path does not exist' }, 404)
        // Validate the REAL path so a symlink inside a workspace cannot make the
        // host open an unrelated location.
        const real = realOrSelf(target)
        const material = realOrSelf(path.join(guardRoot(), 'material'))
        const workspaces = (child.get('workspaceRegistry') as { list?: () => WorkspaceLike[] } | undefined)?.list?.() ?? []
        const allowed = under(real, material) || workspaces.some(workspace => under(real, realOrSelf(workspace.path)))
        if (!allowed) return json({ error: 'path is not revealable' }, 403)
        try {
          await new Promise<void>((resolve, reject) => {
            const childProcess = spawn('xdg-open', [real], { detached: true, stdio: 'ignore' })
            childProcess.once('spawn', () => { childProcess.unref(); resolve() })
            childProcess.once('error', (error: Error) => { reject(error) })
          })
        } catch (error: unknown) {
          return json({ error: error instanceof Error ? error.message : String(error) }, 500)
        }
        return json({ ok: true })
      },
    })
  })
}

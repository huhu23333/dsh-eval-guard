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
  STATE_ROUTE, TANK_TASK, type CheckResult, type GuardState, type ManualPaths,
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
          running: guard.runningSessions(),
          tasks: [...TASKS],
          defaultBlacklist: guard.defaultBlacklist(),
          generatedDefault: guard.configuration.toolBlacklistUseDefault,
          workspaceRoot: dshHome(),
          runsRoot: path.join(guardRoot(), 'runs'),
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
        let body: { workspaceId?: unknown; task?: unknown } = {}
        try { body = await request.json() as typeof body } catch { return json({ error: 'invalid body' }, 400) }
        const workspaceId = typeof body.workspaceId === 'string' ? body.workspaceId : ''
        const taskId = typeof body.task === 'string' && body.task !== '' ? body.task : TANK_TASK
        if (workspaceId === '') return json({ error: 'workspaceId is required' }, 400)
        return json(tasks.install(workspaceId, taskId))
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
        const material = path.resolve(guardRoot(), 'material')
        const workspaces = (child.get('workspaceRegistry') as { list?: () => WorkspaceLike[] } | undefined)?.list?.() ?? []
        const allowed = under(target, material) || workspaces.some(workspace => under(target, workspace.path))
        if (!allowed) return json({ error: 'path is not revealable' }, 403)
        if (!fs.existsSync(target)) return json({ error: 'path does not exist' }, 404)
        try {
          const childProcess = spawn('xdg-open', [target], { detached: true, stdio: 'ignore' })
          childProcess.unref()
        } catch (error: unknown) {
          return json({ error: error instanceof Error ? error.message : String(error) }, 500)
        }
        return json({ ok: true })
      },
    })
  })
}

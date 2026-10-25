/**
 * Evaluation guard, node half.
 *
 * While enabled, every user main conversation is confined to its session cwd:
 * process confinement comes from a bwrap profile plus the host-side seccomp
 * supervisor, tool calls pass a configurable blacklist and path audit, and one
 * wall-clock window per user message bounds the run. Task installation writes a
 * run record the independent evaluator consumes.
 * @module dsh-eval-guard
 */
import type { Context } from '@deepseek-ai/cordis'
import { Config } from './config.ts'
import { Guard } from './guard.ts'
import { registerRoutes } from './routes.ts'
import { TANK_DEFINITION, TASKS, TaskService } from './task.ts'
import { registerAgentSubmissionTools } from './submission-tools.ts'
import { configureRuntimeContext } from './sandbox.ts'
import { installAgentRunGuards } from './agent-guards.ts'

export { Config } from './config.ts'
export type { Config as EvalGuardConfig } from './config.ts'

/** Stable Cordis plugin name. */
export const name = 'eval-guard'

/** Services required before the guard can observe and confine anything. */
export const inject = ['sessions', 'agents', 'tools', 'sandbox', 'sandboxPolicy', 'workspaceRegistry']

/**
 * Plugin body.
 * @param ctx - host context.
 * @param config - live volatile configuration.
 */
export function apply(ctx: Context, config: Partial<import('./config.ts').Config>): void {
  ctx.inject(['sessions', 'agents', 'tools', 'sandbox', 'sandboxPolicy', 'workspaceRegistry'], (child) => {
    const getService = (ctx as unknown as { get?: (key: string) => unknown }).get
    const profile = (typeof getService === 'function' ? getService.call(ctx, 'profileContext') : undefined) as
      { home?: string; dir?: string } | undefined
    configureRuntimeContext({ home: profile?.home, profileDir: profile?.dir })
    const services = {
      sessions: child.get('sessions') as any,
      agents: child.get('agents') as any,
      tools: child.get('tools') as any,
      sandbox: child.get('sandbox') as any,
      sandboxPolicy: child.get('sandboxPolicy') as any,
      approval: child.get('approval') as any,
      jobs: child.get('jobs') as any,
      workspaceRegistry: child.get('workspaceRegistry') as any,
      on: (event: string, listener: (...args: any[]) => void) => child.on(event as never, listener as never),
    }
    const guard = new Guard(ctx, services, config, TANK_DEFINITION)
    const tasks = new TaskService(guard, services.workspaceRegistry)
    guard.onRunAssociated = (agent, run) => { registerAgentSubmissionTools(agent as any, guard, tasks) }
    guard.onAutoRun = (agent, cwd) => tasks.autoBind(agent as any, cwd)
    registerRoutes(ctx, guard, tasks)
    installAgentRunGuards(child, { jobs: services.jobs, on: services.on }, guard, tasks)
    child.inject(['settings'], (settingsCtx) => {
      const settings = settingsCtx.get('settings') as {
        configure(options: { auto: boolean }, fiber: unknown): () => void
      }
      settingsCtx.effect(() => settings.configure({ auto: false }, ctx.fiber), 'eval-guard: settings page')
    })
    child.effect(() => () => { guard.dispose() }, 'eval-guard: dispose')
    // Reconcile immediately: a profile may load with the switch already on.
    guard.update(config ?? {})
    ctx.logger.info('eval-guard: task %s ready (%d task definition(s))', TASKS.map(task => task.id).join(','), TASKS.length)
  })
}

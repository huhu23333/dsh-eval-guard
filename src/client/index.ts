/**
 * Evaluation guard, browser half: the Settings section, the warning toast, and
 * the shell bridge used to seed a new conversation with the task prompt.
 * @module dsh-eval-guard/client
 */
import type { Context } from '@deepseek-ai/cordis'
import { SETTINGS_NS } from '../shared.ts'
import { EvalGuardSettings } from './Settings.tsx'
import { GuardToast } from './Toast.tsx'
import { configureEvalBridge } from './bridge.ts'
import { en, NS, zh, type Key } from './locales.ts'

declare module '@deepseek-ai/dsh-client-ui-slots' {
  interface LocaleNamespaceMap {
    evalGuard: Key
  }
}

export const inject = ['slots', 'locale', 'configForms', 'uiWorkspace']

export function apply(ctx: Context): void {
  ctx.effect(() => ctx.locale.register(NS, { zh, en }))
  configureEvalBridge({
    startSession: (workspaceId, prompt) => {
      ctx.uiWorkspace.startSession(workspaceId as Parameters<typeof ctx.uiWorkspace.startSession>[0], { prompt })
    },
    workspaces: () => {
      const service = (ctx as unknown as { get(key: string): unknown }).get('workspaces') as
        { list?: { getSnapshot(): { items?: readonly Record<string, unknown>[] } } } | undefined
      const items = service?.list?.getSnapshot().items ?? []
      return items.map((item, index) => ({
        id: typeof item['workspaceId'] === 'string' ? item['workspaceId'] : String(item['id'] ?? index),
        label: typeof item['title'] === 'string' && item['title'] !== ''
          ? item['title']
          : String(item['path'] ?? item['workspaceId'] ?? ('workspace ' + String(index + 1))),
      }))
    },
  })
  ctx.effect(() => () => { configureEvalBridge(undefined) })
  const t = ctx.locale.bind(NS)
  const form = ctx.configForms.get(SETTINGS_NS) as {
    getSnapshot(): unknown
    subscribe(listener: () => void): () => void
    mutate(ops: never, revision?: number): unknown
  }
  const formFace = {
    getSnapshot: () => form.getSnapshot(),
    subscribe: (listener: () => void) => form.subscribe(listener),
    mutate: (ops: never, revision?: number) => form.mutate(ops, revision),
  }
  ctx.slots.inject('settings.section', () => ctx.slots.register({
    name: 'settings.section', id: SETTINGS_NS, order: 41,
    label: () => t('settings.nav'), locale: NS,
    inject: () => ({ form: formFace }),
  }, EvalGuardSettings))
  ctx.slots.inject('shell.overlay', () => ctx.slots.register({
    name: 'shell.overlay', id: SETTINGS_NS + '.toast', locale: NS,
    inject: () => ({ t }),
  }, GuardToast))
}

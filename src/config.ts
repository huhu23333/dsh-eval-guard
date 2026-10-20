/**
 * Plugin configuration. The Settings page writes this document through the
 * shared config-forms service; the host resolves it on every decision.
 * @module dsh-eval-guard/config
 */
import z from '@deepseek-ai/schemastery'

/** Persisted plugin configuration. */
export interface Config {
  /** Master switch; off means the plugin must behave as if it were not installed. */
  enabled: boolean
  /** Whether a task turn has a wall-clock limit. */
  timeLimitEnabled: boolean
  /** Minutes allowed for one user-message-to-turn-end window. */
  timeLimitMinutes: number
  /** Extra forbidden absolute paths, one per line, gitignore-like '!' exceptions. */
  forbiddenPaths: string
  /** When true the generated default blacklist applies; editing flips this off. */
  toolBlacklistUseDefault: boolean
  /** The user's blacklist document, one tool name per line. */
  toolBlacklist: string
  /** Preferred clone source for host-side benchmark setup. */
  cloneSource: string
  /** Custom clone URL prefix when cloneSource is custom. */
  customCloneUrl: string
  /** Preferred pip index for host-side benchmark setup. */
  pipIndex: string
  /** Custom pip index URL when pipIndex is custom. */
  customPipUrl: string
  /** Optional proxy used only for git clone/fetch, e.g. 127.0.0.1:7897. */
  gitProxyUrl: string
  /** Optional proxy used only for pip installs, e.g. 127.0.0.1:7897. */
  pipProxyUrl: string
  /** Optional proxy used only for CIFAR-10 downloads, e.g. 127.0.0.1:7897. */
  cifarProxyUrl: string
  /** CIFAR-10 download mirror id, or custom. */
  cifarMirror: string
  /** Custom CIFAR-10 binary tar.gz URL. */
  customCifarUrl: string
}

/** Factory defaults. */
export const DEFAULT_CONFIG: Config = {
  enabled: false,
  timeLimitEnabled: true,
  timeLimitMinutes: 90,
  forbiddenPaths: '',
  toolBlacklistUseDefault: true,
  toolBlacklist: '',
  cloneSource: 'github',
  customCloneUrl: '',
  pipIndex: 'pypi',
  customPipUrl: '',
  gitProxyUrl: '',
  pipProxyUrl: '',
  cifarProxyUrl: '',
  cifarMirror: 'toronto',
  customCifarUrl: '',
}

/** The exported schema; the Loader row id is also this document's namespace. */
export const Config = z.object({
  enabled: z.boolean().default(false).volatile(),
  timeLimitEnabled: z.boolean().default(true).volatile(),
  timeLimitMinutes: z.number().default(90).volatile(),
  forbiddenPaths: z.string().default('').volatile(),
  toolBlacklistUseDefault: z.boolean().default(true).volatile(),
  toolBlacklist: z.string().default('').volatile(),
  cloneSource: z.string().default('github').volatile(),
  customCloneUrl: z.string().default('').volatile(),
  pipIndex: z.string().default('pypi').volatile(),
  customPipUrl: z.string().default('').volatile(),
  gitProxyUrl: z.string().default('').volatile(),
  pipProxyUrl: z.string().default('').volatile(),
  cifarProxyUrl: z.string().default('').volatile(),
  cifarMirror: z.string().default('toronto').volatile(),
  customCifarUrl: z.string().default('').volatile(),
})

/** Clamp and normalize a live configuration object. */
export function resolveConfig(value: Partial<Config> | undefined): Config {
  const minutes = Number.isFinite(value?.timeLimitMinutes) ? Number(value?.timeLimitMinutes) : 90
  return {
    enabled: value?.enabled === true,
    timeLimitEnabled: value?.timeLimitEnabled !== false,
    timeLimitMinutes: Math.min(1440, Math.max(1, Math.round(minutes))),
    forbiddenPaths: typeof value?.forbiddenPaths === 'string' ? value.forbiddenPaths : '',
    toolBlacklistUseDefault: value?.toolBlacklistUseDefault !== false,
    toolBlacklist: typeof value?.toolBlacklist === 'string' ? value.toolBlacklist : '',
    cloneSource: typeof value?.cloneSource === 'string' && value.cloneSource !== '' ? value.cloneSource : 'github',
    customCloneUrl: typeof value?.customCloneUrl === 'string' ? value.customCloneUrl : '',
    pipIndex: typeof value?.pipIndex === 'string' && value.pipIndex !== '' ? value.pipIndex : 'pypi',
    customPipUrl: typeof value?.customPipUrl === 'string' ? value.customPipUrl : '',
    gitProxyUrl: typeof value?.gitProxyUrl === 'string' ? value.gitProxyUrl : '',
    pipProxyUrl: typeof value?.pipProxyUrl === 'string' ? value.pipProxyUrl : '',
    cifarProxyUrl: typeof value?.cifarProxyUrl === 'string' ? value.cifarProxyUrl : '',
    cifarMirror: typeof value?.cifarMirror === 'string' && value.cifarMirror !== '' ? value.cifarMirror : 'toronto',
    customCifarUrl: typeof value?.customCifarUrl === 'string' ? value.customCifarUrl : '',
  }
}

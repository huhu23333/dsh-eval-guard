/**
 * Settings panel for the evaluation guard.
 * @module dsh-eval-guard/client/Settings
 */
import { useEffect, useState, useSyncExternalStore, type CSSProperties, type ReactElement } from 'react'
import {
  CHECK_ROUTE, EVENTS_ROUTE, INSTALL_ROUTE, MANUAL_ROUTE, REVEAL_ROUTE, STATE_ROUTE,
  type CheckResult, type GuardEvent, type GuardState, type InstallResult, type ManualPaths,
} from '../shared.ts'
import { evalBridge } from './bridge.ts'

type Snapshot = {
  status: 'loading' | 'ready' | 'unavailable'
  value?: Record<string, unknown>
  revision?: number
  writable: boolean
}
type Form = {
  getSnapshot: () => Snapshot
  subscribe: (listener: () => void) => () => void
  mutate: (ops: readonly { op: 'set'; path: string[]; value: unknown }[], revision?: number) => Promise<boolean>
}
type Props = { form: Form; t: (key: string) => string }

interface Draft {
  enabled: boolean
  timeLimitEnabled: boolean
  timeLimitMinutes: number
  forbiddenPaths: string
  toolBlacklistUseDefault: boolean
  toolBlacklist: string
}

const EMPTY: Draft = {
  enabled: false,
  timeLimitEnabled: true,
  timeLimitMinutes: 90,
  forbiddenPaths: '',
  toolBlacklistUseDefault: true,
  toolBlacklist: '',
}

const box: CSSProperties = { border: '1px solid var(--vscode-panel-border, #3c3c3c)', borderRadius: 8, padding: 14, marginBottom: 14 }
const row: CSSProperties = { display: 'flex', gap: 10, alignItems: 'center', flexWrap: 'wrap', marginBottom: 8 }
const note: CSSProperties = { opacity: 0.72, fontSize: 12, lineHeight: 1.5, margin: '6px 0' }
const textarea: CSSProperties = { width: '100%', minHeight: 110, fontFamily: 'monospace', fontSize: 12, padding: 8, boxSizing: 'border-box' }
const button: CSSProperties = { padding: '5px 12px', borderRadius: 6, cursor: 'pointer' }
const primary: CSSProperties = { ...button, background: '#2f6fd0', color: '#fff', border: 'none' }
const danger: CSSProperties = { ...button, background: '#a33', color: '#fff', border: 'none' }

async function readJson<T>(response: Response): Promise<T> { return await response.json() as T }

function normalize(value: Record<string, unknown> | undefined): Draft {
  return {
    enabled: value?.enabled === true,
    timeLimitEnabled: value?.timeLimitEnabled !== false,
    timeLimitMinutes: typeof value?.timeLimitMinutes === 'number' ? value.timeLimitMinutes : 90,
    forbiddenPaths: typeof value?.forbiddenPaths === 'string' ? value.forbiddenPaths : '',
    toolBlacklistUseDefault: value?.toolBlacklistUseDefault !== false,
    toolBlacklist: typeof value?.toolBlacklist === 'string' ? value.toolBlacklist : '',
  }
}

/** The settings card. */
export function EvalGuardSettings({ form, t }: Props): ReactElement {
  const snapshot = useSyncExternalStore(form.subscribe, form.getSnapshot, form.getSnapshot)
  const [draft, setDraft] = useState<Draft>(EMPTY)
  const [dirty, setDirty] = useState(false)
  const [state, setState] = useState<GuardState | null>(null)
  const [events, setEvents] = useState<GuardEvent[]>([])
  const [pendingEnable, setPendingEnable] = useState(false)
  const [notice, setNotice] = useState('')
  const [error, setError] = useState('')
  const [workspaceId, setWorkspaceId] = useState('')
  const [taskId, setTaskId] = useState('')
  const [check, setCheck] = useState<CheckResult | null>(null)
  const [manual, setManual] = useState<ManualPaths | null>(null)
  const [prompt, setPrompt] = useState('')
  const [copied, setCopied] = useState('')
  const workspaces = evalBridge()?.workspaces() ?? []

  useEffect(() => {
    if (snapshot.status !== 'ready' || snapshot.value === undefined || dirty) return
    setDraft(normalize(snapshot.value))
  }, [snapshot, dirty])

  const loadState = (): void => {
    fetch(STATE_ROUTE).then(readJson<GuardState>).then(setState).catch(() => setState(null))
  }
  useEffect(() => {
    loadState()
    let since = 0
    const timer = setInterval(() => {
      fetch(EVENTS_ROUTE + '?since=' + String(since)).then(readJson<{ events: GuardEvent[] }>).then(data => {
        if (data.events.length === 0) return
        since = data.events[data.events.length - 1]!.seq
        setEvents(current => [...data.events, ...current].slice(0, 12))
      }).catch(() => undefined)
    }, 2000)
    return () => clearInterval(timer)
  }, [])

  useEffect(() => {
    if (taskId === '' && state !== null && state.tasks.length > 0) setTaskId(state.tasks[0]!.id)
  }, [state, taskId])

  const ready = snapshot.status === 'ready' && snapshot.writable
  const save = async (patch: Partial<Draft>): Promise<void> => {
    if (!ready || snapshot.revision === undefined) return
    const next = { ...draft, ...patch }
    const ops: { op: 'set'; path: string[]; value: unknown }[] = []
    for (const key of Object.keys(patch) as (keyof Draft)[]) ops.push({ op: 'set', path: [key], value: next[key] })
    try {
      const accepted = await form.mutate(ops, snapshot.revision)
      if (!accepted) { setError('保存冲突，请刷新后重试。'); return }
      setDraft(next)
      setDirty(false)
      setError('')
      loadState()
    } catch (reason: unknown) {
      setError(reason instanceof Error ? reason.message : String(reason))
    }
  }
  const edit = (patch: Partial<Draft>): void => {
    setDraft(current => ({ ...current, ...patch }))
    setDirty(true)
  }

  const toggleEnabled = (next: boolean): void => {
    if (!next) { void save({ enabled: false }); return }
    fetch(STATE_ROUTE).then(readJson<GuardState>).then(latest => {
      setState(latest)
      if (latest.running.length > 0) setPendingEnable(true)
      else void save({ enabled: true })
    }).catch(() => void save({ enabled: true }))
  }

  const forceEnable = (): void => {
    setPendingEnable(false)
    void save({ enabled: true })
  }

  const loadManual = (id: string, task: string): void => {
    if (id === '') { setCheck(null); setManual(null); setPrompt(''); return }
    fetch(CHECK_ROUTE + '?workspaceId=' + encodeURIComponent(id)).then(readJson<CheckResult>).then(result => {
      setCheck(result)
      if (result.ok) {
        fetch(MANUAL_ROUTE + '?workspaceId=' + encodeURIComponent(id) + '&task=' + encodeURIComponent(task)).then(readJson<ManualPaths>).then(paths => {
          setManual(paths)
          setPrompt(paths.prompt)
        }).catch(() => undefined)
      }
    }).catch((reason: unknown) => setError(reason instanceof Error ? reason.message : String(reason)))
  }

  const install = async (): Promise<void> => {
    if (workspaceId === '' || taskId === '') return
    setError(''); setNotice('')
    const checked = await fetch(CHECK_ROUTE + '?workspaceId=' + encodeURIComponent(workspaceId)).then(readJson<CheckResult>)
    setCheck(checked)
    if (!checked.ok || !checked.empty) {
      setError(checked.error ?? t('settings.nonEmpty'))
      return
    }
    const result = await fetch(INSTALL_ROUTE, {
      method: 'POST', headers: { 'content-type': 'application/json' },
      body: JSON.stringify({ workspaceId, task: taskId }),
    }).then(readJson<InstallResult>)
    if (!result.ok || result.prompt === undefined) { setError(result.error ?? '安装失败'); return }
    setPrompt(result.prompt)
    setNotice(t('settings.installOk'))
    evalBridge()?.startSession(workspaceId, result.prompt)
    loadState()
  }

  const copy = async (label: string, text: string): Promise<void> => {
    try {
      await navigator.clipboard.writeText(text)
      setCopied(label)
      setTimeout(() => setCopied(''), 1500)
    } catch { setError('复制失败') }
  }
  const reveal = (target: string): void => {
    void fetch(REVEAL_ROUTE, {
      method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify({ path: target }),
    })
  }
  const blacklistText = draft.toolBlacklistUseDefault ? state?.defaultBlacklist ?? '' : draft.toolBlacklist

  if (snapshot.status === 'loading') return <div style={box}>{t('settings.loading')}</div>
  return <div>
    {pendingEnable && <div style={{ ...box, borderColor: '#caa23a' }}>
      <strong>{t('settings.warningTitle')}</strong>
      <p style={note}>{t('settings.runningWarning')}</p>
      <ul style={{ ...note, paddingLeft: 18 }}>{(state?.running ?? []).map(item =>
        <li key={item.sessionId}>{item.title} · {item.cwd}{item.jobs > 0 ? ' · jobs: ' + String(item.jobs) : ''}</li>)}</ul>
      <div style={row}>
        <button style={button} onClick={() => { setPendingEnable(false); loadState() }}>{t('settings.runningCancel')}</button>
        <button style={danger} onClick={forceEnable}>{t('settings.runningForce')}</button>
      </div>
    </div>}

    <section style={box}>
      <div style={row}>
        <label><input type="checkbox" checked={draft.enabled} disabled={!ready} onChange={event => toggleEnabled(event.target.checked)} /> {t('settings.enabled')}</label>
      </div>
      <p style={note}>{t('settings.enabledHint')}</p>
    </section>

    <section style={box}>
      <div style={row}>
        <label><input type="checkbox" checked={draft.timeLimitEnabled} disabled={!ready} onChange={event => void save({ timeLimitEnabled: event.target.checked })} /> {t('settings.timeLimit')}</label>
        <input type="number" min={1} max={1440} value={draft.timeLimitMinutes} disabled={!ready}
          style={{ width: 90 }} onChange={event => edit({ timeLimitMinutes: Number(event.target.value) })}
          onBlur={() => void save({ timeLimitMinutes: draft.timeLimitMinutes })} />
        <span style={note}>{t('settings.minutes')}</span>
      </div>
      <p style={note}>{t('settings.timeLimitHint')}</p>
    </section>

    <section style={box}>
      <strong>{t('settings.forbidden')}</strong>
      <p style={note}>{t('settings.forbiddenHint')}</p>
      <textarea style={textarea} value={draft.forbiddenPaths} disabled={!ready}
        onChange={event => edit({ forbiddenPaths: event.target.value })}
        onBlur={() => void save({ forbiddenPaths: draft.forbiddenPaths })} />
    </section>

    <section style={box}>
      <div style={row}>
        <strong>{t('settings.blacklist')}</strong>
        <button style={button} disabled={!ready} onClick={() => void save({ toolBlacklistUseDefault: true, toolBlacklist: '' })}>{t('settings.reset')}</button>
      </div>
      <p style={note}>{t('settings.blacklistHint')}</p>
      <textarea style={textarea} value={blacklistText} disabled={!ready}
        onChange={event => edit({ toolBlacklistUseDefault: false, toolBlacklist: event.target.value })}
        onBlur={() => void save({ toolBlacklistUseDefault: draft.toolBlacklistUseDefault, toolBlacklist: draft.toolBlacklist })} />
      {draft.toolBlacklistUseDefault && <p style={note}>{t('settings.editingDefault')}</p>}
    </section>

    <section style={box}>
      <strong>{t('settings.task')}</strong>
      <p style={note}>{t('settings.taskHint')}</p>
      <div style={row}>
        <span>{t('settings.taskLabel')}:</span>
        <select value={taskId} disabled={state === null || state.tasks.length === 0}
          onChange={event => {
            const next = event.target.value
            setTaskId(next); setCheck(null); setManual(null); setPrompt('')
            if (workspaceId !== '') loadManual(workspaceId, next)
          }}>
          {(state?.tasks ?? []).map(task => <option key={task.id} value={task.id}>{task.label}</option>)}
        </select>
      </div>
      {state !== null && <p style={note}>{state.tasks.find(task => task.id === taskId)?.description ?? ''}</p>}
      <div style={row}>
        <span>{t('settings.workspace')}:</span>
        <select value={workspaceId} onChange={event => { setWorkspaceId(event.target.value); loadManual(event.target.value, taskId) }}>
          <option value="">{t('settings.pickWorkspace')}</option>
          {workspaces.map(workspace => <option key={workspace.id} value={workspace.id}>{workspace.label}</option>)}
        </select>
        <button style={primary} disabled={workspaceId === '' || taskId === ''} onClick={() => void install()}>{t('settings.check')}</button>
      </div>
      {workspaces.length === 0 && <p style={note}>{t('settings.noWorkspaces')}</p>}
      {check !== null && !check.empty && <p style={{ ...note, color: '#d99' }}>{t('settings.nonEmpty')} ({check.entries.slice(0, 6).join(', ')})</p>}
      {notice !== '' && <p style={{ ...note, color: '#7c7' }}>{notice}</p>}
      {error !== '' && <p style={{ ...note, color: '#d77' }}>{error}</p>}
    </section>

    <section style={box}>
      <strong>{t('settings.manual')}</strong>
      <p style={note}>{t('settings.manualHint')}</p>
      <div style={row}><span>{t('settings.source')}:</span><code style={{ wordBreak: 'break-all' }}>{manual?.source ?? '—'}</code>
        {manual !== null && <button style={button} onClick={() => reveal(manual.source)}>{t('settings.reveal')}</button>}</div>
      <div style={row}><span>{t('settings.target')}:</span><code style={{ wordBreak: 'break-all' }}>{check?.path ?? '—'}</code>
        {check?.path !== undefined && check.path !== '' && <button style={button} onClick={() => reveal(check.path)}>{t('settings.reveal')}</button>}</div>
      <div style={row}><button style={button} disabled={prompt === ''} onClick={() => void copy('prompt', prompt)}>{copied === 'prompt' ? t('settings.copied') : t('settings.copy')}</button></div>
      <textarea style={{ ...textarea, minHeight: 160 }} readOnly value={prompt} placeholder={t('settings.prompt')} />
    </section>

    <section style={box}>
      <strong>{t('settings.recent')}</strong>
      {events.length === 0 && <p style={note}>{t('settings.none')}</p>}
      {events.map(event => <div key={event.seq} style={{ ...note, borderTop: '1px solid #333', paddingTop: 4 }}>
        <strong>[{event.kind}]</strong> {event.tool !== undefined ? event.tool + ' · ' : ''}{event.detail}
      </div>)}
    </section>
  </div>
}

/**
 * Settings panel for the evaluation guard.
 * @module dsh-eval-guard/client/Settings
 */
import { useEffect, useRef, useState, useSyncExternalStore, type CSSProperties, type ReactElement } from 'react'
import {
  CHECK_ROUTE, EVENTS_ROUTE, INSTALL_ROUTE, MANUAL_ROUTE, REVEAL_ROUTE, STATE_ROUTE,
  JOBS_ROUTE, JOB_ROUTE, JOB_CANCEL_ROUTE, EVAL_ROUTE,
  type CheckResult, type GuardEvent, type GuardState, type InstallResult, type ManualPaths, type JobDetail, type JobInfo,
  PIP_MIRRORS, CIFAR_MIRRORS,
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
  jobReminderMinutes: number
  forbiddenPaths: string
  toolBlacklistUseDefault: boolean
  toolBlacklist: string
  cloneSource: string
  customCloneUrl: string
  pipIndex: string
  customPipUrl: string
  gitProxyUrl: string
  pipProxyUrl: string
  cifarProxyUrl: string
  cifarMirror: string
  customCifarUrl: string
  parallelEval: boolean
  taskId: string
}

const EMPTY: Draft = {
  enabled: false,
  timeLimitEnabled: true,
  timeLimitMinutes: 90,
  jobReminderMinutes: 5,
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
  parallelEval: false,
  taskId: '',
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
    jobReminderMinutes: typeof value?.jobReminderMinutes === 'number' ? value.jobReminderMinutes : 5,
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
    parallelEval: value?.parallelEval === true,
    taskId: typeof value?.taskId === 'string' ? value.taskId : '',
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
  const [workspaceId, setWorkspaceId] = useState(() => typeof localStorage === 'undefined' ? '' : localStorage.getItem('evalGuard.workspaceId') ?? '')
  const [taskId, setTaskId] = useState(() => typeof localStorage === 'undefined' ? '' : localStorage.getItem('evalGuard.taskId') ?? '')
  const [check, setCheck] = useState<CheckResult | null>(null)
  const [manual, setManual] = useState<ManualPaths | null>(null)
  const [prompt, setPrompt] = useState('')
  const [copied, setCopied] = useState('')
  const workspaces = evalBridge()?.workspaces() ?? []
  const [job, setJob] = useState<JobDetail | null>(null)
  const [jobNotice, setJobNotice] = useState('')
  const autoOpenJob = useRef('')

  const pollJob = (id: string): void => {
    fetch(JOB_ROUTE + '?id=' + encodeURIComponent(id)).then(readJson<JobDetail>).then(setJob).catch(() => undefined)
  }
  const loadLatestJob = (workspace: string, task: string): void => {
    if (workspace === '' || task === '') { setJob(null); return }
    fetch(JOBS_ROUTE + '?workspaceId=' + encodeURIComponent(workspace) + '&task=' + encodeURIComponent(task))
      .then(readJson<JobInfo[]>).then(items => {
        const latest = items[0]
        if (latest !== undefined) pollJob(latest.id)
        else setJob(null)
      }).catch(() => undefined)
  }
  useEffect(() => {
    if (job === null || (job.status !== 'running' && job.status !== 'queued')) return
    const timer = setInterval(() => { pollJob(job.id) }, 800)
    return () => clearInterval(timer)
  }, [job])
  useEffect(() => { if (job?.status === 'succeeded' && job.kind === 'install' && job.prompt !== undefined) {
    setPrompt(job.prompt); setJobNotice(t('settings.installOk')); loadState()
    if (autoOpenJob.current === job.id) {
      autoOpenJob.current = ''
      evalBridge()?.startSession(job.workspaceId, job.prompt)
    }
  } }, [job])
  useEffect(() => { loadLatestJob(workspaceId, taskId) }, [workspaceId, taskId])

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
    if (state !== null && state.tasks.length > 0 && (taskId === '' || !state.tasks.some(task => task.id === taskId))) {
      if (draft.taskId !== '' && state.tasks.some(task => task.id === draft.taskId)) setTaskId(draft.taskId)
      else setTaskId(state.tasks[0]!.id)
    }
  }, [state, taskId, draft.taskId])

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

  const selectTask = (next: string): void => {
    setTaskId(next)
    try { localStorage.setItem('evalGuard.taskId', next) } catch { /* storage is optional */ }
    setCheck(null); setManual(null); setPrompt(''); setJob(null); setJobNotice('')
    if (workspaceId !== '') loadManual(workspaceId, next)
    const task = state?.tasks.find(item => item.id === next)
    if (task === undefined) {
      void save({ taskId: next })
      return
    }
    const remotes = task.remotes ?? []
    const cloneSource = draft.cloneSource === 'custom' || remotes.some(remote => remote.id === draft.cloneSource)
      ? draft.cloneSource : (remotes[0]?.id ?? 'github')
    const patch: Partial<Draft> = {
      taskId: next,
      timeLimitMinutes: task.timeLimitMinutes,
      timeLimitEnabled: task.timeLimitEnabled,
      cloneSource,
    }
    // All task defaults travel in one atomic mutation. Two concurrent mutate()
    // calls with the same revision lose the second half to the CAS guard.
    setDraft(current => ({ ...current, ...patch }))
    setNotice(t('settings.taskDefaults').replace('{minutes}', String(task.timeLimitMinutes)).replace('{state}', task.timeLimitEnabled ? t('settings.on') : t('settings.off')))
    void save(patch)
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
    await save({ taskId })
    setError(''); setNotice(''); setJobNotice('')
    const checked = await fetch(CHECK_ROUTE + '?workspaceId=' + encodeURIComponent(workspaceId)).then(readJson<CheckResult>)
    setCheck(checked)
    if (!checked.ok || !checked.empty) { setError(checked.error ?? t('settings.nonEmpty')); return }
    const result = await fetch(INSTALL_ROUTE, {
      method: 'POST', headers: { 'content-type': 'application/json' },
      body: JSON.stringify({ workspaceId, task: taskId, cloneSource: draft.cloneSource, customCloneUrl: draft.customCloneUrl,
        pipIndex: draft.pipIndex, customPipUrl: draft.customPipUrl, gitProxyUrl: draft.gitProxyUrl, pipProxyUrl: draft.pipProxyUrl, cifarProxyUrl: draft.cifarProxyUrl, cifarMirror: draft.cifarMirror, customCifarUrl: draft.customCifarUrl }),
    }).then(readJson<{ ok: boolean; jobId?: string; error?: string }>)
    if (!result.ok || result.jobId === undefined) { autoOpenJob.current = ''; setError(result.error ?? '安装失败'); return }
    autoOpenJob.current = result.jobId
    pollJob(result.jobId)
  }

  const evaluate = async (): Promise<void> => {
    if (workspaceId === '' || taskId === '') return
    await save({ taskId })
    setError(''); setNotice(''); setJobNotice('')
    const result = await fetch(EVAL_ROUTE, {
      method: 'POST', headers: { 'content-type': 'application/json' },
      body: JSON.stringify({ workspaceId, task: taskId }),
    }).then(readJson<{ ok: boolean; jobId?: string; error?: string }>)
    if (!result.ok || result.jobId === undefined) { setError(result.error ?? '评测启动失败'); return }
    pollJob(result.jobId)
  }

  const cancelJob = async (): Promise<void> => {
    if (job === null) return
    await fetch(JOB_CANCEL_ROUTE, { method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify({ id: job.id }) })
    pollJob(job.id)
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
      {state?.error !== undefined && <p style={{ ...note, color: '#d99' }}>{t('settings.enableError')}: {state.error}</p>}
      {draft.enabled && state !== null && !state.enabled && <p style={{ ...note, color: '#d99' }}>{t('settings.enableInactive')}</p>}
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
          onChange={event => selectTask(event.target.value)}>
          {(state?.tasks ?? []).map(task => <option key={task.id} value={task.id}>{task.label}</option>)}
        </select>
      </div>
      {state !== null && <p style={note}>{state.tasks.find(task => task.id === taskId)?.description ?? ''}</p>}
      {(() => {
        const selected = state?.tasks.find(item => item.id === taskId)
        if (selected?.needsSetup !== true) return null
        return <div>
          <div style={row}>
            <span>{t('settings.cloneSource')}:</span>
            <select value={draft.cloneSource} disabled={!ready} onChange={event => void save({ cloneSource: event.target.value })}>
              {(selected.remotes ?? []).map(remote => <option key={remote.id} value={remote.id}>{remote.label}</option>)}
              <option value="custom">{t('settings.custom')}</option>
            </select>
            {draft.cloneSource === 'custom' && <input style={{ width: 260 }} disabled={!ready} value={draft.customCloneUrl}
              onChange={event => edit({ customCloneUrl: event.target.value })} onBlur={() => void save({ customCloneUrl: draft.customCloneUrl })} placeholder="https://git.example.com" />}
          </div>
          <div style={row}>
            <span>{t('settings.pipIndex')}:</span>
            <select value={draft.pipIndex} disabled={!ready} onChange={event => void save({ pipIndex: event.target.value })}>
              {PIP_MIRRORS.map(mirror => <option key={mirror.id} value={mirror.id}>{mirror.label}</option>)}
              <option value="custom">{t('settings.custom')}</option>
            </select>
            {draft.pipIndex === 'custom' && <input style={{ width: 260 }} disabled={!ready} value={draft.customPipUrl}
              onChange={event => edit({ customPipUrl: event.target.value })} onBlur={() => void save({ customPipUrl: draft.customPipUrl })} placeholder="https://pypi.example.com/simple" />}
          </div>
          <div style={row}>
            <span>{t('settings.gitProxy')}:</span>
            <input style={{ width: 260 }} disabled={!ready} value={draft.gitProxyUrl}
              onChange={event => edit({ gitProxyUrl: event.target.value })} onBlur={() => void save({ gitProxyUrl: draft.gitProxyUrl })} placeholder="127.0.0.1:7897" />
          </div>
          <div style={row}>
            <span>{t('settings.pipProxy')}:</span>
            <input style={{ width: 260 }} disabled={!ready} value={draft.pipProxyUrl}
              onChange={event => edit({ pipProxyUrl: event.target.value })} onBlur={() => void save({ pipProxyUrl: draft.pipProxyUrl })} placeholder="127.0.0.1:7897" />
          </div>
          <div style={row}>
            <span>{t('settings.cifarMirror')}:</span>
            <select value={draft.cifarMirror} disabled={!ready} onChange={event => void save({ cifarMirror: event.target.value })}>
              {CIFAR_MIRRORS.map(mirror => <option key={mirror.id} value={mirror.id}>{mirror.label}</option>)}
              <option value="custom">{t('settings.custom')}</option>
            </select>
            {draft.cifarMirror === 'custom' && <input style={{ width: 320 }} disabled={!ready} value={draft.customCifarUrl}
              onChange={event => edit({ customCifarUrl: event.target.value })} onBlur={() => void save({ customCifarUrl: draft.customCifarUrl })} placeholder="https://.../cifar-10-binary.tar.gz" />}
          </div>
          <div style={row}>
            <span>{t('settings.cifarProxy')}:</span>
            <input style={{ width: 260 }} disabled={!ready} value={draft.cifarProxyUrl}
              onChange={event => edit({ cifarProxyUrl: event.target.value })} onBlur={() => void save({ cifarProxyUrl: draft.cifarProxyUrl })} placeholder="127.0.0.1:7897" />
          </div>
        </div>
      })()}
      <div style={row}>
        <span>{t('settings.workspace')}:</span>
        <select value={workspaceId} onChange={event => { const next = event.target.value; setWorkspaceId(next); try { localStorage.setItem('evalGuard.workspaceId', next) } catch { /* storage is optional */ } loadManual(next, taskId); loadLatestJob(next, taskId) }}>
          <option value="">{t('settings.pickWorkspace')}</option>
          {workspaces.map(workspace => <option key={workspace.id} value={workspace.id}>{workspace.label}</option>)}
        </select>
        <button style={primary} disabled={workspaceId === '' || taskId === '' || job?.status === 'running'} onClick={() => void install()}>{t('settings.check')}</button>
        <button style={button} disabled={workspaceId === '' || taskId === '' || job?.status === 'running'} onClick={() => void evaluate()}>{t('settings.evaluate')}</button>
      </div>
      <p style={note}>{t('settings.evalHint')}</p>
      <div style={row}>
        <label><input type="checkbox" checked={draft.parallelEval} disabled={!ready} onChange={event => void save({ parallelEval: event.target.checked })} /> {t('settings.parallelEval')}</label>
      </div>
      <p style={note}>{t('settings.parallelEvalHint')}</p>
      {workspaces.length === 0 && <p style={note}>{t('settings.noWorkspaces')}</p>}
      {check !== null && !check.empty && <p style={{ ...note, color: '#d99' }}>{t('settings.nonEmpty')} ({check.entries.slice(0, 6).join(', ')})</p>}
      {notice !== '' && <p style={{ ...note, color: '#7c7' }}>{notice}</p>}
      {error !== '' && <p style={{ ...note, color: '#d77' }}>{error}</p>}
      {jobNotice !== '' && <p style={{ ...note, color: '#7c7' }}>{jobNotice}</p>}
      {job !== null && <div style={{ ...box, borderColor: job.status === 'failed' ? '#a55' : job.status === 'succeeded' ? '#4a7' : '#3a6' }}>
        <div style={row}><strong>{job.kind === 'install' ? t('settings.installJob') : t('settings.evalJob')}</strong><span>{job.status}</span>
          {job.status === 'running' && <button style={danger} onClick={() => void cancelJob()}>{t('settings.cancel')}</button>}
          {job.kind === 'install' && job.status === 'succeeded' && job.prompt !== undefined && workspaceId !== '' && <button style={primary} onClick={() => evalBridge()?.startSession(workspaceId, job.prompt!)}>{t('settings.startSession')}</button>}</div>
        {job.error !== undefined && <p style={{ ...note, color: '#d77' }}>{job.error}</p>}
        <pre style={{ ...textarea, maxHeight: 260, overflow: 'auto' }}>{job.log || (job.status === 'running' || job.status === 'queued' ? t('settings.waiting') : '')}</pre>
        {job.result !== undefined && <pre style={{ ...textarea, maxHeight: 260, overflow: 'auto' }}>{JSON.stringify(job.result, undefined, 2)}</pre>}
      </div>}
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
      <div style={row}>
        <label>{t('settings.jobReminder')}</label>
        <input type="number" min={0} max={60} value={draft.jobReminderMinutes} disabled={!ready}
          style={{ width: 90 }} onChange={event => edit({ jobReminderMinutes: Number(event.target.value) })}
          onBlur={() => void save({ jobReminderMinutes: draft.jobReminderMinutes })} />
        <span style={note}>{t('settings.minutes')}</span>
      </div>
      <p style={note}>{t('settings.jobReminderHint')}</p>
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

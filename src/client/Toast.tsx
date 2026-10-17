/**
 * Frame-wide warning toasts fed by the host event route.
 * @module dsh-eval-guard/client/Toast
 */
import { useEffect, useState, type ReactElement } from 'react'
import { EVENTS_ROUTE, type GuardEvent } from '../shared.ts'

/** One toast with a dismiss deadline. */
interface Shown { event: GuardEvent; until: number }

/** Poll the host event feed and show new warnings. */
export function GuardToast({ t }: { t: (key: string) => string }): ReactElement | null {
  const [shown, setShown] = useState<Shown[]>([])
  useEffect(() => {
    let since = 0
    let alive = true
    const poll = (): void => {
      fetch(EVENTS_ROUTE + '?since=' + String(since)).then(async response => await response.json() as { events: GuardEvent[] }).then(data => {
        if (!alive || data.events.length === 0) return
        since = data.events[data.events.length - 1]!.seq
        const warnings = data.events.filter(event => event.kind !== 'notice')
        if (warnings.length === 0) return
        const now = Date.now()
        setShown(current => [...warnings.map(event => ({ event, until: now + 8000 })), ...current].slice(0, 5))
      }).catch(() => undefined)
    }
    const timer = setInterval(() => {
      poll()
      const now = Date.now()
      setShown(current => current.filter(item => item.until > now))
    }, 1500)
    poll()
    return () => { alive = false; clearInterval(timer) }
  }, [])
  if (shown.length === 0) return null
  return <div style={{ position: 'fixed', right: 18, bottom: 18, zIndex: 9999, display: 'flex', flexDirection: 'column', gap: 8, maxWidth: 420 }}>
    {shown.map(item => <div key={item.event.seq} style={{
      background: '#3a2a12', color: '#ffdca8', border: '1px solid #8a6a2a', borderRadius: 8,
      padding: '10px 12px', fontSize: 12, lineHeight: 1.5, boxShadow: '0 6px 24px rgba(0,0,0,.45)',
    }}>
      <strong>{t('settings.warningTitle')}</strong>
      <div>{item.event.detail}</div>
    </div>)}
  </div>
}

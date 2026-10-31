import { useState } from 'react'
import type { VspClient } from '../lib/api'
import type { LiveStats, LiveStream, Playable, SystemInfo } from '../lib/types'
import { fmtBitrate, fmtBytes, fmtDuration, fmtRelative } from '../lib/format'

interface Props {
  client: VspClient
  system: SystemInfo | null
  streams: LiveStream[]
  stats: Map<string, LiveStats>
  onPlay: (p: Playable) => void
  onRefresh: () => void
}

export function liveToPlayable(s: LiveStream): Playable {
  return {
    id: s.current_video_id || s.id,
    title: s.name,
    playback_url: s.playback_url,
    live: true,
    subtitle: `${s.width}×${s.height} · ${s.fps.toFixed(2)} fps · ${s.video_codec}${s.audio_codec ? ' · ' + s.audio_codec : ''}`,
    renditions: s.live_renditions
  }
}

export default function Live({ client, system, streams, stats, onPlay, onRefresh }: Props) {
  const [name, setName] = useState('')
  const [record, setRecord] = useState(true)
  const [busy, setBusy] = useState('')
  const [revealed, setRevealed] = useState<Set<string>>(new Set())
  const [copied, setCopied] = useState('')
  const enabled = system?.live?.enabled !== false

  const create = async () => {
    setBusy('create')
    try {
      await client.createLive({ name: name.trim() || 'Live stream', record })
      setName('')
    } catch (e) {
      alert((e as Error).message)
    } finally {
      setBusy('')
    }
  }
  const act = async (id: string, fn: () => Promise<unknown>) => {
    setBusy(id)
    try {
      await fn()
    } catch (e) {
      alert((e as Error).message)
    } finally {
      setBusy('')
      onRefresh()
    }
  }
  const copy = async (text: string, tag: string) => {
    await navigator.clipboard.writeText(text)
    setCopied(tag)
    setTimeout(() => setCopied(''), 1200)
  }
  const toggleReveal = (id: string) =>
    setRevealed((s) => {
      const n = new Set(s)
      n.has(id) ? n.delete(id) : n.add(id)
      return n
    })

  return (
    <section>
      <header className="page-header">
        <h1>Live</h1>
        <button className="btn" onClick={onRefresh}>Refresh</button>
      </header>

      {!enabled && <div className="banner error">RTMP ingest is disabled on this server (start it without <code>--no-rtmp</code>).</div>}

      <div className="panel">
        <h2>New stream</h2>
        <div className="row gap wrap">
          <input placeholder="Stream name" value={name} onChange={(e) => setName(e.target.value)} style={{ flex: 1, minWidth: 220 }} />
          <label className="field inline check" style={{ margin: 0 }}>
            <input type="checkbox" checked={record} onChange={(e) => setRecord(e.target.checked)} />
            <span>Record to library</span>
          </label>
          <button className="btn primary" onClick={create} disabled={busy === 'create' || !enabled}>Create stream key</button>
        </div>
        {system?.live && (
          <p className="muted small" style={{ marginTop: 10 }}>
            Ingest: RTMP on port {system.live.rtmp_port}, app <code>{system.live.rtmp_app}</code> · LL-HLS {system.live.segment_ms} ms segments /
            {system.live.part_ms} ms parts · {system.live.passthrough ? 'source passthrough + ' : ''}
            {system.live.default_renditions.join(', ')} via {system.video_encoder.element}.
            Set your encoder's <b>keyframe interval to {system.live.segment_ms / 1000} s</b> and use <b>zerolatency / no B-frames</b> for the lowest latency.
          </p>
        )}
      </div>

      {streams.length === 0 && <div className="empty"><p>No live streams yet — create a stream key above and point OBS at it.</p></div>}

      <div className="live-list">
        {streams.map((s) => {
          const st = stats.get(s.id) ?? s.stats ?? undefined
          const isLive = s.status === 'live'
          const canPlay = isLive && !!s.playback_url && s.live_renditions.length > 0
          const keyShown = revealed.has(s.id)
          return (
            <article key={s.id} className={`panel live-card ${isLive ? 'is-live' : ''}`}>
              <div className="row between">
                <div className="row gap">
                  <h3 style={{ margin: 0 }}>{s.name}</h3>
                  {isLive ? <span className="live-tag inline">LIVE</span> : <span className="badge">idle</span>}
                  {s.record && <span className="badge">records</span>}
                </div>
                <div className="row gap">
                  {canPlay && <button className="btn small primary" onClick={() => onPlay(liveToPlayable(s))}>Watch</button>}
                  {isLive && <button className="btn small danger" disabled={busy === s.id} onClick={() => act(s.id, () => client.stopLive(s.id))}>Stop</button>}
                  {!isLive && <button className="btn small" disabled={busy === s.id} onClick={() => act(s.id, () => client.rotateLiveKey(s.id))}>Rotate key</button>}
                  {!isLive && <button className="btn small danger" disabled={busy === s.id}
                    onClick={() => confirm(`Delete "${s.name}"? Recordings are kept in the library.`) && act(s.id, () => client.deleteLive(s.id))}>Delete</button>}
                </div>
              </div>

              <div className="ingest">
                <div className="field inline"><span>Server</span>
                  <code className="mono">{s.ingest_server}</code>
                  <button className="link small" onClick={() => copy(s.ingest_server ?? '', s.id + 'srv')}>{copied === s.id + 'srv' ? 'copied' : 'copy'}</button>
                </div>
                <div className="field inline"><span>Stream key</span>
                  <code className="mono">{keyShown ? s.stream_key : '•'.repeat(24)}</code>
                  <button className="link small" onClick={() => toggleReveal(s.id)}>{keyShown ? 'hide' : 'show'}</button>
                  <button className="link small" onClick={() => copy(s.stream_key, s.id + 'key')}>{copied === s.id + 'key' ? 'copied' : 'copy'}</button>
                </div>
                <div className="field inline"><span>Full URL</span>
                  <code className="mono small">{keyShown ? s.ingest_url : (s.ingest_server ?? '') + '/' + '•'.repeat(12)}</code>
                  <button className="link small" onClick={() => copy(s.ingest_url ?? '', s.id + 'url')}>{copied === s.id + 'url' ? 'copied' : 'copy'}</button>
                </div>
              </div>

              {isLive && (
                <div className="live-stats mono small">
                  <span>{s.width}×{s.height} @ {s.fps.toFixed(2)} fps · {s.video_codec}{s.audio_codec ? ` · ${s.audio_codec}` : ''}</span>
                  <span>in: {st ? fmtBitrate((st.bytes_in * 8) / Math.max(1, st.uptime_s)) : '—'}</span>
                  <span>encode: {st ? `${st.encode_fps.toFixed(1)} fps` : '—'}</span>
                  <span>backlog: {st ? fmtBytes(st.queued_bytes) : '—'}</span>
                  <span>uptime: {st ? fmtDuration(st.uptime_s * 1000) : '—'}</span>
                  <span>from {s.publisher}</span>
                </div>
              )}
              {isLive && s.live_renditions.length > 0 && (
                <div className="muted small">
                  renditions: {s.live_renditions.filter((r) => r.kind === 'video').map((r) => `${r.name} ${r.width}×${r.height} ${fmtBitrate(r.measured_bandwidth)}`).join(' · ')}
                  {s.live_renditions.some((r) => r.kind === 'audio') ? ' · AAC' : ''}
                </div>
              )}
              {!isLive && (
                <div className="muted small">
                  {s.sessions ? `${s.sessions} session${s.sessions > 1 ? 's' : ''} · last ended ${fmtRelative(s.ended_at)}` : 'never went live'}
                  {s.error && <span className="error-text"> · {s.error}</span>}
                  {s.last_video_id && <> · <span className="ok-text">last recording in library</span></>}
                </div>
              )}
            </article>
          )
        })}
      </div>
    </section>
  )
}

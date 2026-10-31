import { useState } from 'react'
import type { VspClient } from '../lib/api'
import type { JobProgressEvent, Video } from '../lib/types'
import { fmtBytes, fmtDuration, fmtRelative } from '../lib/format'
import StatusBadge from './StatusBadge'

interface Props {
  client: VspClient
  videos: Video[]
  progress: Map<string, JobProgressEvent>
  onPlay: (v: Video) => void
  onUpload: () => void
  onRefresh: () => void
}

export default function Library({ client, videos, progress, onPlay, onUpload, onRefresh }: Props) {
  const [filter, setFilter] = useState('')
  const [busy, setBusy] = useState<string>('')

  const shown = videos.filter(
    (v) => !filter || v.title.toLowerCase().includes(filter.toLowerCase()) || v.original_filename.includes(filter)
  )

  const remove = async (v: Video) => {
    if (!confirm(`Delete "${v.title}" and all its renditions?`)) return
    setBusy(v.id)
    try {
      await client.deleteVideo(v.id)
    } finally {
      setBusy('')
      onRefresh()
    }
  }

  const retranscode = async (v: Video) => {
    setBusy(v.id)
    try {
      await client.retranscode(v.id)
    } catch (e) {
      alert((e as Error).message)
    } finally {
      setBusy('')
    }
  }

  return (
    <section>
      <header className="page-header">
        <h1>Library</h1>
        <div className="row gap">
          <input className="search" placeholder="Search…" value={filter} onChange={(e) => setFilter(e.target.value)} />
          <button className="btn" onClick={onRefresh}>Refresh</button>
          <button className="btn primary" onClick={onUpload}>+ Upload</button>
        </div>
      </header>

      {shown.length === 0 && (
        <div className="empty">
          <p>No videos yet.</p>
          <button className="btn primary" onClick={onUpload}>Upload your first video</button>
        </div>
      )}

      <div className="grid">
        {shown.map((v) => {
          const p = progress.get(v.id)
          const live = v.status === 'processing' && p?.playable
          const canPlay = v.status === 'ready' || live
          const pct = Math.round((v.status === 'processing' ? (p?.progress ?? v.progress) : v.progress) * 100)
          return (
            <article key={v.id} className={`card ${canPlay ? 'playable' : ''}`}>
              <div className="thumb" onClick={() => canPlay && onPlay(v)}>
                {v.has_thumbnail ? (
                  <img src={client.url(v.thumbnail_url) + `?t=${Date.parse(v.updated_at)}`} alt="" />
                ) : (
                  <div className="thumb-placeholder">{v.status === 'processing' ? '⏳' : '🎬'}</div>
                )}
                {canPlay && <div className="play-overlay">▶</div>}
                {v.duration_ms > 0 && <span className="duration">{fmtDuration(v.duration_ms)}</span>}
                {live && <span className="live-tag">LIVE PREVIEW</span>}
              </div>
              <div className="card-body">
                <div className="row between">
                  <h3 title={v.title}>{v.title}</h3>
                  <StatusBadge status={v.status} />
                </div>
                <div className="muted small">
                  {v.width ? `${v.width}×${v.height} · ${v.fps.toFixed(2)} fps · ` : ''}
                  {fmtBytes(v.size_bytes)} · {fmtRelative(v.created_at)}
                </div>
                {(v.status === 'processing' || v.status === 'queued' || v.status === 'uploading') && (
                  <div className="progress">
                    <div className="bar" style={{ width: `${pct}%` }} />
                    <span className="progress-label">
                      {v.status === 'processing' ? `${pct}%${p?.fps ? ` · ${Math.round(p.fps)} fps` : ''}` : v.status}
                    </span>
                  </div>
                )}
                {v.status === 'ready' && (
                  <div className="muted small">
                    {v.renditions.filter((r) => r.kind === 'video').map((r) => r.name).join(' · ')}
                    {v.renditions.some((r) => r.kind === 'audio') ? ' · AAC' : ''} · {v.encoder_used}
                  </div>
                )}
                {v.status === 'failed' && <div className="error-text small">{v.error}</div>}
                <div className="row gap card-actions">
                  {canPlay && <button className="btn small primary" onClick={() => onPlay(v)}>Play</button>}
                  {(v.status === 'ready' || v.status === 'failed' || v.status === 'uploaded') && (
                    <button className="btn small" disabled={busy === v.id} onClick={() => retranscode(v)}>Re-transcode</button>
                  )}
                  <button className="btn small danger" disabled={busy === v.id} onClick={() => remove(v)}>Delete</button>
                </div>
              </div>
            </article>
          )
        })}
      </div>
    </section>
  )
}

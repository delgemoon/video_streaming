import type { VspClient } from '../lib/api'
import type { Job, JobProgressEvent, Video } from '../lib/types'
import { fmtRelative } from '../lib/format'

interface Props {
  client: VspClient
  jobs: Job[]
  videos: Map<string, Video>
  progress: Map<string, JobProgressEvent>
  onRefresh: () => void
}

export default function Jobs({ client, jobs, videos, progress, onRefresh }: Props) {
  const sorted = [...jobs].sort((a, b) => b.created_at.localeCompare(a.created_at))
  return (
    <section>
      <header className="page-header">
        <h1>Jobs</h1>
        <button className="btn" onClick={onRefresh}>Refresh</button>
      </header>
      {sorted.length === 0 && <div className="empty"><p>No jobs yet.</p></div>}
      <table className="table">
        <thead>
          <tr>
            <th>Video</th>
            <th>Status</th>
            <th>Stage</th>
            <th style={{ width: 260 }}>Progress</th>
            <th>Speed</th>
            <th>Encoder</th>
            <th>Created</th>
            <th></th>
          </tr>
        </thead>
        <tbody>
          {sorted.map((j) => {
            const v = videos.get(j.video_id)
            const p = progress.get(j.video_id)
            const frac = j.status === 'running' ? (p?.progress ?? j.progress) : j.progress
            return (
              <tr key={j.id}>
                <td>{v?.title ?? j.video_id.slice(0, 8)}</td>
                <td><span className={`badge badge-job-${j.status}`}>{j.status}</span></td>
                <td>{j.stage}</td>
                <td>
                  <div className="progress inline">
                    <div className="bar" style={{ width: `${Math.round(frac * 100)}%` }} />
                    <span className="progress-label">{Math.round(frac * 100)}%</span>
                  </div>
                  {j.error && <div className="error-text small">{j.error}</div>}
                </td>
                <td>{j.status === 'running' && p?.fps ? `${Math.round(p.fps)} fps` : '—'}</td>
                <td className="mono small">{j.encoder}</td>
                <td className="muted small">{fmtRelative(j.created_at)}</td>
                <td>
                  {(j.status === 'queued' || j.status === 'running') && (
                    <button className="btn small danger" onClick={() => client.cancelJob(j.id).catch((e) => alert(e.message))}>
                      Cancel
                    </button>
                  )}
                </td>
              </tr>
            )
          })}
        </tbody>
      </table>
    </section>
  )
}

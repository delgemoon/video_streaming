import { useState } from 'react'
import type { SystemInfo } from '../lib/types'

interface Props {
  serverUrl: string
  system: SystemInfo | null
  online: boolean
  onChange: (url: string) => void
}

export default function Settings({ serverUrl, system, online, onChange }: Props) {
  const [url, setUrl] = useState(serverUrl)
  return (
    <section>
      <header className="page-header"><h1>Settings</h1></header>

      <div className="panel">
        <h2>Server</h2>
        <label className="field">
          <span>Backend URL</span>
          <div className="row gap">
            <input value={url} onChange={(e) => setUrl(e.target.value)} placeholder="http://127.0.0.1:8080" />
            <button className="btn primary" onClick={() => onChange(url.trim())} disabled={!url.trim()}>Connect</button>
          </div>
        </label>
        <div className={`status-pill ${online ? 'ok' : 'bad'}`}><span className="dot" /> {online ? 'Connected' : 'Not connected'}</div>
      </div>

      {system && (
        <div className="panel">
          <h2>Server capabilities</h2>
          <dl className="kv">
            <dt>Version</dt><dd>{system.version}</dd>
            <dt>Video encoder</dt>
            <dd>{system.video_encoder.element} {system.video_encoder.hardware ? '— hardware accelerated' : '— software'}</dd>
            <dt>Audio encoder</dt><dd>{system.audio_encoder}</dd>
            <dt>Segment / part</dt><dd>{system.segment_duration_ms} ms / {system.part_duration_ms || 'off'} ms (LL-HLS CMAF)</dd>
            <dt>Audio bitrate</dt><dd>{system.audio_bitrate_kbps} kbps AAC</dd>
            {system.observability && (<>
              <dt>Metrics</dt><dd><code>{system.observability.metrics_path}</code> (Prometheus)</dd>
              <dt>Tracing</dt><dd>{system.observability.tracing ? `OTLP → ${system.observability.otlp_endpoint}` : 'off'}</dd>
            </>)}
          </dl>
          <h3>Encoder candidates</h3>
          <table className="table compact">
            <thead><tr><th>Element</th><th>Type</th><th>Status</th></tr></thead>
            <tbody>
              {system.candidates.map((c) => (
                <tr key={c.element}>
                  <td className="mono">{c.element}</td>
                  <td>{c.hardware ? 'hardware' : 'software'}</td>
                  <td className={c.usable ? 'ok-text' : 'muted'}>{c.note}</td>
                </tr>
              ))}
            </tbody>
          </table>
          <h3>ABR ladder</h3>
          <table className="table compact">
            <thead><tr><th>Name</th><th>Height</th><th>Target bitrate</th></tr></thead>
            <tbody>
              {system.ladder.map((l) => (
                <tr key={l.name}><td>{l.name}</td><td>{l.height}</td><td>{l.bitrate_kbps} kbps</td></tr>
              ))}
            </tbody>
          </table>
        </div>
      )}
    </section>
  )
}

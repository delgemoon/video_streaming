import { useCallback, useEffect, useMemo, useRef, useState } from 'react'
import { VspClient } from './lib/api'
import { bridge } from './lib/bridge'
import { useServerEvents } from './lib/events'
import type { Job, JobProgressEvent, LiveStats, LiveStream, Playable, SystemInfo, Video } from './lib/types'
import Library from './components/Library'
import UploadWizard from './components/UploadWizard'
import Player from './components/Player'
import Jobs from './components/Jobs'
import Settings from './components/Settings'
import Live, { liveToPlayable } from './components/Live'

type View = 'library' | 'upload' | 'live' | 'jobs' | 'settings'

const LS_SERVER = 'vsp.serverUrl'

export default function App() {
  const [serverUrl, setServerUrl] = useState<string>(() => localStorage.getItem(LS_SERVER) || '')
  const [view, setView] = useState<View>('library')
  const [videos, setVideos] = useState<Map<string, Video>>(new Map())
  const [jobs, setJobs] = useState<Map<string, Job>>(new Map())
  const [progress, setProgress] = useState<Map<string, JobProgressEvent>>(new Map())
  const [system, setSystem] = useState<SystemInfo | null>(null)
  const [live, setLive] = useState<Map<string, LiveStream>>(new Map())
  const [liveStats, setLiveStats] = useState<Map<string, LiveStats>>(new Map())
  const [playing, setPlaying] = useState<Playable | null>(null)
  const [online, setOnline] = useState(false)
  const [error, setError] = useState<string>('')
  const [appVersion, setAppVersion] = useState('')

  const client = useMemo(() => (serverUrl ? new VspClient(serverUrl) : null), [serverUrl])
  const clientRef = useRef(client)
  clientRef.current = client

  useEffect(() => {
    if (serverUrl) return
    bridge.defaults().then((d) => {
      setServerUrl(d.serverUrl)
      setAppVersion(d.version)
    })
  }, [serverUrl])

  useEffect(() => {
    if (!appVersion) bridge.defaults().then((d) => setAppVersion(d.version))
  }, [appVersion])

  const refresh = useCallback(async () => {
    const c = clientRef.current
    if (!c) return
    try {
      const [v, j, s, l] = await Promise.all([c.listVideos(), c.listJobs(), c.system(), c.listLive()])
      setVideos(new Map(v.videos.map((x) => [x.id, x])))
      setJobs(new Map(j.jobs.map((x) => [x.id, x])))
      setSystem(s)
      setLive(new Map(l.streams.map((x) => [x.id, x])))
      setOnline(true)
      setError('')
    } catch (e) {
      setOnline(false)
      setError(`Cannot reach server at ${c.baseUrl}: ${(e as Error).message}`)
    }
  }, [])

  useEffect(() => {
    if (!client) return
    localStorage.setItem(LS_SERVER, client.baseUrl)
    refresh()
    const t = setInterval(() => {
      if (!online) refresh()
    }, 5000)
    return () => clearInterval(t)
  }, [client, refresh, online])

  const sseConnected = useServerEvents(serverUrl, {
    onVideo: (v) => {
      setVideos((m) => new Map(m).set(v.id, v))
      setPlaying((p) => (p && !p.live && p.id === v.id ? videoToPlayable(v) : p))
    },
    onLive: (s) => {
      setLive((m) => {
        const prev = m.get(s.id)
        // SSE payloads lack ingest fields; keep the ones we fetched via REST
        const merged = { ...prev, ...s, ingest_url: prev?.ingest_url, ingest_server: prev?.ingest_server }
        return new Map(m).set(s.id, merged as LiveStream)
      })
      setPlaying((p) => (p && p.live && (p.id === s.current_video_id) ? (s.status === 'live' ? liveToPlayable({ ...s }) : null) : p))
      if (s.status !== 'live') setLiveStats((m) => { const n = new Map(m); n.delete(s.id); return n })
    },
    onLiveDeleted: (id) => setLive((m) => { const n = new Map(m); n.delete(id); return n }),
    onLiveStats: (st) => setLiveStats((m) => new Map(m).set(st.stream_id, st)),
    onVideoDeleted: (id) =>
      setVideos((m) => {
        const n = new Map(m)
        n.delete(id)
        return n
      }),
    onJob: (j) => setJobs((m) => new Map(m).set(j.id, j)),
    onProgress: (p) => setProgress((m) => new Map(m).set(p.video_id, p))
  })

  useEffect(() => {
    if (sseConnected) refresh()
  }, [sseConnected, refresh])

  const videoList = useMemo(
    () => [...videos.values()].sort((a, b) => b.created_at.localeCompare(a.created_at)),
    [videos]
  )
  const liveList = useMemo(() => [...live.values()].sort((a, b) => b.created_at.localeCompare(a.created_at)), [live])
  const liveCount = useMemo(() => liveList.filter((s) => s.status === 'live').length, [liveList])
  const activeJobs = useMemo(
    () => [...jobs.values()].filter((j) => j.status === 'queued' || j.status === 'running').length,
    [jobs]
  )

  return (
    <div className="app">
      <aside className="sidebar">
        <div className="brand">
          <span className="brand-dot" />
          VSP <span className="muted">Desktop</span>
        </div>
        <nav>
          <NavItem active={view === 'library'} onClick={() => setView('library')} icon="▣" label="Library" count={videos.size} />
          <NavItem active={view === 'upload'} onClick={() => setView('upload')} icon="⤒" label="Upload" />
          <NavItem active={view === 'live'} onClick={() => setView('live')} icon="●" label="Live" count={liveCount || undefined} hot={liveCount > 0} />
          <NavItem active={view === 'jobs'} onClick={() => setView('jobs')} icon="⚙" label="Jobs" count={activeJobs || undefined} />
          <NavItem active={view === 'settings'} onClick={() => setView('settings')} icon="⚒" label="Settings" />
        </nav>
        <div className="sidebar-footer">
          <div className={`status-pill ${online ? 'ok' : 'bad'}`}>
            <span className="dot" /> {online ? 'Connected' : 'Offline'}
            {online && <span className="muted"> · {sseConnected ? 'live' : 'polling'}</span>}
          </div>
          {system && (
            <div className="muted small">
              {system.video_encoder.element} {system.video_encoder.hardware ? '(HW)' : '(SW)'} · v{system.version}
            </div>
          )}
          {appVersion && <div className="muted small">app v{appVersion}</div>}
        </div>
      </aside>

      <main className="content">
        {error && (
          <div className="banner error">
            {error} <button className="link" onClick={() => setView('settings')}>Change server</button>
          </div>
        )}
        {client && view === 'library' && (
          <Library
            client={client}
            videos={videoList}
            progress={progress}
            onPlay={(v) => setPlaying(videoToPlayable(v))}
            onUpload={() => setView('upload')}
            onRefresh={refresh}
          />
        )}
        {client && view === 'upload' && (
          <UploadWizard
            client={client}
            system={system}
            onDone={(v) => {
              setVideos((m) => new Map(m).set(v.id, v))
              setView('library')
            }}
          />
        )}
        {client && view === 'live' && (
          <Live client={client} system={system} streams={liveList} stats={liveStats} onPlay={(p) => setPlaying(p)} onRefresh={refresh} />
        )}
        {client && view === 'jobs' && (
          <Jobs client={client} jobs={[...jobs.values()]} videos={videos} progress={progress} onRefresh={refresh} />
        )}
        {view === 'settings' && (
          <Settings
            serverUrl={serverUrl}
            system={system}
            online={online}
            onChange={(url) => {
              setServerUrl(url)
              setOnline(false)
            }}
          />
        )}
      </main>

      {playing && client && (
        <Player client={client} item={playing} progress={progress.get(playing.id)} onClose={() => setPlaying(null)}
                otlpEndpoint={system?.observability?.tracing ? system.observability.otlp_endpoint_public || system.observability.otlp_endpoint : undefined} />
      )}
    </div>
  )
}

function videoToPlayable(v: Video): Playable {
  return {
    id: v.id,
    title: v.title,
    playback_url: v.playback_url,
    live: false,
    subtitle: `${v.width}×${v.height} · ${v.encoder_used}`,
    renditions: v.renditions,
    duration_ms: v.duration_ms,
    status: v.status,
    progress: v.progress
  }
}

function NavItem(props: { active: boolean; onClick: () => void; icon: string; label: string; count?: number; hot?: boolean }) {
  return (
    <button className={`nav-item ${props.active ? 'active' : ''}`} onClick={props.onClick}>
      <span className={`nav-icon ${props.hot ? 'hot' : ''}`}>{props.icon}</span>
      <span>{props.label}</span>
      {props.count !== undefined && <span className="nav-count">{props.count}</span>}
    </button>
  )
}

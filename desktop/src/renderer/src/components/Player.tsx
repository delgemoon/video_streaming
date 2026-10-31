import { useEffect, useRef, useState } from 'react'
import Hls, { type Level } from 'hls.js'
import type { VspClient } from '../lib/api'
import type { JobProgressEvent, Playable } from '../lib/types'
import { fmtBitrate, fmtDuration } from '../lib/format'

interface Props {
  client: VspClient
  item: Playable
  progress?: JobProgressEvent
  onClose: () => void
}

interface Stats {
  level: number
  levels: Level[]
  bandwidthEstimate: number
  buffer: number
  latency: number
  live: boolean
  dropped: number
  resolution: string
}

export default function Player({ client, item: video, progress, onClose }: Props) {
  const videoRef = useRef<HTMLVideoElement>(null)
  const hlsRef = useRef<Hls | null>(null)
  const [stats, setStats] = useState<Stats>({
    level: -1,
    levels: [],
    bandwidthEstimate: 0,
    buffer: 0,
    latency: 0,
    live: false,
    dropped: 0,
    resolution: ''
  })
  const [manualLevel, setManualLevel] = useState(-1)
  const [error, setError] = useState('')
  const [showStats, setShowStats] = useState(true)
  const src = client.url(video.playback_url)

  useEffect(() => {
    const el = videoRef.current
    if (!el) return
    setError('')

    if (!Hls.isSupported()) {
      el.src = src
      el.play().catch(() => {})
      return
    }

    const hls = new Hls({
      debug: localStorage.getItem('vsp.hlsDebug') === '1',
      lowLatencyMode: true,
      enableWorker: true,
      backBufferLength: video.live ? 10 : 30,
      maxBufferLength: video.live ? 6 : 20,
      // Live: honour the playlist's PART-HOLD-BACK (3 parts) instead of a fixed offset.
      ...(video.live ? {} : { liveSyncDuration: 3 }),
      liveDurationInfinity: video.live,
      startLevel: -1,
      capLevelToPlayerSize: false
    })
    hlsRef.current = hls
    hls.attachMedia(el)
    hls.on(Hls.Events.MEDIA_ATTACHED, () => hls.loadSource(src))
    hls.on(Hls.Events.MANIFEST_PARSED, () => {
      el.play().catch(() => {})
    })
    hls.on(Hls.Events.LEVEL_SWITCHED, (_, d) => setStats((s) => ({ ...s, level: d.level })))
    hls.on(Hls.Events.LEVEL_LOADED, (_, d) =>
      setStats((s) => ({ ...s, live: d.details.live }))
    )
    hls.on(Hls.Events.ERROR, (_, data) => {
      if (!data.fatal) return
      switch (data.type) {
        case Hls.ErrorTypes.NETWORK_ERROR:
          setError(`Network error: ${data.details} — retrying…`)
          setTimeout(() => hls.startLoad(), 1000)
          break
        case Hls.ErrorTypes.MEDIA_ERROR:
          setError(`Media error: ${data.details} — recovering…`)
          hls.recoverMediaError()
          break
        default:
          setError(`Fatal: ${data.details}`)
          hls.destroy()
      }
    })

    const timer = setInterval(() => {
      const v = videoRef.current
      if (!v) return
      let buffer = 0
      for (let i = 0; i < v.buffered.length; i++) {
        if (v.buffered.start(i) <= v.currentTime && v.currentTime <= v.buffered.end(i)) {
          buffer = v.buffered.end(i) - v.currentTime
        }
      }
      const q = v.getVideoPlaybackQuality?.()
      setStats((s) => ({
        ...s,
        levels: hls.levels,
        bandwidthEstimate: hls.bandwidthEstimate,
        buffer,
        latency: hls.latency,
        dropped: q?.droppedVideoFrames ?? 0,
        resolution: v.videoWidth ? `${v.videoWidth}×${v.videoHeight}` : ''
      }))
    }, 500)

    return () => {
      clearInterval(timer)
      hls.destroy()
      hlsRef.current = null
    }
  }, [src, video.live])

  // When a processing video finishes, reload so the player picks up the final VOD playlist.
  const wasProcessing = useRef(video.status === 'processing')
  useEffect(() => {
    if (wasProcessing.current && video.status === 'ready' && hlsRef.current) {
      const t = videoRef.current?.currentTime ?? 0
      hlsRef.current.loadSource(src)
      hlsRef.current.once(Hls.Events.MANIFEST_PARSED, () => {
        if (videoRef.current) videoRef.current.currentTime = t
      })
      wasProcessing.current = false
    }
  }, [video.status, src])

  const changeLevel = (lvl: number) => {
    setManualLevel(lvl)
    if (hlsRef.current) hlsRef.current.currentLevel = lvl
  }

  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      if (e.key === 'Escape') onClose()
    }
    window.addEventListener('keydown', onKey)
    return () => window.removeEventListener('keydown', onKey)
  }, [onClose])

  const current = stats.levels[stats.level]
  return (
    <div className="modal-backdrop" onClick={onClose}>
      <div className="player" onClick={(e) => e.stopPropagation()}>
        <header className="player-header">
          <div>
            <h2>{video.title}</h2>
            <div className="muted small">
              {video.live && <span className="live-tag inline">LIVE</span>}
              {video.status === 'processing' && (
                <span className="live-tag inline">LIVE PREVIEW · transcoding {Math.round((progress?.progress ?? video.progress ?? 0) * 100)}%</span>
              )}{' '}
              {video.subtitle}
            </div>
          </div>
          <div className="row gap">
            <select value={manualLevel} onChange={(e) => changeLevel(Number(e.target.value))} className="select">
              <option value={-1}>Auto{current ? ` (${current.height}p)` : ''}</option>
              {stats.levels.map((l, i) => (
                <option key={i} value={i}>
                  {l.height}p · {fmtBitrate(l.bitrate)}
                </option>
              ))}
            </select>
            <button className="btn" onClick={() => setShowStats((s) => !s)}>Stats</button>
            <button className="btn" onClick={onClose}>✕</button>
          </div>
        </header>

        <div className="video-wrap">
          <video ref={videoRef} controls playsInline />
          {showStats && (
            <div className="stats-overlay mono">
              <div>level: {stats.level >= 0 && current ? `${current.height}p @ ${fmtBitrate(current.bitrate)}` : '—'}</div>
              <div>decoded: {stats.resolution || '—'}</div>
              <div>bw est: {fmtBitrate(stats.bandwidthEstimate)}</div>
              <div>buffer: {stats.buffer.toFixed(2)} s</div>
              {stats.live && <div>latency: {stats.latency.toFixed(2)} s (LL-HLS)</div>}
              <div>dropped: {stats.dropped}</div>
              <div>mode: {video.live ? 'LIVE' : stats.live ? 'EVENT (in progress)' : 'VOD'}</div>
            </div>
          )}
          {error && <div className="player-error">{error}</div>}
        </div>

        <footer className="player-footer muted small">
          <span className="mono">{src}</span>
          <span>
            {video.renditions
              .filter((r) => r.kind === 'video')
              .map((r) => `${r.name} ${fmtBitrate(r.measured_bandwidth || r.bitrate_kbps * 1000)}`)
              .join(' · ')}
            {video.duration_ms ? ` · ${fmtDuration(video.duration_ms)}` : ''}
          </span>
        </footer>
      </div>
    </div>
  )
}

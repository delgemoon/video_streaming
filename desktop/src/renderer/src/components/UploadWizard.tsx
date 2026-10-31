import { useCallback, useEffect, useMemo, useRef, useState } from 'react'
import type { VspClient } from '../lib/api'
import type { SystemInfo, Video } from '../lib/types'
import { fmtBytes, fmtDuration, fmtTimecode } from '../lib/format'

interface Props {
  client: VspClient
  system: SystemInfo | null
  onDone: (v: Video) => void
}

interface Probe {
  duration_ms: number
  width: number
  height: number
  playable: boolean
}

type Phase = 'pick' | 'prepare' | 'uploading' | 'done'

export default function UploadWizard({ client, system, onDone }: Props) {
  const [file, setFile] = useState<File | null>(null)
  const [objectUrl, setObjectUrl] = useState('')
  const [probe, setProbe] = useState<Probe | null>(null)
  const [phase, setPhase] = useState<Phase>('pick')
  const [title, setTitle] = useState('')
  const [description, setDescription] = useState('')
  const [trimStart, setTrimStart] = useState(0)
  const [trimEnd, setTrimEnd] = useState(0)
  const [renditions, setRenditions] = useState<Set<string>>(new Set())
  const [audioKbps, setAudioKbps] = useState(0)
  const [keepSource, setKeepSource] = useState(true)
  const [poster, setPoster] = useState<{ blob: Blob; url: string; at: number } | null>(null)
  const [dragOver, setDragOver] = useState(false)
  const [upload, setUpload] = useState({ sent: 0, total: 0, rate: 0 })
  const [error, setError] = useState('')
  const [created, setCreated] = useState<Video | null>(null)
  const videoRef = useRef<HTMLVideoElement>(null)
  const fileInput = useRef<HTMLInputElement>(null)
  const abortRef = useRef<AbortController | null>(null)

  const ladder = useMemo(() => system?.ladder ?? [], [system])
  const applicable = useMemo(
    () => ladder.filter((l) => !probe?.height || l.height <= probe.height),
    [ladder, probe]
  )

  // ---- file selection --------------------------------------------------
  const choose = useCallback((f: File) => {
    setFile(f)
    setTitle(f.name.replace(/\.[^.]+$/, ''))
    setProbe(null)
    setPoster(null)
    setTrimStart(0)
    setTrimEnd(0)
    setError('')
    setPhase('prepare')
    setObjectUrl((old) => {
      if (old) URL.revokeObjectURL(old)
      return URL.createObjectURL(f)
    })
  }, [])

  useEffect(() => {
    return () => {
      if (objectUrl) URL.revokeObjectURL(objectUrl)
    }
  }, [objectUrl])

  useEffect(() => {
    // Default rendition selection once the ladder/probe are known.
    setRenditions(new Set(applicable.map((l) => l.name)))
  }, [applicable])

  const onDrop = (e: React.DragEvent) => {
    e.preventDefault()
    setDragOver(false)
    const f = e.dataTransfer.files?.[0]
    if (f) choose(f)
  }

  // ---- client-side probe via <video> ----------------------------------
  const onMeta = () => {
    const v = videoRef.current
    if (!v) return
    const dur = Math.round((isFinite(v.duration) ? v.duration : 0) * 1000)
    setProbe({ duration_ms: dur, width: v.videoWidth, height: v.videoHeight, playable: v.videoWidth > 0 })
    setTrimEnd(dur)
    if (dur > 0) v.currentTime = Math.min(dur / 1000 * 0.1, 5)
  }
  const onVideoError = () => {
    setProbe({ duration_ms: 0, width: 0, height: 0, playable: false })
  }

  // Keep preview playback inside the trim range.
  const onTimeUpdate = () => {
    const v = videoRef.current
    if (!v || !probe?.duration_ms) return
    if (trimEnd > 0 && v.currentTime * 1000 > trimEnd + 50) {
      v.currentTime = trimStart / 1000
    }
  }

  const seekTo = (ms: number) => {
    const v = videoRef.current
    if (v) v.currentTime = ms / 1000
  }
  const setInHere = () => {
    const t = Math.round((videoRef.current?.currentTime ?? 0) * 1000)
    setTrimStart(Math.min(t, trimEnd - 500))
  }
  const setOutHere = () => {
    const t = Math.round((videoRef.current?.currentTime ?? 0) * 1000)
    setTrimEnd(Math.max(t, trimStart + 500))
  }

  const capturePoster = async () => {
    const v = videoRef.current
    if (!v || !v.videoWidth) return
    const canvas = document.createElement('canvas')
    const scale = Math.min(1, 1280 / v.videoWidth)
    canvas.width = Math.round(v.videoWidth * scale)
    canvas.height = Math.round(v.videoHeight * scale)
    canvas.getContext('2d')!.drawImage(v, 0, 0, canvas.width, canvas.height)
    const blob = await new Promise<Blob | null>((r) => canvas.toBlob(r, 'image/jpeg', 0.86))
    if (!blob) return
    setPoster((old) => {
      if (old) URL.revokeObjectURL(old.url)
      return { blob, url: URL.createObjectURL(blob), at: Math.round(v.currentTime * 1000) }
    })
  }

  // ---- upload ----------------------------------------------------------
  const start = async () => {
    if (!file) return
    setError('')
    setPhase('uploading')
    const ac = new AbortController()
    abortRef.current = ac
    try {
      const isTrimmed = probe?.duration_ms && (trimStart > 0 || trimEnd < probe.duration_ms)
      const v = await client.createVideo({
        title: title.trim() || file.name,
        description,
        filename: file.name,
        size_bytes: file.size,
        options: {
          trim_start_ms: isTrimmed ? trimStart : 0,
          trim_end_ms: isTrimmed && trimEnd < (probe?.duration_ms ?? 0) ? trimEnd : 0,
          renditions: renditions.size === applicable.length ? [] : [...renditions],
          audio_bitrate_kbps: audioKbps,
          keep_source: keepSource
        }
      })
      setCreated(v)
      if (poster) await client.uploadThumbnail(v.id, poster.blob).catch(() => {})
      const res = await client.uploadSource(v.id, file, (sent, total, rate) => setUpload({ sent, total, rate }), ac.signal)
      setPhase('done')
      setCreated(res.video)
      setTimeout(() => onDone(res.video), 600)
    } catch (e) {
      setError((e as Error).message)
      setPhase('prepare')
    }
  }

  const cancelUpload = () => abortRef.current?.abort()

  // ---- render ----------------------------------------------------------
  if (phase === 'pick' || !file) {
    return (
      <section>
        <header className="page-header"><h1>Upload</h1></header>
        <div
          className={`dropzone ${dragOver ? 'over' : ''}`}
          onDragOver={(e) => { e.preventDefault(); setDragOver(true) }}
          onDragLeave={() => setDragOver(false)}
          onDrop={onDrop}
          onClick={() => fileInput.current?.click()}
        >
          <div className="dz-icon">⤒</div>
          <p>Drop a video here or <b>click to browse</b></p>
          <p className="muted small">MP4, MOV, MKV, WebM, AVI… Anything GStreamer can decode. Uploads stream directly from disk.</p>
          <input ref={fileInput} type="file" accept="video/*,.mkv,.avi,.ts,.mts,.mxf" hidden
                 onChange={(e) => e.target.files?.[0] && choose(e.target.files[0])} />
        </div>
      </section>
    )
  }

  const dur = probe?.duration_ms ?? 0
  const trimmedMs = dur ? trimEnd - trimStart : 0
  const pct = upload.total ? Math.round((upload.sent / upload.total) * 100) : 0

  return (
    <section>
      <header className="page-header">
        <h1>Upload &amp; pre-process</h1>
        <button className="btn" onClick={() => { setFile(null); setPhase('pick') }} disabled={phase === 'uploading'}>← Choose another file</button>
      </header>

      {error && <div className="banner error">{error}</div>}

      <div className="wizard">
        <div className="wizard-preview">
          <div className="preview-box">
            <video
              ref={videoRef}
              src={objectUrl}
              controls
              muted
              onLoadedMetadata={onMeta}
              onError={onVideoError}
              onTimeUpdate={onTimeUpdate}
            />
            {probe && !probe.playable && (
              <div className="preview-note">
                Preview not available for this container/codec in the desktop app — the server will still transcode it.
                Trimming and poster capture are disabled.
              </div>
            )}
          </div>

          <div className="panel">
            <h2>Source</h2>
            <dl className="kv">
              <dt>File</dt><dd className="mono small">{file.name}</dd>
              <dt>Size</dt><dd>{fmtBytes(file.size)}</dd>
              {probe?.playable && (<>
                <dt>Resolution</dt><dd>{probe.width}×{probe.height}</dd>
                <dt>Duration</dt><dd>{fmtDuration(dur)}</dd>
              </>)}
            </dl>
          </div>

          {probe?.playable && (
            <div className="panel">
              <h2>Trim</h2>
              <div className="trim-row">
                <label>In <input type="number" min={0} max={trimEnd - 500} step={100} value={trimStart}
                       onChange={(e) => setTrimStart(Math.max(0, Math.min(Number(e.target.value), trimEnd - 500)))} /> ms</label>
                <button className="btn small" onClick={setInHere}>Set in at playhead</button>
                <button className="btn small" onClick={() => seekTo(trimStart)}>Go</button>
              </div>
              <div className="trim-row">
                <label>Out <input type="number" min={trimStart + 500} max={dur} step={100} value={trimEnd}
                       onChange={(e) => setTrimEnd(Math.min(dur, Math.max(Number(e.target.value), trimStart + 500)))} /> ms</label>
                <button className="btn small" onClick={setOutHere}>Set out at playhead</button>
                <button className="btn small" onClick={() => seekTo(Math.max(0, trimEnd - 2000))}>Go</button>
              </div>
              <div className="range-track">
                <div className="range-sel" style={{ left: `${(trimStart / dur) * 100}%`, width: `${(trimmedMs / dur) * 100}%` }} />
              </div>
              <div className="row between muted small">
                <span>{fmtTimecode(trimStart)} → {fmtTimecode(trimEnd)}</span>
                <span>Output: {fmtDuration(trimmedMs)}{trimmedMs !== dur ? ` (−${fmtDuration(dur - trimmedMs)})` : ' (full)'}</span>
                <button className="link" onClick={() => { setTrimStart(0); setTrimEnd(dur) }}>Reset</button>
              </div>
            </div>
          )}
        </div>

        <div className="wizard-form">
          <div className="panel">
            <h2>Details</h2>
            <label className="field"><span>Title</span><input value={title} onChange={(e) => setTitle(e.target.value)} /></label>
            <label className="field"><span>Description</span>
              <textarea rows={3} value={description} onChange={(e) => setDescription(e.target.value)} /></label>
          </div>

          <div className="panel">
            <h2>Poster frame</h2>
            <div className="row gap">
              <button className="btn" onClick={capturePoster} disabled={!probe?.playable}>Capture current frame</button>
              {poster && <button className="btn small" onClick={() => setPoster(null)}>Clear</button>}
            </div>
            {poster ? (
              <div className="poster-preview"><img src={poster.url} alt="" /><span className="muted small">at {fmtTimecode(poster.at)} · {fmtBytes(poster.blob.size)}</span></div>
            ) : (
              <p className="muted small">Scrub the preview to a frame and capture it; otherwise the server picks one automatically.</p>
            )}
          </div>

          <div className="panel">
            <h2>Renditions</h2>
            {applicable.length === 0 && <p className="muted small">Ladder will be chosen automatically by the server.</p>}
            <div className="chips">
              {applicable.map((l) => (
                <label key={l.name} className={`chip ${renditions.has(l.name) ? 'on' : ''}`}>
                  <input type="checkbox" checked={renditions.has(l.name)}
                    onChange={(e) => setRenditions((s) => { const n = new Set(s); e.target.checked ? n.add(l.name) : n.delete(l.name); return n })} />
                  {l.name} <span className="muted">{l.bitrate_kbps} kbps</span>
                </label>
              ))}
            </div>
            <label className="field inline">
              <span>Audio</span>
              <select value={audioKbps} onChange={(e) => setAudioKbps(Number(e.target.value))} className="select">
                <option value={0}>Server default ({system?.audio_bitrate_kbps ?? 128} kbps)</option>
                <option value={96}>96 kbps</option>
                <option value={128}>128 kbps</option>
                <option value={192}>192 kbps</option>
                <option value={256}>256 kbps</option>
              </select>
            </label>
            <label className="field inline check">
              <input type="checkbox" checked={keepSource} onChange={(e) => setKeepSource(e.target.checked)} />
              <span>Keep original on server (needed for re-transcode)</span>
            </label>
          </div>

          {phase === 'uploading' ? (
            <div className="panel">
              <h2>Uploading…</h2>
              <div className="progress big">
                <div className="bar" style={{ width: `${pct}%` }} />
                <span className="progress-label">{pct}% · {fmtBytes(upload.sent)} / {fmtBytes(upload.total)} · {fmtBytes(upload.rate)}/s</span>
              </div>
              <button className="btn danger" onClick={cancelUpload}>Cancel</button>
            </div>
          ) : phase === 'done' ? (
            <div className="panel ok-panel">✓ Uploaded. Transcode job queued{created ? ` for "${created.title}"` : ''}.</div>
          ) : (
            <button className="btn primary big" onClick={start} disabled={renditions.size === 0 && applicable.length > 0}>
              Upload &amp; transcode
            </button>
          )}
        </div>
      </div>
    </section>
  )
}

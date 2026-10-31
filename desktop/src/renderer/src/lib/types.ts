export type VideoStatus = 'created' | 'uploading' | 'uploaded' | 'queued' | 'processing' | 'ready' | 'failed'

export interface TranscodeOptions {
  trim_start_ms: number
  trim_end_ms: number
  renditions: string[]
  audio_bitrate_kbps: number
  keep_source: boolean
}

export interface Rendition {
  name: string
  kind: 'video' | 'audio'
  width: number
  height: number
  bitrate_kbps: number
  measured_bandwidth: number
  measured_avg_bandwidth: number
  codecs: string
  path: string
}

export interface Video {
  id: string
  title: string
  description: string
  original_filename: string
  size_bytes: number
  status: VideoStatus
  duration_ms: number
  width: number
  height: number
  fps: number
  video_codec: string
  audio_codec: string
  options: TranscodeOptions
  renditions: Rendition[]
  has_thumbnail: boolean
  encoder_used: string
  error: string
  progress: number
  playback_url: string
  thumbnail_url: string
  created_at: string
  updated_at: string
  upload_url?: string
}

export interface Job {
  id: string
  video_id: string
  type: string
  status: 'queued' | 'running' | 'done' | 'failed' | 'cancelled'
  progress: number
  stage: string
  fps: number
  encoder: string
  error: string
  created_at: string
  started_at: string
  finished_at: string
}

export interface LadderStep {
  name: string
  height: number
  bitrate_kbps: number
}

export interface SystemInfo {
  version: string
  video_encoder: { id: string; element: string; hardware: boolean }
  audio_encoder: string
  candidates: { id: string; element: string; hardware: boolean; usable: boolean; note: string }[]
  segment_duration_ms: number
  part_duration_ms: number
  audio_bitrate_kbps: number
  ladder: LadderStep[]
  observability?: {
    metrics_path: string
    tracing: boolean
    otlp_endpoint: string
    otlp_endpoint_public?: string
  }
  live?: {
    enabled: boolean
    rtmp_port: number
    rtmp_app: string
    segment_ms: number
    part_ms: number
    passthrough: boolean
    default_renditions: string[]
  }
}

export interface JobProgressEvent {
  job_id: string
  video_id: string
  progress: number
  fps: number
  position_ms: number
  playable: boolean
}

export type LiveStatus = 'idle' | 'live'

export interface LiveStats {
  stream_id: string
  bytes_in: number
  video_frames: number
  encode_fps: number
  queued_bytes: number
  duration_s: number
  uptime_s: number
}

export interface LiveStream {
  id: string
  name: string
  stream_key: string
  status: LiveStatus
  record: boolean
  renditions: string[]
  current_video_id: string
  last_video_id: string
  publisher: string
  width: number
  height: number
  fps: number
  video_codec: string
  audio_codec: string
  live_renditions: Rendition[]
  playback_url: string
  started_at: string
  ended_at: string
  created_at: string
  updated_at: string
  sessions: number
  error: string
  ingest_url?: string
  ingest_server?: string
  active?: boolean
  stats?: LiveStats | null
}

/** Anything the Player can play: a VOD Video or a LiveStream. */
export interface Playable {
  id: string
  title: string
  playback_url: string
  live: boolean
  subtitle?: string
  renditions: Rendition[]
  duration_ms?: number
  status?: string
  progress?: number
}

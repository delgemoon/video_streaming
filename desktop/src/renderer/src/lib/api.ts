import type { Job, SystemInfo, TranscodeOptions, Video } from './types'

export class ApiError extends Error {
  constructor(
    public status: number,
    message: string
  ) {
    super(message)
  }
}

export class VspClient {
  constructor(public baseUrl: string) {
    this.baseUrl = baseUrl.replace(/\/+$/, '')
  }

  url(path: string): string {
    return this.baseUrl + path
  }

  private async json<T>(path: string, init?: RequestInit): Promise<T> {
    const res = await fetch(this.url(path), {
      ...init,
      headers: { 'Content-Type': 'application/json', ...(init?.headers || {}) }
    })
    const text = await res.text()
    let body: unknown = null
    try {
      body = text ? JSON.parse(text) : null
    } catch {
      body = { error: text }
    }
    if (!res.ok) {
      const msg = (body as { error?: string } | null)?.error || res.statusText
      throw new ApiError(res.status, msg)
    }
    return body as T
  }

  health(): Promise<{ status: string; version: string; encoder: string }> {
    return this.json('/api/health')
  }
  system(): Promise<SystemInfo> {
    return this.json('/api/system')
  }
  listVideos(): Promise<{ videos: Video[] }> {
    return this.json('/api/videos')
  }
  getVideo(id: string): Promise<Video> {
    return this.json(`/api/videos/${id}`)
  }
  createVideo(data: {
    title: string
    description?: string
    filename: string
    size_bytes: number
    options: Partial<TranscodeOptions>
  }): Promise<Video> {
    return this.json('/api/videos', { method: 'POST', body: JSON.stringify(data) })
  }
  updateVideo(id: string, data: { title?: string; description?: string }): Promise<Video> {
    return this.json(`/api/videos/${id}`, { method: 'PATCH', body: JSON.stringify(data) })
  }
  deleteVideo(id: string): Promise<{ deleted: string }> {
    return this.json(`/api/videos/${id}`, { method: 'DELETE' })
  }
  retranscode(id: string, options?: Partial<TranscodeOptions>): Promise<{ job: Job; video: Video }> {
    return this.json(`/api/videos/${id}/transcode`, {
      method: 'POST',
      body: JSON.stringify(options ? { options } : {})
    })
  }
  listJobs(): Promise<{ jobs: Job[] }> {
    return this.json('/api/jobs')
  }
  cancelJob(id: string): Promise<unknown> {
    return this.json(`/api/jobs/${id}/cancel`, { method: 'POST' })
  }

  /**
   * Streams a File straight from disk with XHR (Chromium streams File bodies without buffering
   * the whole file in memory) and reports upload progress.
   */
  uploadSource(
    id: string,
    file: File,
    onProgress: (sent: number, total: number, bytesPerSec: number) => void,
    signal?: AbortSignal
  ): Promise<{ video: Video; job: Job }> {
    return new Promise((resolve, reject) => {
      const xhr = new XMLHttpRequest()
      xhr.open('PUT', this.url(`/api/videos/${id}/source`))
      xhr.setRequestHeader('Content-Type', 'application/octet-stream')
      const t0 = performance.now()
      xhr.upload.onprogress = (e) => {
        const secs = (performance.now() - t0) / 1000
        onProgress(e.loaded, e.total || file.size, secs > 0 ? e.loaded / secs : 0)
      }
      xhr.onload = () => {
        try {
          const body = JSON.parse(xhr.responseText)
          if (xhr.status >= 200 && xhr.status < 300) resolve(body)
          else reject(new ApiError(xhr.status, body.error || xhr.statusText))
        } catch {
          reject(new ApiError(xhr.status, 'invalid server response'))
        }
      }
      xhr.onerror = () => reject(new ApiError(0, 'network error during upload'))
      xhr.onabort = () => reject(new ApiError(0, 'upload cancelled'))
      signal?.addEventListener('abort', () => xhr.abort())
      xhr.send(file)
    })
  }

  async uploadThumbnail(id: string, blob: Blob): Promise<Video> {
    const res = await fetch(this.url(`/api/videos/${id}/thumbnail`), {
      method: 'PUT',
      headers: { 'Content-Type': 'image/jpeg' },
      body: blob
    })
    if (!res.ok) throw new ApiError(res.status, 'thumbnail upload failed')
    return res.json()
  }
}

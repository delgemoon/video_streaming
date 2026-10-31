import { useEffect, useRef, useState } from 'react'
import type { Job, JobProgressEvent, Video } from './types'

export interface ServerEvents {
  onVideo?: (v: Video) => void
  onVideoDeleted?: (id: string) => void
  onJob?: (j: Job) => void
  onProgress?: (p: JobProgressEvent) => void
}

/** Subscribes to the server's SSE stream; reconnects automatically (EventSource built-in). */
export function useServerEvents(baseUrl: string, handlers: ServerEvents): boolean {
  const [connected, setConnected] = useState(false)
  const ref = useRef(handlers)
  ref.current = handlers

  useEffect(() => {
    if (!baseUrl) return
    const es = new EventSource(baseUrl.replace(/\/+$/, '') + '/api/events')
    es.onopen = () => setConnected(true)
    es.onerror = () => setConnected(false)
    es.addEventListener('video.updated', (e) => ref.current.onVideo?.(JSON.parse((e as MessageEvent).data)))
    es.addEventListener('video.deleted', (e) =>
      ref.current.onVideoDeleted?.(JSON.parse((e as MessageEvent).data).id)
    )
    es.addEventListener('job.updated', (e) => ref.current.onJob?.(JSON.parse((e as MessageEvent).data)))
    es.addEventListener('job.progress', (e) => ref.current.onProgress?.(JSON.parse((e as MessageEvent).data)))
    return () => es.close()
  }, [baseUrl])

  return connected
}

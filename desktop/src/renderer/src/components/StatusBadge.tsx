import type { VideoStatus } from '../lib/types'

const LABELS: Record<VideoStatus, string> = {
  created: 'Created',
  uploading: 'Uploading',
  uploaded: 'Uploaded',
  queued: 'Queued',
  processing: 'Transcoding',
  ready: 'Ready',
  failed: 'Failed'
}

export default function StatusBadge({ status }: { status: VideoStatus | string }) {
  return <span className={`badge badge-${status}`}>{LABELS[status as VideoStatus] || status}</span>
}

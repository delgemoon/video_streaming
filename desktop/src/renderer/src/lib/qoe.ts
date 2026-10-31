import type Hls from 'hls.js'
import type { VspClient } from './api'

/**
 * Player QoE telemetry: observes an hls.js instance + <video> element, aggregates events locally
 * and posts them to the server every 5 s (POST /api/metrics/player), where they become Prometheus
 * histograms/counters. Also emits an OTLP/JSON "player.session" span to the collector so a
 * playback session can be correlated with server-side traces.
 */
export interface QoeOptions {
  client: VspClient
  mode: 'vod' | 'live'
  itemId: string
  otlpEndpoint?: string   // e.g. http://127.0.0.1:4318/v1/traces; omit to disable tracing
}

interface Ev { type: string; seconds?: number }

const hex = (bytes: number) =>
  Array.from(crypto.getRandomValues(new Uint8Array(bytes)), (b) => b.toString(16).padStart(2, '0')).join('')

export class PlayerQoe {
  private events: Ev[] = []
  private timer: number | undefined
  private stallStart = 0
  private startedAt = performance.now()
  private firstFrame = false
  private latency = 0
  private levelSwitches = 0
  private rebuffers = 0
  private stopped = false
  private readonly traceId = hex(16)
  private readonly spanId = hex(8)
  private readonly sessionStart = Date.now()
  private detach: (() => void)[] = []

  constructor(private opts: QoeOptions) {}

  attach(hls: Hls, video: HTMLVideoElement, HlsCtor: typeof Hls): void {
    const on = <K extends keyof HTMLMediaElementEventMap>(ev: K, fn: () => void) => {
      video.addEventListener(ev, fn)
      this.detach.push(() => video.removeEventListener(ev, fn))
    }
    on('playing', () => {
      if (!this.firstFrame) {
        this.firstFrame = true
        this.events.push({ type: 'startup', seconds: (performance.now() - this.startedAt) / 1000 })
        this.events.push({ type: 'start' })
      }
      if (this.stallStart) {
        this.events.push({ type: 'rebuffer', seconds: (performance.now() - this.stallStart) / 1000 })
        this.rebuffers++
        this.stallStart = 0
      }
    })
    on('waiting', () => {
      if (this.firstFrame && !this.stallStart) this.stallStart = performance.now()
    })
    on('seeking', () => { this.stallStart = 0 })   // user seeks are not rebuffers
    on('error', () => this.events.push({ type: 'error' }))

    hls.on(HlsCtor.Events.LEVEL_SWITCHED, () => {
      if (this.firstFrame) {
        this.events.push({ type: 'level_switch' })
        this.levelSwitches++
      }
    })
    hls.on(HlsCtor.Events.ERROR, (_, d) => {
      if (d.fatal) this.events.push({ type: 'fatal' })
    })
    const latencyTick = window.setInterval(() => {
      if (this.opts.mode === 'live' && Number.isFinite(hls.latency)) this.latency = hls.latency
    }, 1000)
    this.detach.push(() => clearInterval(latencyTick))

    this.timer = window.setInterval(() => void this.flush(), 5000)
  }

  private async flush(final = false): Promise<void> {
    const batch = this.events.splice(0)
    if (!batch.length && !final && this.opts.mode !== 'live') return
    const body = {
      mode: this.opts.mode,
      item_id: this.opts.itemId,
      events: batch,
      latency_s: this.opts.mode === 'live' && this.latency > 0 ? this.latency : undefined,
      active_sessions: final ? 0 : 1
    }
    try {
      await fetch(this.opts.client.url('/api/metrics/player'), {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(body),
        keepalive: final
      })
    } catch {
      /* metrics are best-effort */
    }
  }

  async stop(): Promise<void> {
    if (this.stopped) return
    this.stopped = true
    if (this.timer) clearInterval(this.timer)
    this.detach.forEach((d) => d())
    await this.flush(true)
    this.emitSpan()
  }

  private emitSpan(): void {
    if (!this.opts.otlpEndpoint) return
    const now = Date.now()
    const attr = (key: string, value: unknown) => ({
      key,
      value:
        typeof value === 'number'
          ? Number.isInteger(value) ? { intValue: String(value) } : { doubleValue: value }
          : typeof value === 'boolean' ? { boolValue: value } : { stringValue: String(value) }
    })
    const span = {
      traceId: this.traceId,
      spanId: this.spanId,
      name: 'player.session',
      kind: 3,
      startTimeUnixNano: String(this.sessionStart) + '000000',
      endTimeUnixNano: String(now) + '000000',
      attributes: [
        attr('player.mode', this.opts.mode),
        attr('media.id', this.opts.itemId),
        attr('player.rebuffers', this.rebuffers),
        attr('player.level_switches', this.levelSwitches),
        attr('player.live_latency_s', this.latency),
        attr('player.first_frame', this.firstFrame),
        attr('session.duration_s', (now - this.sessionStart) / 1000)
      ],
      status: { code: this.firstFrame ? 1 : 2 }
    }
    const doc = {
      resourceSpans: [{
        resource: { attributes: [attr('service.name', 'vsp-desktop'), attr('telemetry.sdk.language', 'webjs')] },
        scopeSpans: [{ scope: { name: 'vsp-desktop' }, spans: [span] }]
      }]
    }
    // fetch+keepalive rather than sendBeacon: a Blob beacon triggers a CORS preflight that the
    // collector's Allow-Origin:* + Allow-Credentials:true combination fails.
    fetch(this.opts.otlpEndpoint, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(doc),
      keepalive: true
    }).catch(() => {})
  }
}

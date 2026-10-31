# Observability

VSP exposes Prometheus metrics, OpenTelemetry traces and player QoE telemetry out of the box, and
ships a local Grafana stack to view them. Nothing here is required to run the platform — the
server works identically with the stack absent.

```
vsp-server ──/metrics──────────────► Prometheus ──► Grafana (6 dashboards) + Alertmanager (18 rules)
           ──OTLP/JSON /v1/traces──► otel-collector ──► Tempo (traces) + span-metrics ──► Prometheus
Electron   ──POST /api/metrics/player──► vsp-server (aggregated into Prometheus histograms)
           ──OTLP/JSON player.session──► otel-collector
host       ──node_exporter :9100, nvidia_smi_exporter :9835──► Prometheus
```

## Quick start

```bash
./scripts/observability-up.sh                 # docker compose up + host GPU exporter
backend/build/vsp-server --otlp-endpoint http://127.0.0.1:4318/v1/traces
open http://localhost:3000                    # Grafana, admin / vsp → folder "VSP"
./scripts/observability-up.sh down
```

Docker and the Compose plugin are installed by `scripts/setup-ubuntu.sh` on recent Ubuntu
(`docker.io docker-compose-v2`). The GPU exporter runs on the host because `nvidia-smi` needs the
driver; it is a 150-line stdlib Python script (`deploy/observability/gpu/nvidia_smi_exporter.py`).

| Component | Port | Purpose |
|---|---|---|
| Grafana | 3000 | dashboards, Explore (PromQL / TraceQL), alert view |
| Prometheus | 9090 | 5 s scrape, 15 d retention, alert rules |
| Alertmanager | 9093 | routing — add Slack/webhook receivers in `alertmanager/alertmanager.yml` |
| Tempo | 3200 | trace storage, 7 d retention, service graph + span metrics |
| OTel collector | 4318 / 4317 | OTLP HTTP/gRPC ingest; CORS enabled for the Electron renderer |
| node_exporter | 9100 | host CPU/mem/disk/net |
| nvidia_smi_exporter | 9835 | GPU/NVENC/NVDEC utilisation, memory, power, encoder sessions |

## Metrics

Served at `GET /metrics` (Prometheus text 0.0.4). The registry (`src/observability/metrics.*`) is
lock-free on the observe path — relaxed atomics, no allocation — so it is safe to call from
GStreamer streaming threads and RTMP I/O coroutines. All families are declared in one place,
`src/observability/vsp_metrics.hpp`; dashboards and alerts reference those names.

| Family | Labels | Notes |
|---|---|---|
| `vsp_http_request_duration_seconds` (hist) | route, method, status_class | routes are normalised (`/media/:id/:rendition/segment`) to stay low-cardinality |
| `vsp_http_requests_total`, `vsp_http_response_bytes_total` | route, method, status | |
| `vsp_http_connections`, `vsp_sse_clients` | | gauges |
| `vsp_hls_blocking_reload_hold_seconds` (hist), `vsp_hls_blocking_reload_timeouts_total` | | how long `_HLS_msn` requests were held; p50 ≈ half a part when players poll back-to-back |
| `vsp_media_requests_total`, `vsp_media_bytes_total` | kind = playlist/segment/init/thumbnail | |
| `vsp_upload_bytes_total`, `vsp_upload_duration_seconds` | | |
| `vsp_jobs_queued`, `vsp_jobs_running`, `vsp_jobs_total{result}` | | |
| `vsp_job_duration_seconds` (hist) | stage = probe/transcode/thumbnail/total | |
| `vsp_job_realtime_factor` (hist) | | media seconds per wall second; < 1 means slower than realtime |
| `vsp_job_encode_fps` | job_id | removed when the job ends |
| `vsp_job_failures_total` | reason | reason is the error text up to the first `:` |
| `vsp_job_source_bytes`, `vsp_job_source_duration_seconds` (hist) | | |
| `vsp_live_streams`, `vsp_live_sessions_total{result}`, `vsp_live_session_duration_seconds` | | |
| `vsp_live_ingest_bitrate_bps`, `vsp_live_encode_fps`, `vsp_live_backlog_bytes` | stream | removed when the stream ends |
| `vsp_live_frames_total`, `vsp_live_dropped_tags_total` | stream | dropped > 0 means the encoders could not keep up |
| `vsp_live_part_latency_seconds` (hist) | | RTMP arrival of the newest media → part on disk. Measures *server* processing only (~4 ms p50 / 44 ms p99 on the reference box); the rest of glass-to-glass latency is player hold-back |
| `vsp_rtmp_bytes_total`, `vsp_rtmp_messages_total{type}`, `vsp_rtmp_connections`, `vsp_rtmp_errors_total{kind}` | | |
| `vsp_packager_segments_total`, `vsp_packager_parts_total`, `vsp_packager_bytes_total` | mode = vod/live | |
| `vsp_packager_segment_duration_seconds` (hist) | mode | should cluster at the configured segment length |
| `vsp_packager_playlist_write_seconds` (hist) | | atomic write + rename |
| `vsp_gst_pipelines{kind}`, `vsp_gst_errors_total{kind}`, `vsp_gst_warnings_total{kind}` | | |
| `vsp_player_events_total{event,mode}` | start/startup/rebuffer/level_switch/error/fatal | pushed by clients |
| `vsp_player_startup_seconds`, `vsp_player_live_latency_seconds`, `vsp_player_rebuffer_seconds` (hist) | | |
| `vsp_player_sessions{mode}` | | |
| `vsp_build_info{version,gst_version,encoder}` | | |
| `process_*` | | CPU, RSS, FDs, threads, page faults from `/proc/self` |

### Player QoE

The desktop app's player (`desktop/src/renderer/src/lib/qoe.ts`) observes hls.js + the `<video>`
element and `POST`s a batch to `/api/metrics/player` every 5 s:

```json
{ "mode": "live", "item_id": "…", "latency_s": 2.3, "active_sessions": 1,
  "events": [ {"type": "startup", "seconds": 0.42}, {"type": "rebuffer", "seconds": 0.8}, {"type": "level_switch"} ] }
```

Any client (web player, mobile) can post the same shape. On session end it also emits a
`player.session` span (service `vsp-desktop`) with rebuffer count, level switches and latency.

## Traces

`src/observability/tracing.*` is a minimal OTLP/JSON exporter (no opentelemetry-cpp dependency):
spans are batched on a background thread and posted to `--otlp-endpoint` every 2 s. Disabled
(zero-cost no-op spans) when the endpoint is empty. W3C `traceparent` on incoming API requests is
honoured, so a client can link its own trace to the server's.

Spans emitted:

| Span | Children / attributes |
|---|---|
| `HTTP <METHOD> <route>` (server) | every `/api/*` request except SSE; `http.response.status_code`, `client.address`, `upload.bytes` |
| `transcode.job` | `transcode.probe` → `transcode.encode` (`encoder`, `renditions`, `realtime_factor`) → `transcode.thumbnail`; `source.*` attributes |
| `live.session` (server) | one per publisher connection; event `master_playlist_published`; `source.*`, `duration_s`, error status |
| `player.session` (client) | from the Electron app |

The collector's `spanmetrics` connector turns these into `otel_traces_span_metrics_*` series, so
span latency percentiles are also available in Prometheus without sampling concerns.

Tempo Explore: `{ name = "transcode.job" }`, `{ resource.service.name = "vsp-desktop" }`,
`{ status = error }`.

## Dashboards

Generated from `deploy/observability/grafana/gen/gen_dashboards.py` (edit the spec, rerun, Grafana
reloads within 30 s). Provisioned read-write; UI edits are allowed but regenerating overwrites them.

| Dashboard | What to look at |
|---|---|
| **Overview** | one screen for the whole system: throughput, latency p99s, GPU/CPU, errors |
| **Live ingest** | per-stream bitrate/fps/backlog, part-latency heatmap, segment duration, RTMP message mix |
| **Transcode jobs** | queue depth, realtime factor distribution, stage durations, failure reasons, upload throughput |
| **HTTP & delivery** | per-route rate/latency, media bytes by kind, blocking-reload holds, process resources |
| **Player QoE** | startup time, rebuffers, player-reported live latency, correlated against server-side latency |
| **Host & GPU** | CPU by mode, memory, NVENC/NVDEC utilisation and sessions, power/temp, disk, network |

## Alerts

`deploy/observability/prometheus/alerts.yml` — 18 rules across live, transcode, HTTP, player and
host groups. The ones that matter most for a streaming box:

- `LiveEncoderStalled` (critical) — a stream is ingesting but encode fps is 0 for 10 s
- `LiveBacklogGrowing` / `LiveTagsDropped` — encoders falling behind; the latter means viewers saw a gap
- `LivePartLatencyHigh` — server processing latency p95 above 1.5 s
- `BlockingReloadTimeouts` — packager not advancing while players wait
- `TranscodeSlow` — median realtime factor < 1 (hardware encoder lost?)
- `GpuEncoderSaturated`, `DiskSpaceLow`, `VspServerDown`

Validate after editing: `docker run --rm -v $PWD/deploy/observability/prometheus:/cfg:ro --entrypoint promtool prom/prometheus:v3.2.1 check rules /cfg/alerts.yml`.

## Configuration

| Setting | Default | |
|---|---|---|
| `otlp_endpoint` / `--otlp-endpoint` / `VSP_OTLP_ENDPOINT` / `OTEL_EXPORTER_OTLP_TRACES_ENDPOINT` | *(empty = off)* | where the server posts spans |
| `otlp_endpoint_public` / `VSP_OTLP_ENDPOINT_PUBLIC` | = `otlp_endpoint` | what clients are told to post to (`GET /api/system` → `observability.otlp_endpoint_public`) |
| `service_name` / `OTEL_SERVICE_NAME` | `vsp-server` | |

`/metrics` is always on and unauthenticated; put it behind your reverse proxy's allow-list if the
server is exposed.

## Overhead

Measured on the reference box with one 1080p live stream + a VOD job: counters/histograms add no
measurable CPU (relaxed atomics on existing code paths); `/metrics` renders ~55 families in < 1 ms;
tracing posts one small JSON batch every 2 s. The per-request `RequestRecord` is a single heap
allocation per HTTP request.

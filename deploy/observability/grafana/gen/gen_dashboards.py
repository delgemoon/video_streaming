#!/usr/bin/env python3
"""Generates the provisioned Grafana dashboards (JSON) from a compact spec.

Run after changing a panel:  python3 gen_dashboards.py
Dashboards land in ../dashboards/*.json and are picked up by Grafana within 30 s.
"""
import json
import pathlib

OUT = pathlib.Path(__file__).resolve().parent.parent / "dashboards"
PROM = {"type": "prometheus", "uid": "prometheus"}
_id = 0


def nid():
    global _id
    _id += 1
    return _id


def target(expr, legend=""):
    return {"datasource": PROM, "expr": expr, "legendFormat": legend, "refId": f"R{nid()}"}


def ts(title, targets, unit="short", w=12, h=8, stack=False, min0=True, desc="", thresholds=None):
    p = {
        "id": nid(), "type": "timeseries", "title": title, "description": desc, "datasource": PROM,
        "gridPos": {"w": w, "h": h, "x": 0, "y": 0}, "targets": targets,
        "fieldConfig": {"defaults": {"unit": unit, "custom": {"lineWidth": 1, "fillOpacity": 12 if stack else 8,
                                                                 "stacking": {"mode": "normal" if stack else "none"},
                                                                 "showPoints": "never"}}, "overrides": []},
        "options": {"legend": {"displayMode": "list", "placement": "bottom", "calcs": ["mean", "max", "lastNotNull"]},
                    "tooltip": {"mode": "multi", "sort": "desc"}},
    }
    if min0:
        p["fieldConfig"]["defaults"]["min"] = 0
    if thresholds:
        p["fieldConfig"]["defaults"]["thresholds"] = {"mode": "absolute", "steps": [{"color": "green", "value": None}] +
                                                      [{"color": c, "value": v} for v, c in thresholds]}
        p["fieldConfig"]["defaults"]["custom"]["thresholdsStyle"] = {"mode": "line"}
    return p


def stat(title, expr, unit="short", w=4, h=4, thresholds=None, desc="", legend=""):
    return {
        "id": nid(), "type": "stat", "title": title, "description": desc, "datasource": PROM,
        "gridPos": {"w": w, "h": h, "x": 0, "y": 0}, "targets": [target(expr, legend)],
        "fieldConfig": {"defaults": {"unit": unit, "thresholds": {"mode": "absolute", "steps": [{"color": "green", "value": None}] +
                                                                  [{"color": c, "value": v} for v, c in (thresholds or [])]}},
                        "overrides": []},
        "options": {"reduceOptions": {"calcs": ["lastNotNull"], "fields": "", "values": False}, "colorMode": "value",
                    "graphMode": "area", "textMode": "auto"},
    }


def heatmap(title, expr, unit="s", w=12, h=8, desc=""):
    return {
        "id": nid(), "type": "heatmap", "title": title, "description": desc, "datasource": PROM,
        "gridPos": {"w": w, "h": h, "x": 0, "y": 0},
        "targets": [{"datasource": PROM, "expr": expr, "format": "heatmap", "legendFormat": "{{le}}", "refId": "A"}],
        "options": {"calculate": False, "yAxis": {"unit": unit}, "color": {"scheme": "Spectral", "mode": "scheme", "reverse": True},
                    "cellGap": 1, "tooltip": {"mode": "single", "yHistogram": True}},
    }


def row(title):
    return {"id": nid(), "type": "row", "title": title, "collapsed": False,
            "gridPos": {"w": 24, "h": 1, "x": 0, "y": 0}, "panels": []}


def layout(panels):
    x = y = line_h = 0
    for p in panels:
        w, h = p["gridPos"]["w"], p["gridPos"]["h"]
        if p["type"] == "row":
            if x:
                y += line_h
            p["gridPos"].update(x=0, y=y)
            y += 1
            x = line_h = 0
            continue
        if x + w > 24:
            y += line_h
            x = line_h = 0
        p["gridPos"].update(x=x, y=y)
        x += w
        line_h = max(line_h, h)
    return panels


def dashboard(uid, title, panels, tags, refresh="5s", rng="now-30m"):
    return {
        "uid": uid, "title": title, "tags": ["vsp"] + tags, "timezone": "browser", "schemaVersion": 39, "version": 1,
        "editable": True, "graphTooltip": 1, "refresh": refresh, "time": {"from": rng, "to": "now"},
        "templating": {"list": []}, "panels": layout(panels),
        "links": [{"title": "VSP dashboards", "type": "dashboards", "tags": ["vsp"], "asDropdown": True}],
    }


def quant(p, metric, sel="", by="", window="1m"):
    """histogram_quantile over rate(<metric>_bucket{sel}[window]) summed by le[,by]."""
    s = "{" + sel + "}" if sel else ""
    b = f", {by}" if by else ""
    return f"histogram_quantile({p}, sum(rate({metric}_bucket{s}[{window}])) by (le{b}))"


SEG = 'route=~"/media/.*segment"'

# ---------------------------------------------------------------------------------------------
overview = dashboard("vsp-overview", "VSP · Overview", [
    stat("Server", 'up{job="vsp-server"}', thresholds=[(0, "red"), (1, "green")], w=3),
    stat("Live streams", "vsp_live_streams", w=3),
    stat("Jobs running", "vsp_jobs_running", w=3),
    stat("Jobs queued", "vsp_jobs_queued", thresholds=[(3, "yellow"), (6, "red")], w=3),
    stat("HTTP req/s", "sum(rate(vsp_http_requests_total[1m]))", unit="reqps", w=3),
    stat("Media egress", "sum(rate(vsp_media_bytes_total[1m]))*8", unit="bps", w=3),
    stat("RTMP ingress", "sum(rate(vsp_rtmp_bytes_total[1m]))*8", unit="bps", w=3),
    stat("Players", "sum(vsp_player_sessions)", w=3),
    row("Throughput"),
    ts("HTTP requests by route", [target("sum(rate(vsp_http_requests_total[1m])) by (route)", "{{route}}")], unit="reqps", stack=True),
    ts("Bytes served by kind", [target("sum(rate(vsp_media_bytes_total[1m])) by (kind)*8", "{{kind}}")], unit="bps", stack=True),
    ts("Live ingest bitrate", [target("vsp_live_ingest_bitrate_bps", "{{stream}}")], unit="bps"),
    ts("Encode fps (live + jobs)", [target("vsp_live_encode_fps", "live {{stream}}"), target("vsp_job_encode_fps", "job {{job_id}}")], unit="fps"),
    row("Latency"),
    ts("HTTP latency p99 by route", [target(quant(0.99, "vsp_http_request_duration_seconds", by="route"), "{{route}}")], unit="s", thresholds=[(0.5, "red")]),
    ts("LL-HLS part latency (ingest→published)", [target(quant(p, "vsp_live_part_latency_seconds"), f"p{int(p*100)}") for p in (0.5, 0.95, 0.99)], unit="s"),
    ts("Player live latency", [target(quant(0.5, "vsp_player_live_latency_seconds", window="5m"), "p50"), target(quant(0.9, "vsp_player_live_latency_seconds", window="5m"), "p90")], unit="s", thresholds=[(5, "red")]),
    ts("Blocking playlist reload hold", [target(quant(0.5, "vsp_hls_blocking_reload_hold_seconds"), "p50"), target(quant(0.99, "vsp_hls_blocking_reload_hold_seconds"), "p99"), target("rate(vsp_hls_blocking_reload_timeouts_total[1m])", "timeouts/s")], unit="s"),
    row("Host"),
    ts("CPU", [target('1 - avg(rate(node_cpu_seconds_total{mode="idle"}[1m]))', "host"), target('rate(process_cpu_seconds_total{job="vsp-server"}[1m])', "vsp-server (cores)")], unit="percentunit", w=8),
    ts("GPU", [target("nvidia_gpu_utilization_ratio", "gpu {{gpu}}"), target("nvidia_gpu_encoder_utilization_ratio", "nvenc {{gpu}}"), target("nvidia_gpu_decoder_utilization_ratio", "nvdec {{gpu}}")], unit="percentunit", w=8),
    ts("Memory", [target('process_resident_memory_bytes{job="vsp-server"}', "vsp-server RSS"), target("nvidia_gpu_memory_used_bytes", "GPU {{gpu}}")], unit="bytes", w=8),
    row("Errors"),
    ts("HTTP 5xx / 4xx", [target('sum(rate(vsp_http_requests_total{status=~"5.."}[1m]))', "5xx"), target('sum(rate(vsp_http_requests_total{status=~"4.."}[1m]))', "4xx")], unit="reqps", w=8),
    ts("GStreamer errors / warnings", [target("sum(rate(vsp_gst_errors_total[5m])) by (kind)", "err {{kind}}"), target("sum(rate(vsp_gst_warnings_total[5m])) by (kind)", "warn {{kind}}")], w=8),
    ts("Job / RTMP failures", [target("sum(rate(vsp_job_failures_total[5m])) by (reason)", "job: {{reason}}"), target("sum(rate(vsp_rtmp_errors_total[5m])) by (kind)", "rtmp: {{kind}}")], w=8),
], ["overview"])

live = dashboard("vsp-live", "VSP · Live ingest", [
    stat("Live streams", "vsp_live_streams", w=3),
    stat("RTMP connections", "vsp_rtmp_connections", w=3),
    stat("Ingress", "sum(rate(vsp_rtmp_bytes_total[1m]))*8", unit="bps", w=3),
    stat("Backlog", "max(vsp_live_backlog_bytes)", unit="bytes", thresholds=[(4e6, "yellow"), (8e6, "red")], w=3),
    stat("Dropped tags (1h)", "sum(increase(vsp_live_dropped_tags_total[1h]))", thresholds=[(1, "red")], w=3),
    stat("Part latency p95", quant(0.95, "vsp_live_part_latency_seconds"), unit="s", thresholds=[(1, "yellow"), (1.5, "red")], w=3),
    stat("Sessions (24h)", "sum(increase(vsp_live_sessions_total[24h])) by (result)", legend="{{result}}", w=3),
    stat("NVENC", "max(nvidia_gpu_encoder_utilization_ratio)", unit="percentunit", thresholds=[(0.8, "yellow"), (0.95, "red")], w=3),
    row("Per stream"),
    ts("Ingest bitrate", [target("vsp_live_ingest_bitrate_bps", "{{stream}}")], unit="bps"),
    ts("Encode fps", [target("vsp_live_encode_fps", "{{stream}}")], unit="fps", thresholds=[(25, "red")]),
    ts("Pipeline backlog", [target("vsp_live_backlog_bytes", "{{stream}}")], unit="bytes", thresholds=[(8e6, "red")]),
    ts("Frames / dropped tags per second", [target("sum(rate(vsp_live_frames_total[1m])) by (stream)", "frames {{stream}}"), target("sum(rate(vsp_live_dropped_tags_total[1m])) by (stream)", "DROPPED {{stream}}")]),
    row("Packaging & delivery"),
    heatmap("Part publish latency distribution", "sum(increase(vsp_live_part_latency_seconds_bucket[30s])) by (le)"),
    ts("Parts / segments written per second", [target('rate(vsp_packager_parts_total{mode="live"}[1m])', "parts"), target('rate(vsp_packager_segments_total{mode="live"}[1m])', "segments")]),
    ts("Live segment duration (target 2 s)", [target(quant(0.5, "vsp_packager_segment_duration_seconds", 'mode="live"', window="5m"), "p50"), target(quant(0.99, "vsp_packager_segment_duration_seconds", 'mode="live"', window="5m"), "p99")], unit="s", min0=False),
    ts("Playlist requests & blocking holds", [target('sum(rate(vsp_media_requests_total{kind="playlist"}[1m]))', "playlist req/s"), target(quant(0.5, "vsp_hls_blocking_reload_hold_seconds"), "hold p50 (s)"), target(quant(0.99, "vsp_hls_blocking_reload_hold_seconds"), "hold p99 (s)")]),
    row("RTMP protocol"),
    ts("Messages by type", [target("sum(rate(vsp_rtmp_messages_total[1m])) by (type)", "{{type}}")], stack=True),
    ts("Errors by kind", [target("sum(increase(vsp_rtmp_errors_total[5m])) by (kind)", "{{kind}}")]),
    heatmap("Session duration", "sum(increase(vsp_live_session_duration_seconds_bucket[1h])) by (le)"),
    ts("Players (live) & reported latency", [target('vsp_player_sessions{mode="live"}', "players"), target(quant(0.9, "vsp_player_live_latency_seconds", window="5m"), "latency p90 (s)")]),
], ["live"], rng="now-15m")

jobs = dashboard("vsp-jobs", "VSP · Transcode jobs", [
    stat("Running", "vsp_jobs_running", w=3),
    stat("Queued", "vsp_jobs_queued", thresholds=[(3, "yellow"), (6, "red")], w=3),
    stat("Done (24h)", 'sum(increase(vsp_jobs_total{result="done"}[24h]))', w=3),
    stat("Failed (24h)", 'sum(increase(vsp_jobs_total{result="failed"}[24h]))', thresholds=[(1, "red")], w=3),
    stat("Realtime factor p50", quant(0.5, "vsp_job_realtime_factor", window="1h"), unit="none", thresholds=[(1, "yellow"), (2, "green")], w=3, desc="media seconds per wall second; <1 is slower than realtime"),
    stat("Transcode p95", quant(0.95, "vsp_job_duration_seconds", 'stage="transcode"', window="1h"), unit="s", w=3),
    stat("Pipelines", "sum(vsp_gst_pipelines) by (kind)", legend="{{kind}}", w=3),
    stat("NVENC", "max(nvidia_gpu_encoder_utilization_ratio)", unit="percentunit", w=3),
    row("Throughput"),
    ts("Jobs finished per hour", [target("sum(increase(vsp_jobs_total[1h])) by (result)", "{{result}}")], stack=True),
    ts("Queue depth / running", [target("vsp_jobs_queued", "queued"), target("vsp_jobs_running", "running")]),
    ts("Encode fps per job", [target("vsp_job_encode_fps", "{{job_id}}")], unit="fps"),
    heatmap("Realtime factor distribution", "sum(increase(vsp_job_realtime_factor_bucket[10m])) by (le)", unit="none"),
    row("Stages"),
    ts("Stage duration p50 / p95", [target(quant(0.5, "vsp_job_duration_seconds", by="stage", window="30m"), "p50 {{stage}}"), target(quant(0.95, "vsp_job_duration_seconds", by="stage", window="30m"), "p95 {{stage}}")], unit="s"),
    heatmap("Source duration", "sum(increase(vsp_job_source_duration_seconds_bucket[1h])) by (le)"),
    heatmap("Source size", "sum(increase(vsp_job_source_bytes_bucket[1h])) by (le)", unit="bytes"),
    ts("Failures by reason", [target("sum(increase(vsp_job_failures_total[1h])) by (reason)", "{{reason}}")]),
    row("Uploads & packaging"),
    ts("Upload throughput", [target("rate(vsp_upload_bytes_total[1m])*8", "bps")], unit="bps"),
    heatmap("Upload duration", "sum(increase(vsp_upload_duration_seconds_bucket[1h])) by (le)"),
    ts("VOD segments / parts per second", [target('rate(vsp_packager_segments_total{mode="vod"}[1m])', "segments"), target('rate(vsp_packager_parts_total{mode="vod"}[1m])', "parts")]),
    ts("Playlist write latency", [target(quant(0.5, "vsp_packager_playlist_write_seconds"), "p50"), target(quant(0.99, "vsp_packager_playlist_write_seconds"), "p99")], unit="s"),
], ["jobs"], rng="now-6h")

http = dashboard("vsp-http", "VSP · HTTP & delivery", [
    stat("req/s", "sum(rate(vsp_http_requests_total[1m]))", unit="reqps", w=3),
    stat("Connections", "vsp_http_connections", w=3),
    stat("SSE clients", "vsp_sse_clients", w=3),
    stat("Egress", "sum(rate(vsp_http_response_bytes_total[1m]))*8", unit="bps", w=3),
    stat("5xx rate", 'sum(rate(vsp_http_requests_total{status=~"5.."}[5m])) / sum(rate(vsp_http_requests_total[5m]))', unit="percentunit", thresholds=[(0.001, "yellow"), (0.01, "red")], w=3),
    stat("Segment p99", quant(0.99, "vsp_http_request_duration_seconds", SEG), unit="s", thresholds=[(0.25, "yellow"), (0.5, "red")], w=3),
    stat("Open FDs", 'process_open_fds{job="vsp-server"}', w=3),
    stat("Threads", 'process_threads{job="vsp-server"}', w=3),
    row("Requests"),
    ts("Requests by route", [target("sum(rate(vsp_http_requests_total[1m])) by (route)", "{{route}}")], unit="reqps", stack=True),
    ts("Requests by status", [target("sum(rate(vsp_http_requests_total[1m])) by (status)", "{{status}}")], unit="reqps", stack=True),
    ts("Latency p50 / p99 by route", [target(quant(0.5, "vsp_http_request_duration_seconds", by="route"), "p50 {{route}}"), target(quant(0.99, "vsp_http_request_duration_seconds", by="route"), "p99 {{route}}")], unit="s"),
    heatmap("Latency distribution (all routes)", "sum(increase(vsp_http_request_duration_seconds_bucket[30s])) by (le)"),
    row("Media delivery"),
    ts("Media requests by kind", [target("sum(rate(vsp_media_requests_total[1m])) by (kind)", "{{kind}}")], unit="reqps", stack=True),
    ts("Media bytes by kind", [target("sum(rate(vsp_media_bytes_total[1m])) by (kind)*8", "{{kind}}")], unit="bps", stack=True),
    ts("Blocking reload holds", [target(quant(p, "vsp_hls_blocking_reload_hold_seconds"), f"p{int(p*100)}") for p in (0.5, 0.95, 0.99)], unit="s"),
    ts("Blocking reload timeouts", [target("rate(vsp_hls_blocking_reload_timeouts_total[1m])", "timeouts/s")]),
    row("Process"),
    ts("CPU (cores)", [target('rate(process_cpu_seconds_total{job="vsp-server"}[1m])', "vsp-server")], unit="none", w=8),
    ts("Memory", [target('process_resident_memory_bytes{job="vsp-server"}', "RSS"), target('process_virtual_memory_bytes{job="vsp-server"}', "virtual")], unit="bytes", w=8),
    ts("FDs / threads", [target('process_open_fds{job="vsp-server"}', "fds"), target('process_threads{job="vsp-server"}', "threads")], w=8),
], ["http"])

player = dashboard("vsp-player", "VSP · Player QoE", [
    stat("Active players", "sum(vsp_player_sessions) by (mode)", legend="{{mode}}", w=4),
    stat("Startup p50", quant(0.5, "vsp_player_startup_seconds", window="15m"), unit="s", thresholds=[(1, "yellow"), (2, "red")], w=4),
    stat("Rebuffers / min", 'sum(rate(vsp_player_events_total{event="rebuffer"}[5m]))*60', thresholds=[(0.5, "yellow"), (2, "red")], w=4),
    stat("Live latency p90", quant(0.9, "vsp_player_live_latency_seconds", window="5m"), unit="s", thresholds=[(3, "yellow"), (5, "red")], w=4),
    stat("Errors (1h)", 'sum(increase(vsp_player_events_total{event=~"error|fatal"}[1h]))', thresholds=[(1, "red")], w=4),
    stat("Level switches / min", 'sum(rate(vsp_player_events_total{event="level_switch"}[5m]))*60', w=4),
    row("Experience"),
    ts("Events per minute", [target("sum(rate(vsp_player_events_total[5m])) by (event, mode)*60", "{{event}} ({{mode}})")], stack=True),
    heatmap("Startup time", "sum(increase(vsp_player_startup_seconds_bucket[5m])) by (le)"),
    heatmap("Live latency reported by players", "sum(increase(vsp_player_live_latency_seconds_bucket[5m])) by (le)"),
    heatmap("Rebuffer stall length", "sum(increase(vsp_player_rebuffer_seconds_bucket[5m])) by (le)"),
    row("Correlate with server"),
    ts("Server part latency vs player latency", [target(quant(0.95, "vsp_live_part_latency_seconds"), "server part p95"), target(quant(0.9, "vsp_player_live_latency_seconds", window="5m"), "player p90")], unit="s"),
    ts("Segment serving p99 vs rebuffers", [target(quant(0.99, "vsp_http_request_duration_seconds", SEG), "segment p99 (s)"), target('sum(rate(vsp_player_events_total{event="rebuffer"}[5m]))*60', "rebuffers/min")]),
], ["player"], refresh="10s")

host = dashboard("vsp-host", "VSP · Host & GPU", [
    stat("CPU", '1 - avg(rate(node_cpu_seconds_total{mode="idle"}[1m]))', unit="percentunit", thresholds=[(0.7, "yellow"), (0.9, "red")], w=3),
    stat("Load (1m)", "node_load1", w=3),
    stat("Mem used", "1 - node_memory_MemAvailable_bytes / node_memory_MemTotal_bytes", unit="percentunit", thresholds=[(0.8, "yellow"), (0.9, "red")], w=3),
    stat("Root disk free", 'node_filesystem_avail_bytes{mountpoint="/",fstype!="tmpfs"}', unit="bytes", w=3),
    stat("GPU util", "max(nvidia_gpu_utilization_ratio)", unit="percentunit", w=3),
    stat("NVENC util", "max(nvidia_gpu_encoder_utilization_ratio)", unit="percentunit", thresholds=[(0.8, "yellow"), (0.95, "red")], w=3),
    stat("NVDEC util", "max(nvidia_gpu_decoder_utilization_ratio)", unit="percentunit", w=3),
    stat("GPU temp", "max(nvidia_gpu_temperature_celsius)", unit="celsius", thresholds=[(75, "yellow"), (85, "red")], w=3),
    row("CPU & memory"),
    ts("CPU by mode", [target('sum(rate(node_cpu_seconds_total{mode!="idle"}[1m])) by (mode) / count(node_cpu_seconds_total{mode="idle"})', "{{mode}}")], unit="percentunit", stack=True),
    ts("Memory", [target("node_memory_MemTotal_bytes - node_memory_MemAvailable_bytes", "used"), target("node_memory_Cached_bytes", "cache"), target('process_resident_memory_bytes{job="vsp-server"}', "vsp-server")], unit="bytes"),
    row("GPU"),
    ts("GPU engines", [target("nvidia_gpu_utilization_ratio", "sm {{gpu}}"), target("nvidia_gpu_encoder_utilization_ratio", "nvenc {{gpu}}"), target("nvidia_gpu_decoder_utilization_ratio", "nvdec {{gpu}}"), target("nvidia_gpu_memory_utilization_ratio", "mem bw {{gpu}}")], unit="percentunit"),
    ts("GPU memory", [target("nvidia_gpu_memory_used_bytes", "used {{gpu}}"), target("nvidia_gpu_memory_total_bytes", "total {{gpu}}")], unit="bytes", w=6),
    ts("Power", [target("nvidia_gpu_power_watts", "draw {{gpu}}"), target("nvidia_gpu_power_limit_watts", "limit {{gpu}}")], unit="watt", w=6),
    ts("NVENC sessions & fps", [target("nvidia_gpu_encoder_sessions", "sessions {{gpu}}"), target("nvidia_gpu_encoder_average_fps", "avg fps {{gpu}}"), target("nvidia_gpu_encoder_average_latency_microseconds / 1000", "avg latency ms {{gpu}}")]),
    ts("GPU clocks", [target("nvidia_gpu_clock_sm_hertz", "sm {{gpu}}"), target("nvidia_gpu_clock_mem_hertz", "mem {{gpu}}")], unit="hertz"),
    row("Disk & network"),
    ts("Disk throughput", [target("sum(rate(node_disk_read_bytes_total[1m]))", "read"), target("sum(rate(node_disk_written_bytes_total[1m]))", "write")], unit="Bps"),
    ts("Disk IO time", [target("sum(rate(node_disk_io_time_seconds_total[1m])) by (device)", "{{device}}")], unit="percentunit"),
    ts("Network", [target('sum(rate(node_network_receive_bytes_total{device!~"lo|docker.*|veth.*|br-.*"}[1m]))*8', "rx"), target('sum(rate(node_network_transmit_bytes_total{device!~"lo|docker.*|veth.*|br-.*"}[1m]))*8', "tx")], unit="bps"),
    ts("Filesystem free", [target('node_filesystem_avail_bytes{fstype!~"tmpfs|overlay|squashfs"}', "{{mountpoint}}")], unit="bytes"),
], ["host"], rng="now-1h")

OUT.mkdir(parents=True, exist_ok=True)
for d in (overview, live, jobs, http, player, host):
    (OUT / f"{d['uid']}.json").write_text(json.dumps(d, indent=1) + "\n")
    print("wrote", (OUT / f"{d['uid']}.json").name, f"({len(d['panels'])} panels)")

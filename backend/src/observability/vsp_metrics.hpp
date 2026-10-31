#pragma once

// All VSP metric families in one place. Names follow Prometheus conventions
// (vsp_<subsystem>_<what>_<unit>). Grafana dashboards and alert rules in deploy/observability
// reference these names, so changing one here means updating them too.

#include "observability/metrics.hpp"

namespace vsp::metrics {

struct VspMetrics {
    // ---- HTTP
    HistogramFamily* http_request_duration;     // route, method, status_class
    CounterFamily* http_requests_total;         // route, method, status
    CounterFamily* http_response_bytes_total;   // route
    GaugeFamily* http_connections;              // (none)
    GaugeFamily* sse_clients;                   // (none)
    HistogramFamily* hls_block_hold;            // (none) seconds a blocking playlist reload was held
    CounterFamily* hls_block_timeouts_total;    // (none)
    CounterFamily* media_requests_total;        // kind = playlist|segment|init|other
    CounterFamily* media_bytes_total;           // kind
    CounterFamily* upload_bytes_total;          // (none)
    HistogramFamily* upload_duration;           // (none)

    // ---- transcode jobs
    GaugeFamily* jobs_queued;                   // (none)
    GaugeFamily* jobs_running;                  // (none)
    CounterFamily* jobs_total;                  // result = done|failed|cancelled
    HistogramFamily* job_duration;              // stage = probe|transcode|thumbnail|total
    HistogramFamily* job_realtime_factor;       // (none)  media seconds per wall second
    GaugeFamily* job_encode_fps;                // job_id
    CounterFamily* job_failures_total;          // reason
    HistogramFamily* job_source_bytes;          // (none)
    HistogramFamily* job_source_duration;       // (none) seconds

    // ---- live
    GaugeFamily* live_streams;                  // (none)
    GaugeFamily* live_ingest_bitrate;           // stream   bits/s
    GaugeFamily* live_encode_fps;               // stream
    GaugeFamily* live_backlog_bytes;            // stream
    CounterFamily* live_frames_total;           // stream
    CounterFamily* live_dropped_tags_total;     // stream
    HistogramFamily* live_part_latency;         // (none) seconds from first-frame capture to part published
    CounterFamily* live_sessions_total;         // result = ok|error|rejected
    HistogramFamily* live_session_duration;     // (none)
    CounterFamily* rtmp_bytes_total;            // (none)
    CounterFamily* rtmp_messages_total;         // type = audio|video|command|control|data
    GaugeFamily* rtmp_connections;              // (none)
    CounterFamily* rtmp_errors_total;           // kind

    // ---- packager
    CounterFamily* packager_segments_total;     // mode = vod|live
    CounterFamily* packager_parts_total;        // mode
    CounterFamily* packager_bytes_total;        // mode
    HistogramFamily* packager_segment_duration; // mode
    HistogramFamily* packager_write_duration;   // (none) fs write+rename of a playlist

    // ---- gstreamer
    GaugeFamily* gst_pipelines;                 // kind = transcode|live|probe|thumbnail
    CounterFamily* gst_errors_total;            // kind
    CounterFamily* gst_warnings_total;          // kind

    // ---- player QoE (pushed by clients)
    CounterFamily* player_events_total;         // event = start|rebuffer|level_switch|error|fatal
    HistogramFamily* player_startup_time;       // (none)
    HistogramFamily* player_live_latency;       // (none)
    HistogramFamily* player_rebuffer_duration;  // (none)
    GaugeFamily* player_sessions;               // mode = vod|live

    GaugeFamily* build_info;                    // version, gst_version, encoder

    static VspMetrics& get();
};

inline VspMetrics& M() { return VspMetrics::get(); }

// Collapses /media/<id>/v_720p/seg_000123.m4s -> /media/:id/:rendition/segment etc. so the
// route label stays low-cardinality.
std::string route_label(std::string_view path);
std::string media_kind(std::string_view path);

} // namespace vsp::metrics

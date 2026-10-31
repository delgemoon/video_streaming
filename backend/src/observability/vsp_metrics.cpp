#include "observability/vsp_metrics.hpp"

#include <gst/gst.h>

namespace vsp::metrics {

VspMetrics& VspMetrics::get() {
    static VspMetrics m = [] {
        auto& r = Registry::global();
        VspMetrics v{};
        v.http_request_duration = r.histogram("vsp_http_request_duration_seconds", "HTTP request latency", latency_buckets_s(),
                                              {"route", "method", "status_class"});
        v.http_requests_total = r.counter("vsp_http_requests_total", "HTTP requests", {"route", "method", "status"});
        v.http_response_bytes_total = r.counter("vsp_http_response_bytes_total", "HTTP response body bytes", {"route"});
        v.http_connections = r.gauge("vsp_http_connections", "Open HTTP connections");
        v.sse_clients = r.gauge("vsp_sse_clients", "Connected SSE clients");
        v.hls_block_hold = r.histogram("vsp_hls_blocking_reload_hold_seconds",
                                       "Time a _HLS_msn/_HLS_part blocking playlist reload was held", latency_buckets_s());
        v.hls_block_timeouts_total = r.counter("vsp_hls_blocking_reload_timeouts_total",
                                               "Blocking reloads that hit the timeout without the part arriving");
        v.media_requests_total = r.counter("vsp_media_requests_total", "Media file requests", {"kind"});
        v.media_bytes_total = r.counter("vsp_media_bytes_total", "Media bytes served", {"kind"});
        v.upload_bytes_total = r.counter("vsp_upload_bytes_total", "Bytes received in source uploads");
        v.upload_duration = r.histogram("vsp_upload_duration_seconds", "Source upload duration", duration_buckets_s());

        v.jobs_queued = r.gauge("vsp_jobs_queued", "Transcode jobs waiting");
        v.jobs_running = r.gauge("vsp_jobs_running", "Transcode jobs running");
        v.jobs_total = r.counter("vsp_jobs_total", "Finished transcode jobs", {"result"});
        v.job_duration = r.histogram("vsp_job_duration_seconds", "Transcode job stage duration", duration_buckets_s(), {"stage"});
        v.job_realtime_factor = r.histogram("vsp_job_realtime_factor", "Media seconds transcoded per wall-clock second",
                                            ratio_buckets());
        v.job_encode_fps = r.gauge("vsp_job_encode_fps", "Source frames per second being processed", {"job_id"});
        v.job_failures_total = r.counter("vsp_job_failures_total", "Transcode failures by reason", {"reason"});
        v.job_source_bytes = r.histogram("vsp_job_source_bytes", "Source file size", bytes_buckets());
        v.job_source_duration = r.histogram("vsp_job_source_duration_seconds", "Source media duration",
                                            {5, 15, 30, 60, 120, 300, 600, 1800, 3600, 7200});

        v.live_streams = r.gauge("vsp_live_streams", "Live streams currently ingesting");
        v.live_ingest_bitrate = r.gauge("vsp_live_ingest_bitrate_bps", "Inbound RTMP bitrate", {"stream"});
        v.live_encode_fps = r.gauge("vsp_live_encode_fps", "Live video frames per second through the pipeline", {"stream"});
        v.live_backlog_bytes = r.gauge("vsp_live_backlog_bytes", "Bytes queued ahead of the live pipeline", {"stream"});
        v.live_frames_total = r.counter("vsp_live_frames_total", "Live video frames processed", {"stream"});
        v.live_dropped_tags_total = r.counter("vsp_live_dropped_tags_total", "Media tags dropped under overload", {"stream"});
        v.live_part_latency = r.histogram("vsp_live_part_latency_seconds",
                                          "Ingest-to-published latency of LL-HLS parts (RTMP arrival of newest media to part on disk)", latency_buckets_s());
        v.live_sessions_total = r.counter("vsp_live_sessions_total", "Live publish sessions", {"result"});
        v.live_session_duration = r.histogram("vsp_live_session_duration_seconds", "Live session length",
                                              {10, 30, 60, 300, 900, 1800, 3600, 7200, 14400, 28800});
        v.rtmp_bytes_total = r.counter("vsp_rtmp_bytes_total", "Bytes received over RTMP");
        v.rtmp_messages_total = r.counter("vsp_rtmp_messages_total", "RTMP messages received", {"type"});
        v.rtmp_connections = r.gauge("vsp_rtmp_connections", "Open RTMP connections");
        v.rtmp_errors_total = r.counter("vsp_rtmp_errors_total", "RTMP protocol/handshake errors", {"kind"});

        v.packager_segments_total = r.counter("vsp_packager_segments_total", "CMAF segments closed", {"mode"});
        v.packager_parts_total = r.counter("vsp_packager_parts_total", "LL-HLS parts written", {"mode"});
        v.packager_bytes_total = r.counter("vsp_packager_bytes_total", "Bytes written by the packager", {"mode"});
        v.packager_segment_duration = r.histogram("vsp_packager_segment_duration_seconds", "Closed segment duration",
                                                  {0.5, 1, 1.5, 1.9, 2, 2.1, 2.5, 3, 4, 6}, {"mode"});
        v.packager_write_duration = r.histogram("vsp_packager_playlist_write_seconds", "Playlist write+rename latency",
                                                {0.0001, 0.00025, 0.0005, 0.001, 0.0025, 0.005, 0.01, 0.025, 0.05, 0.1});

        v.gst_pipelines = r.gauge("vsp_gst_pipelines", "Active GStreamer pipelines", {"kind"});
        v.gst_errors_total = r.counter("vsp_gst_errors_total", "GStreamer bus errors", {"kind"});
        v.gst_warnings_total = r.counter("vsp_gst_warnings_total", "GStreamer bus warnings", {"kind"});

        v.player_events_total = r.counter("vsp_player_events_total", "Player QoE events reported by clients", {"event", "mode"});
        v.player_startup_time = r.histogram("vsp_player_startup_seconds", "Time from load to first frame",
                                            {0.1, 0.25, 0.5, 0.75, 1, 1.5, 2, 3, 5, 10});
        v.player_live_latency = r.histogram("vsp_player_live_latency_seconds", "Live latency reported by players",
                                            {0.5, 1, 1.5, 2, 2.5, 3, 4, 5, 7.5, 10, 15, 30});
        v.player_rebuffer_duration = r.histogram("vsp_player_rebuffer_seconds", "Rebuffer stall length",
                                                 {0.1, 0.25, 0.5, 1, 2, 3, 5, 10});
        v.player_sessions = r.gauge("vsp_player_sessions", "Players that reported within the last interval", {"mode"});

        v.build_info = r.gauge("vsp_build_info", "Build information", {"version", "gst_version", "encoder"});
        return v;
    }();
    return m;
}

std::string route_label(std::string_view p) {
    if (p.starts_with("/media/")) {
        auto rest = p.substr(7);
        auto s1 = rest.find('/');
        if (s1 == std::string_view::npos) return "/media/:id";
        auto tail = rest.substr(s1 + 1);
        if (tail == "master.m3u8") return "/media/:id/master.m3u8";
        if (tail == "thumbnail.jpg") return "/media/:id/thumbnail.jpg";
        auto s2 = tail.find('/');
        if (s2 == std::string_view::npos) return "/media/:id/:file";
        auto file = tail.substr(s2 + 1);
        if (file == "index.m3u8") return "/media/:id/:rendition/index.m3u8";
        if (file == "init.mp4") return "/media/:id/:rendition/init.mp4";
        return "/media/:id/:rendition/segment";
    }
    if (p.starts_with("/api/videos/")) {
        auto rest = p.substr(12);
        auto s = rest.find('/');
        return s == std::string_view::npos ? "/api/videos/:id" : std::string("/api/videos/:id/") + std::string(rest.substr(s + 1));
    }
    if (p.starts_with("/api/jobs/")) {
        auto rest = p.substr(10);
        auto s = rest.find('/');
        return s == std::string_view::npos ? "/api/jobs/:id" : std::string("/api/jobs/:id/") + std::string(rest.substr(s + 1));
    }
    if (p.starts_with("/api/live/")) {
        auto rest = p.substr(10);
        auto s = rest.find('/');
        return s == std::string_view::npos ? "/api/live/:id" : std::string("/api/live/:id/") + std::string(rest.substr(s + 1));
    }
    if (p.starts_with("/api/") || p == "/metrics") return std::string(p);
    return "other";
}

std::string media_kind(std::string_view p) {
    if (p.ends_with(".m3u8")) return "playlist";
    if (p.ends_with("init.mp4")) return "init";
    if (p.ends_with(".m4s")) return "segment";
    if (p.ends_with(".jpg")) return "thumbnail";
    return "other";
}

} // namespace vsp::metrics

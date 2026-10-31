#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace vsp {

struct Config {
    std::string host = "0.0.0.0";
    uint16_t port = 8080;
    unsigned http_threads = 0;          // 0 = auto (hw_concurrency / 4, min 2)
    std::filesystem::path data_dir = "./data";
    std::string encoder = "auto";       // auto | nvenc | vaapi | x264
    unsigned concurrent_jobs = 0;       // 0 = auto (1 for hw encoders, 2 for x264)
    unsigned segment_duration_ms = 2000;
    unsigned part_duration_ms = 1000;   // 0 disables LL-HLS parts (segment == fragment)
    unsigned audio_bitrate_kbps = 128;
    std::string cors_origin = "*";
    std::string log_level = "info";
    std::string public_base_url;        // optional, used to build absolute media URLs

    // ---- live ingest (RTMP)
    bool rtmp_enabled = true;
    std::string rtmp_host = "0.0.0.0";
    uint16_t rtmp_port = 1935;
    std::string rtmp_app = "live";              // publishers push to rtmp://host:port/<rtmp_app>/<stream-key>
    unsigned live_segment_ms = 2000;
    unsigned live_part_ms = 500;
    unsigned live_window_segments = 10;         // sliding window when not recording
    bool live_passthrough = true;               // top rendition = source H.264 remuxed, no re-encode
    std::vector<std::string> live_renditions = {"720p", "480p", "360p"};   // encoded ABR tiers below source
    unsigned live_block_timeout_ms = 3000;      // max hold for _HLS_msn blocking reloads
    unsigned live_decode_hw = 1;                // prefer hardware decoder for the ABR branch

    std::filesystem::path uploads_dir() const { return data_dir / "uploads"; }
    std::filesystem::path media_dir() const { return data_dir / "media"; }
    std::filesystem::path db_path() const { return data_dir / "vsp.sqlite3"; }

    unsigned effective_http_threads() const;
    unsigned effective_jobs(bool hardware_encoder) const;

    static Config load(int argc, char** argv);
};

} // namespace vsp

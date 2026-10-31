#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

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

    std::filesystem::path uploads_dir() const { return data_dir / "uploads"; }
    std::filesystem::path media_dir() const { return data_dir / "media"; }
    std::filesystem::path db_path() const { return data_dir / "vsp.sqlite3"; }

    unsigned effective_http_threads() const;
    unsigned effective_jobs(bool hardware_encoder) const;

    static Config load(int argc, char** argv);
};

} // namespace vsp

#include "core/config.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <thread>

namespace vsp {

namespace {

template <typename T>
void read_json(const nlohmann::json& j, const char* key, T& out) {
    if (auto it = j.find(key); it != j.end() && !it->is_null()) out = it->get<T>();
}

void read_env(const char* name, std::string& out) {
    if (const char* v = std::getenv(name)) out = v;
}

template <typename Int>
void read_env_int(const char* name, Int& out) {
    if (const char* v = std::getenv(name)) out = static_cast<Int>(std::strtoul(v, nullptr, 10));
}

} // namespace

unsigned Config::effective_http_threads() const {
    if (http_threads) return http_threads;
    return std::max(2u, std::thread::hardware_concurrency() / 4);
}

unsigned Config::effective_jobs(bool hardware_encoder) const {
    if (concurrent_jobs) return concurrent_jobs;
    return hardware_encoder ? 1u : 2u;
}

Config Config::load(int argc, char** argv) {
    Config cfg;
    std::string config_path;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 < argc) return argv[++i];
            std::cerr << "missing value for " << a << "\n";
            std::exit(2);
        };
        if (a == "--config" || a == "-c") config_path = next();
        else if (a == "--port" || a == "-p") cfg.port = static_cast<uint16_t>(std::stoi(next()));
        else if (a == "--host") cfg.host = next();
        else if (a == "--data-dir" || a == "-d") cfg.data_dir = next();
        else if (a == "--encoder") cfg.encoder = next();
        else if (a == "--jobs") cfg.concurrent_jobs = std::stoul(next());
        else if (a == "--log-level") cfg.log_level = next();
        else if (a == "--rtmp-port") cfg.rtmp_port = static_cast<uint16_t>(std::stoi(next()));
        else if (a == "--no-rtmp") cfg.rtmp_enabled = false;
        else if (a == "--help" || a == "-h") {
            std::cout << "vsp-server [--config file.json] [--host H] [--port P] [--data-dir DIR]\n"
                         "           [--encoder auto|nvenc|vaapi|x264] [--jobs N] [--log-level LVL]\n"
                         "           [--rtmp-port P] [--no-rtmp]\n";
            std::exit(0);
        }
    }

    if (config_path.empty() && std::filesystem::exists("config.json")) config_path = "config.json";

    if (!config_path.empty()) {
        std::ifstream in(config_path);
        if (!in) {
            std::cerr << "cannot open config " << config_path << "\n";
            std::exit(2);
        }
        nlohmann::json j = nlohmann::json::parse(in, nullptr, true, true);
        read_json(j, "host", cfg.host);
        read_json(j, "port", cfg.port);
        read_json(j, "http_threads", cfg.http_threads);
        std::string data_dir;
        read_json(j, "data_dir", data_dir);
        if (!data_dir.empty()) cfg.data_dir = data_dir;
        read_json(j, "encoder", cfg.encoder);
        read_json(j, "concurrent_jobs", cfg.concurrent_jobs);
        read_json(j, "segment_duration_ms", cfg.segment_duration_ms);
        read_json(j, "part_duration_ms", cfg.part_duration_ms);
        read_json(j, "audio_bitrate_kbps", cfg.audio_bitrate_kbps);
        read_json(j, "cors_origin", cfg.cors_origin);
        read_json(j, "log_level", cfg.log_level);
        read_json(j, "public_base_url", cfg.public_base_url);
        read_json(j, "rtmp_enabled", cfg.rtmp_enabled);
        read_json(j, "rtmp_host", cfg.rtmp_host);
        read_json(j, "rtmp_port", cfg.rtmp_port);
        read_json(j, "rtmp_app", cfg.rtmp_app);
        read_json(j, "live_segment_ms", cfg.live_segment_ms);
        read_json(j, "live_part_ms", cfg.live_part_ms);
        read_json(j, "live_window_segments", cfg.live_window_segments);
        read_json(j, "live_passthrough", cfg.live_passthrough);
        read_json(j, "live_renditions", cfg.live_renditions);
        read_json(j, "live_block_timeout_ms", cfg.live_block_timeout_ms);
        read_json(j, "live_decode_hw", cfg.live_decode_hw);
    }

    read_env("VSP_HOST", cfg.host);
    read_env_int("VSP_PORT", cfg.port);
    std::string env_data;
    read_env("VSP_DATA_DIR", env_data);
    if (!env_data.empty()) cfg.data_dir = env_data;
    read_env("VSP_ENCODER", cfg.encoder);
    read_env_int("VSP_JOBS", cfg.concurrent_jobs);
    read_env("VSP_LOG_LEVEL", cfg.log_level);
    read_env_int("VSP_RTMP_PORT", cfg.rtmp_port);
    if (const char* v = std::getenv("VSP_RTMP_ENABLED")) cfg.rtmp_enabled = std::string(v) != "0" && std::string(v) != "false";

    if (cfg.part_duration_ms > cfg.segment_duration_ms) cfg.part_duration_ms = cfg.segment_duration_ms;
    if (cfg.live_part_ms > cfg.live_segment_ms) cfg.live_part_ms = cfg.live_segment_ms;
    cfg.data_dir = std::filesystem::absolute(cfg.data_dir);
    return cfg;
}

} // namespace vsp

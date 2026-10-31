#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>

namespace vsp {

struct ProbeResult {
    bool has_video = false;
    bool has_audio = false;
    int width = 0, height = 0;
    int fps_n = 0, fps_d = 1;
    int64_t duration_ms = 0;
    std::string video_codec, audio_codec;
    int audio_channels = 0, audio_rate = 0;
    std::string container;

    double fps() const { return fps_d > 0 && fps_n > 0 ? static_cast<double>(fps_n) / fps_d : 0.0; }
};

std::expected<ProbeResult, std::string> probe_file(const std::filesystem::path& path, int timeout_s = 30);

} // namespace vsp

#pragma once

#include "media/encoder_detect.hpp"
#include "media/probe.hpp"
#include "storage/models.hpp"

#include <atomic>
#include <expected>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace vsp {

struct TranscodeRequest {
    std::filesystem::path source;
    std::filesystem::path out_dir;        // media/<video_id>
    ProbeResult probe;
    std::vector<Rendition> renditions;    // from build_renditions()
    TranscodeOptions options;
    EncoderInfo encoder;
    std::string audio_encoder;
    unsigned segment_ms = 2000;
    unsigned part_ms = 1000;
};

struct TranscodeProgress {
    double fraction = 0;        // 0..1
    double fps = 0;             // source frames per second being processed
    int64_t position_ms = 0;
    bool master_ready = false;  // master.m3u8 exists, playback can start
};

struct TranscodeResult {
    std::vector<Rendition> renditions;   // with measured bandwidths and codec strings
    int64_t duration_ms = 0;
};

using ProgressFn = std::function<void(const TranscodeProgress&)>;

// Runs one single-pass, multi-rendition transcode. Blocks until finished, failed or cancelled.
std::expected<TranscodeResult, std::string> run_transcode(const TranscodeRequest& req, const ProgressFn& on_progress,
                                                          std::atomic<bool>& cancel);

// Writes master.m3u8 for the given renditions (bandwidths/codecs must already be populated).
void write_master_playlist(const std::filesystem::path& out_dir, const std::vector<Rendition>& renditions,
                           double fps);

} // namespace vsp

#pragma once

#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace vsp {

// Options the client chooses during pre-processing in the desktop app.
struct TranscodeOptions {
    int64_t trim_start_ms = 0;
    int64_t trim_end_ms = 0;                 // 0 = until end
    std::vector<std::string> renditions;     // e.g. {"1080p","720p"}; empty = auto ladder
    unsigned audio_bitrate_kbps = 0;         // 0 = server default
    bool keep_source = true;

    nlohmann::json to_json() const;
    static TranscodeOptions from_json(const nlohmann::json& j);
};

struct Rendition {
    std::string name;        // "1080p", "audio"
    std::string kind;        // "video" | "audio"
    int width = 0, height = 0;
    unsigned bitrate_kbps = 0;
    unsigned measured_bandwidth = 0;         // peak bits/s measured by the packager
    unsigned measured_avg_bandwidth = 0;
    std::string codecs;      // RFC 6381, e.g. avc1.640028
    std::string path;        // relative dir inside media/<video_id>/

    nlohmann::json to_json() const;
    static Rendition from_json(const nlohmann::json& j);
};

enum class VideoStatus { Created, Uploading, Uploaded, Queued, Processing, Ready, Failed };
std::string to_string(VideoStatus s);
VideoStatus video_status_from_string(const std::string& s);

struct Video {
    std::string id;
    std::string title;
    std::string description;
    std::string original_filename;
    std::string source_path;
    int64_t size_bytes = 0;
    VideoStatus status = VideoStatus::Created;
    int64_t duration_ms = 0;
    int width = 0, height = 0;
    double fps = 0;
    std::string video_codec, audio_codec;
    TranscodeOptions options;
    std::vector<Rendition> renditions;
    bool has_thumbnail = false;
    std::string encoder_used;
    std::string error;
    double progress = 0;
    int64_t created_at = 0, updated_at = 0;

    nlohmann::json to_json() const;
};

enum class LiveStatus { Idle, Live };
std::string to_string(LiveStatus s);
LiveStatus live_status_from_string(const std::string& s);

// A persistent ingest endpoint (stream key). Each time a publisher connects, a new recording id
// is minted; the live output lives in media/<current_video_id>/ and becomes a Video when recorded.
struct LiveStream {
    std::string id;
    std::string name;
    std::string stream_key;
    LiveStatus status = LiveStatus::Idle;
    bool record = true;
    std::vector<std::string> renditions;     // encoded ABR tiers; empty = server default
    std::string current_video_id;            // output dir / recording while live
    std::string last_video_id;               // most recent finished recording
    std::string publisher;                   // remote address while live
    int width = 0, height = 0;
    double fps = 0;
    std::string video_codec, audio_codec;
    std::vector<Rendition> live_renditions;  // as published in master.m3u8
    int64_t started_at = 0, ended_at = 0;
    int64_t created_at = 0, updated_at = 0;
    int sessions = 0;                        // number of times it went live
    std::string error;

    nlohmann::json to_json() const;
};

enum class JobStatus { Queued, Running, Done, Failed, Cancelled };
std::string to_string(JobStatus s);
JobStatus job_status_from_string(const std::string& s);

struct Job {
    std::string id;
    std::string video_id;
    std::string type = "transcode";
    JobStatus status = JobStatus::Queued;
    double progress = 0;
    std::string stage;        // probing | transcoding | finalizing
    double fps = 0;           // encode speed
    std::string encoder;
    std::string error;
    int64_t created_at = 0, started_at = 0, finished_at = 0;

    nlohmann::json to_json() const;
};

} // namespace vsp

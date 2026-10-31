#pragma once

// Live ingest pipeline: receives FLV tags (from the RTMP session) and produces an LL-HLS
// multi-rendition output in real time.
//
//   appsrc(FLV) → flvdemux ─┬─ video → h264parse → tee ─┬─ [passthrough] → mp4mux → packager "source"
//                           │                            └─ decode → videoconvert → tee ─┬─ scale → enc → mp4mux → packager "720p"
//                           │                                                           └─ …
//                           └─ audio → aacparse → mp4mux → packager "audio"   (non-AAC audio is re-encoded)
//
// Branch construction is deferred until the first video caps arrive so the ABR ladder can be derived
// from the actual source resolution. All GStreamer work happens on GStreamer threads; the RTMP
// session only calls push() (non-blocking).

#include "live/rtmp/sink.hpp"
#include "media/cmaf_packager.hpp"
#include "media/encoder_detect.hpp"
#include "storage/models.hpp"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

typedef struct _GstElement GstElement;
typedef struct _GstPad GstPad;
typedef struct _GstBuffer GstBuffer;

namespace vsp {

struct LivePipelineConfig {
    std::filesystem::path out_dir;
    std::string name;
    unsigned segment_ms = 2000;
    unsigned part_ms = 500;
    unsigned window_segments = 10;         // 0 = keep everything (recording)
    bool passthrough = true;
    std::vector<std::string> renditions;   // encoded tiers, e.g. {"720p","480p"}; filtered to < source height
    unsigned audio_bitrate_kbps = 128;
    EncoderInfo encoder;
    std::string audio_encoder;
    bool prefer_hw_decode = true;
    PlaylistListener on_playlist;
};

struct LiveInfo {
    int width = 0, height = 0;
    double fps = 0;
    std::string video_codec, audio_codec;
    bool has_audio = false;
    std::vector<Rendition> renditions;
    bool master_ready = false;
};

struct LiveStats {
    uint64_t bytes_in = 0;
    uint64_t video_frames = 0;
    uint64_t queued_bytes = 0;     // appsrc backlog (should stay near 0)
    double encode_fps = 0;
    double duration_s = 0;         // media time packaged so far
};

class LivePipeline : public rtmp::MediaSink, public std::enable_shared_from_this<LivePipeline> {
public:
    struct Branch;
    using StateFn = std::function<void(const LiveInfo&)>;         // master playlist published / info changed
    using EndFn = std::function<void(const std::string& error)>;  // pipeline stopped (empty = clean)

    explicit LivePipeline(LivePipelineConfig cfg);
    ~LivePipeline() override;

    void start(StateFn on_state, EndFn on_end);
    void stop();                      // graceful: EOS, finalize playlists
    LiveInfo info() const;
    LiveStats stats() const;
    std::vector<Rendition> final_renditions() const;   // after stop, with measured bandwidths
    double duration_s() const;

    // rtmp::MediaSink
    rtmp::MessageBuffer allocate(uint8_t type, uint32_t len) override;
    void on_media(uint8_t type, uint32_t timestamp_ms, rtmp::MessageBuffer buf) override;
    void on_data(uint32_t timestamp_ms, const uint8_t* amf, size_t len) override;
    void on_end(const std::string& reason) override;

private:
    void push_flv_header();
    void push_tag(GstBuffer* tag);
    void on_demux_pad(GstPad* pad);
    void on_video_caps(GstPad* parse_src);
    void on_audio_caps(GstPad* parse_src, bool is_aac);
    void build_video_branches(int width, int height, int fps_n, int fps_d);
    void build_audio_branch(bool passthrough_aac);
    Branch* add_mux_branch(const std::string& rendition_name, Rendition rendition, GstElement* upstream,
                           const char* mux_pad_template);
    void bus_loop();
    void publish_master(bool measured);
    void fail(const std::string& why);

    LivePipelineConfig cfg_;
    GstElement* pipeline_ = nullptr;
    GstElement* appsrc_ = nullptr;
    GstElement* flvdemux_ = nullptr;
    GstElement* video_tee_ = nullptr;
    GstElement* raw_tee_ = nullptr;
    std::vector<std::unique_ptr<Branch>> branches_;
    std::thread bus_thread_;
    StateFn on_state_;
    EndFn on_end_;

    mutable std::mutex mu_;
    LiveInfo info_;
    std::atomic<bool> started_{false}, stopping_{false}, ended_{false}, header_sent_{false}, metadata_sent_{false};
    std::atomic<bool> video_built_{false}, audio_built_{false}, video_is_h264_{false};
    std::atomic<uint64_t> bytes_in_{0}, frames_{0};
    std::atomic<double> fps_now_{0};
    std::string error_;
    std::unique_ptr<std::ofstream> dump_;
};

} // namespace vsp

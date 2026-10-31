#pragma once

#include <string>
#include <vector>

typedef struct _GstElement GstElement;

namespace vsp {

struct EncoderInfo {
    std::string id;           // nvenc | vaapi | qsv | x264 | openh264
    std::string element;      // GStreamer factory name
    std::string raw_format;   // preferred raw input format (NV12 / I420)
    bool hardware = false;
    bool usable = false;
    std::string note;
};

struct EncoderDetection {
    EncoderInfo video;                  // selected
    std::string audio_element;          // selected AAC encoder factory
    std::vector<EncoderInfo> candidates;
};

// Probes every known H.264 encoder by actually running a tiny pipeline, so a factory that exists
// but has no usable device (e.g. nvh264enc without a GPU) is correctly rejected.
EncoderDetection detect_encoders(const std::string& preference);

// Applies bitrate/GOP/latency settings in an encoder-specific way. Properties that don't exist
// on the installed plugin version are silently skipped.
void configure_video_encoder(GstElement* enc, const EncoderInfo& info, unsigned bitrate_kbps, int gop_frames,
                             bool low_latency = false);
void configure_audio_encoder(GstElement* enc, const std::string& element, unsigned bitrate_kbps);

} // namespace vsp

#pragma once

// Interfaces between the RTMP transport and the media side. Kept free of GStreamer types so the
// protocol layer can be unit-tested on its own.

#include "live/rtmp/amf0.hpp"
#include "live/rtmp/chunk.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace vsp::rtmp {

// Receives the demuxed media of one publishing session. All calls happen on the session's
// I/O thread and must not block.
class MediaSink {
public:
    virtual ~MediaSink() = default;

    // Storage for an audio (8) / video (9) message payload. `data` must point at `len` writable
    // bytes; the sink may reserve space around it (e.g. an FLV tag header) via `user`.
    virtual MessageBuffer allocate(uint8_t type, uint32_t len) = 0;
    // Payload fully received. Ownership of `buf` passes to the sink.
    virtual void on_media(uint8_t type, uint32_t timestamp_ms, MessageBuffer buf) = 0;
    // AMF0 data message (typically onMetaData). `amf` is the raw AMF0 payload, with any leading
    // "@setDataFrame" already stripped.
    virtual void on_data(uint32_t timestamp_ms, const uint8_t* amf, size_t len) = 0;
    // The publisher stopped (deleteStream/FCUnpublish), disconnected, or errored.
    virtual void on_end(const std::string& reason) = 0;
};

struct PublishRequest {
    std::string app;            // from connect(): e.g. "live"
    std::string stream_name;    // from publish(): the stream key, query string stripped
    std::string query;          // anything after '?' in the stream name
    std::string tc_url;
    std::string flash_version;
    std::string remote;         // client address
    std::string session_id;
    std::function<void()> close;   // thread-safe: asks the session to disconnect the publisher
};

class PublishHandler {
public:
    virtual ~PublishHandler() = default;
    // Return a sink to accept the publish, or nullptr (with `reject_reason` set) to refuse.
    virtual std::shared_ptr<MediaSink> on_publish(const PublishRequest& req, std::string& reject_reason) = 0;
};

struct SessionStats {
    uint64_t bytes_received = 0;
    uint64_t media_messages = 0;
    uint32_t last_video_ts = 0;
    uint32_t last_audio_ts = 0;
    double bitrate_bps = 0;     // smoothed inbound bitrate
};

} // namespace vsp::rtmp

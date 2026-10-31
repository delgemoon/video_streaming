#pragma once

// Glue between RTMP publishers and live pipelines: authenticates stream keys, runs one
// LivePipeline per active publisher, tracks state in the database, emits SSE events, and turns
// finished sessions into Video recordings.

#include "core/config.hpp"
#include "http/events.hpp"
#include "http/playlist_registry.hpp"
#include "live/live_pipeline.hpp"
#include "live/rtmp/sink.hpp"
#include "media/encoder_detect.hpp"
#include "observability/tracing.hpp"
#include "storage/database.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace vsp {

class LiveManager : public rtmp::PublishHandler {
public:
    LiveManager(const Config& cfg, Database& db, EventBroker& broker, const EncoderDetection& enc,
                PlaylistRegistry& playlists, boost::asio::io_context& ioc);
    ~LiveManager() override;

    void start();        // periodic stats publisher
    void shutdown();     // stop every active pipeline and wait

    // rtmp::PublishHandler
    std::shared_ptr<rtmp::MediaSink> on_publish(const rtmp::PublishRequest& req, std::string& reject_reason) override;

    // API helpers
    LiveStream create(const std::string& name, bool record, std::vector<std::string> renditions);
    bool stop(const std::string& stream_id);            // disconnect the publisher
    bool is_active(const std::string& stream_id) const;
    nlohmann::json live_stats(const std::string& stream_id) const;
    std::string ingest_url(const std::string& host, const LiveStream& s) const;

private:
    struct Active {
        std::shared_ptr<LivePipeline> pipeline;
        rtmp::PublishRequest request;
        std::string video_id;
        int64_t started_at = 0;
        std::shared_ptr<trace::Span> span;
        bool announced = false;
    };

    void on_state(const std::string& stream_id, const LiveInfo& info);
    void on_end(const std::string& stream_id, const std::string& error);
    void publish(const LiveStream& s);
    boost::asio::awaitable<void> stats_loop();

    const Config& cfg_;
    Database& db_;
    EventBroker& broker_;
    const EncoderDetection& enc_;
    PlaylistRegistry& playlists_;
    boost::asio::io_context& ioc_;

    mutable std::mutex mu_;
    std::unordered_map<std::string, Active> active_;   // stream id -> session
};

} // namespace vsp

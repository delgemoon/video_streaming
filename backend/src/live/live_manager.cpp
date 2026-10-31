#include "live/live_manager.hpp"

#include "core/util.hpp"

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <chrono>
#include <format>
#include <spdlog/spdlog.h>

namespace vsp {

namespace asio = boost::asio;
using namespace std::chrono_literals;

LiveManager::LiveManager(const Config& cfg, Database& db, EventBroker& broker, const EncoderDetection& enc,
                         PlaylistRegistry& playlists, asio::io_context& ioc)
    : cfg_(cfg), db_(db), broker_(broker), enc_(enc), playlists_(playlists), ioc_(ioc) {
    // Anything marked live in the DB from a previous run is stale.
    for (auto s : db_.list_live()) {
        if (s.status == LiveStatus::Live) {
            s.status = LiveStatus::Idle;
            s.publisher.clear();
            s.error = "server restarted while live";
            s.ended_at = util::now_ms();
            s.updated_at = s.ended_at;
            db_.update_live(s);
        }
    }
}

LiveManager::~LiveManager() { shutdown(); }

void LiveManager::start() { asio::co_spawn(ioc_, stats_loop(), asio::detached); }

void LiveManager::shutdown() {
    std::vector<std::shared_ptr<LivePipeline>> pipes;
    std::vector<rtmp::PublishRequest> reqs;
    {
        std::lock_guard lk(mu_);
        for (auto& [id, a] : active_) {
            pipes.push_back(a.pipeline);
            reqs.push_back(a.request);
        }
    }
    for (auto& r : reqs) if (r.close) r.close();
    for (auto& p : pipes) p->stop();
    for (int i = 0; i < 100; ++i) {
        {
            std::lock_guard lk(mu_);
            if (active_.empty()) break;
        }
        std::this_thread::sleep_for(50ms);
    }
}

std::string LiveManager::ingest_url(const std::string& host, const LiveStream& s) const {
    std::string h = host;
    if (!cfg_.public_base_url.empty()) {
        // strip scheme and port from the public URL
        auto p = cfg_.public_base_url.find("://");
        h = p == std::string::npos ? cfg_.public_base_url : cfg_.public_base_url.substr(p + 3);
        if (auto slash = h.find('/'); slash != std::string::npos) h = h.substr(0, slash);
        if (auto colon = h.rfind(':'); colon != std::string::npos && h.find(']') == std::string::npos) h = h.substr(0, colon);
    }
    if (h.empty()) h = "127.0.0.1";
    return std::format("rtmp://{}:{}/{}/{}", h, cfg_.rtmp_port, cfg_.rtmp_app, s.stream_key);
}

LiveStream LiveManager::create(const std::string& name, bool record, std::vector<std::string> renditions) {
    LiveStream s;
    s.id = util::generate_id();
    s.name = name.empty() ? "Live stream" : name;
    s.stream_key = util::generate_id().substr(0, 24);
    s.record = record;
    s.renditions = std::move(renditions);
    s.created_at = s.updated_at = util::now_ms();
    db_.insert_live(s);
    publish(s);
    return s;
}

bool LiveManager::is_active(const std::string& id) const {
    std::lock_guard lk(mu_);
    return active_.contains(id);
}

bool LiveManager::stop(const std::string& id) {
    rtmp::PublishRequest req;
    std::shared_ptr<LivePipeline> p;
    {
        std::lock_guard lk(mu_);
        auto it = active_.find(id);
        if (it == active_.end()) return false;
        req = it->second.request;
        p = it->second.pipeline;
    }
    if (req.close) req.close();   // closing the socket ends the session -> sink on_end -> pipeline stop
    return true;
}

nlohmann::json LiveManager::live_stats(const std::string& id) const {
    std::shared_ptr<LivePipeline> p;
    int64_t started = 0;
    {
        std::lock_guard lk(mu_);
        auto it = active_.find(id);
        if (it == active_.end()) return nullptr;
        p = it->second.pipeline;
        started = it->second.started_at;
    }
    auto st = p->stats();
    return {{"stream_id", id},
            {"bytes_in", st.bytes_in},
            {"video_frames", st.video_frames},
            {"encode_fps", st.encode_fps},
            {"queued_bytes", st.queued_bytes},
            {"duration_s", st.duration_s},
            {"uptime_s", (util::now_ms() - started) / 1000.0}};
}

void LiveManager::publish(const LiveStream& s) { broker_.publish("live.updated", s.to_json()); }

// ---------------------------------------------------------------------------------------------

std::shared_ptr<rtmp::MediaSink> LiveManager::on_publish(const rtmp::PublishRequest& req, std::string& reason) {
    if (!cfg_.rtmp_app.empty() && req.app != cfg_.rtmp_app) {
        reason = std::format("unknown application '{}' (expected '{}')", req.app, cfg_.rtmp_app);
        return nullptr;
    }
    auto stream = db_.get_live_by_key(req.stream_name);
    if (!stream) {
        reason = "unknown stream key";
        return nullptr;
    }
    {
        std::lock_guard lk(mu_);
        if (active_.contains(stream->id)) {
            reason = "stream is already live";
            return nullptr;
        }
    }

    const std::string video_id = util::generate_id();
    LivePipelineConfig pc;
    pc.out_dir = cfg_.media_dir() / video_id;
    pc.name = stream->name;
    pc.segment_ms = cfg_.live_segment_ms;
    pc.part_ms = cfg_.live_part_ms;
    pc.window_segments = stream->record ? 0 : cfg_.live_window_segments;
    pc.passthrough = cfg_.live_passthrough;
    pc.renditions = stream->renditions.empty() ? cfg_.live_renditions : stream->renditions;
    pc.audio_bitrate_kbps = cfg_.audio_bitrate_kbps;
    pc.encoder = enc_.video;
    pc.audio_encoder = enc_.audio_element;
    pc.prefer_hw_decode = cfg_.live_decode_hw != 0;
    pc.on_playlist = [this](const std::filesystem::path& p, const PlaylistUpdate& u) {
        if (u.final) playlists_.remove(p);
        else playlists_.publish(p, u);
    };

    std::shared_ptr<LivePipeline> pipeline;
    try {
        pipeline = std::make_shared<LivePipeline>(std::move(pc));
        const std::string sid = stream->id;
        pipeline->start([this, sid](const LiveInfo& info) { on_state(sid, info); },
                        [this, sid](const std::string& err) { on_end(sid, err); });
    } catch (const std::exception& e) {
        reason = std::string("cannot start pipeline: ") + e.what();
        spdlog::error("live '{}': {}", stream->name, reason);
        return nullptr;
    }

    {
        std::lock_guard lk(mu_);
        active_[stream->id] = Active{pipeline, req, video_id, util::now_ms()};
    }
    stream->status = LiveStatus::Live;
    stream->current_video_id = video_id;
    stream->publisher = req.remote;
    stream->started_at = util::now_ms();
    stream->ended_at = 0;
    stream->updated_at = stream->started_at;
    stream->sessions += 1;
    stream->error.clear();
    stream->width = stream->height = 0;
    stream->fps = 0;
    stream->live_renditions.clear();
    db_.update_live(*stream);
    publish(*stream);
    spdlog::info("live '{}' started by {} -> media/{}", stream->name, req.remote, video_id);
    return pipeline;
}

void LiveManager::on_state(const std::string& id, const LiveInfo& info) {
    auto s = db_.get_live(id);
    if (!s) return;
    s->width = info.width;
    s->height = info.height;
    s->fps = info.fps;
    s->video_codec = info.video_codec;
    s->audio_codec = info.audio_codec;
    s->live_renditions = info.renditions;
    s->updated_at = util::now_ms();
    db_.update_live(*s);
    publish(*s);
}

void LiveManager::on_end(const std::string& id, const std::string& error) {
    Active a;
    {
        std::lock_guard lk(mu_);
        auto it = active_.find(id);
        if (it == active_.end()) return;
        a = it->second;
        active_.erase(it);
    }
    auto s = db_.get_live(id);
    const int64_t now = util::now_ms();
    const auto out_dir = cfg_.media_dir() / a.video_id;
    std::error_code ec;

    // forget live playlists (they are VOD now, or about to be deleted)
    for (const auto& entry : std::filesystem::directory_iterator(out_dir, ec))
        if (entry.is_directory(ec)) playlists_.remove(entry.path() / "index.m3u8");

    const double duration = a.pipeline->duration_s();
    const bool record = s ? s->record : false;
    if (record && error.empty() && duration > 0.5) {
        Video v;
        v.id = a.video_id;
        v.title = std::format("{} — {}", s ? s->name : "Live", util::iso8601(a.started_at).substr(0, 16));
        v.description = "Live recording";
        v.original_filename = "live-recording";
        v.status = VideoStatus::Ready;
        v.duration_ms = static_cast<int64_t>(duration * 1000);
        auto info = a.pipeline->info();
        v.width = info.width;
        v.height = info.height;
        v.fps = info.fps;
        v.video_codec = info.video_codec;
        v.audio_codec = info.audio_codec;
        v.renditions = a.pipeline->final_renditions();
        v.encoder_used = cfg_.live_passthrough ? "passthrough+" + enc_.video.element : enc_.video.element;
        v.progress = 1.0;
        v.options.keep_source = false;
        v.created_at = a.started_at;
        v.updated_at = now;
        v.size_bytes = 0;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(out_dir, ec))
            if (entry.is_regular_file(ec)) v.size_bytes += static_cast<int64_t>(entry.file_size(ec));
        db_.insert_video(v);
        broker_.publish("video.updated", v.to_json());
        spdlog::info("live '{}' recorded as video {} ({:.1f}s)", s ? s->name : id, v.id, duration);
    } else {
        std::filesystem::remove_all(out_dir, ec);
    }

    if (s) {
        s->status = LiveStatus::Idle;
        s->publisher.clear();
        s->current_video_id.clear();
        if (record && error.empty() && duration > 0.5) s->last_video_id = a.video_id;
        s->ended_at = now;
        s->updated_at = now;
        s->error = error;
        db_.update_live(*s);
        publish(*s);
    }
    spdlog::info("live '{}' ended{} after {:.1f}s", s ? s->name : id, error.empty() ? "" : " (" + error + ")", duration);
}

asio::awaitable<void> LiveManager::stats_loop() {
    asio::steady_timer timer(co_await asio::this_coro::executor);
    for (;;) {
        timer.expires_after(2s);
        auto [ec] = co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));
        if (ec) co_return;
        std::vector<std::string> ids;
        {
            std::lock_guard lk(mu_);
            for (auto& [id, a] : active_) ids.push_back(id);
        }
        for (const auto& id : ids) {
            auto st = live_stats(id);
            if (!st.is_null()) broker_.publish("live.stats", st);
        }
    }
}

} // namespace vsp

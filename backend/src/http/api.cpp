#include "http/api.hpp"

#include "core/util.hpp"
#include "media/ladder.hpp"
#include "observability/vsp_metrics.hpp"

#include <fstream>
#include <memory>
#include <spdlog/spdlog.h>

namespace vsp {

namespace {

using json = nlohmann::json;

json parse_body(const RequestContext& ctx) {
    if (ctx.body.empty()) return json::object();
    auto j = json::parse(ctx.body, nullptr, false);
    if (j.is_discarded()) throw std::invalid_argument("invalid JSON body");
    return j;
}

json ladder_json() {
    json arr = json::array();
    for (const auto& s : default_ladder())
        arr.push_back({{"name", s.name}, {"height", s.height}, {"bitrate_kbps", s.bitrate_kbps}});
    return arr;
}

// Streams a request body straight to disk with a large buffer; no intermediate copies.
struct FileSink {
    std::filesystem::path path;
    std::ofstream out;
    std::unique_ptr<char[]> buf;
    explicit FileSink(std::filesystem::path p) : path(std::move(p)), buf(new char[1 << 20]) {
        out.rdbuf()->pubsetbuf(buf.get(), 1 << 20);
        out.open(path, std::ios::binary | std::ios::trunc);
    }
    bool ok() const { return static_cast<bool>(out); }
    bool write(const char* d, size_t n) {
        out.write(d, static_cast<std::streamsize>(n));
        return static_cast<bool>(out);
    }
    bool close() {
        out.close();
        return static_cast<bool>(out) || out.eof();
    }
};

} // namespace

void register_routes(Router& r, App& app) {
    // ---------- system ----------
    r.add_json(http::verb::get, "/api/health", [&](const RequestContext&) {
        return JsonResponse::ok({{"status", "ok"},
                                 {"version", VSP_VERSION},
                                 {"encoder", app.encoders.video.element},
                                 {"jobs_pending", app.jobs.pending()},
                                 {"jobs_running", app.jobs.running()},
                                 {"sse_clients", app.broker.subscriber_count()}});
    });

    r.add_json(http::verb::get, "/api/system", [&](const RequestContext&) {
        json cands = json::array();
        for (const auto& c : app.encoders.candidates)
            cands.push_back({{"id", c.id}, {"element", c.element}, {"hardware", c.hardware},
                             {"usable", c.usable}, {"note", c.note}});
        return JsonResponse::ok({{"version", VSP_VERSION},
                                 {"video_encoder", {{"id", app.encoders.video.id},
                                                    {"element", app.encoders.video.element},
                                                    {"hardware", app.encoders.video.hardware}}},
                                 {"audio_encoder", app.encoders.audio_element},
                                 {"candidates", cands},
                                 {"segment_duration_ms", app.cfg.segment_duration_ms},
                                 {"part_duration_ms", app.cfg.part_duration_ms},
                                 {"audio_bitrate_kbps", app.cfg.audio_bitrate_kbps},
                                 {"ladder", ladder_json()},
                                 {"observability", {{"metrics_path", "/metrics"},
                                                    {"tracing", !app.cfg.otlp_endpoint.empty()},
                                                    {"otlp_endpoint", app.cfg.otlp_endpoint},
                                                    {"otlp_endpoint_public", app.cfg.otlp_endpoint_public}}},
                                 {"live", {{"enabled", app.live != nullptr},
                                           {"rtmp_port", app.cfg.rtmp_port},
                                           {"rtmp_app", app.cfg.rtmp_app},
                                           {"segment_ms", app.cfg.live_segment_ms},
                                           {"part_ms", app.cfg.live_part_ms},
                                           {"passthrough", app.cfg.live_passthrough},
                                           {"default_renditions", app.cfg.live_renditions}}}});
    });

    // ---------- videos ----------
    r.add_json(http::verb::post, "/api/videos", [&](const RequestContext& ctx) {
        json body = parse_body(ctx);
        Video v;
        v.id = util::generate_id();
        v.title = util::trim(body.value("title", ""));
        v.description = body.value("description", "");
        v.original_filename = util::sanitize_filename(body.value("filename", "source.mp4"));
        if (v.title.empty()) v.title = v.original_filename;
        if (v.title.size() > 200) v.title.resize(200);
        v.size_bytes = body.value("size_bytes", int64_t{0});
        v.options = TranscodeOptions::from_json(body.value("options", json::object()));
        v.status = VideoStatus::Created;
        v.created_at = v.updated_at = util::now_ms();
        app.db.insert_video(v);
        app.broker.publish("video.updated", v.to_json());
        json out = v.to_json();
        out["upload_url"] = "/api/videos/" + v.id + "/source";
        return JsonResponse::created(out);
    });

    r.add_json(http::verb::get, "/api/videos", [&](const RequestContext& ctx) {
        std::optional<VideoStatus> st;
        if (auto s = ctx.query_or("status"); !s.empty()) st = video_status_from_string(s);
        int limit = std::clamp(std::stoi(ctx.query_or("limit", "200")), 1, 1000);
        json arr = json::array();
        for (const auto& v : app.db.list_videos(st, limit)) arr.push_back(v.to_json());
        return JsonResponse::ok({{"videos", arr}});
    });

    r.add_json(http::verb::get, "/api/videos/{id}", [&](const RequestContext& ctx) {
        auto v = app.db.get_video(ctx.params.at("id"));
        if (!v) return JsonResponse::error(http::status::not_found, "video not found");
        return JsonResponse::ok(v->to_json());
    });

    r.add_json(http::verb::patch, "/api/videos/{id}", [&](const RequestContext& ctx) {
        auto v = app.db.get_video(ctx.params.at("id"));
        if (!v) return JsonResponse::error(http::status::not_found, "video not found");
        json body = parse_body(ctx);
        if (body.contains("title")) v->title = util::trim(body["title"].get<std::string>());
        if (body.contains("description")) v->description = body["description"].get<std::string>();
        v->updated_at = util::now_ms();
        app.db.update_video(*v);
        app.broker.publish("video.updated", v->to_json());
        return JsonResponse::ok(v->to_json());
    });

    r.add_json(http::verb::delete_, "/api/videos/{id}", [&](const RequestContext& ctx) {
        const auto& id = ctx.params.at("id");
        auto v = app.db.get_video(id);
        if (!v) return JsonResponse::error(http::status::not_found, "video not found");
        std::error_code ec;
        std::filesystem::remove_all(app.cfg.media_dir() / id, ec);
        std::filesystem::remove_all(app.cfg.uploads_dir() / id, ec);
        app.db.delete_video(id);
        app.broker.publish("video.deleted", {{"id", id}});
        return JsonResponse::ok({{"deleted", id}});
    });

    r.add_json(http::verb::post, "/api/videos/{id}/transcode", [&](const RequestContext& ctx) {
        auto v = app.db.get_video(ctx.params.at("id"));
        if (!v) return JsonResponse::error(http::status::not_found, "video not found");
        if (v->source_path.empty() || !std::filesystem::exists(v->source_path))
            return JsonResponse::error(http::status::conflict, "source file not available (upload first)");
        if (v->status == VideoStatus::Processing || v->status == VideoStatus::Queued)
            return JsonResponse::error(http::status::conflict, "a transcode is already in progress");
        json body = parse_body(ctx);
        if (body.contains("options")) {
            v->options = TranscodeOptions::from_json(body["options"]);
            app.db.update_video(*v);
        }
        Job job = app.jobs.enqueue_transcode(v->id);
        return JsonResponse::ok({{"job", job.to_json()}, {"video", app.db.get_video(v->id)->to_json()}});
    });

    // ---------- upload (streaming) ----------
    r.add_upload(http::verb::put, "/api/videos/{id}/source", [&](const RequestContext& ctx)
                                                                 -> std::expected<UploadSink, JsonResponse> {
        auto v = app.db.get_video(ctx.params.at("id"));
        if (!v) return std::unexpected(JsonResponse::error(http::status::not_found, "video not found"));
        if (v->status == VideoStatus::Processing || v->status == VideoStatus::Queued)
            return std::unexpected(JsonResponse::error(http::status::conflict, "video is being processed"));

        auto dir = app.cfg.uploads_dir() / v->id;
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        auto sink = std::make_shared<FileSink>(dir / v->original_filename);
        if (!sink->ok())
            return std::unexpected(JsonResponse::error(http::status::internal_server_error, "cannot open upload target"));

        v->status = VideoStatus::Uploading;
        v->updated_at = util::now_ms();
        app.db.update_video(*v);
        app.broker.publish("video.updated", v->to_json());

        UploadSink us;
        us.write = [sink](const char* d, size_t n) { return sink->write(d, n); };
        us.abort = [sink, id = v->id, &app] {
            sink->close();
            std::error_code e;
            std::filesystem::remove(sink->path, e);
            if (auto vv = app.db.get_video(id)) {
                vv->status = VideoStatus::Created;
                vv->updated_at = util::now_ms();
                app.db.update_video(*vv);
                app.broker.publish("video.updated", vv->to_json());
            }
        };
        us.finish = [sink, id = v->id, &app](uint64_t total) {
            if (!sink->close())
                return JsonResponse::error(http::status::internal_server_error, "failed to flush upload");
            auto vv = app.db.get_video(id);
            if (!vv) return JsonResponse::error(http::status::not_found, "video vanished during upload");
            vv->source_path = sink->path.string();
            vv->size_bytes = static_cast<int64_t>(total);
            vv->status = VideoStatus::Uploaded;
            vv->updated_at = util::now_ms();
            app.db.update_video(*vv);
            Job job = app.jobs.enqueue_transcode(vv->id);
            spdlog::info("upload complete for video {} ({} bytes), job {}", id, total, job.id);
            return JsonResponse::ok({{"video", app.db.get_video(id)->to_json()}, {"job", job.to_json()}});
        };
        return us;
    });

    r.add_upload(http::verb::put, "/api/videos/{id}/thumbnail", [&](const RequestContext& ctx)
                                                                    -> std::expected<UploadSink, JsonResponse> {
        auto v = app.db.get_video(ctx.params.at("id"));
        if (!v) return std::unexpected(JsonResponse::error(http::status::not_found, "video not found"));
        auto dir = app.cfg.media_dir() / v->id;
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        auto sink = std::make_shared<FileSink>(dir / "thumbnail.jpg.upload");
        if (!sink->ok())
            return std::unexpected(JsonResponse::error(http::status::internal_server_error, "cannot open target"));
        UploadSink us;
        us.max_bytes = 10 * 1024 * 1024;
        us.write = [sink](const char* d, size_t n) { return sink->write(d, n); };
        us.abort = [sink] {
            sink->close();
            std::error_code e;
            std::filesystem::remove(sink->path, e);
        };
        us.finish = [sink, id = v->id, dir, &app](uint64_t) {
            sink->close();
            std::error_code e;
            std::filesystem::rename(sink->path, dir / "thumbnail.jpg", e);
            if (e) return JsonResponse::error(http::status::internal_server_error, "cannot store thumbnail");
            auto vv = app.db.get_video(id);
            if (!vv) return JsonResponse::error(http::status::not_found, "video not found");
            vv->has_thumbnail = true;
            vv->updated_at = util::now_ms();
            app.db.update_video(*vv);
            app.broker.publish("video.updated", vv->to_json());
            return JsonResponse::ok(vv->to_json());
        };
        return us;
    });

    // ---------- jobs ----------
    r.add_json(http::verb::get, "/api/jobs", [&](const RequestContext& ctx) {
        int limit = std::clamp(std::stoi(ctx.query_or("limit", "100")), 1, 1000);
        json arr = json::array();
        for (const auto& j : app.db.list_jobs(limit)) arr.push_back(j.to_json());
        return JsonResponse::ok({{"jobs", arr}});
    });

    r.add_json(http::verb::get, "/api/jobs/{id}", [&](const RequestContext& ctx) {
        auto j = app.db.get_job(ctx.params.at("id"));
        if (!j) return JsonResponse::error(http::status::not_found, "job not found");
        return JsonResponse::ok(j->to_json());
    });

    r.add_json(http::verb::post, "/api/jobs/{id}/cancel", [&](const RequestContext& ctx) {
        if (!app.jobs.cancel(ctx.params.at("id")))
            return JsonResponse::error(http::status::not_found, "job not active");
        return JsonResponse::ok({{"cancelled", ctx.params.at("id")}});
    });

    // ---------- live (RTMP ingest) ----------
    auto host_of = [](const RequestContext& ctx) {
        std::string h(ctx.header[http::field::host]);
        if (auto c = h.rfind(':'); c != std::string::npos && h.find(']') == std::string::npos) h = h.substr(0, c);
        return h;
    };
    auto live_json = [&, host_of](const RequestContext& ctx, const LiveStream& s) {
        json j = s.to_json();
        if (app.live) {
            j["ingest_url"] = app.live->ingest_url(host_of(ctx), s);
            j["ingest_server"] = app.live->ingest_url(host_of(ctx), s).substr(0, app.live->ingest_url(host_of(ctx), s).rfind('/'));
            j["active"] = app.live->is_active(s.id);
            j["stats"] = app.live->live_stats(s.id);
        }
        return j;
    };

    r.add_json(http::verb::get, "/api/live", [&, live_json](const RequestContext& ctx) {
        json arr = json::array();
        for (const auto& s : app.db.list_live()) arr.push_back(live_json(ctx, s));
        return JsonResponse::ok({{"streams", arr}, {"enabled", app.live != nullptr}});
    });

    r.add_json(http::verb::post, "/api/live", [&, live_json](const RequestContext& ctx) {
        if (!app.live) return JsonResponse::error(http::status::service_unavailable, "RTMP ingest is disabled");
        json body = parse_body(ctx);
        std::vector<std::string> rends;
        if (body.contains("renditions") && body["renditions"].is_array())
            for (const auto& x : body["renditions"]) if (x.is_string()) rends.push_back(x.get<std::string>());
        auto s = app.live->create(util::trim(body.value("name", "")), body.value("record", true), rends);
        return JsonResponse::created(live_json(ctx, s));
    });

    r.add_json(http::verb::get, "/api/live/{id}", [&, live_json](const RequestContext& ctx) {
        auto s = app.db.get_live(ctx.params.at("id"));
        if (!s) return JsonResponse::error(http::status::not_found, "live stream not found");
        return JsonResponse::ok(live_json(ctx, *s));
    });

    r.add_json(http::verb::patch, "/api/live/{id}", [&, live_json](const RequestContext& ctx) {
        auto s = app.db.get_live(ctx.params.at("id"));
        if (!s) return JsonResponse::error(http::status::not_found, "live stream not found");
        json body = parse_body(ctx);
        if (body.contains("name")) s->name = util::trim(body["name"].get<std::string>());
        if (body.contains("record")) s->record = body["record"].get<bool>();
        if (body.contains("renditions") && body["renditions"].is_array()) {
            s->renditions.clear();
            for (const auto& x : body["renditions"]) if (x.is_string()) s->renditions.push_back(x.get<std::string>());
        }
        s->updated_at = util::now_ms();
        app.db.update_live(*s);
        app.broker.publish("live.updated", s->to_json());
        return JsonResponse::ok(live_json(ctx, *s));
    });

    r.add_json(http::verb::post, "/api/live/{id}/key", [&, live_json](const RequestContext& ctx) {
        auto s = app.db.get_live(ctx.params.at("id"));
        if (!s) return JsonResponse::error(http::status::not_found, "live stream not found");
        if (app.live && app.live->is_active(s->id))
            return JsonResponse::error(http::status::conflict, "cannot rotate the key while live");
        s->stream_key = util::generate_id().substr(0, 24);
        s->updated_at = util::now_ms();
        app.db.update_live(*s);
        app.broker.publish("live.updated", s->to_json());
        return JsonResponse::ok(live_json(ctx, *s));
    });

    r.add_json(http::verb::post, "/api/live/{id}/stop", [&](const RequestContext& ctx) {
        if (!app.live || !app.live->stop(ctx.params.at("id")))
            return JsonResponse::error(http::status::conflict, "stream is not live");
        return JsonResponse::ok({{"stopping", ctx.params.at("id")}});
    });

    r.add_json(http::verb::delete_, "/api/live/{id}", [&](const RequestContext& ctx) {
        const auto& id = ctx.params.at("id");
        if (app.live && app.live->is_active(id))
            return JsonResponse::error(http::status::conflict, "stop the stream before deleting it");
        if (!app.db.delete_live(id)) return JsonResponse::error(http::status::not_found, "live stream not found");
        app.broker.publish("live.deleted", {{"id", id}});
        return JsonResponse::ok({{"deleted", id}});
    });

    // ---------- player QoE (pushed by clients, aggregated into Prometheus) ----------
    r.add_json(http::verb::post, "/api/metrics/player", [&](const RequestContext& ctx) {
        json body = parse_body(ctx);
        using namespace vsp::metrics;
        std::string mode = body.value("mode", "vod") == "live" ? "live" : "vod";
        if (body.contains("events") && body["events"].is_array()) {
            for (const auto& e : body["events"]) {
                std::string type = e.value("type", "");
                if (type.empty() || type.size() > 32) continue;
                M().player_events_total->with({type, mode})->inc();
                if (type == "startup" && e.contains("seconds")) M().player_startup_time->get()->observe(e.value("seconds", 0.0));
                if (type == "rebuffer" && e.contains("seconds")) M().player_rebuffer_duration->get()->observe(e.value("seconds", 0.0));
            }
        }
        if (mode == "live" && body.contains("latency_s")) M().player_live_latency->get()->observe(body.value("latency_s", 0.0));
        if (body.contains("active_sessions")) M().player_sessions->with({mode})->set(body.value("active_sessions", 0.0));
        return JsonResponse::ok({{"ok", true}});
    });

    // ---------- events + media ----------
    r.add_sse("/api/events");

    r.add_static("/media/{path...}", [&](const RequestContext& ctx) -> std::expected<std::filesystem::path, JsonResponse> {
        const auto& rel = ctx.params.at("path");
        if (!util::is_safe_relative_path(rel))
            return std::unexpected(JsonResponse::error(http::status::bad_request, "invalid path"));
        return app.cfg.media_dir() / rel;
    });
}

} // namespace vsp

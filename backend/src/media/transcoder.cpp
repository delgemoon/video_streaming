#include "media/transcoder.hpp"

#include "media/cmaf_packager.hpp"
#include "media/mxf_fixup.hpp"
#include "observability/vsp_metrics.hpp"

#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <set>
#include <format>
#include <fstream>
#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <memory>
#include <spdlog/spdlog.h>

namespace vsp {

namespace {

struct GstUnref {
    void operator()(GstElement* e) const { if (e) gst_object_unref(e); }
    void operator()(GstBus* b) const { if (b) gst_object_unref(b); }
    void operator()(GstCaps* c) const { if (c) gst_caps_unref(c); }
    void operator()(GstPad* p) const { if (p) gst_object_unref(p); }
};
template <typename T> using gptr = std::unique_ptr<T, GstUnref>;

struct Branch {
    Rendition rendition;
    std::unique_ptr<CmafPackager> packager;
    GstElement* appsink = nullptr;
};

struct Context {
    GstElement* pipeline = nullptr;
    GstElement* video_head = nullptr;   // sink pad target for the decoded video stream
    GstElement* audio_head = nullptr;
    std::atomic<bool> video_linked{false};
    std::atomic<bool> audio_linked{false};
    std::atomic<uint64_t> frames{0};
    std::vector<std::unique_ptr<Branch>> branches;
    std::string link_error;
};

GstElement* make(const char* factory, const std::string& name) {
    GstElement* e = gst_element_factory_make(factory, name.c_str());
    if (!e) throw std::runtime_error(std::format("missing GStreamer element '{}'", factory));
    return e;
}

void set_caps(GstElement* capsfilter, const std::string& caps_str) {
    gptr<GstCaps> caps(gst_caps_from_string(caps_str.c_str()));
    if (!caps) throw std::runtime_error("bad caps: " + caps_str);
    g_object_set(capsfilter, "caps", caps.get(), nullptr);
}

void link_or_throw(GstElement* a, GstElement* b) {
    if (!gst_element_link(a, b))
        throw std::runtime_error(std::format("cannot link {} -> {}", GST_ELEMENT_NAME(a), GST_ELEMENT_NAME(b)));
}

template <typename... E>
void link_chain(E... elements) {
    GstElement* arr[] = {elements...};
    for (size_t i = 0; i + 1 < sizeof...(E); ++i) link_or_throw(arr[i], arr[i + 1]);
}

GstFlowReturn on_new_sample(GstAppSink* sink, gpointer user) {
    auto* branch = static_cast<Branch*>(user);
    GstSample* sample = gst_app_sink_pull_sample(sink);
    if (!sample) return GST_FLOW_OK;
    if (GstBuffer* buf = gst_sample_get_buffer(sample)) {
        GstMapInfo map;
        if (gst_buffer_map(buf, &map, GST_MAP_READ)) {
            branch->packager->feed(map.data, map.size);
            gst_buffer_unmap(buf, &map);
        }
    }
    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

GstPadProbeReturn count_frames(GstPad*, GstPadProbeInfo*, gpointer user) {
    static_cast<Context*>(user)->frames.fetch_add(1, std::memory_order_relaxed);
    return GST_PAD_PROBE_OK;
}

struct TrimGate {
    std::mutex m;
    std::condition_variable cv;
    std::set<GstPad*> blocked;
};

GstPadProbeReturn on_gate_blocked(GstPad* pad, GstPadProbeInfo*, gpointer user) {
    auto* gate = static_cast<TrimGate*>(user);
    {
        std::lock_guard lk(gate->m);
        gate->blocked.insert(pad);
    }
    gate->cv.notify_all();
    return GST_PAD_PROBE_OK;   // stay blocked until the probe is removed
}

void on_pad_added(GstElement*, GstPad* pad, gpointer user) {
    auto* ctx = static_cast<Context*>(user);
    gptr<GstCaps> caps(gst_pad_get_current_caps(pad));
    if (!caps) caps.reset(gst_pad_query_caps(pad, nullptr));
    const gchar* name = caps ? gst_structure_get_name(gst_caps_get_structure(caps.get(), 0)) : "";
    GstElement* target = nullptr;
    std::atomic<bool>* flag = nullptr;
    if (g_str_has_prefix(name, "video/") && ctx->video_head) { target = ctx->video_head; flag = &ctx->video_linked; }
    else if (g_str_has_prefix(name, "audio/") && ctx->audio_head) { target = ctx->audio_head; flag = &ctx->audio_linked; }

    if (!target || flag->exchange(true)) {
        // Unused stream (subtitles, second audio track…): sink it so the demuxer doesn't stall.
        GstElement* fs = gst_element_factory_make("fakesink", nullptr);
        g_object_set(fs, "sync", FALSE, "async", FALSE, nullptr);
        gst_bin_add(GST_BIN(ctx->pipeline), fs);
        gst_element_sync_state_with_parent(fs);
        gptr<GstPad> sink(gst_element_get_static_pad(fs, "sink"));
        gst_pad_link(pad, sink.get());
        spdlog::debug("ignoring stream {}", name);
        return;
    }
    gptr<GstPad> sink(gst_element_get_static_pad(target, "sink"));
    GstPadLinkReturn r = gst_pad_link(pad, sink.get());
    if (GST_PAD_LINK_FAILED(r)) ctx->link_error = std::format("failed to link {} stream ({})", name, static_cast<int>(r));
    else spdlog::debug("linked {} stream", name);
}

std::string describe_error(GstMessage* msg) {
    GError* err = nullptr;
    gchar* dbg = nullptr;
    gst_message_parse_error(msg, &err, &dbg);
    std::string out = std::format("{}: {}", GST_OBJECT_NAME(msg->src), err ? err->message : "unknown");
    if (dbg) {
        spdlog::debug("gst debug: {}", dbg);
        g_free(dbg);
    }
    if (err) g_error_free(err);
    return out;
}

} // namespace

void write_master_playlist(const std::filesystem::path& out_dir, const std::vector<Rendition>& renditions,
                           double fps) {
    std::string pl = "#EXTM3U\n#EXT-X-VERSION:7\n#EXT-X-INDEPENDENT-SEGMENTS\n";
    const Rendition* audio = nullptr;
    for (const auto& r : renditions) if (r.kind == "audio") audio = &r;

    if (audio) {
        pl += std::format("#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"aac\",NAME=\"Audio\",LANGUAGE=\"und\",DEFAULT=YES,"
                          "AUTOSELECT=YES,CHANNELS=\"2\",URI=\"{}/index.m3u8\"\n", audio->path);
    }
    for (const auto& r : renditions) {
        if (r.kind != "video") continue;
        unsigned bw = r.measured_bandwidth ? r.measured_bandwidth : r.bitrate_kbps * 1000 * 12 / 10;
        unsigned avg = r.measured_avg_bandwidth ? r.measured_avg_bandwidth : r.bitrate_kbps * 1000;
        std::string codecs = r.codecs.empty() ? "avc1.64001F" : r.codecs;
        if (audio) {
            bw += audio->measured_bandwidth ? audio->measured_bandwidth : audio->bitrate_kbps * 1000;
            avg += audio->measured_avg_bandwidth ? audio->measured_avg_bandwidth : audio->bitrate_kbps * 1000;
            codecs += "," + (audio->codecs.empty() ? std::string("mp4a.40.2") : audio->codecs);
        }
        pl += std::format("#EXT-X-STREAM-INF:BANDWIDTH={},AVERAGE-BANDWIDTH={},RESOLUTION={}x{},CODECS=\"{}\"",
                          bw, avg, r.width, r.height, codecs);
        if (fps > 0) pl += std::format(",FRAME-RATE={:.3f}", fps);
        if (audio) pl += ",AUDIO=\"aac\"";
        pl += std::format("\n{}/index.m3u8\n", r.path);
    }
    auto tmp = out_dir / "master.m3u8.tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f << pl;
    }
    std::filesystem::rename(tmp, out_dir / "master.m3u8");
}

std::expected<TranscodeResult, std::string> run_transcode(const TranscodeRequest& req, const ProgressFn& on_progress,
                                                          std::atomic<bool>& cancel) {
    using namespace std::chrono;
    Context ctx;
    gptr<GstElement> pipeline(gst_pipeline_new("transcode"));
    ctx.pipeline = pipeline.get();
    install_mxf_h264_fixup(ctx.pipeline);
    metrics::M().gst_pipelines->with({"transcode"})->inc();
    struct PipeGauge { ~PipeGauge() { metrics::M().gst_pipelines->with({"transcode"})->dec(); } } pipe_gauge;

    const unsigned part_ms = (req.part_ms == 0 || req.part_ms > req.segment_ms) ? req.segment_ms : req.part_ms;
    double fps = req.probe.fps();
    int fps_n = req.probe.fps_n, fps_d = req.probe.fps_d;
    if (fps <= 0 || fps > 240) { fps = 30; fps_n = 30; fps_d = 1; }
    if (fps > 60) { fps = 60; fps_n = 60; fps_d = 1; }   // cap for encoder load; also keeps GOPs sane
    const int gop_frames = std::max(1, static_cast<int>(std::lround(fps * part_ms / 1000.0)));
    GstElement* src_element = nullptr;

    try {
        GstElement* src = make("uridecodebin3", "src");
        src_element = src;
        gchar* uri = gst_filename_to_uri(req.source.c_str(), nullptr);
        if (!uri) throw std::runtime_error("bad source path");
        g_object_set(src, "uri", uri, nullptr);
        g_free(uri);
        gst_bin_add(GST_BIN(ctx.pipeline), src);
        g_signal_connect(src, "pad-added", G_CALLBACK(on_pad_added), &ctx);

        // ---- shared video front-end: queue ! videorate ! videoconvert ! caps ! tee
        GstElement* vq = make("queue", "vq");
        GstElement* vrate = make("videorate", "vrate");
        GstElement* vconv = make("videoconvert", "vconv");
        GstElement* vcaps = make("capsfilter", "vcaps");
        GstElement* tee = make("tee", "vtee");
        g_object_set(vq, "max-size-buffers", 8u, "max-size-bytes", 0u, "max-size-time", guint64{0}, nullptr);
        g_object_set(vconv, "n-threads", 4u, nullptr);
        g_object_set(tee, "allow-not-linked", TRUE, nullptr);
        set_caps(vcaps, std::format("video/x-raw,format={},framerate={}/{}", req.encoder.raw_format, fps_n, fps_d));
        gst_bin_add_many(GST_BIN(ctx.pipeline), vq, vrate, vconv, vcaps, tee, nullptr);
        link_chain(vq, vrate, vconv, vcaps, tee);
        ctx.video_head = vq;
        {
            gptr<GstPad> teesink(gst_element_get_static_pad(tee, "sink"));
            gst_pad_add_probe(teesink.get(), GST_PAD_PROBE_TYPE_BUFFER, count_frames, &ctx, nullptr);
        }

        // ---- one encode branch per video rendition
        for (const auto& r : req.renditions) {
            if (r.kind != "video") continue;
            auto b = std::make_unique<Branch>();
            b->rendition = r;
            b->packager = std::make_unique<CmafPackager>(
                PackagerConfig{req.out_dir / r.path, req.segment_ms, part_ms, r.name});

            GstElement* q = make("queue", "q_" + r.name);
            GstElement* scale = make("videoscale", "scale_" + r.name);
            GstElement* scaps = make("capsfilter", "scaps_" + r.name);
            GstElement* enc = make(req.encoder.element.c_str(), "enc_" + r.name);
            GstElement* ecaps = make("capsfilter", "ecaps_" + r.name);
            GstElement* parse = make("h264parse", "parse_" + r.name);
            GstElement* pcaps = make("capsfilter", "pcaps_" + r.name);
            GstElement* mux = make("mp4mux", "mux_" + r.name);
            GstElement* sink = make("appsink", "sink_" + r.name);

            g_object_set(q, "max-size-buffers", 16u, "max-size-bytes", 0u, "max-size-time", guint64{0}, nullptr);
            g_object_set(scale, "method", 1, "add-borders", FALSE, "n-threads", 2u, nullptr);
            set_caps(scaps, std::format("video/x-raw,width={},height={},pixel-aspect-ratio=1/1", r.width, r.height));
            configure_video_encoder(enc, req.encoder, r.bitrate_kbps, gop_frames);
            // Ask for High profile when the encoder supports it (better compression); negotiation
            // falls back automatically otherwise because we don't fix the profile on h264parse.
            set_caps(ecaps, req.encoder.hardware || req.encoder.element == "x264enc"
                                ? "video/x-h264,profile=high" : "video/x-h264");
            set_caps(pcaps, "video/x-h264,stream-format=avc,alignment=au");
            g_object_set(mux, "fragment-duration", part_ms, "streamable", TRUE, nullptr);
            g_object_set(sink, "sync", FALSE, "async", FALSE, "max-buffers", 32u, "emit-signals", FALSE, nullptr);
            GstAppSinkCallbacks cbs{};
            cbs.new_sample = on_new_sample;
            gst_app_sink_set_callbacks(GST_APP_SINK(sink), &cbs, b.get(), nullptr);

            gst_bin_add_many(GST_BIN(ctx.pipeline), q, scale, scaps, enc, ecaps, parse, pcaps, mux, sink, nullptr);
            link_or_throw(tee, q);
            link_chain(q, scale, scaps, enc, ecaps, parse, pcaps);
            if (!gst_element_link_pads(pcaps, "src", mux, "video_%u"))
                throw std::runtime_error("cannot link h264 to mp4mux");
            link_or_throw(mux, sink);
            b->appsink = sink;
            ctx.branches.push_back(std::move(b));
        }

        // ---- audio branch
        for (const auto& r : req.renditions) {
            if (r.kind != "audio") continue;
            auto b = std::make_unique<Branch>();
            b->rendition = r;
            b->packager = std::make_unique<CmafPackager>(
                PackagerConfig{req.out_dir / r.path, req.segment_ms, part_ms, r.name});

            GstElement* aq = make("queue", "aq");
            GstElement* aconv = make("audioconvert", "aconv");
            GstElement* ares = make("audioresample", "ares");
            GstElement* acaps = make("capsfilter", "acaps");
            GstElement* aenc = make(req.audio_encoder.c_str(), "aenc");
            GstElement* aparse = make("aacparse", "aparse");
            GstElement* amux = make("mp4mux", "amux");
            GstElement* asink = make("appsink", "asink");

            g_object_set(aq, "max-size-buffers", 0u, "max-size-bytes", 0u, "max-size-time", guint64{3} * GST_SECOND, nullptr);
            set_caps(acaps, "audio/x-raw,rate=48000,channels=2");
            configure_audio_encoder(aenc, req.audio_encoder, r.bitrate_kbps);
            g_object_set(amux, "fragment-duration", part_ms, "streamable", TRUE, nullptr);
            g_object_set(asink, "sync", FALSE, "async", FALSE, "max-buffers", 32u, "emit-signals", FALSE, nullptr);
            GstAppSinkCallbacks cbs{};
            cbs.new_sample = on_new_sample;
            gst_app_sink_set_callbacks(GST_APP_SINK(asink), &cbs, b.get(), nullptr);

            gst_bin_add_many(GST_BIN(ctx.pipeline), aq, aconv, ares, acaps, aenc, aparse, amux, asink, nullptr);
            link_chain(aq, aconv, ares, acaps, aenc, aparse);
            if (!gst_element_link_pads(aparse, "src", amux, "audio_%u"))
                throw std::runtime_error("cannot link aac to mp4mux");
            link_or_throw(amux, asink);
            ctx.audio_head = aq;
            b->appsink = asink;
            ctx.branches.push_back(std::move(b));
            break;
        }
    } catch (const std::exception& e) {
        gst_element_set_state(ctx.pipeline, GST_STATE_NULL);
        return std::unexpected(std::string("pipeline build failed: ") + e.what());
    }

    spdlog::info("transcode start: {} renditions, encoder {}, gop {} frames, segment {}ms, part {}ms",
                 ctx.branches.size(), req.encoder.element, gop_frames, req.segment_ms, part_ms);

    gptr<GstBus> bus(gst_element_get_bus(ctx.pipeline));
    auto fail = [&](const std::string& why) -> std::expected<TranscodeResult, std::string> {
        gst_element_set_state(ctx.pipeline, GST_STATE_NULL);
        return std::unexpected(why);
    };

    const int64_t total_ms = req.probe.duration_ms;
    int64_t start_ms = std::clamp<int64_t>(req.options.trim_start_ms, 0, total_ms);
    int64_t end_ms = req.options.trim_end_ms > 0 ? std::clamp<int64_t>(req.options.trim_end_ms, 0, total_ms) : total_ms;
    if (end_ms <= start_ms) return fail("invalid trim range");
    const bool trimming = start_ms > 0 || end_ms < total_ms;
    const int64_t span_ms = end_ms - start_ms;

    // Trimming strategy: block the decoded streams right after the decoder, seek the *source*
    // (never through the muxers, whose segments are in BYTES), then release the blocks so the
    // encoders/muxers only ever see post-seek data starting at running time 0.
    TrimGate gate;
    std::vector<std::pair<GstPad*, gulong>> gate_probes;
    if (trimming) {
        for (GstElement* head : {ctx.video_head, ctx.audio_head}) {
            if (!head) continue;
            GstPad* pad = gst_element_get_static_pad(head, "sink");
            gulong id = gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BLOCK_DOWNSTREAM, on_gate_blocked, &gate, nullptr);
            gate_probes.emplace_back(pad, id);
        }
    }

    if (gst_element_set_state(ctx.pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
        return fail("pipeline refused to start");

    if (trimming) {
        {
            std::unique_lock lk(gate.m);
            bool ready = gate.cv.wait_for(lk, 60s, [&] { return gate.blocked.size() >= gate_probes.size(); });
            if (!ready) {
                for (auto& [pad, id] : gate_probes) { gst_pad_remove_probe(pad, id); gst_object_unref(pad); }
                GstMessage* m = gst_bus_pop_filtered(bus.get(), GST_MESSAGE_ERROR);
                std::string why = m ? describe_error(m) : "source did not start producing streams";
                if (m) gst_message_unref(m);
                return fail(why);
            }
        }
        if (!ctx.link_error.empty()) {
            for (auto& [pad, id] : gate_probes) { gst_pad_remove_probe(pad, id); gst_object_unref(pad); }
            return fail(ctx.link_error);
        }
        bool ok = gst_element_seek(src_element, 1.0, GST_FORMAT_TIME,
                                   static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE),
                                   GST_SEEK_TYPE_SET, start_ms * GST_MSECOND,
                                   end_ms < total_ms ? GST_SEEK_TYPE_SET : GST_SEEK_TYPE_NONE,
                                   end_ms < total_ms ? end_ms * GST_MSECOND : GST_CLOCK_TIME_NONE);
        for (auto& [pad, id] : gate_probes) { gst_pad_remove_probe(pad, id); gst_object_unref(pad); }
        if (!ok) return fail("trim seek rejected by source (not seekable?)");
        spdlog::info("trim applied: {}ms -> {}ms", start_ms, end_ms);
    }

    bool master_written = false;
    auto t0 = steady_clock::now();
    auto last_report = t0;
    uint64_t last_frames = 0;
    double fps_now = 0;
    uint64_t stall_frames = 0;
    auto stall_since = t0;

    auto all_inits_ready = [&] {
        for (const auto& b : ctx.branches) if (!b->packager->has_init()) return false;
        return true;
    };
    auto publish_master = [&](bool measured) {
        std::vector<Rendition> rends;
        for (const auto& b : ctx.branches) {
            Rendition r = b->rendition;
            auto st = b->packager->stats();
            r.codecs = st.codec;
            if (measured) {
                r.measured_bandwidth = st.peak_bandwidth;
                r.measured_avg_bandwidth = st.avg_bandwidth;
            }
            rends.push_back(r);
        }
        write_master_playlist(req.out_dir, rends, fps);
        return rends;
    };

    std::string error;
    bool eos = false;
    while (!eos && error.empty()) {
        GstMessage* msg = gst_bus_timed_pop_filtered(
            bus.get(), 250 * GST_MSECOND,
            static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR | GST_MESSAGE_WARNING));
        if (msg) {
            switch (GST_MESSAGE_TYPE(msg)) {
            case GST_MESSAGE_EOS: eos = true; break;
            case GST_MESSAGE_ERROR:
                error = describe_error(msg);
                metrics::M().gst_errors_total->with({"transcode"})->inc();
                break;
            case GST_MESSAGE_WARNING: {
                GError* w = nullptr;
                gst_message_parse_warning(msg, &w, nullptr);
                metrics::M().gst_warnings_total->with({"transcode"})->inc();
                spdlog::warn("gst warning from {}: {}", GST_OBJECT_NAME(msg->src), w ? w->message : "?");
                if (w) g_error_free(w);
                break;
            }
            default: break;
            }
            gst_message_unref(msg);
        }
        if (cancel.load()) { error = "cancelled"; break; }
        if (!ctx.link_error.empty()) { error = ctx.link_error; break; }

        auto now = steady_clock::now();
        {
            uint64_t f = ctx.frames.load(std::memory_order_relaxed);
            if (f != stall_frames) { stall_frames = f; stall_since = now; }
            else if (now - stall_since > 90s) { error = "pipeline stalled (no frames for 90s)"; break; }
        }
        if (now - last_report >= 500ms || eos) {
            if (!master_written && all_inits_ready()) {
                publish_master(false);
                master_written = true;
                spdlog::info("master playlist published (playable while transcoding)");
            }
            gint64 pos = 0;
            if (gst_element_query_position(ctx.pipeline, GST_FORMAT_TIME, &pos) && pos >= 0) {
                int64_t pos_ms = pos / GST_MSECOND;
                double elapsed = duration_cast<duration<double>>(now - last_report).count();
                uint64_t f = ctx.frames.load(std::memory_order_relaxed);
                if (elapsed > 0) fps_now = static_cast<double>(f - last_frames) / elapsed;
                last_frames = f;
                TranscodeProgress p;
                p.position_ms = pos_ms;
                p.fraction = span_ms > 0 ? std::clamp(static_cast<double>(pos_ms - start_ms) / span_ms, 0.0, 1.0) : 0;
                p.fps = fps_now;
                p.master_ready = master_written;
                if (on_progress) on_progress(p);
            }
            last_report = now;
        }
    }

    gst_element_set_state(ctx.pipeline, GST_STATE_NULL);
    for (auto& b : ctx.branches) b->packager->finish();
    if (!error.empty()) return std::unexpected(error);

    TranscodeResult result;
    result.renditions = publish_master(true);
    double dur = 0;
    for (const auto& b : ctx.branches) dur = std::max(dur, b->packager->stats().total_duration_s);
    result.duration_ms = static_cast<int64_t>(dur * 1000);
    auto secs = duration_cast<duration<double>>(steady_clock::now() - t0).count();
    spdlog::info("transcode done in {:.1f}s ({:.2f}x realtime)", secs, secs > 0 ? span_ms / 1000.0 / secs : 0.0);
    return result;
}

} // namespace vsp

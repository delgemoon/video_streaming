#include "live/live_pipeline.hpp"

#include "core/util.hpp"
#include "media/gst_util.hpp"
#include "media/ladder.hpp"
#include "media/transcoder.hpp"
#include "observability/vsp_metrics.hpp"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <format>
#include <fstream>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <gst/pbutils/pbutils.h>
#include <spdlog/spdlog.h>

namespace vsp {

using namespace std::chrono_literals;

// One muxed output (rendition) feeding a packager.
struct LivePipeline::Branch {
    Rendition rendition;
    std::unique_ptr<CmafPackager> packager;
    GstElement* appsink = nullptr;
    bool passthrough = false;
};

namespace {

constexpr size_t kFlvTagHeader = 11;
constexpr size_t kFlvPrevSize = 4;
constexpr uint64_t kBacklogDropBytes = 24ull * 1024 * 1024;   // start dropping when appsrc backlog exceeds this
constexpr uint64_t kBacklogResumeBytes = 4ull * 1024 * 1024;

struct TagAlloc {
    GstBuffer* buffer = nullptr;
    GstMapInfo map{};
};

GstFlowReturn on_new_sample(GstAppSink* sink, gpointer user) {
    auto* branch = static_cast<LivePipeline::Branch*>(user);
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

void put24(uint8_t* p, uint32_t v) { p[0] = (v >> 16) & 0xff; p[1] = (v >> 8) & 0xff; p[2] = v & 0xff; }
void put32(uint8_t* p, uint32_t v) { p[0] = v >> 24; put24(p + 1, v & 0xffffff); }

std::string caps_name(GstPad* pad) {
    gst::ptr<GstCaps> caps(gst_pad_get_current_caps(pad));
    if (!caps) caps.reset(gst_pad_query_caps(pad, nullptr));
    if (!caps || gst_caps_is_empty(caps.get()) || gst_caps_is_any(caps.get())) return {};
    return gst_structure_get_name(gst_caps_get_structure(caps.get(), 0));
}

} // namespace

// ---------------------------------------------------------------------------------------------

LivePipeline::LivePipeline(LivePipelineConfig cfg) : cfg_(std::move(cfg)) {
    if (cfg_.part_ms == 0 || cfg_.part_ms > cfg_.segment_ms) cfg_.part_ms = cfg_.segment_ms;
    std::filesystem::create_directories(cfg_.out_dir);
}

LivePipeline::~LivePipeline() {
    stopping_ = true;
    if (pipeline_) gst_element_set_state(pipeline_, GST_STATE_NULL);
    if (bus_thread_.joinable()) {
        // The bus thread owns a shared_ptr to us; if it is the one releasing the last reference,
        // it cannot join itself.
        if (bus_thread_.get_id() == std::this_thread::get_id()) bus_thread_.detach();
        else bus_thread_.join();
    }
    if (pipeline_) gst_object_unref(pipeline_);
}

LiveInfo LivePipeline::info() const {
    std::lock_guard lk(mu_);
    return info_;
}

double LivePipeline::duration_s() const {
    double d = 0;
    std::lock_guard lk(mu_);
    for (const auto& b : branches_) d = std::max(d, b->packager->stats().total_duration_s);
    return d;
}

LiveStats LivePipeline::stats() const {
    LiveStats s;
    s.bytes_in = bytes_in_.load();
    s.video_frames = frames_.load();
    s.encode_fps = fps_now_.load();
    if (appsrc_) {
        guint64 lvl = 0;
        g_object_get(appsrc_, "current-level-bytes", &lvl, nullptr);
        s.queued_bytes = lvl;
    }
    s.duration_s = duration_s();
    return s;
}

std::vector<Rendition> LivePipeline::final_renditions() const {
    std::lock_guard lk(mu_);
    std::vector<Rendition> out;
    for (const auto& b : branches_) {
        Rendition r = b->rendition;
        auto st = b->packager->stats();
        r.codecs = st.codec;
        r.measured_bandwidth = st.peak_bandwidth;
        r.measured_avg_bandwidth = st.avg_bandwidth;
        out.push_back(r);
    }
    return out;
}

// ---------------------------------------------------------------------------------------------
// start / stop

void LivePipeline::start(StateFn on_state, EndFn on_end) {
    on_state_ = std::move(on_state);
    on_end_ = std::move(on_end);

    pipeline_ = gst_pipeline_new(("live-" + cfg_.name).c_str());
    appsrc_ = gst::make("appsrc", "src");
    flvdemux_ = gst::make("flvdemux", "demux");

    gst::ptr<GstCaps> caps(gst_caps_new_empty_simple("video/x-flv"));
    g_object_set(appsrc_, "caps", caps.get(), "stream-type", GST_APP_STREAM_TYPE_STREAM, "is-live", TRUE,
                 "format", GST_FORMAT_BYTES, "do-timestamp", FALSE, "block", FALSE, "max-bytes", guint64{0},
                 "emit-signals", FALSE, nullptr);
    gst_bin_add_many(GST_BIN(pipeline_), appsrc_, flvdemux_, nullptr);
    gst::link_or_throw(appsrc_, flvdemux_);

    g_signal_connect(flvdemux_, "pad-added",
                     G_CALLBACK(+[](GstElement*, GstPad* pad, gpointer self) {
                         static_cast<LivePipeline*>(self)->on_demux_pad(pad);
                     }),
                     this);

    if (gst_element_set_state(pipeline_, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
        throw std::runtime_error("live pipeline refused to start");
    started_ = true;
    metrics::M().gst_pipelines->with({"live"})->inc();
    bus_thread_ = std::thread([self = shared_from_this()] { self->bus_loop(); });
    spdlog::info("[live {}] pipeline started (segment {}ms, part {}ms, window {}, passthrough {})", cfg_.name,
                 cfg_.segment_ms, cfg_.part_ms, cfg_.window_segments, cfg_.passthrough);
}

void LivePipeline::stop() {
    if (!started_ || stopping_.exchange(true)) return;
    spdlog::info("[live {}] stopping", cfg_.name);
    if (appsrc_) gst_app_src_end_of_stream(GST_APP_SRC(appsrc_));
}

void LivePipeline::fail(const std::string& why) {
    {
        std::lock_guard lk(mu_);
        if (error_.empty()) error_ = why;
    }
    stopping_ = true;
}

// ---------------------------------------------------------------------------------------------
// rtmp::MediaSink — called on the RTMP I/O thread

rtmp::MessageBuffer LivePipeline::allocate(uint8_t, uint32_t len) {
    auto* a = new TagAlloc;
    a->buffer = gst_buffer_new_allocate(nullptr, kFlvTagHeader + len + kFlvPrevSize, nullptr);
    gst_buffer_map(a->buffer, &a->map, GST_MAP_WRITE);
    return rtmp::MessageBuffer{a->map.data + kFlvTagHeader, len, a};
}

void LivePipeline::on_media(uint8_t type, uint32_t ts, rtmp::MessageBuffer buf) {
    std::unique_ptr<TagAlloc> a(static_cast<TagAlloc*>(buf.user));
    uint8_t* p = a->map.data;
    p[0] = type;
    put24(p + 1, buf.length);
    put24(p + 4, ts & 0xffffff);
    p[7] = (ts >> 24) & 0xff;
    put24(p + 8, 0);
    put32(p + kFlvTagHeader + buf.length, static_cast<uint32_t>(kFlvTagHeader + buf.length));
    gst_buffer_unmap(a->buffer, &a->map);
    bytes_in_ += buf.length;
    if (type == 9) {
        const int64_t now = util::now_ms();
        std::lock_guard lk(mu_);
        for (auto& b : branches_) b->packager->note_input_arrival(now);
    }

    // Overload protection: if the encoders can't keep up, drop whole tags until the next keyframe
    // rather than letting the backlog (and latency) grow without bound.
    static thread_local bool dropping = false;
    if (appsrc_) {
        guint64 lvl = 0;
        g_object_get(appsrc_, "current-level-bytes", &lvl, nullptr);
        if (!dropping && lvl > kBacklogDropBytes) {
            dropping = true;
            spdlog::warn("[live {}] encoder backlog {} MiB, dropping until next keyframe", cfg_.name, lvl >> 20);
        }
        if (dropping) {
            bool key = type == 9 && buf.length > 0 && ((buf.data[0] >> 4) & 0x7) == 1;
            if (key && lvl < kBacklogResumeBytes) dropping = false;
            else {
                metrics::M().live_dropped_tags_total->with({cfg_.name})->inc();
                gst_buffer_unref(a->buffer);
                return;
            }
        }
    }
    push_tag(a->buffer);
}

void LivePipeline::on_data(uint32_t ts, const uint8_t* amf, size_t len) {
    // Only the first onMetaData is forwarded: flvdemux (1.28) re-initialises its tracks on every
    // metadata tag and drops the AAC codec data, which silences the audio from then on. Repeats
    // from encoders carry nothing the pipeline needs anyway.
    if (metadata_sent_.exchange(true)) return;
    // Wrap as an FLV script tag so flvdemux picks up onMetaData (framerate, dimensions, codecs).
    GstBuffer* b = gst_buffer_new_allocate(nullptr, kFlvTagHeader + len + kFlvPrevSize, nullptr);
    GstMapInfo m;
    gst_buffer_map(b, &m, GST_MAP_WRITE);
    m.data[0] = 18;
    put24(m.data + 1, static_cast<uint32_t>(len));
    put24(m.data + 4, ts & 0xffffff);
    m.data[7] = (ts >> 24) & 0xff;
    put24(m.data + 8, 0);
    std::memcpy(m.data + kFlvTagHeader, amf, len);
    put32(m.data + kFlvTagHeader + len, static_cast<uint32_t>(kFlvTagHeader + len));
    gst_buffer_unmap(b, &m);
    push_tag(b);
}

void LivePipeline::on_end(const std::string& reason) {
    spdlog::info("[live {}] publisher ended: {}", cfg_.name, reason);
    stop();
}

void LivePipeline::push_flv_header() {
    static const uint8_t hdr[13] = {'F', 'L', 'V', 1, 0x05, 0, 0, 0, 9, 0, 0, 0, 0};
    if (const char* dir = std::getenv("VSP_DUMP_FLV_DIR")) {
        // debugging aid: write the exact byte stream handed to flvdemux
        dump_ = std::make_unique<std::ofstream>(std::filesystem::path(dir) / (cfg_.name + ".flv"), std::ios::binary);
        dump_->write(reinterpret_cast<const char*>(hdr), sizeof hdr);
    }
    GstBuffer* b = gst_buffer_new_memdup(hdr, sizeof hdr);
    gst_app_src_push_buffer(GST_APP_SRC(appsrc_), b);
}

void LivePipeline::push_tag(GstBuffer* tag) {
    if (!started_ || stopping_) {
        gst_buffer_unref(tag);
        return;
    }
    if (!header_sent_.exchange(true)) push_flv_header();
    if (dump_) {
        GstMapInfo m;
        if (gst_buffer_map(tag, &m, GST_MAP_READ)) {
            dump_->write(reinterpret_cast<const char*>(m.data), static_cast<std::streamsize>(m.size));
            gst_buffer_unmap(tag, &m);
        }
    }
    if (gst_app_src_push_buffer(GST_APP_SRC(appsrc_), tag) != GST_FLOW_OK) {
        // pipeline is shutting down or errored; the bus loop reports it
    }
}

// ---------------------------------------------------------------------------------------------
// dynamic graph construction (GStreamer streaming threads)

void LivePipeline::on_demux_pad(GstPad* pad) {
    std::string name = caps_name(pad);
    spdlog::debug("[live {}] demuxer pad {} ({})", cfg_.name, GST_PAD_NAME(pad), name);

    if (name.starts_with("video/")) {
        if (video_built_.exchange(true)) return;   // second video track: ignore
        GstElement* q = gst::make("queue", "vq_in");
        g_object_set(q, "max-size-buffers", 0u, "max-size-bytes", 0u, "max-size-time", guint64{2} * GST_SECOND, nullptr);
        GstElement* parse = nullptr;
        GstElement* vcaps = gst::make("capsfilter", "vcaps_in");
        video_is_h264_ = name == "video/x-h264";
        if (video_is_h264_) {
            parse = gst::make("h264parse", "vparse");
            g_object_set(parse, "config-interval", -1, nullptr);   // SPS/PPS before every keyframe (passthrough)
            gst::set_caps(vcaps, "video/x-h264,stream-format=avc,alignment=au");
        } else {
            parse = gst::make("identity", "vparse");
            gst::set_caps(vcaps, "ANY");
        }
        video_tee_ = gst::make("tee", "vtee");
        g_object_set(video_tee_, "allow-not-linked", TRUE, nullptr);
        gst_bin_add_many(GST_BIN(pipeline_), q, parse, vcaps, video_tee_, nullptr);
        gst::link_chain(q, parse, vcaps, video_tee_);
        for (GstElement* e : {q, parse, vcaps, video_tee_}) gst_element_sync_state_with_parent(e);

        // Build branches when the parsed caps (with dimensions) are known.
        gst::ptr<GstPad> src(gst_element_get_static_pad(vcaps, "src"));
        gst_pad_add_probe(
            src.get(), GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
            +[](GstPad* p, GstPadProbeInfo* info, gpointer self) -> GstPadProbeReturn {
                if (GST_EVENT_TYPE(GST_PAD_PROBE_INFO_EVENT(info)) != GST_EVENT_CAPS) return GST_PAD_PROBE_OK;
                static_cast<LivePipeline*>(self)->on_video_caps(p);
                return GST_PAD_PROBE_REMOVE;
            },
            this, nullptr);

        gst::ptr<GstPad> sink(gst_element_get_static_pad(q, "sink"));
        gst_pad_link(pad, sink.get());
        return;
    }

    if (name.starts_with("audio/")) {
        if (audio_built_.exchange(true)) return;
        GstElement* q = gst::make("queue", "aq_in");
        g_object_set(q, "max-size-buffers", 0u, "max-size-bytes", 0u, "max-size-time", guint64{2} * GST_SECOND, nullptr);
        gst_bin_add(GST_BIN(pipeline_), q);
        gst_element_sync_state_with_parent(q);
        gst::ptr<GstPad> sink(gst_element_get_static_pad(q, "sink"));
        gst_pad_link(pad, sink.get());

        bool aac = name == "audio/mpeg";
        {
            std::lock_guard lk(mu_);
            info_.has_audio = true;
            info_.audio_codec = aac ? "AAC" : name;
        }
        GstElement* tail = q;
        if (!aac) {
            // decode + re-encode to AAC
            GstElement* dec = gst::make("decodebin3", "adec");
            GstElement* conv = gst::make("audioconvert", "aconv");
            GstElement* res = gst::make("audioresample", "ares");
            GstElement* caps = gst::make("capsfilter", "acaps");
            GstElement* enc = gst::make(cfg_.audio_encoder.c_str(), "aenc");
            gst::set_caps(caps, "audio/x-raw,rate=48000,channels=2");
            configure_audio_encoder(enc, cfg_.audio_encoder, cfg_.audio_bitrate_kbps);
            gst_bin_add_many(GST_BIN(pipeline_), dec, conv, res, caps, enc, nullptr);
            gst::link_or_throw(q, dec);
            gst::link_chain(conv, res, caps, enc);
            g_signal_connect(dec, "pad-added",
                             G_CALLBACK(+[](GstElement*, GstPad* p, gpointer convp) {
                                 gst::ptr<GstPad> s(gst_element_get_static_pad(static_cast<GstElement*>(convp), "sink"));
                                 if (!gst_pad_is_linked(s.get())) gst_pad_link(p, s.get());
                             }),
                             conv);
            for (GstElement* e : {dec, conv, res, caps, enc}) gst_element_sync_state_with_parent(e);
            tail = enc;
        }
        GstElement* aparse = gst::make("aacparse", "aparse");
        gst_bin_add(GST_BIN(pipeline_), aparse);
        gst::link_or_throw(tail, aparse);
        gst_element_sync_state_with_parent(aparse);

        Rendition a;
        a.name = "audio";
        a.kind = "audio";
        a.bitrate_kbps = cfg_.audio_bitrate_kbps;
        a.path = "a_" + std::to_string(cfg_.audio_bitrate_kbps) + "k";
        add_mux_branch("audio", a, aparse, "audio_%u");
        return;
    }
    spdlog::debug("[live {}] ignoring stream {}", cfg_.name, name);
}

void LivePipeline::on_video_caps(GstPad* src) {
    gst::ptr<GstCaps> caps(gst_pad_get_current_caps(src));
    if (!caps) return;
    const GstStructure* s = gst_caps_get_structure(caps.get(), 0);
    int w = 0, h = 0, fn = 0, fd = 1;
    gst_structure_get_int(s, "width", &w);
    gst_structure_get_int(s, "height", &h);
    gst_structure_get_fraction(s, "framerate", &fn, &fd);
    if (w <= 0 || h <= 0) {
        spdlog::warn("[live {}] video caps without dimensions: {}", cfg_.name, gst_caps_to_string(caps.get()));
        return;
    }
    {
        std::lock_guard lk(mu_);
        info_.width = w;
        info_.height = h;
        info_.fps = fd > 0 && fn > 0 ? static_cast<double>(fn) / fd : 0;
        gchar* desc = gst_pb_utils_get_codec_description(caps.get());
        info_.video_codec = desc ? desc : gst_structure_get_name(s);
        g_free(desc);
    }
    spdlog::info("[live {}] video {}x{} @ {}/{} ({})", cfg_.name, w, h, fn, fd, gst_structure_get_name(s));
    try {
        build_video_branches(w, h, fn, fd);
    } catch (const std::exception& e) {
        fail(std::string("cannot build live branches: ") + e.what());
    }
}

LivePipeline::Branch* LivePipeline::add_mux_branch(const std::string& rendition_name, Rendition rendition,
                                                   GstElement* upstream, const char* mux_pad_template) {
    auto b = std::make_unique<Branch>();
    b->rendition = std::move(rendition);
    PackagerConfig pc;
    pc.out_dir = cfg_.out_dir / b->rendition.path;
    pc.segment_ms = cfg_.segment_ms;
    pc.part_ms = cfg_.part_ms;
    pc.name = cfg_.name + "/" + rendition_name;
    pc.live = true;
    pc.window_segments = cfg_.window_segments;
    pc.on_update = cfg_.on_playlist;
    b->packager = std::make_unique<CmafPackager>(std::move(pc));

    GstElement* q = gst::make("queue", "mq_" + rendition_name);
    GstElement* mux = gst::make("mp4mux", "mux_" + rendition_name);
    GstElement* sink = gst::make("appsink", "sink_" + rendition_name);
    g_object_set(q, "max-size-buffers", 0u, "max-size-bytes", 0u, "max-size-time", guint64{3} * GST_SECOND, nullptr);
    g_object_set(mux, "fragment-duration", cfg_.part_ms, "streamable", TRUE, nullptr);
    g_object_set(sink, "sync", FALSE, "async", FALSE, "max-buffers", 64u, "emit-signals", FALSE, nullptr);
    GstAppSinkCallbacks cbs{};
    cbs.new_sample = on_new_sample;
    gst_app_sink_set_callbacks(GST_APP_SINK(sink), &cbs, b.get(), nullptr);
    gst_bin_add_many(GST_BIN(pipeline_), q, mux, sink, nullptr);
    gst::link_or_throw(upstream, q);
    if (!gst_element_link_pads(q, "src", mux, mux_pad_template))
        throw std::runtime_error("cannot link " + rendition_name + " to mp4mux");
    gst::link_or_throw(mux, sink);
    for (GstElement* e : {q, mux, sink}) gst_element_sync_state_with_parent(e);
    b->appsink = sink;
    Branch* raw = b.get();
    std::lock_guard lk(mu_);
    branches_.push_back(std::move(b));
    return raw;
}

void LivePipeline::build_video_branches(int width, int height, int fps_n, int fps_d) {
    double fps = fps_d > 0 && fps_n > 0 ? static_cast<double>(fps_n) / fps_d : 30.0;
    if (fps > 60) fps = 60;
    const int gop = std::max(1, static_cast<int>(std::lround(fps * cfg_.segment_ms / 1000.0)));
    const bool h264 = video_is_h264_.load();

    // ---- passthrough: remux the source H.264 as the top rendition (no decode/encode)
    if (cfg_.passthrough && h264) {
        Rendition r;
        r.name = "source";
        r.kind = "video";
        r.width = width;
        r.height = height;
        r.bitrate_kbps = 0;   // measured by the packager
        r.path = "v_source";
        auto* b = add_mux_branch("source", r, video_tee_, "video_%u");
        b->passthrough = true;
    }

    // ---- encoded ABR tiers below the source height
    ProbeResult pr;
    pr.width = width;
    pr.height = height;
    pr.fps_n = fps_n;
    pr.fps_d = fps_d > 0 ? fps_d : 1;
    pr.has_video = true;
    TranscodeOptions opts;
    opts.renditions = cfg_.renditions;
    std::vector<Rendition> tiers;
    for (auto& r : build_renditions(pr, opts, cfg_.audio_bitrate_kbps)) {
        if (r.kind != "video") continue;
        if ((cfg_.passthrough && h264) && r.height >= height) continue;   // passthrough covers the source size
        tiers.push_back(r);
    }
    if (!(cfg_.passthrough && h264) && tiers.empty()) {
        // no passthrough possible (non-H.264 source) and nothing selected: encode at source size
        Rendition r;
        r.name = std::format("{}p", height);
        r.kind = "video";
        r.width = width;
        r.height = height;
        r.bitrate_kbps = static_cast<unsigned>(std::clamp(height * height * 0.0035 + 300.0, 300.0, 8000.0));
        r.path = "v_" + r.name;
        tiers.push_back(r);
    }

    if (!tiers.empty()) {
        GstElement* q = gst::make("queue", "dq");
        g_object_set(q, "max-size-buffers", 0u, "max-size-bytes", 0u, "max-size-time", guint64{2} * GST_SECOND, nullptr);
        // decodebin3 picks a decoder that actually accepts the stream's caps (profile/chroma):
        // NVDEC for 4:2:0 H.264 when available, libav otherwise.
        GstElement* dec = gst::make("decodebin3", "vdec");
        GstElement* conv = gst::make("videoconvert", "vconv");
        GstElement* rcaps = gst::make("capsfilter", "rcaps");
        raw_tee_ = gst::make("tee", "rtee");
        g_object_set(conv, "n-threads", 4u, nullptr);
        g_object_set(raw_tee_, "allow-not-linked", TRUE, nullptr);
        gst::set_caps(rcaps, std::format("video/x-raw,format={}", cfg_.encoder.raw_format));
        gst_bin_add_many(GST_BIN(pipeline_), q, dec, conv, rcaps, raw_tee_, nullptr);
        gst::link_or_throw(video_tee_, q);
        gst::link_or_throw(q, dec);
        g_signal_connect(dec, "pad-added",
                         G_CALLBACK(+[](GstElement* db, GstPad* p, gpointer selfp) {
                             auto* self = static_cast<LivePipeline*>(selfp);
                             GstElement* conv = gst_bin_get_by_name(GST_BIN(self->pipeline_), "vconv");
                             gst::ptr<GstPad> s(gst_element_get_static_pad(conv, "sink"));
                             if (!gst_pad_is_linked(s.get())) gst_pad_link(p, s.get());
                             gst_object_unref(conv);
                             // report which decoder decodebin3 chose
                             GstIterator* it = gst_bin_iterate_recurse(GST_BIN(db));
                             GValue v = G_VALUE_INIT;
                             while (gst_iterator_next(it, &v) == GST_ITERATOR_OK) {
                                 auto* e = GST_ELEMENT(g_value_get_object(&v));
                                 if (GstElementFactory* f = gst_element_get_factory(e)) {
                                     const gchar* klass = gst_element_factory_get_metadata(f, GST_ELEMENT_METADATA_KLASS);
                                     if (klass && g_strrstr(klass, "Decoder/Video"))
                                         spdlog::info("[live {}] video decoder: {}", self->cfg_.name,
                                                      gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(f)));
                                 }
                                 g_value_reset(&v);
                             }
                             g_value_unset(&v);
                             gst_iterator_free(it);
                         }),
                         this);
        gst::link_chain(conv, rcaps, raw_tee_);
        for (GstElement* e : {q, dec, conv, rcaps, raw_tee_}) gst_element_sync_state_with_parent(e);
        spdlog::info("[live {}] {} ABR tier(s) will be encoded with {}", cfg_.name, tiers.size(), cfg_.encoder.element);

        for (const auto& r : tiers) {
            GstElement* eq = gst::make("queue", "q_" + r.name);
            GstElement* scale = gst::make("videoscale", "scale_" + r.name);
            GstElement* scaps = gst::make("capsfilter", "scaps_" + r.name);
            GstElement* enc = gst::make(cfg_.encoder.element.c_str(), "enc_" + r.name);
            GstElement* ecaps = gst::make("capsfilter", "ecaps_" + r.name);
            GstElement* parse = gst::make("h264parse", "parse_" + r.name);
            GstElement* pcaps = gst::make("capsfilter", "pcaps_" + r.name);
            g_object_set(eq, "max-size-buffers", 0u, "max-size-bytes", 0u, "max-size-time", guint64{1} * GST_SECOND, nullptr);
            g_object_set(scale, "method", 1, "add-borders", FALSE, "n-threads", 2u, nullptr);
            gst::set_caps(scaps, std::format("video/x-raw,width={},height={},pixel-aspect-ratio=1/1", r.width, r.height));
            configure_video_encoder(enc, cfg_.encoder, r.bitrate_kbps, gop, /*low_latency=*/true);
            gst::set_caps(ecaps, cfg_.encoder.hardware || cfg_.encoder.element == "x264enc" ? "video/x-h264,profile=high"
                                                                                           : "video/x-h264");
            gst::set_caps(pcaps, "video/x-h264,stream-format=avc,alignment=au");
            gst_bin_add_many(GST_BIN(pipeline_), eq, scale, scaps, enc, ecaps, parse, pcaps, nullptr);
            gst::link_or_throw(raw_tee_, eq);
            gst::link_chain(eq, scale, scaps, enc, ecaps, parse, pcaps);
            for (GstElement* e : {eq, scale, scaps, enc, ecaps, parse, pcaps}) gst_element_sync_state_with_parent(e);
            add_mux_branch(r.name, r, pcaps, "video_%u");
        }
    }

    // frame counter for the fps stat
    gst::ptr<GstPad> tsink(gst_element_get_static_pad(video_tee_, "sink"));
    gst_pad_add_probe(
        tsink.get(), GST_PAD_PROBE_TYPE_BUFFER,
        +[](GstPad*, GstPadProbeInfo*, gpointer self) -> GstPadProbeReturn {
            static_cast<LivePipeline*>(self)->frames_.fetch_add(1, std::memory_order_relaxed);
            return GST_PAD_PROBE_OK;
        },
        this, nullptr);
}

// ---------------------------------------------------------------------------------------------

void LivePipeline::publish_master(bool measured) {
    std::vector<Rendition> rends;
    bool all_ready = true;
    {
        std::lock_guard lk(mu_);
        for (const auto& b : branches_) {
            auto st = b->packager->stats();
            if (!st.has_init) { all_ready = false; continue; }
            Rendition r = b->rendition;
            r.codecs = st.codec;
            if (measured || b->passthrough || r.bitrate_kbps == 0) {
                r.measured_bandwidth = st.peak_bandwidth;
                r.measured_avg_bandwidth = st.avg_bandwidth;
            }
            // Before any measurement exists for the passthrough tier, estimate from the inbound bitrate.
            if (b->passthrough && r.measured_bandwidth == 0) {
                r.measured_bandwidth = r.measured_avg_bandwidth = 6'000'000;
            }
            rends.push_back(r);
        }
    }
    if (rends.empty()) return;
    double fps;
    {
        std::lock_guard lk(mu_);
        fps = info_.fps;
    }
    write_master_playlist(cfg_.out_dir, rends, fps);
    LiveInfo snapshot;
    {
        std::lock_guard lk(mu_);
        info_.renditions = rends;
        info_.master_ready = true;
        snapshot = info_;
    }
    (void)all_ready;
    if (on_state_) on_state_(snapshot);
}

void LivePipeline::bus_loop() {
    gst::ptr<GstBus> bus(gst_element_get_bus(pipeline_));
    auto last_tick = std::chrono::steady_clock::now();
    auto last_master = last_tick;
    uint64_t last_frames = 0;
    size_t published_branches = 0;
    bool eos = false;
    auto stop_requested_at = std::chrono::steady_clock::time_point::min();

    while (!eos) {
        GstMessage* msg = gst_bus_timed_pop_filtered(
            bus.get(), 200 * GST_MSECOND,
            static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR | GST_MESSAGE_WARNING));
        if (msg) {
            switch (GST_MESSAGE_TYPE(msg)) {
            case GST_MESSAGE_EOS: eos = true; break;
            case GST_MESSAGE_ERROR:
                metrics::M().gst_errors_total->with({"live"})->inc();
                fail(gst::describe_error(msg));
                eos = true;
                break;
            case GST_MESSAGE_WARNING: {
                GError* w = nullptr;
                gst_message_parse_warning(msg, &w, nullptr);
                metrics::M().gst_warnings_total->with({"live"})->inc();
                spdlog::warn("[live {}] {}: {}", cfg_.name, GST_OBJECT_NAME(msg->src), w ? w->message : "?");
                if (w) g_error_free(w);
                break;
            }
            default: break;
            }
            gst_message_unref(msg);
        }

        auto now = std::chrono::steady_clock::now();
        if (stopping_ && stop_requested_at == std::chrono::steady_clock::time_point::min()) stop_requested_at = now;
        if (stopping_ && now - stop_requested_at > 5s) {
            spdlog::warn("[live {}] EOS did not arrive within 5s, forcing shutdown", cfg_.name);
            break;
        }
        if (!error_.empty()) break;

        if (now - last_tick >= 500ms) {
            double dt = std::chrono::duration<double>(now - last_tick).count();
            uint64_t f = frames_.load(std::memory_order_relaxed);
            fps_now_.store(static_cast<double>(f - last_frames) / dt);
            metrics::M().live_encode_fps->with({cfg_.name})->set(fps_now_.load());
            metrics::M().live_frames_total->with({cfg_.name})->inc(static_cast<double>(f - last_frames));
            if (appsrc_) {
                guint64 lvl = 0;
                g_object_get(appsrc_, "current-level-bytes", &lvl, nullptr);
                metrics::M().live_backlog_bytes->with({cfg_.name})->set(static_cast<double>(lvl));
            }
            last_frames = f;
            last_tick = now;

            size_t ready = 0, total = 0;
            {
                std::lock_guard lk(mu_);
                total = branches_.size();
                for (const auto& b : branches_) if (b->packager->has_init()) ++ready;
            }
            // Publish/refresh the master playlist when a new rendition becomes available, and
            // periodically with measured bandwidths so ABR decisions use real numbers.
            if (ready && ready == total && (ready != published_branches || now - last_master >= 10s)) {
                publish_master(ready == published_branches);
                published_branches = ready;
                last_master = now;
            }
        }
    }

    gst_element_set_state(pipeline_, GST_STATE_NULL);
    metrics::M().gst_pipelines->with({"live"})->dec();
    metrics::M().live_encode_fps->remove({cfg_.name});
    metrics::M().live_backlog_bytes->remove({cfg_.name});
    {
        std::lock_guard lk(mu_);
        for (auto& b : branches_) b->packager->finish();
    }
    if (error_.empty()) publish_master(true);
    ended_ = true;
    std::string err;
    {
        std::lock_guard lk(mu_);
        err = error_;
    }
    spdlog::info("[live {}] pipeline finished{}", cfg_.name, err.empty() ? "" : " with error: " + err);
    if (on_end_) on_end_(err);
}

} // namespace vsp

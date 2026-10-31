#include "media/probe.hpp"

#include <gst/gst.h>
#include <gst/pbutils/pbutils.h>
#include <gst/video/video.h>
#include <spdlog/spdlog.h>

namespace vsp {

namespace {

std::string codec_name(GstDiscovererStreamInfo* s) {
    GstCaps* caps = gst_discoverer_stream_info_get_caps(s);
    std::string out;
    if (caps) {
        gchar* d = gst_pb_utils_get_codec_description(caps);
        if (d) {
            out = d;
            g_free(d);
        } else {
            out = gst_structure_get_name(gst_caps_get_structure(caps, 0));
        }
        gst_caps_unref(caps);
    }
    return out;
}

// Bits per component of the decoded video, derived from the codec caps. 0 when unknown.
// (GstDiscoverer's get_depth() returns the sum over components, which is misleading for 4:4:4.)
unsigned bits_per_component(GstDiscovererStreamInfo* s) {
    GstCaps* caps = gst_discoverer_stream_info_get_caps(s);
    if (!caps) return 0;
    unsigned depth = 0;
    const GstStructure* st = gst_caps_get_structure(caps, 0);
    if (const gchar* fmt = gst_structure_get_string(st, "format")) {
        GstVideoFormat f = gst_video_format_from_string(fmt);
        if (const GstVideoFormatInfo* fi = gst_video_format_get_info(f)) depth = GST_VIDEO_FORMAT_INFO_DEPTH(fi, 0);
    }
    if (!depth) {
        int d = 0;
        if (gst_structure_get_int(st, "bit-depth-luma", &d) && d > 0) depth = static_cast<unsigned>(d);
    }
    if (!depth) {
        // H.264/HEVC profile names carry the depth
        if (const gchar* prof = gst_structure_get_string(st, "profile")) {
            std::string p = prof;
            if (p.find("12") != std::string::npos) depth = 12;
            else if (p.find("10") != std::string::npos) depth = 10;
            else if (p.find("main") != std::string::npos || p.find("high") != std::string::npos ||
                     p.find("baseline") != std::string::npos) depth = 8;
        }
    }
    gst_caps_unref(caps);
    return depth;
}

} // namespace

std::expected<ProbeResult, std::string> probe_file(const std::filesystem::path& path, int timeout_s) {
    GError* err = nullptr;
    GstDiscoverer* disc = gst_discoverer_new(static_cast<GstClockTime>(timeout_s) * GST_SECOND, &err);
    if (!disc) {
        std::string msg = err ? err->message : "cannot create discoverer";
        if (err) g_error_free(err);
        return std::unexpected(msg);
    }

    gchar* uri = gst_filename_to_uri(path.c_str(), nullptr);
    if (!uri) {
        g_object_unref(disc);
        return std::unexpected("invalid path");
    }
    GstDiscovererInfo* info = gst_discoverer_discover_uri(disc, uri, &err);
    g_free(uri);
    g_object_unref(disc);

    if (!info) {
        std::string msg = err ? err->message : "discovery failed";
        if (err) g_error_free(err);
        return std::unexpected(msg);
    }

    auto result = gst_discoverer_info_get_result(info);
    if (result != GST_DISCOVERER_OK) {
        std::string msg;
        switch (result) {
        case GST_DISCOVERER_URI_INVALID: msg = "invalid URI"; break;
        case GST_DISCOVERER_ERROR: msg = err ? err->message : "error"; break;
        case GST_DISCOVERER_TIMEOUT: msg = "probe timed out"; break;
        case GST_DISCOVERER_MISSING_PLUGINS: {
            msg = "missing decoder plugins:";
            const GstStructure* misc = gst_discoverer_info_get_misc(info);
            if (misc) {
                gchar* s = gst_structure_to_string(misc);
                msg += s ? s : "";
                g_free(s);
            }
            break;
        }
        default: msg = "unknown probe error";
        }
        if (err) g_error_free(err);
        gst_discoverer_info_unref(info);
        return std::unexpected(msg);
    }
    if (err) g_error_free(err);

    ProbeResult r;
    r.duration_ms = static_cast<int64_t>(gst_discoverer_info_get_duration(info) / GST_MSECOND);

    if (GstDiscovererStreamInfo* sinfo = gst_discoverer_info_get_stream_info(info)) {
        GstCaps* caps = gst_discoverer_stream_info_get_caps(sinfo);
        if (caps) {
            r.container = gst_structure_get_name(gst_caps_get_structure(caps, 0));
            gst_caps_unref(caps);
        }
        gst_discoverer_stream_info_unref(sinfo);
    }

    GList* vs = gst_discoverer_info_get_video_streams(info);
    for (GList* l = vs; l; l = l->next) {
        auto* v = GST_DISCOVERER_VIDEO_INFO(l->data);
        if (gst_discoverer_video_info_is_image(v)) {
            if (!r.has_video) r.image_only = true;
            continue;
        }
        r.image_only = false;
        ++r.video_streams;
        if (r.has_video) continue;
        r.has_video = true;
        r.width = static_cast<int>(gst_discoverer_video_info_get_width(v));
        r.height = static_cast<int>(gst_discoverer_video_info_get_height(v));
        r.fps_n = static_cast<int>(gst_discoverer_video_info_get_framerate_num(v));
        r.fps_d = static_cast<int>(gst_discoverer_video_info_get_framerate_denom(v));
        if (r.fps_d <= 0) r.fps_d = 1;
        r.interlaced = gst_discoverer_video_info_is_interlaced(v);
        r.video_depth = bits_per_component(GST_DISCOVERER_STREAM_INFO(v));
        r.video_bitrate = gst_discoverer_video_info_get_bitrate(v);
        r.video_codec = codec_name(GST_DISCOVERER_STREAM_INFO(v));
    }
    gst_discoverer_stream_info_list_free(vs);

    GList* as = gst_discoverer_info_get_audio_streams(info);
    for (GList* l = as; l; l = l->next) {
        auto* a = GST_DISCOVERER_AUDIO_INFO(l->data);
        ++r.audio_streams;
        if (r.has_audio) continue;
        r.has_audio = true;
        r.audio_channels = static_cast<int>(gst_discoverer_audio_info_get_channels(a));
        r.audio_rate = static_cast<int>(gst_discoverer_audio_info_get_sample_rate(a));
        r.audio_codec = codec_name(GST_DISCOVERER_STREAM_INFO(a));
    }
    gst_discoverer_stream_info_list_free(as);

    gst_discoverer_info_unref(info);

    if (r.image_only) return std::unexpected("still image, not a video");
    if (!r.has_video) return std::unexpected(r.has_audio ? "audio-only, no video stream" : "no video stream found");
    if (r.duration_ms <= 0) return std::unexpected("could not determine duration");
    return r;
}

} // namespace vsp

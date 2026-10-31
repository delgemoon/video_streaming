#include "media/probe.hpp"

#include <gst/gst.h>
#include <gst/pbutils/pbutils.h>
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
        if (gst_discoverer_video_info_is_image(v)) continue;
        if (r.has_video) continue;
        r.has_video = true;
        r.width = static_cast<int>(gst_discoverer_video_info_get_width(v));
        r.height = static_cast<int>(gst_discoverer_video_info_get_height(v));
        r.fps_n = static_cast<int>(gst_discoverer_video_info_get_framerate_num(v));
        r.fps_d = static_cast<int>(gst_discoverer_video_info_get_framerate_denom(v));
        if (r.fps_d <= 0) r.fps_d = 1;
        r.video_codec = codec_name(GST_DISCOVERER_STREAM_INFO(v));
    }
    gst_discoverer_stream_info_list_free(vs);

    GList* as = gst_discoverer_info_get_audio_streams(info);
    for (GList* l = as; l; l = l->next) {
        auto* a = GST_DISCOVERER_AUDIO_INFO(l->data);
        if (r.has_audio) continue;
        r.has_audio = true;
        r.audio_channels = static_cast<int>(gst_discoverer_audio_info_get_channels(a));
        r.audio_rate = static_cast<int>(gst_discoverer_audio_info_get_sample_rate(a));
        r.audio_codec = codec_name(GST_DISCOVERER_STREAM_INFO(a));
    }
    gst_discoverer_stream_info_list_free(as);

    gst_discoverer_info_unref(info);

    if (!r.has_video) return std::unexpected("no video stream found");
    if (r.duration_ms <= 0) return std::unexpected("could not determine duration");
    return r;
}

} // namespace vsp

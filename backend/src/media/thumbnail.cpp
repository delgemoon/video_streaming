#include "media/thumbnail.hpp"

#include <format>
#include <fstream>
#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <spdlog/spdlog.h>

namespace vsp {

std::expected<void, std::string> generate_thumbnail(const std::filesystem::path& source,
                                                    const std::filesystem::path& out_jpeg, int64_t position_ms,
                                                    int width) {
    gchar* uri = gst_filename_to_uri(source.c_str(), nullptr);
    if (!uri) return std::unexpected("bad source path");
    std::string desc = std::format(
        "uridecodebin3 uri=\"{}\" ! videoconvert ! videoscale ! "
        "video/x-raw,format=RGB,width={},pixel-aspect-ratio=1/1 ! jpegenc quality=85 ! appsink name=sink sync=false",
        uri, width);
    g_free(uri);

    GError* err = nullptr;
    GstElement* p = gst_parse_launch(desc.c_str(), &err);
    if (!p || err) {
        std::string msg = err ? err->message : "cannot build thumbnail pipeline";
        if (err) g_error_free(err);
        if (p) gst_object_unref(p);
        return std::unexpected(msg);
    }
    GstElement* sink = gst_bin_get_by_name(GST_BIN(p), "sink");
    auto cleanup = [&] {
        gst_element_set_state(p, GST_STATE_NULL);
        if (sink) gst_object_unref(sink);
        gst_object_unref(p);
    };

    gst_element_set_state(p, GST_STATE_PAUSED);
    if (gst_element_get_state(p, nullptr, nullptr, 20 * GST_SECOND) == GST_STATE_CHANGE_FAILURE) {
        cleanup();
        return std::unexpected("thumbnail pipeline failed to preroll");
    }
    if (position_ms > 0) {
        gst_element_seek_simple(p, GST_FORMAT_TIME,
                                static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT),
                                position_ms * GST_MSECOND);
        gst_element_get_state(p, nullptr, nullptr, 20 * GST_SECOND);
    }

    GstSample* sample = gst_app_sink_try_pull_preroll(GST_APP_SINK(sink), 20 * GST_SECOND);
    if (!sample) {
        cleanup();
        return std::unexpected("no frame available for thumbnail");
    }
    GstBuffer* buf = gst_sample_get_buffer(sample);
    GstMapInfo map;
    bool ok = false;
    if (buf && gst_buffer_map(buf, &map, GST_MAP_READ)) {
        std::ofstream f(out_jpeg, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(map.data), static_cast<std::streamsize>(map.size));
        ok = static_cast<bool>(f);
        gst_buffer_unmap(buf, &map);
    }
    gst_sample_unref(sample);
    cleanup();
    if (!ok) return std::unexpected("failed to write thumbnail");
    return {};
}

} // namespace vsp

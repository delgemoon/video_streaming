#include "media/gst_init.hpp"

#include <gst/gst.h>
#include <mutex>
#include <spdlog/spdlog.h>

namespace vsp {

namespace {

void set_rank(const char* factory, GstRank rank) {
    GstRegistry* reg = gst_registry_get();
    GstPluginFeature* f = gst_registry_lookup_feature(reg, factory);
    if (!f) return;
    gst_plugin_feature_set_rank(f, rank);
    gst_object_unref(f);
}

} // namespace

void gst_platform_init() {
    static std::once_flag once;
    std::call_once(once, [] {
        gst_init(nullptr, nullptr);
        GstRegistry* reg = gst_registry_get();
        if (GstPluginFeature* av = gst_registry_lookup_feature(reg, "avdemux_mxf")) {
            // libav's MXF demuxer copes with Avid OP-Atom / P2 variants that mxfdemux rejects.
            set_rank("avdemux_mxf", static_cast<GstRank>(GST_RANK_PRIMARY + 1));
            set_rank("mxfdemux", GST_RANK_SECONDARY);
            gst_object_unref(av);
            spdlog::debug("MXF: preferring avdemux_mxf, mxfdemux as fallback");
        }
    });
}

} // namespace vsp

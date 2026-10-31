#include "media/mxf_fixup.hpp"

#include <cstring>
#include <gst/gst.h>
#include <spdlog/spdlog.h>

namespace vsp {

namespace {

struct PadState {
    GstBuffer* header = nullptr;   // Annex-B SPS/PPS to prepend to the first buffer
    bool prepended = false;
};

void pad_state_free(gpointer p) {
    auto* s = static_cast<PadState*>(p);
    if (s->header) gst_buffer_unref(s->header);
    delete s;
}

// Converts an avcC (ISO 14496-15) decoder configuration record to Annex-B NAL units.
// Returns nullptr if the blob is not avcC (e.g. already Annex-B), in which case it is used as-is.
GstBuffer* avcc_to_annexb(const guint8* d, gsize n) {
    if (n < 7 || d[0] != 1) return nullptr;
    GByteArray* out = g_byte_array_new();
    static const guint8 sc[4] = {0, 0, 0, 1};
    gsize off = 5;
    guint sps_count = d[off++] & 0x1f;
    for (guint i = 0; i < sps_count && off + 2 <= n; ++i) {
        gsize len = (d[off] << 8) | d[off + 1];
        off += 2;
        if (off + len > n) break;
        g_byte_array_append(out, sc, 4);
        g_byte_array_append(out, d + off, static_cast<guint>(len));
        off += len;
    }
    if (off < n) {
        guint pps_count = d[off++];
        for (guint i = 0; i < pps_count && off + 2 <= n; ++i) {
            gsize len = (d[off] << 8) | d[off + 1];
            off += 2;
            if (off + len > n) break;
            g_byte_array_append(out, sc, 4);
            g_byte_array_append(out, d + off, static_cast<guint>(len));
            off += len;
        }
    }
    gsize total = out->len;
    guint8* data = g_byte_array_free(out, FALSE);
    return gst_buffer_new_wrapped(data, total);
}

GstPadProbeReturn on_probe(GstPad* pad, GstPadProbeInfo* info, gpointer) {
    auto* st = static_cast<PadState*>(g_object_get_data(G_OBJECT(pad), "vsp-mxf-fixup"));
    if (!st) return GST_PAD_PROBE_OK;

    if (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM) {
        GstEvent* ev = GST_PAD_PROBE_INFO_EVENT(info);
        if (GST_EVENT_TYPE(ev) != GST_EVENT_CAPS) return GST_PAD_PROBE_OK;
        GstCaps* caps = nullptr;
        gst_event_parse_caps(ev, &caps);
        const GstStructure* s = gst_caps_get_structure(caps, 0);
        if (!gst_structure_has_name(s, "video/x-h264")) return GST_PAD_PROBE_OK;
        const GValue* cd = gst_structure_get_value(s, "codec_data");
        if (!cd) return GST_PAD_PROBE_OK;

        GstBuffer* blob = gst_value_get_buffer(cd);
        GstMapInfo map;
        if (blob && gst_buffer_map(blob, &map, GST_MAP_READ)) {
            bool annexb = map.size >= 4 && map.data[0] == 0 && map.data[1] == 0 &&
                          (map.data[2] == 1 || (map.data[2] == 0 && map.data[3] == 1));
            if (st->header) gst_buffer_unref(st->header);
            st->header = annexb ? gst_buffer_copy(blob) : avcc_to_annexb(map.data, map.size);
            gst_buffer_unmap(blob, &map);
        }
        if (!st->header) return GST_PAD_PROBE_OK;

        // Replace caps: byte-stream, no codec_data, keep geometry/framerate/par.
        GstCaps* fixed = gst_caps_copy(caps);
        GstStructure* fs = gst_caps_get_structure(fixed, 0);
        gst_structure_remove_fields(fs, "codec_data", "stream-format", "alignment", "profile", "level", nullptr);
        gst_structure_set(fs, "stream-format", G_TYPE_STRING, "byte-stream", "alignment", G_TYPE_STRING, "au", nullptr);
        gst_event_unref(ev);
        GST_PAD_PROBE_INFO_DATA(info) = gst_event_new_caps(fixed);
        gst_caps_unref(fixed);
        st->prepended = false;
        spdlog::debug("mxf fixup: rewrote H.264 caps to byte-stream, {} bytes of SPS/PPS queued",
                      gst_buffer_get_size(st->header));
        return GST_PAD_PROBE_OK;
    }

    if ((GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER) && st->header && !st->prepended) {
        GstBuffer* buf = GST_PAD_PROBE_INFO_BUFFER(info);
        GstBuffer* hdr = gst_buffer_copy(st->header);
        // Carry timestamps/flags of the original buffer on the merged one.
        gst_buffer_copy_into(hdr, buf, static_cast<GstBufferCopyFlags>(GST_BUFFER_COPY_METADATA), 0, -1);
        GstBuffer* merged = gst_buffer_append(hdr, buf);
        GST_PAD_PROBE_INFO_DATA(info) = merged;
        st->prepended = true;
    }
    return GST_PAD_PROBE_OK;
}

void on_demux_pad_added(GstElement*, GstPad* pad, gpointer) {
    if (GST_PAD_DIRECTION(pad) != GST_PAD_SRC) return;
    g_object_set_data_full(G_OBJECT(pad), "vsp-mxf-fixup", new PadState, pad_state_free);
    gst_pad_add_probe(pad,
                      static_cast<GstPadProbeType>(GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM | GST_PAD_PROBE_TYPE_BUFFER),
                      on_probe, nullptr, nullptr);
}

void on_deep_element_added(GstBin*, GstBin*, GstElement* element, gpointer) {
    GstElementFactory* f = gst_element_get_factory(element);
    if (!f) return;
    const gchar* name = gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(f));
    if (g_strcmp0(name, "avdemux_mxf") != 0 && g_strcmp0(name, "mxfdemux") != 0) return;
    g_signal_connect(element, "pad-added", G_CALLBACK(on_demux_pad_added), nullptr);
    // pads that already exist (unlikely at this point, but cheap to cover)
    GstIterator* it = gst_element_iterate_src_pads(element);
    GValue v = G_VALUE_INIT;
    while (gst_iterator_next(it, &v) == GST_ITERATOR_OK) {
        on_demux_pad_added(element, GST_PAD(g_value_get_object(&v)), nullptr);
        g_value_reset(&v);
    }
    g_value_unset(&v);
    gst_iterator_free(it);
}

} // namespace

void install_mxf_h264_fixup(GstElement* bin) {
    g_signal_connect(bin, "deep-element-added", G_CALLBACK(on_deep_element_added), nullptr);
}

} // namespace vsp

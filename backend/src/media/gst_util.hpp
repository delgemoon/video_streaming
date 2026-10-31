#pragma once

// Small RAII/convenience helpers shared by the VOD transcoder and the live pipeline.

#include <format>
#include <gst/gst.h>
#include <memory>
#include <stdexcept>
#include <string>

namespace vsp::gst {

struct Unref {
    void operator()(GstElement* e) const { if (e) gst_object_unref(e); }
    void operator()(GstBus* b) const { if (b) gst_object_unref(b); }
    void operator()(GstCaps* c) const { if (c) gst_caps_unref(c); }
    void operator()(GstPad* p) const { if (p) gst_object_unref(p); }
};
template <typename T> using ptr = std::unique_ptr<T, Unref>;

inline GstElement* make(const char* factory, const std::string& name) {
    GstElement* e = gst_element_factory_make(factory, name.c_str());
    if (!e) throw std::runtime_error(std::format("missing GStreamer element '{}'", factory));
    return e;
}

inline void set_caps(GstElement* capsfilter, const std::string& caps_str) {
    ptr<GstCaps> caps(gst_caps_from_string(caps_str.c_str()));
    if (!caps) throw std::runtime_error("bad caps: " + caps_str);
    g_object_set(capsfilter, "caps", caps.get(), nullptr);
}

inline void link_or_throw(GstElement* a, GstElement* b) {
    if (!gst_element_link(a, b))
        throw std::runtime_error(std::format("cannot link {} -> {}", GST_ELEMENT_NAME(a), GST_ELEMENT_NAME(b)));
}

template <typename... E>
void link_chain(E... elements) {
    GstElement* arr[] = {elements...};
    for (size_t i = 0; i + 1 < sizeof...(E); ++i) link_or_throw(arr[i], arr[i + 1]);
}

inline bool has_factory(const char* name) {
    GstElementFactory* f = gst_element_factory_find(name);
    if (!f) return false;
    gst_object_unref(f);
    return true;
}

inline std::string describe_error(GstMessage* msg) {
    GError* err = nullptr;
    gchar* dbg = nullptr;
    gst_message_parse_error(msg, &err, &dbg);
    std::string out = std::format("{}: {}", GST_OBJECT_NAME(msg->src), err ? err->message : "unknown");
    if (dbg) {
        out += std::format(" [{}]", dbg);
        g_free(dbg);
    }
    if (err) g_error_free(err);
    return out;
}

} // namespace vsp::gst

#include "media/encoder_detect.hpp"

#include <format>
#include <gst/gst.h>
#include <spdlog/spdlog.h>

namespace vsp {

namespace {

bool has_property(GstElement* e, const char* name) {
    return g_object_class_find_property(G_OBJECT_GET_CLASS(e), name) != nullptr;
}

void set_if(GstElement* e, const char* name, const std::string& value) {
    if (has_property(e, name)) gst_util_set_object_arg(G_OBJECT(e), name, value.c_str());
}

bool test_pipeline(const std::string& desc, int timeout_s) {
    GError* err = nullptr;
    GstElement* p = gst_parse_launch(desc.c_str(), &err);
    if (!p || err) {
        if (err) g_error_free(err);
        if (p) gst_object_unref(p);
        return false;
    }
    bool ok = false;
    if (gst_element_set_state(p, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE) {
        GstBus* bus = gst_element_get_bus(p);
        GstMessage* msg = gst_bus_timed_pop_filtered(
            bus, static_cast<GstClockTime>(timeout_s) * GST_SECOND,
            static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
        if (msg) {
            ok = GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS;
            if (!ok) {
                GError* e = nullptr;
                gchar* dbg = nullptr;
                gst_message_parse_error(msg, &e, &dbg);
                spdlog::debug("encoder test failed: {} ({})", e ? e->message : "?", dbg ? dbg : "");
                if (e) g_error_free(e);
                g_free(dbg);
            }
            gst_message_unref(msg);
        }
        gst_object_unref(bus);
    }
    gst_element_set_state(p, GST_STATE_NULL);
    gst_object_unref(p);
    return ok;
}

const std::vector<EncoderInfo> kVideoCandidates = {
    {"nvenc", "nvh264enc", "NV12", true},
    {"vaapi", "vah264enc", "NV12", true},
    {"vaapi", "vaapih264enc", "NV12", true},
    {"qsv", "qsvh264enc", "NV12", true},
    {"x264", "x264enc", "I420", false},
    {"openh264", "openh264enc", "I420", false},
};

const std::vector<std::string> kAudioCandidates = {"fdkaacenc", "avenc_aac", "voaacenc"};

} // namespace

EncoderDetection detect_encoders(const std::string& preference) {
    EncoderDetection det;
    for (EncoderInfo c : kVideoCandidates) {
        GstElementFactory* f = gst_element_factory_find(c.element.c_str());
        if (!f) {
            c.note = "plugin not installed";
            det.candidates.push_back(c);
            continue;
        }
        gst_object_unref(f);
        std::string desc = std::format(
            "videotestsrc num-buffers=15 ! video/x-raw,format={},width=320,height=240,framerate=30/1 ! "
            "{} ! h264parse ! fakesink",
            c.raw_format, c.element);
        c.usable = test_pipeline(desc, 15);
        c.note = c.usable ? "ok" : "failed self-test";
        spdlog::info("encoder {:<14} {}", c.element, c.note);
        det.candidates.push_back(c);
    }

    auto pick = [&](auto pred) -> bool {
        for (const auto& c : det.candidates)
            if (c.usable && pred(c)) { det.video = c; return true; }
        return false;
    };

    bool chosen = false;
    if (preference != "auto" && !preference.empty())
        chosen = pick([&](const EncoderInfo& c) { return c.id == preference; });
    if (!chosen && preference != "auto" && !preference.empty())
        spdlog::warn("preferred encoder '{}' unavailable, falling back to auto", preference);
    if (!chosen) chosen = pick([](const EncoderInfo& c) { return c.hardware; });
    if (!chosen) chosen = pick([](const EncoderInfo&) { return true; });
    if (!chosen) throw std::runtime_error("no usable H.264 encoder found (install gstreamer1.0-plugins-ugly)");

    for (const auto& a : kAudioCandidates) {
        GstElementFactory* f = gst_element_factory_find(a.c_str());
        if (f) {
            gst_object_unref(f);
            det.audio_element = a;
            break;
        }
    }
    if (det.audio_element.empty()) throw std::runtime_error("no AAC encoder found (install gstreamer1.0-libav)");

    spdlog::info("selected video encoder: {} ({}), audio encoder: {}", det.video.element,
                 det.video.hardware ? "hardware" : "software", det.audio_element);
    return det;
}

void configure_video_encoder(GstElement* enc, const EncoderInfo& info, unsigned bitrate_kbps, int gop_frames,
                             bool low_latency) {
    const std::string gop = std::to_string(gop_frames);
    const std::string kbps = std::to_string(bitrate_kbps);
    if (info.element == "nvh264enc") {
        set_if(enc, "preset", low_latency ? "p2" : "p4");
        set_if(enc, "tune", low_latency ? "low-latency" : "high-quality");
        set_if(enc, "rc-mode", "cbr");
        set_if(enc, "bitrate", kbps);
        set_if(enc, "max-bitrate", std::to_string(bitrate_kbps * 12 / 10));
        set_if(enc, "gop-size", gop);
        set_if(enc, "strict-gop", "true");
        set_if(enc, "bframes", low_latency ? "0" : "2");
        set_if(enc, "b-adapt", low_latency ? "false" : "true");
        set_if(enc, "spatial-aq", "true");
        set_if(enc, "aq-strength", "8");
        set_if(enc, "zerolatency", low_latency ? "true" : "false");
        set_if(enc, "repeat-sequence-header", "true");
    } else if (info.element == "vah264enc") {
        set_if(enc, "rate-control", "cbr");
        set_if(enc, "bitrate", kbps);
        set_if(enc, "key-int-max", gop);
        set_if(enc, "target-usage", "4");
        set_if(enc, "b-frames", "0");
    } else if (info.element == "vaapih264enc") {
        set_if(enc, "rate-control", "cbr");
        set_if(enc, "bitrate", kbps);
        set_if(enc, "keyframe-period", gop);
        set_if(enc, "max-bframes", "0");
    } else if (info.element == "qsvh264enc") {
        set_if(enc, "rate-control", "cbr");
        set_if(enc, "bitrate", kbps);
        set_if(enc, "gop-size", gop);
        set_if(enc, "b-frames", "0");
    } else if (info.element == "x264enc") {
        set_if(enc, "pass", "cbr");
        set_if(enc, "bitrate", kbps);
        set_if(enc, "key-int-max", gop);
        set_if(enc, "speed-preset", low_latency ? "superfast" : "veryfast");
        if (low_latency) {
            set_if(enc, "tune", "zerolatency");
            set_if(enc, "bframes", "0");
            set_if(enc, "rc-lookahead", "0");
        }
        set_if(enc, "byte-stream", "false");
        set_if(enc, "threads", "0");
        set_if(enc, "vbv-buf-capacity", "1000");
        // scenecut=0 + min-keyint keep GOPs identical across renditions so segments align.
        set_if(enc, "option-string", std::format("scenecut=0:min-keyint={}:keyint={}", gop_frames, gop_frames));
    } else if (info.element == "openh264enc") {
        set_if(enc, "rate-control", "bitrate");
        set_if(enc, "bitrate", std::to_string(bitrate_kbps * 1000));
        set_if(enc, "gop-size", gop);
        set_if(enc, "scene-change-detection", "false");
    }
}

void configure_audio_encoder(GstElement* enc, const std::string& element, unsigned bitrate_kbps) {
    // every supported AAC encoder takes bits/s
    set_if(enc, "bitrate", std::to_string(bitrate_kbps * 1000));
    if (element == "fdkaacenc") set_if(enc, "afterburner", "true");
}

} // namespace vsp

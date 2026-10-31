#include "media/ladder.hpp"

#include <algorithm>
#include <cmath>
#include <format>

namespace vsp {

const std::vector<LadderStep>& default_ladder() {
    static const std::vector<LadderStep> ladder = {
        {"2160p", 2160, 14000}, {"1440p", 1440, 8000}, {"1080p", 1080, 5000}, {"720p", 720, 2800},
        {"480p", 480, 1400},    {"360p", 360, 800},    {"240p", 240, 400},
    };
    return ladder;
}

namespace {

int even(int v) { return v % 2 ? v + 1 : v; }

Rendition make_video(const char* name, int height, unsigned kbps, const ProbeResult& probe) {
    Rendition r;
    r.name = name;
    r.kind = "video";
    r.height = even(height);
    double aspect = probe.height > 0 ? static_cast<double>(probe.width) / probe.height : 16.0 / 9.0;
    r.width = even(static_cast<int>(std::lround(r.height * aspect)));
    if (probe.fps() >= 50) kbps = static_cast<unsigned>(kbps * 1.4);
    r.bitrate_kbps = kbps;
    r.path = std::format("v_{}", name);
    return r;
}

} // namespace

std::vector<Rendition> build_renditions(const ProbeResult& probe, const TranscodeOptions& opts,
                                        unsigned default_audio_kbps) {
    std::vector<Rendition> out;
    const int src_h = probe.height;

    for (const auto& step : default_ladder()) {
        if (step.height > src_h) continue;
        if (!opts.renditions.empty() &&
            std::find(opts.renditions.begin(), opts.renditions.end(), step.name) == opts.renditions.end())
            continue;
        out.push_back(make_video(step.name, step.height, step.bitrate_kbps, probe));
    }

    if (out.empty()) {
        // Source smaller than the smallest step, or the requested list didn't match: emit source-size rendition.
        unsigned kbps = static_cast<unsigned>(std::clamp(src_h * src_h * 0.0035 + 300.0, 300.0, 6000.0));
        std::string name = std::format("{}p", even(src_h));
        out.push_back(make_video(name.c_str(), src_h, kbps, probe));
        out.back().name = name;
        out.back().path = "v_" + name;
    }

    if (probe.has_audio) {
        Rendition a;
        a.name = "audio";
        a.kind = "audio";
        a.bitrate_kbps = opts.audio_bitrate_kbps ? opts.audio_bitrate_kbps : default_audio_kbps;
        a.path = std::format("a_{}k", a.bitrate_kbps);
        out.push_back(a);
    }
    return out;
}

} // namespace vsp

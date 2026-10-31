#pragma once

#include "media/probe.hpp"
#include "storage/models.hpp"

#include <vector>

namespace vsp {

struct LadderStep {
    const char* name;
    int height;
    unsigned bitrate_kbps;
};

// Standard ABR ladder (H.264). Steps above the source resolution are never emitted (no upscaling).
const std::vector<LadderStep>& default_ladder();

// Builds the rendition list (video renditions + optional audio) for a source.
std::vector<Rendition> build_renditions(const ProbeResult& probe, const TranscodeOptions& opts,
                                        unsigned default_audio_kbps);

} // namespace vsp

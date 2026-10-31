#pragma once

typedef struct _GstElement GstElement;

namespace vsp {

// Works around libav's MXF demuxer advertising AVC-Intra / H.264 essence as
// stream-format=avc with an Annex-B SPS/PPS blob in codec_data (which h264parse rejects).
// Installs a `deep-element-added` hook on `bin` that, for every avdemux_mxf created inside it,
// rewrites the video caps to byte-stream and prepends the SPS/PPS to the first buffer so the
// stream becomes self-describing. No-op for every other demuxer/codec.
void install_mxf_h264_fixup(GstElement* bin);

} // namespace vsp

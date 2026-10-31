#pragma once

namespace vsp {

// Initialises GStreamer and applies platform-wide plugin preferences:
//  - prefers libav's MXF demuxer over mxfdemux (the latter fails or aborts on Avid OP-Atom and
//    Panasonic P2 flavours, which are common in broadcast archives).
// Safe to call more than once.
void gst_platform_init();

} // namespace vsp

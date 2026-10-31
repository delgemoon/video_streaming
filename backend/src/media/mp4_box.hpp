#pragma once

// Minimal ISOBMFF (MP4) box parsing: enough to split a fragmented MP4 byte stream into
// init (ftyp+moov) and media fragments (moof+mdat), compute fragment durations and derive
// RFC 6381 codec strings. No allocation on the hot path.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace vsp::mp4 {

struct BoxHeader {
    uint64_t size = 0;        // total box size including header
    char type[5] = {0};       // NUL-terminated FourCC
    size_t header_size = 8;   // 8, or 16 when largesize is used
    std::string_view fourcc() const { return std::string_view(type, 4); }
};

// Returns nullopt if fewer than header_size bytes are available.
std::optional<BoxHeader> parse_box_header(const uint8_t* data, size_t len);

// Iterates child boxes within a container payload. Callback receives (fourcc, box_start, box_total_len).
// Returning false from the callback stops iteration.
void for_each_box(const uint8_t* data, size_t len,
                  const std::function<bool(std::string_view, const uint8_t*, size_t)>& cb);

// Finds the first box with the given fourcc inside a container payload. Returns pointer to box start.
const uint8_t* find_box(const uint8_t* data, size_t len, std::string_view fourcc, size_t* out_len);

// Walks a path like {"trak","mdia","mdhd"} starting from the payload of `data`.
const uint8_t* find_path(const uint8_t* data, size_t len, std::initializer_list<std::string_view> path,
                         size_t* out_len);

struct TrackInfo {
    uint32_t timescale = 0;
    uint32_t trex_default_duration = 0;
    uint32_t trex_default_flags = 0;
    std::string codec;           // "avc1.640028", "mp4a.40.2", or empty if unknown
    bool is_video = false;
    bool is_audio = false;
};

// Parses the first track of a moov box (box start pointer, full length).
std::optional<TrackInfo> parse_moov(const uint8_t* moov, size_t len);

struct FragmentInfo {
    uint64_t duration_ticks = 0;    // sum of sample durations of the first traf
    uint32_t sample_count = 0;
    bool first_sample_is_sync = true;
};

// Parses a moof box (box start pointer, full length).
FragmentInfo parse_moof(const uint8_t* moof, size_t len, const TrackInfo& track);

uint32_t be32(const uint8_t* p);
uint64_t be64(const uint8_t* p);
uint16_t be16(const uint8_t* p);

} // namespace vsp::mp4

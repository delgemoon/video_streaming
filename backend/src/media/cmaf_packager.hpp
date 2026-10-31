#pragma once

// Consumes the byte stream emitted by a fragmented mp4mux (ftyp moov [moof mdat]*) and
// writes CMAF segments + an HLS media playlist. Each moof+mdat fragment becomes an LL-HLS
// Partial Segment (exposed as a BYTERANGE of its parent segment file), and `segment_ms`
// worth of fragments become one segment. The playlist is rewritten atomically after every
// fragment while transcoding (EVENT playlist with EXT-X-PART) and finalized as VOD at EOS,
// which lets players start watching before the transcode is complete.

#include "media/mp4_box.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace vsp {

struct PackagerConfig {
    std::filesystem::path out_dir;
    unsigned segment_ms = 2000;
    unsigned part_ms = 1000;        // 0 = parts disabled
    std::string name;               // for logging
};

struct PackagerStats {
    uint64_t total_bytes = 0;
    double total_duration_s = 0;
    unsigned peak_bandwidth = 0;    // bits/s over the worst segment
    unsigned avg_bandwidth = 0;
    unsigned segment_count = 0;
    std::string codec;              // RFC 6381
    bool has_init = false;
};

class CmafPackager {
public:
    explicit CmafPackager(PackagerConfig cfg);
    ~CmafPackager();

    // Thread-safe; called from the GStreamer appsink streaming thread.
    void feed(const uint8_t* data, size_t len);
    // Flushes the current segment, writes the final VOD playlist. Idempotent.
    void finish();

    PackagerStats stats() const;
    bool has_init() const;
    std::string codec() const;

private:
    PackagerStats stats_unlocked() const;
    struct Part {
        uint64_t offset = 0;
        uint64_t size = 0;
        double duration_s = 0;
        bool independent = true;
    };
    struct Segment {
        unsigned index = 0;
        std::string filename;
        double duration_s = 0;
        uint64_t bytes = 0;
        std::vector<Part> parts;
        bool closed = false;
    };

    void process_pending();
    void on_box(std::string_view type, const uint8_t* box, size_t len);
    void on_init_complete();
    void on_fragment(const uint8_t* moof, size_t moof_len, const uint8_t* mdat, size_t mdat_len);
    void open_segment();
    void close_segment();
    void write_playlist(bool final);
    void atomic_write(const std::filesystem::path& p, const std::string& content);

    PackagerConfig cfg_;
    mutable std::mutex mu_;

    std::vector<uint8_t> pending_;           // bytes not yet forming a complete box
    std::vector<uint8_t> init_;              // ftyp + moov
    std::vector<uint8_t> moof_;              // last moof, waiting for its mdat
    std::vector<uint8_t> prefix_;            // styp/sidx/etc. preceding a moof
    bool init_done_ = false;
    bool finished_ = false;
    mp4::TrackInfo track_;

    std::vector<Segment> segments_;
    std::ofstream seg_file_;
    unsigned next_index_ = 1;
    double max_part_s_ = 0;
    double max_segment_s_ = 0;
};

} // namespace vsp

#include "media/cmaf_packager.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <spdlog/spdlog.h>

namespace vsp {

CmafPackager::CmafPackager(PackagerConfig cfg) : cfg_(std::move(cfg)) {
    if (cfg_.part_ms == 0 || cfg_.part_ms > cfg_.segment_ms) cfg_.part_ms = cfg_.segment_ms;
    std::filesystem::create_directories(cfg_.out_dir);
    pending_.reserve(1 << 20);
}

CmafPackager::~CmafPackager() {
    if (!finished_) {
        try { finish(); } catch (...) {}
    }
}

bool CmafPackager::has_init() const {
    std::lock_guard lk(mu_);
    return init_done_;
}

std::string CmafPackager::codec() const {
    std::lock_guard lk(mu_);
    return track_.codec;
}

PackagerStats CmafPackager::stats() const {
    std::lock_guard lk(mu_);
    return stats_unlocked();
}

PackagerStats CmafPackager::stats_unlocked() const {
    PackagerStats s;
    s.has_init = init_done_;
    s.codec = track_.codec;
    for (const auto& seg : segments_) {
        s.total_bytes += seg.bytes;
        s.total_duration_s += seg.duration_s;
        if (seg.duration_s > 0.2) {
            auto bw = static_cast<unsigned>(seg.bytes * 8.0 / seg.duration_s);
            s.peak_bandwidth = std::max(s.peak_bandwidth, bw);
        }
    }
    s.segment_count = static_cast<unsigned>(segments_.size());
    if (s.total_duration_s > 0) s.avg_bandwidth = static_cast<unsigned>(s.total_bytes * 8.0 / s.total_duration_s);
    if (s.peak_bandwidth < s.avg_bandwidth) s.peak_bandwidth = s.avg_bandwidth;
    return s;
}

void CmafPackager::feed(const uint8_t* data, size_t len) {
    std::lock_guard lk(mu_);
    if (finished_) return;
    pending_.insert(pending_.end(), data, data + len);
    process_pending();
}

void CmafPackager::process_pending() {
    size_t off = 0;
    while (true) {
        auto h = mp4::parse_box_header(pending_.data() + off, pending_.size() - off);
        if (!h) break;
        if (off + h->size > pending_.size()) break;   // wait for the rest of this box
        on_box(h->fourcc(), pending_.data() + off, static_cast<size_t>(h->size));
        off += static_cast<size_t>(h->size);
    }
    if (off) pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(off));
}

void CmafPackager::on_box(std::string_view type, const uint8_t* box, size_t len) {
    if (!init_done_) {
        if (type == "ftyp" || type == "free" || type == "skip") {
            init_.insert(init_.end(), box, box + len);
        } else if (type == "moov") {
            init_.insert(init_.end(), box, box + len);
            on_init_complete();
        } else {
            spdlog::warn("[{}] unexpected box '{}' before moov, ignoring", cfg_.name, type);
        }
        return;
    }
    if (type == "moof") {
        moof_.assign(box, box + len);
    } else if (type == "mdat") {
        if (moof_.empty()) {
            spdlog::warn("[{}] mdat without moof, dropping {} bytes", cfg_.name, len);
            return;
        }
        on_fragment(moof_.data(), moof_.size(), box, len);
        moof_.clear();
        prefix_.clear();
    } else if (type == "styp" || type == "sidx" || type == "prft" || type == "emsg") {
        prefix_.insert(prefix_.end(), box, box + len);
    } else if (type == "mfra" || type == "free" || type == "skip") {
        // trailing/padding boxes: ignored
    } else {
        spdlog::debug("[{}] ignoring box '{}' ({} bytes)", cfg_.name, type, len);
    }
}

void CmafPackager::on_init_complete() {
    size_t moov_len = 0;
    const uint8_t* moov = mp4::find_box(init_.data(), init_.size(), "moov", &moov_len);
    if (auto t = moov ? mp4::parse_moov(moov, moov_len) : std::nullopt) {
        track_ = *t;
    } else {
        spdlog::error("[{}] could not parse moov; durations will be estimated", cfg_.name);
        track_.timescale = 1000;
    }
    std::ofstream f(cfg_.out_dir / "init.mp4", std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(init_.data()), static_cast<std::streamsize>(init_.size()));
    init_done_ = true;
    spdlog::debug("[{}] init segment {} bytes, timescale {}, codec {}", cfg_.name, init_.size(),
                  track_.timescale, track_.codec);
    write_playlist(false);
}

void CmafPackager::open_segment() {
    Segment s;
    s.index = next_index_++;
    s.filename = std::format("seg_{:05}.m4s", s.index);
    segments_.push_back(std::move(s));
    seg_file_.open(cfg_.out_dir / segments_.back().filename, std::ios::binary | std::ios::trunc);
}

void CmafPackager::close_segment() {
    if (segments_.empty() || segments_.back().closed) return;
    seg_file_.close();
    auto& s = segments_.back();
    s.closed = true;
    max_segment_s_ = std::max(max_segment_s_, s.duration_s);
}

void CmafPackager::on_fragment(const uint8_t* moof, size_t moof_len, const uint8_t* mdat, size_t mdat_len) {
    auto fi = mp4::parse_moof(moof, moof_len, track_);
    double dur_s = track_.timescale ? static_cast<double>(fi.duration_ticks) / track_.timescale : 0.0;
    if (dur_s <= 0) dur_s = cfg_.part_ms / 1000.0;   // defensive: never emit zero-length parts

    if (segments_.empty() || segments_.back().closed) open_segment();
    auto& seg = segments_.back();

    Part p;
    p.offset = seg.bytes;
    p.size = prefix_.size() + moof_len + mdat_len;
    p.duration_s = dur_s;
    p.independent = fi.first_sample_is_sync;

    if (!prefix_.empty())
        seg_file_.write(reinterpret_cast<const char*>(prefix_.data()), static_cast<std::streamsize>(prefix_.size()));
    seg_file_.write(reinterpret_cast<const char*>(moof), static_cast<std::streamsize>(moof_len));
    seg_file_.write(reinterpret_cast<const char*>(mdat), static_cast<std::streamsize>(mdat_len));
    seg_file_.flush();

    seg.bytes += p.size;
    seg.duration_s += dur_s;
    seg.parts.push_back(p);
    max_part_s_ = std::max(max_part_s_, dur_s);

    // Close once we've reached the target; the half-part tolerance absorbs audio frame granularity.
    double target_s = cfg_.segment_ms / 1000.0;
    double tol_s = cfg_.part_ms / 2000.0;
    if (seg.duration_s >= target_s - tol_s) close_segment();

    write_playlist(false);
}

void CmafPackager::finish() {
    std::lock_guard lk(mu_);
    if (finished_) return;
    finished_ = true;
    if (!init_done_) {
        spdlog::warn("[{}] finished without receiving an init segment", cfg_.name);
        return;
    }
    close_segment();
    write_playlist(true);
    auto s = stats_unlocked();
    spdlog::info("[{}] packaged {} segments, {:.2f}s, {} bytes", cfg_.name, segments_.size(), s.total_duration_s,
                 s.total_bytes);
}

void CmafPackager::write_playlist(bool final) {
    const bool parts_enabled = cfg_.part_ms < cfg_.segment_ms;
    const double nominal_target = cfg_.segment_ms / 1000.0;
    // TARGETDURATION must be >= every EXTINF rounded to the nearest integer.
    unsigned target = static_cast<unsigned>(std::max(std::lround(nominal_target), std::lround(max_segment_s_)));
    if (!segments_.empty() && !segments_.back().closed)
        target = std::max<unsigned>(target, static_cast<unsigned>(std::lround(segments_.back().duration_s)));
    if (target == 0) target = 1;
    double part_target = std::max(cfg_.part_ms / 1000.0, std::ceil(max_part_s_ * 1000.0) / 1000.0);

    std::string pl;
    pl.reserve(256 + segments_.size() * 96);
    pl += "#EXTM3U\n";
    pl += std::format("#EXT-X-VERSION:{}\n", parts_enabled && !final ? 9 : 7);
    pl += std::format("#EXT-X-TARGETDURATION:{}\n", target);
    pl += std::format("#EXT-X-PLAYLIST-TYPE:{}\n", final ? "VOD" : "EVENT");
    pl += "#EXT-X-INDEPENDENT-SEGMENTS\n";
    if (parts_enabled && !final) {
        pl += std::format("#EXT-X-SERVER-CONTROL:PART-HOLD-BACK={:.3f}\n", part_target * 3);
        pl += std::format("#EXT-X-PART-INF:PART-TARGET={:.3f}\n", part_target);
    }
    pl += "#EXT-X-MEDIA-SEQUENCE:0\n";
    pl += "#EXT-X-MAP:URI=\"init.mp4\"\n";

    // LL-HLS: parts are only listed for the last few segments (spec recommends >= 3 target durations).
    const size_t parts_from = segments_.size() > 3 ? segments_.size() - 3 : 0;
    for (size_t i = 0; i < segments_.size(); ++i) {
        const auto& s = segments_[i];
        if (parts_enabled && !final && i >= parts_from) {
            for (const auto& p : s.parts) {
                pl += std::format("#EXT-X-PART:DURATION={:.5f},URI=\"{}\",BYTERANGE=\"{}@{}\"{}\n", p.duration_s,
                                  s.filename, p.size, p.offset, p.independent ? ",INDEPENDENT=YES" : "");
            }
        }
        if (s.closed) {
            pl += std::format("#EXTINF:{:.5f},\n{}\n", s.duration_s, s.filename);
        }
    }
    if (final) pl += "#EXT-X-ENDLIST\n";

    atomic_write(cfg_.out_dir / "index.m3u8", pl);
}

void CmafPackager::atomic_write(const std::filesystem::path& p, const std::string& content) {
    auto tmp = p;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f.write(content.data(), static_cast<std::streamsize>(content.size()));
    }
    std::error_code ec;
    std::filesystem::rename(tmp, p, ec);
    if (ec) spdlog::error("[{}] failed to publish {}: {}", cfg_.name, p.string(), ec.message());
}

} // namespace vsp

#include "media/cmaf_packager.hpp"

#include "core/util.hpp"
#include "observability/vsp_metrics.hpp"

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
    s.total_bytes = dropped_bytes_;
    s.total_duration_s = dropped_duration_s_;
    for (const auto& seg : segments_) {
        s.total_bytes += seg.bytes;
        s.total_duration_s += seg.duration_s;
        if (seg.duration_s > 0.2) {
            auto bw = static_cast<unsigned>(seg.bytes * 8.0 / seg.duration_s);
            s.peak_bandwidth = std::max(s.peak_bandwidth, bw);
        }
    }
    s.segment_count = static_cast<unsigned>(segments_.size()) + dropped_segments_;
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
    s.filename = std::format("seg_{:06}.m4s", s.index);
    if (segments_.empty()) media_sequence_ = s.index;
    segments_.push_back(std::move(s));
    seg_file_.open(cfg_.out_dir / segments_.back().filename, std::ios::binary | std::ios::trunc);
}

void CmafPackager::close_segment() {
    if (segments_.empty() || segments_.back().closed) return;
    seg_file_.close();
    auto& s = segments_.back();
    s.closed = true;
    max_segment_s_ = std::max(max_segment_s_, s.duration_s);
    const char* mode = cfg_.live ? "live" : "vod";
    metrics::M().packager_segments_total->with({mode})->inc();
    metrics::M().packager_segment_duration->with({mode})->observe(s.duration_s);
    if (cfg_.live && cfg_.window_segments) trim_window();
}

void CmafPackager::trim_window() {
    size_t closed = segments_.size() - (segments_.back().closed ? 0 : 1);
    while (closed > cfg_.window_segments) {
        const auto& old = segments_.front();
        dropped_bytes_ += old.bytes;
        dropped_duration_s_ += old.duration_s;
        ++dropped_segments_;
        std::error_code ec;
        std::filesystem::remove(cfg_.out_dir / old.filename, ec);
        segments_.erase(segments_.begin());
        --closed;
        if (!segments_.empty()) media_sequence_ = segments_.front().index;
    }
}

void CmafPackager::on_fragment(const uint8_t* moof, size_t moof_len, const uint8_t* mdat, size_t mdat_len) {
    auto fi = mp4::parse_moof(moof, moof_len, track_);
    double dur_s = track_.timescale ? static_cast<double>(fi.duration_ticks) / track_.timescale : 0.0;
    if (dur_s <= 0) dur_s = cfg_.part_ms / 1000.0;   // defensive: never emit zero-length parts

    // Segments must start on a keyframe (EXT-X-INDEPENDENT-SEGMENTS), so a segment that has reached
    // its target length is closed when the *next* independent fragment arrives. Parts inside a
    // segment may start on any frame. Audio samples are all sync, so audio closes exactly on target.
    const double target_s = cfg_.segment_ms / 1000.0;
    const double tol_s = cfg_.part_ms / 2000.0;
    if (!segments_.empty() && !segments_.back().closed) {
        const auto& cur = segments_.back();
        const bool due = cur.duration_s >= target_s - tol_s;
        const bool force = cur.duration_s >= 2 * target_s;   // publisher keyint longer than 2 segments
        if ((due && fi.first_sample_is_sync) || force) close_segment();
    }
    if (segments_.empty() || segments_.back().closed) open_segment();
    auto& seg = segments_.back();
    // The first fragment completes one fragment-duration after its first frame was captured.
    if (cfg_.live && !pdt_anchor_ms_) pdt_anchor_ms_ = util::now_ms() - static_cast<int64_t>(dur_s * 1000);

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
    {
        const char* mode = cfg_.live ? "live" : "vod";
        metrics::M().packager_parts_total->with({mode})->inc();
        metrics::M().packager_bytes_total->with({mode})->inc(static_cast<double>(p.size));
        if (cfg_.live) {
            // ingest-to-published latency: the newest input that could be in this part arrived at
            // last_input_arrival_ms_; the part is on disk now. Covers demux/decode/encode/mux/write.
            int64_t arrived = last_input_arrival_ms_.load(std::memory_order_relaxed);
            if (arrived) {
                double lat = static_cast<double>(util::now_ms() - arrived) / 1000.0;
                if (lat >= 0 && lat < 60) metrics::M().live_part_latency->get()->observe(lat);
            }
        }
    }

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
    const bool sliding = cfg_.live && cfg_.window_segments > 0;
    pl += std::format("#EXT-X-VERSION:{}\n", parts_enabled && !final ? 9 : 7);
    pl += std::format("#EXT-X-TARGETDURATION:{}\n", target);
    // A sliding-window live playlist has no PLAYLIST-TYPE; a growing one is EVENT; finished is VOD.
    if (final && !sliding) pl += "#EXT-X-PLAYLIST-TYPE:VOD\n";
    else if (!sliding) pl += "#EXT-X-PLAYLIST-TYPE:EVENT\n";
    pl += "#EXT-X-INDEPENDENT-SEGMENTS\n";
    if (!final) {
        std::string sc = cfg_.live ? "CAN-BLOCK-RELOAD=YES" : "";
        if (parts_enabled) sc += std::format("{}PART-HOLD-BACK={:.3f}", sc.empty() ? "" : ",", part_target * 3);
        if (cfg_.live) sc += std::format(",HOLD-BACK={:.3f}", std::max(3.0 * target, nominal_target * 3));
        if (!sc.empty()) pl += "#EXT-X-SERVER-CONTROL:" + sc + "\n";
        if (parts_enabled) pl += std::format("#EXT-X-PART-INF:PART-TARGET={:.3f}\n", part_target);
    }
    pl += std::format("#EXT-X-MEDIA-SEQUENCE:{}\n", media_sequence_);
    pl += "#EXT-X-MAP:URI=\"init.mp4\"\n";
    if (cfg_.live && pdt_anchor_ms_ && !segments_.empty())
        pl += std::format("#EXT-X-PROGRAM-DATE-TIME:{}\n",
                          util::iso8601(pdt_anchor_ms_ + static_cast<int64_t>(dropped_duration_s_ * 1000)));

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

    if (cfg_.on_update) {
        PlaylistUpdate u;
        u.media_sequence = media_sequence_;
        u.last_segment = segments_.empty() ? 0 : segments_.back().index;
        u.last_part = segments_.empty() || segments_.back().parts.empty()
                          ? 0 : static_cast<unsigned>(segments_.back().parts.size() - 1);
        u.final = final;
        cfg_.on_update(cfg_.out_dir / "index.m3u8", u);
    }
}

void CmafPackager::atomic_write(const std::filesystem::path& p, const std::string& content) {
    metrics::Timer t(metrics::M().packager_write_duration->get());
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

#include "media/mp4_box.hpp"

#include <cstring>
#include <format>

namespace vsp::mp4 {

uint16_t be16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }
uint32_t be32(const uint8_t* p) {
    return (uint32_t{p[0]} << 24) | (uint32_t{p[1]} << 16) | (uint32_t{p[2]} << 8) | uint32_t{p[3]};
}
uint64_t be64(const uint8_t* p) { return (uint64_t{be32(p)} << 32) | be32(p + 4); }

std::optional<BoxHeader> parse_box_header(const uint8_t* data, size_t len) {
    if (len < 8) return std::nullopt;
    BoxHeader h;
    h.size = be32(data);
    std::memcpy(h.type, data + 4, 4);
    h.header_size = 8;
    if (h.size == 1) {
        if (len < 16) return std::nullopt;
        h.size = be64(data + 8);
        h.header_size = 16;
    } else if (h.size == 0) {
        h.size = len;   // "to end of stream" — only meaningful for the last box in a file
    }
    if (h.size < h.header_size) return std::nullopt;   // corrupt
    return h;
}

void for_each_box(const uint8_t* data, size_t len,
                  const std::function<bool(std::string_view, const uint8_t*, size_t)>& cb) {
    size_t off = 0;
    while (off + 8 <= len) {
        auto h = parse_box_header(data + off, len - off);
        if (!h || h->size == 0 || off + h->size > len) break;
        if (!cb(h->fourcc(), data + off, static_cast<size_t>(h->size))) break;
        off += static_cast<size_t>(h->size);
    }
}

const uint8_t* find_box(const uint8_t* data, size_t len, std::string_view fourcc, size_t* out_len) {
    const uint8_t* found = nullptr;
    for_each_box(data, len, [&](std::string_view t, const uint8_t* p, size_t n) {
        if (t == fourcc) {
            found = p;
            if (out_len) *out_len = n;
            return false;
        }
        return true;
    });
    return found;
}

namespace {

// Returns pointer/len of a box's payload (skipping its header).
bool payload_of(const uint8_t* box, size_t box_len, const uint8_t** payload, size_t* payload_len) {
    auto h = parse_box_header(box, box_len);
    if (!h) return false;
    *payload = box + h->header_size;
    *payload_len = box_len - h->header_size;
    return true;
}

} // namespace

const uint8_t* find_path(const uint8_t* data, size_t len, std::initializer_list<std::string_view> path,
                         size_t* out_len) {
    const uint8_t* cur = data;
    size_t cur_len = len;
    const uint8_t* box = nullptr;
    size_t box_len = 0;
    for (auto fourcc : path) {
        box = find_box(cur, cur_len, fourcc, &box_len);
        if (!box) return nullptr;
        if (!payload_of(box, box_len, &cur, &cur_len)) return nullptr;
    }
    if (out_len) *out_len = box_len;
    return box;
}

namespace {

std::string parse_avcC(const uint8_t* p, size_t n) {
    // p points to avcC box start
    const uint8_t* pl;
    size_t pln;
    if (!payload_of(p, n, &pl, &pln) || pln < 4) return {};
    return std::format("avc1.{:02X}{:02X}{:02X}", pl[1], pl[2], pl[3]);
}

std::string parse_hvcC(const uint8_t* p, size_t n) {
    const uint8_t* pl;
    size_t pln;
    if (!payload_of(p, n, &pl, &pln) || pln < 13) return {};
    uint8_t profile_space = pl[1] >> 6;
    uint8_t tier = (pl[1] >> 5) & 1;
    uint8_t profile_idc = pl[1] & 0x1f;
    uint32_t compat = be32(pl + 2);
    uint32_t compat_rev = 0;   // the compatibility flags are written bit-reversed
    for (int i = 0; i < 32; ++i) if (compat & (1u << i)) compat_rev |= 1u << (31 - i);
    uint8_t level = pl[12];
    std::string out = "hvc1.";
    if (profile_space) out += static_cast<char>('A' + profile_space - 1);
    out += std::format("{}.{:X}.{}{}", profile_idc, compat_rev, tier ? 'H' : 'L', level);
    // constraint bytes: append non-zero trailing bytes
    std::string cons;
    for (int i = 5; i >= 0; --i) {
        if (pl[6 + i] || !cons.empty()) cons = std::format(".{:X}", pl[6 + i]) + cons;
    }
    return out + cons;
}

// Reads an MPEG-4 descriptor length (1..4 bytes, 7 bits each, MSB continuation).
size_t read_desc_len(const uint8_t*& p, const uint8_t* end) {
    size_t len = 0;
    for (int i = 0; i < 4 && p < end; ++i) {
        uint8_t b = *p++;
        len = (len << 7) | (b & 0x7f);
        if (!(b & 0x80)) break;
    }
    return len;
}

std::string parse_esds(const uint8_t* p, size_t n) {
    const uint8_t* pl;
    size_t pln;
    if (!payload_of(p, n, &pl, &pln) || pln < 4) return {};
    const uint8_t* cur = pl + 4;   // skip fullbox version/flags
    const uint8_t* end = pl + pln;
    if (cur >= end || *cur != 0x03) return "mp4a.40.2";
    ++cur;
    read_desc_len(cur, end);
    if (cur + 3 > end) return "mp4a.40.2";
    uint8_t flags = cur[2];
    cur += 3;
    if (flags & 0x80) cur += 2;
    if (flags & 0x40) { if (cur < end) cur += 1 + *cur; }
    if (flags & 0x20) cur += 2;
    if (cur >= end || *cur != 0x04) return "mp4a.40.2";
    ++cur;
    read_desc_len(cur, end);
    if (cur + 13 > end) return "mp4a.40.2";
    uint8_t oti = cur[0];
    cur += 13;
    if (oti != 0x40) return std::format("mp4a.{:02X}", oti);
    if (cur >= end || *cur != 0x05) return "mp4a.40.2";
    ++cur;
    size_t dsi_len = read_desc_len(cur, end);
    if (dsi_len < 1 || cur >= end) return "mp4a.40.2";
    uint8_t aot = cur[0] >> 3;
    if (aot == 31 && cur + 1 < end) aot = 32 + (((cur[0] & 0x7) << 3) | (cur[1] >> 5));
    return std::format("mp4a.40.{}", aot);
}

} // namespace

std::optional<TrackInfo> parse_moov(const uint8_t* moov, size_t len) {
    const uint8_t* pl;
    size_t pln;
    if (!payload_of(moov, len, &pl, &pln)) return std::nullopt;
    TrackInfo info;

    size_t mdhd_len = 0;
    if (const uint8_t* mdhd = find_path(pl, pln, {"trak", "mdia", "mdhd"}, &mdhd_len)) {
        const uint8_t* m;
        size_t mn;
        if (payload_of(mdhd, mdhd_len, &m, &mn) && mn >= 4) {
            uint8_t version = m[0];
            if (version == 1 && mn >= 24) info.timescale = be32(m + 20);
            else if (version == 0 && mn >= 16) info.timescale = be32(m + 12);
        }
    }

    size_t hdlr_len = 0;
    if (const uint8_t* hdlr = find_path(pl, pln, {"trak", "mdia", "hdlr"}, &hdlr_len)) {
        const uint8_t* h;
        size_t hn;
        if (payload_of(hdlr, hdlr_len, &h, &hn) && hn >= 12) {
            std::string_view handler(reinterpret_cast<const char*>(h + 8), 4);
            info.is_video = handler == "vide";
            info.is_audio = handler == "soun";
        }
    }

    size_t trex_len = 0;
    if (const uint8_t* trex = find_path(pl, pln, {"mvex", "trex"}, &trex_len)) {
        const uint8_t* t;
        size_t tn;
        if (payload_of(trex, trex_len, &t, &tn) && tn >= 24) {
            info.trex_default_duration = be32(t + 12);
            info.trex_default_flags = be32(t + 20);
        }
    }

    size_t stsd_len = 0;
    if (const uint8_t* stsd = find_path(pl, pln, {"trak", "mdia", "minf", "stbl", "stsd"}, &stsd_len)) {
        const uint8_t* s;
        size_t sn;
        if (payload_of(stsd, stsd_len, &s, &sn) && sn >= 8) {
            const uint8_t* entries = s + 8;
            size_t entries_len = sn - 8;
            for_each_box(entries, entries_len, [&](std::string_view t, const uint8_t* e, size_t en) {
                const uint8_t* ep;
                size_t epn;
                if (!payload_of(e, en, &ep, &epn)) return false;
                if (t == "avc1" || t == "avc3" || t == "hvc1" || t == "hev1") {
                    if (epn <= 78) return false;
                    const uint8_t* children = ep + 78;
                    size_t children_len = epn - 78;
                    size_t cl = 0;
                    if (const uint8_t* c = find_box(children, children_len, "avcC", &cl)) info.codec = parse_avcC(c, cl);
                    else if (const uint8_t* c2 = find_box(children, children_len, "hvcC", &cl)) info.codec = parse_hvcC(c2, cl);
                    if (info.codec.empty()) info.codec = std::string(t) == "avc1" || t == "avc3" ? "avc1.64001F" : "hvc1.1.6.L93.B0";
                } else if (t == "mp4a") {
                    if (epn <= 28) return false;
                    const uint8_t* children = ep + 28;
                    size_t children_len = epn - 28;
                    size_t cl = 0;
                    if (const uint8_t* c = find_box(children, children_len, "esds", &cl)) info.codec = parse_esds(c, cl);
                    else info.codec = "mp4a.40.2";
                } else if (t == "Opus") {
                    info.codec = "Opus";
                } else if (t == "ac-3") {
                    info.codec = "ac-3";
                } else if (t == "ec-3") {
                    info.codec = "ec-3";
                } else if (t == "av01") {
                    info.codec = "av01.0.08M.08";
                } else if (t == "vp09") {
                    info.codec = "vp09.00.10.08";
                }
                return false;   // only the first sample entry
            });
        }
    }

    if (info.timescale == 0) return std::nullopt;
    return info;
}

FragmentInfo parse_moof(const uint8_t* moof, size_t len, const TrackInfo& track) {
    FragmentInfo fi;
    const uint8_t* pl;
    size_t pln;
    if (!payload_of(moof, len, &pl, &pln)) return fi;

    size_t traf_len = 0;
    const uint8_t* traf = find_box(pl, pln, "traf", &traf_len);
    if (!traf) return fi;
    const uint8_t* tp;
    size_t tpn;
    if (!payload_of(traf, traf_len, &tp, &tpn)) return fi;

    uint32_t default_duration = track.trex_default_duration;
    uint32_t default_flags = track.trex_default_flags;
    bool have_default_flags = false;

    size_t tfhd_len = 0;
    if (const uint8_t* tfhd = find_box(tp, tpn, "tfhd", &tfhd_len)) {
        const uint8_t* h;
        size_t hn;
        if (payload_of(tfhd, tfhd_len, &h, &hn) && hn >= 8) {
            uint32_t flags = be32(h) & 0xffffff;
            const uint8_t* cur = h + 8;
            const uint8_t* end = h + hn;
            if (flags & 0x1) cur += 8;
            if (flags & 0x2) cur += 4;
            if ((flags & 0x8) && cur + 4 <= end) { default_duration = be32(cur); cur += 4; }
            if ((flags & 0x10) && cur + 4 <= end) cur += 4;
            if ((flags & 0x20) && cur + 4 <= end) { default_flags = be32(cur); have_default_flags = true; }
        }
    }

    bool first_trun = true;
    for_each_box(tp, tpn, [&](std::string_view t, const uint8_t* b, size_t bn) {
        if (t != "trun") return true;
        const uint8_t* r;
        size_t rn;
        if (!payload_of(b, bn, &r, &rn) || rn < 8) return true;
        uint32_t flags = be32(r) & 0xffffff;
        uint32_t count = be32(r + 4);
        const uint8_t* cur = r + 8;
        const uint8_t* end = r + rn;
        if (flags & 0x1) cur += 4;
        std::optional<uint32_t> first_sample_flags;
        if (flags & 0x4) { if (cur + 4 <= end) first_sample_flags = be32(cur); cur += 4; }
        size_t per_sample = (flags & 0x100 ? 4 : 0) + (flags & 0x200 ? 4 : 0) + (flags & 0x400 ? 4 : 0) +
                            (flags & 0x800 ? 4 : 0);
        for (uint32_t i = 0; i < count && cur + per_sample <= end; ++i) {
            uint32_t dur = default_duration;
            uint32_t sflags = default_flags;
            if (flags & 0x100) { dur = be32(cur); cur += 4; }
            if (flags & 0x200) cur += 4;
            if (flags & 0x400) { sflags = be32(cur); cur += 4; }
            if (flags & 0x800) cur += 4;
            fi.duration_ticks += dur;
            if (first_trun && i == 0) {
                uint32_t f = first_sample_flags ? *first_sample_flags : sflags;
                bool known = first_sample_flags || (flags & 0x400) || have_default_flags || track.trex_default_flags;
                fi.first_sample_is_sync = !known || ((f >> 16) & 1) == 0;
            }
        }
        fi.sample_count += count;
        first_trun = false;
        return true;
    });
    return fi;
}

} // namespace vsp::mp4

// Unit tests for the ISOBMFF parser using hand-built boxes (no GStreamer needed).
#include "media/mp4_box.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace vsp::mp4;

namespace {

int failures = 0;
#define CHECK(cond)                                                                                     \
    do {                                                                                                \
        if (!(cond)) {                                                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                 \
            ++failures;                                                                                 \
        }                                                                                               \
    } while (0)

void put32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(x >> 24); v.push_back(x >> 16); v.push_back(x >> 8); v.push_back(x);
}
void put16(std::vector<uint8_t>& v, uint16_t x) { v.push_back(x >> 8); v.push_back(x); }
void put_fourcc(std::vector<uint8_t>& v, const char* f) { v.insert(v.end(), f, f + 4); }

std::vector<uint8_t> box(const char* type, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> out;
    put32(out, static_cast<uint32_t>(8 + payload.size()));
    put_fourcc(out, type);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}
std::vector<uint8_t> cat(std::initializer_list<std::vector<uint8_t>> parts) {
    std::vector<uint8_t> out;
    for (const auto& p : parts) out.insert(out.end(), p.begin(), p.end());
    return out;
}

std::vector<uint8_t> fullbox_hdr(uint8_t version, uint32_t flags) {
    std::vector<uint8_t> v;
    put32(v, (uint32_t{version} << 24) | (flags & 0xffffff));
    return v;
}

std::vector<uint8_t> make_moov_video() {
    std::vector<uint8_t> mdhd = fullbox_hdr(0, 0);
    put32(mdhd, 0); put32(mdhd, 0); put32(mdhd, 90000); put32(mdhd, 0); put16(mdhd, 0x55c4); put16(mdhd, 0);
    std::vector<uint8_t> hdlr = fullbox_hdr(0, 0);
    put32(hdlr, 0); put_fourcc(hdlr, "vide"); put32(hdlr, 0); put32(hdlr, 0); put32(hdlr, 0); hdlr.push_back(0);

    std::vector<uint8_t> avcC = {1, 0x64, 0x00, 0x28, 0xff, 0xe1, 0, 0, 1, 0, 0};
    std::vector<uint8_t> avc1(78, 0);
    put16(avc1, 0); avc1.resize(78);
    auto avc1_box = box("avc1", cat({avc1, box("avcC", avcC)}));
    std::vector<uint8_t> stsd = fullbox_hdr(0, 0);
    put32(stsd, 1);
    stsd = cat({stsd, avc1_box});
    auto stbl = box("stbl", box("stsd", stsd));
    auto minf = box("minf", stbl);
    auto mdia = box("mdia", cat({box("mdhd", mdhd), box("hdlr", hdlr), minf}));
    auto trak = box("trak", mdia);

    std::vector<uint8_t> trex = fullbox_hdr(0, 0);
    put32(trex, 1); put32(trex, 1); put32(trex, 3000); put32(trex, 0); put32(trex, 0x10000);
    auto mvex = box("mvex", box("trex", trex));
    return box("moov", cat({trak, mvex}));
}

std::vector<uint8_t> make_moov_audio() {
    std::vector<uint8_t> mdhd = fullbox_hdr(1, 0);
    mdhd.insert(mdhd.end(), 16, 0); put32(mdhd, 48000); mdhd.insert(mdhd.end(), 8, 0); put32(mdhd, 0);
    std::vector<uint8_t> hdlr = fullbox_hdr(0, 0);
    put32(hdlr, 0); put_fourcc(hdlr, "soun"); hdlr.insert(hdlr.end(), 13, 0);

    // esds: ES_Descriptor(3) { DecoderConfig(4) { oti=0x40 ... DecoderSpecificInfo(5) { AAC-LC, 48k, stereo } } }
    std::vector<uint8_t> esds = fullbox_hdr(0, 0);
    std::vector<uint8_t> dsi = {0x11, 0x90};                       // AOT=2 (5 bits) ...
    std::vector<uint8_t> dcd = {0x40, 0x15, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    dcd.push_back(0x05); dcd.push_back(static_cast<uint8_t>(dsi.size())); dcd.insert(dcd.end(), dsi.begin(), dsi.end());
    std::vector<uint8_t> esd = {0, 1, 0};
    esd.push_back(0x04); esd.push_back(static_cast<uint8_t>(dcd.size())); esd.insert(esd.end(), dcd.begin(), dcd.end());
    esds.push_back(0x03); esds.push_back(static_cast<uint8_t>(esd.size())); esds.insert(esds.end(), esd.begin(), esd.end());

    std::vector<uint8_t> mp4a(28, 0);
    auto mp4a_box = box("mp4a", cat({mp4a, box("esds", esds)}));
    std::vector<uint8_t> stsd = fullbox_hdr(0, 0);
    put32(stsd, 1);
    stsd = cat({stsd, mp4a_box});
    auto mdia = box("mdia", cat({box("mdhd", mdhd), box("hdlr", hdlr), box("minf", box("stbl", box("stsd", stsd)))}));
    return box("moov", box("trak", mdia));
}

std::vector<uint8_t> make_moof(uint32_t samples, uint32_t default_dur, bool per_sample_dur, bool first_sync) {
    std::vector<uint8_t> tfhd = fullbox_hdr(0, 0x8 | 0x20);
    put32(tfhd, 1); put32(tfhd, default_dur); put32(tfhd, 0x1010000);   // default flags: non-sync
    uint32_t trun_flags = 0x1 | 0x4 | (per_sample_dur ? 0x100 : 0) | 0x200;
    std::vector<uint8_t> trun = fullbox_hdr(0, trun_flags);
    put32(trun, samples); put32(trun, 0);
    put32(trun, first_sync ? 0x2000000 : 0x1010000);
    for (uint32_t i = 0; i < samples; ++i) {
        if (per_sample_dur) put32(trun, default_dur + i);
        put32(trun, 100);
    }
    auto traf = box("traf", cat({box("tfhd", tfhd), box("trun", trun)}));
    std::vector<uint8_t> mfhd = fullbox_hdr(0, 0);
    put32(mfhd, 1);
    return box("moof", cat({box("mfhd", mfhd), traf}));
}

} // namespace

int main() {
    // header parsing
    {
        std::vector<uint8_t> b = box("ftyp", {1, 2, 3, 4});
        auto h = parse_box_header(b.data(), b.size());
        CHECK(h && h->size == 12 && h->fourcc() == "ftyp" && h->header_size == 8);
        CHECK(!parse_box_header(b.data(), 7));
        std::vector<uint8_t> large = {0, 0, 0, 1, 'm', 'd', 'a', 't', 0, 0, 0, 0, 0, 0, 0, 24, 0, 0, 0, 0, 0, 0, 0, 0};
        auto hl = parse_box_header(large.data(), large.size());
        CHECK(hl && hl->size == 24 && hl->header_size == 16);
    }
    // moov video
    {
        auto moov = make_moov_video();
        auto t = parse_moov(moov.data(), moov.size());
        CHECK(t.has_value());
        if (t) {
            CHECK(t->timescale == 90000);
            CHECK(t->is_video);
            CHECK(t->codec == "avc1.640028");
            CHECK(t->trex_default_duration == 3000);
        }
    }
    // moov audio (v1 mdhd + esds)
    {
        auto moov = make_moov_audio();
        auto t = parse_moov(moov.data(), moov.size());
        CHECK(t.has_value());
        if (t) {
            CHECK(t->timescale == 48000);
            CHECK(t->is_audio);
            CHECK(t->codec == "mp4a.40.2");
        }
    }
    // moof durations
    {
        TrackInfo track;
        track.timescale = 90000;
        auto moof = make_moof(60, 1500, false, true);
        auto fi = parse_moof(moof.data(), moof.size(), track);
        CHECK(fi.sample_count == 60);
        CHECK(fi.duration_ticks == 60 * 1500);
        CHECK(fi.first_sample_is_sync);

        auto moof2 = make_moof(3, 100, true, false);
        auto fi2 = parse_moof(moof2.data(), moof2.size(), track);
        CHECK(fi2.duration_ticks == 100 + 101 + 102);
        CHECK(!fi2.first_sample_is_sync);
    }
    // path walking + find_box
    {
        auto moov = make_moov_video();
        size_t len = 0;
        const uint8_t* mdhd = find_path(moov.data() + 8, moov.size() - 8, {"trak", "mdia", "mdhd"}, &len);
        CHECK(mdhd != nullptr && len == 8 + 24);
        CHECK(find_box(moov.data(), moov.size(), "nope", nullptr) == nullptr);
    }

    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("all mp4 box tests passed\n");
    return 0;
}

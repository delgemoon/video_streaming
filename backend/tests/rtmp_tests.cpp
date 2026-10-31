// Protocol-level tests for the RTMP layer: AMF0 round-trips and chunk stream reassembly
// (including chunk-size changes, extended timestamps, interleaved chunk streams and type-3
// continuation headers), without any sockets.
#include "live/rtmp/amf0.hpp"
#include "live/rtmp/chunk.hpp"

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace vsp::rtmp;
namespace amf0 = vsp::amf0;

namespace {
int failures = 0;
#define CHECK(c)                                                                                        \
    do {                                                                                                \
        if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; }              \
    } while (0)

struct Collected {
    MessageHeader hdr;
    std::vector<uint8_t> payload;
};

// Feeds `bytes` to a reader in slices of `step` bytes (exercises partial-chunk handling).
std::vector<Collected> run_reader(const std::string& bytes, size_t step, uint32_t initial_chunk = 128) {
    std::vector<Collected> out;
    std::map<const void*, std::vector<uint8_t>*> live;
    ChunkReader reader(
        [&](const MessageHeader& h) {
            auto* v = new std::vector<uint8_t>(h.length);
            return MessageBuffer{v->data(), h.length, v};
        },
        [&](const MessageHeader& h, MessageBuffer b) {
            auto* v = static_cast<std::vector<uint8_t>*>(b.user);
            out.push_back({h, *v});
            if (h.type == kSetChunkSize && v->size() >= 4)
                reader.set_chunk_size((uint32_t{(*v)[0]} << 24) | (uint32_t{(*v)[1]} << 16) | (uint32_t{(*v)[2]} << 8) | (*v)[3]);
            delete v;
        });
    reader.set_chunk_size(initial_chunk);
    std::vector<uint8_t> pending;
    std::string err;
    for (size_t i = 0; i < bytes.size(); i += step) {
        pending.insert(pending.end(), bytes.begin() + i, bytes.begin() + std::min(bytes.size(), i + step));
        size_t used = reader.feed(pending.data(), pending.size(), err);
        CHECK(err.empty());
        pending.erase(pending.begin(), pending.begin() + used);
    }
    CHECK(pending.empty());
    return out;
}
} // namespace

int main() {
    // ---- AMF0 round trip
    {
        amf0::Object obj{{"app", "live"}, {"tcUrl", "rtmp://x/live"}, {"objectEncoding", 0.0}, {"fpad", false}};
        std::string enc = amf0::encode_all({"connect", 1.0, obj, amf0::Null{}});
        auto vals = amf0::decode_all(reinterpret_cast<const uint8_t*>(enc.data()), enc.size());
        CHECK(vals.size() == 4);
        CHECK(vals[0].str() == "connect");
        CHECK(vals[1].number() == 1.0);
        CHECK(vals[2].get("app") && vals[2].get("app")->str() == "live");
        CHECK(vals[2].get("fpad") && std::get<bool>(vals[2].get("fpad")->v) == false);
        CHECK(vals[3].is_null());
    }
    // ECMA array (as sent in onMetaData) and strict array
    {
        std::string ecma;
        ecma.push_back(0x08);
        ecma.append("\x00\x00\x00\x02", 4);
        ecma.append("\x00\x05width", 7);
        ecma.push_back(0x00);
        ecma.append("\x40\x9e\x00\x00\x00\x00\x00\x00", 8);   // 1920.0
        ecma.append("\x00\x06height", 8);
        ecma.push_back(0x00);
        ecma.append("\x40\x90\xe0\x00\x00\x00\x00\x00", 8);   // 1080.0
        ecma.append("\x00\x00\x09", 3);
        size_t pos = 0;
        auto v = amf0::decode(reinterpret_cast<const uint8_t*>(ecma.data()), ecma.size(), pos);
        CHECK(v && v->get("width") && v->get("width")->number() == 1920.0);
        CHECK(v && v->get("height") && v->get("height")->number() == 1080.0);
        CHECK(pos == ecma.size());

        std::string arr = amf0::encode_all({amf0::Array{1.0, "two", true}});
        auto a = amf0::decode_all(reinterpret_cast<const uint8_t*>(arr.data()), arr.size());
        CHECK(a.size() == 1 && std::holds_alternative<std::shared_ptr<amf0::Array>>(a[0].v));
    }

    // ---- chunk writer -> reader, small chunk size, multiple chunks per message
    {
        ChunkWriter w;
        w.set_chunk_size(128);
        std::vector<uint8_t> payload(1000);
        for (size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<uint8_t>(i * 7);
        MessageHeader h;
        h.type = kVideo;
        h.stream_id = 1;
        h.timestamp = 1234;
        std::string wire;
        w.write(wire, 6, h, payload.data(), payload.size());
        for (size_t step : {1, 7, 100, 4096}) {
            auto got = run_reader(wire, step);
            CHECK(got.size() == 1);
            if (got.size() == 1) {
                CHECK(got[0].hdr.type == kVideo);
                CHECK(got[0].hdr.timestamp == 1234);
                CHECK(got[0].hdr.stream_id == 1);
                CHECK(got[0].payload == payload);
            }
        }
    }
    // ---- extended timestamp
    {
        ChunkWriter w;
        w.set_chunk_size(128);
        std::vector<uint8_t> payload(300, 0xAB);
        MessageHeader h;
        h.type = kAudio;
        h.stream_id = 1;
        h.timestamp = 0x01234567;   // > 0xffffff
        std::string wire;
        w.write(wire, 4, h, payload.data(), payload.size());
        auto got = run_reader(wire, 13);
        CHECK(got.size() == 1 && got[0].hdr.timestamp == 0x01234567 && got[0].payload == payload);
    }
    // ---- set chunk size mid-stream + interleaved chunk streams + type 1/2/3 headers
    {
        std::string wire;
        auto put32 = [](std::string& o, uint32_t v) {
            o.push_back(static_cast<char>(v >> 24)); o.push_back(static_cast<char>(v >> 16));
            o.push_back(static_cast<char>(v >> 8)); o.push_back(static_cast<char>(v));
        };
        // SetChunkSize(4096) on csid 2
        {
            ChunkWriter w;
            MessageHeader h;
            h.type = kSetChunkSize;
            std::string p;
            put32(p, 4096);
            w.write(wire, 2, h, p);
        }
        ChunkWriter w;
        w.set_chunk_size(4096);
        std::vector<uint8_t> v1(3000, 1), a1(200, 2), v2(2500, 3);
        MessageHeader hv;
        hv.type = kVideo; hv.stream_id = 1; hv.timestamp = 1000;
        MessageHeader ha;
        ha.type = kAudio; ha.stream_id = 1; ha.timestamp = 1010;
        w.write(wire, 6, hv, v1.data(), v1.size());
        w.write(wire, 4, ha, a1.data(), a1.size());
        // hand-build a type-1 header continuation on csid 6: delta 33ms, new length
        wire.push_back(static_cast<char>((1 << 6) | 6));
        wire.append("\x00\x00\x21", 3);                      // ts delta 33
        wire.push_back(0); wire.push_back(static_cast<char>(v2.size() >> 8)); wire.push_back(static_cast<char>(v2.size() & 0xff));
        wire.push_back(static_cast<char>(kVideo));
        wire.append(reinterpret_cast<const char*>(v2.data()), v2.size());
        // type-2 header: only delta (same length/type) -> another 2500-byte video message at +33
        wire.push_back(static_cast<char>((2 << 6) | 6));
        wire.append("\x00\x00\x21", 3);
        wire.append(reinterpret_cast<const char*>(v2.data()), v2.size());
        // type-3 header: reuse everything incl. delta -> +33 again
        wire.push_back(static_cast<char>((3 << 6) | 6));
        wire.append(reinterpret_cast<const char*>(v2.data()), v2.size());

        auto got = run_reader(wire, 97);
        CHECK(got.size() == 6);
        if (got.size() == 6) {
            CHECK(got[0].hdr.type == kSetChunkSize);
            CHECK(got[1].hdr.type == kVideo && got[1].payload.size() == 3000 && got[1].hdr.timestamp == 1000);
            CHECK(got[2].hdr.type == kAudio && got[2].payload.size() == 200 && got[2].hdr.timestamp == 1010);
            CHECK(got[3].hdr.type == kVideo && got[3].payload.size() == 2500 && got[3].hdr.timestamp == 1033);
            CHECK(got[4].hdr.timestamp == 1066);
            CHECK(got[5].hdr.timestamp == 1099);
            CHECK(got[5].payload == v2);
        }
    }

    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("all rtmp tests passed\n");
    return 0;
}

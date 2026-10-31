#include "live/rtmp/chunk.hpp"

#include <algorithm>
#include <format>

namespace vsp::rtmp {

namespace {

uint32_t rd24(const uint8_t* p) { return (uint32_t{p[0]} << 16) | (uint32_t{p[1]} << 8) | p[2]; }
uint32_t rd32(const uint8_t* p) { return (uint32_t{p[0]} << 24) | (uint32_t{p[1]} << 16) | (uint32_t{p[2]} << 8) | p[3]; }
uint32_t rd32le(const uint8_t* p) { return (uint32_t{p[3]} << 24) | (uint32_t{p[2]} << 16) | (uint32_t{p[1]} << 8) | p[0]; }

void put8(std::string& o, uint8_t b) { o.push_back(static_cast<char>(b)); }
void put24(std::string& o, uint32_t v) { put8(o, (v >> 16) & 0xff); put8(o, (v >> 8) & 0xff); put8(o, v & 0xff); }
void put32(std::string& o, uint32_t v) { put8(o, v >> 24); put24(o, v & 0xffffff); }
void put32le(std::string& o, uint32_t v) { put8(o, v & 0xff); put8(o, (v >> 8) & 0xff); put8(o, (v >> 16) & 0xff); put8(o, v >> 24); }

constexpr size_t kHeaderSize[4] = {11, 7, 3, 0};

} // namespace

size_t ChunkReader::feed(const uint8_t* data, size_t len, std::string& error) {
    size_t pos = 0;
    while (pos < len) {
        const uint8_t* p = data + pos;
        size_t avail = len - pos;

        // ---- basic header
        if (avail < 1) break;
        uint8_t fmt = p[0] >> 6;
        uint32_t csid = p[0] & 0x3f;
        size_t bh = 1;
        if (csid == 0) {
            if (avail < 2) break;
            csid = 64 + p[1];
            bh = 2;
        } else if (csid == 1) {
            if (avail < 3) break;
            csid = 64 + p[1] + (uint32_t{p[2]} << 8);
            bh = 3;
        }

        // ---- message header
        size_t mh = kHeaderSize[fmt];
        if (avail < bh + mh) break;
        Stream& s = stream(csid);
        const uint8_t* h = p + bh;

        if (fmt == 3 && !s.active && s.hdr.length == 0) {
            error = std::format("type-3 chunk on unknown chunk stream {}", csid);
            return pos;
        }

        MessageHeader hdr = s.hdr;
        bool ext = false;
        uint32_t ts_field = 0;
        switch (fmt) {
        case 0:
            ts_field = rd24(h);
            hdr.length = rd24(h + 3);
            hdr.type = h[6];
            hdr.stream_id = rd32le(h + 7);
            break;
        case 1:
            ts_field = rd24(h);
            hdr.length = rd24(h + 3);
            hdr.type = h[6];
            break;
        case 2:
            ts_field = rd24(h);
            break;
        case 3:
            break;
        }

        size_t extlen = 0;
        if (fmt < 3) {
            ext = ts_field == 0xffffff;
            if (ext) {
                if (avail < bh + mh + 4) break;
                ts_field = rd32(h + mh);
                extlen = 4;
            }
        } else if (s.ext_ts) {
            // Type-3 chunks of a message whose header used an extended timestamp repeat it.
            if (avail < bh + 4) break;
            if (rd32(h) == (s.active ? s.hdr.timestamp : s.hdr.timestamp + s.ts_delta)) extlen = 4;
            ext = true;
        }

        const size_t header_total = bh + mh + extlen;

        // ---- start of a new message?
        if (!s.active) {
            if (fmt == 0) {
                hdr.timestamp = ts_field;
                s.ts_delta = 0;
            } else if (fmt == 1 || fmt == 2) {
                s.ts_delta = ts_field;
                hdr.timestamp = s.hdr.timestamp + ts_field;
            } else {
                hdr.timestamp = s.hdr.timestamp + s.ts_delta;   // fmt 3: reuse previous delta
            }
            if (hdr.length > 64 * 1024 * 1024) {
                error = std::format("message too large ({} bytes) on chunk stream {}", hdr.length, csid);
                return pos;
            }
            s.hdr = hdr;
            s.ext_ts = fmt < 3 ? ext : s.ext_ts;
            s.buf = alloc_(hdr);
            if (hdr.length && !s.buf.data) {
                error = "allocation failed";
                return pos;
            }
            s.received = 0;
            s.active = true;
        }
        // (continuation chunks of an active message must be fmt 3; tolerate others by ignoring header)

        // ---- payload
        uint32_t remaining = s.hdr.length - s.received;
        size_t want = std::min<size_t>(remaining, chunk_size_);
        if (avail < header_total + want) break;   // wait for the whole chunk
        if (want) std::memcpy(s.buf.data + s.received, p + header_total, want);
        s.received += static_cast<uint32_t>(want);
        pos += header_total + want;

        if (s.received == s.hdr.length) {
            s.active = false;
            MessageBuffer done = s.buf;
            s.buf = {};
            done_(s.hdr, done);
        }
    }
    return pos;
}

void ChunkWriter::write(std::string& out, uint32_t csid, const MessageHeader& hdr, const uint8_t* payload,
                        size_t len) const {
    auto basic = [&](uint8_t fmt) {
        if (csid < 64) put8(out, static_cast<uint8_t>((fmt << 6) | csid));
        else if (csid < 320) { put8(out, static_cast<uint8_t>(fmt << 6)); put8(out, static_cast<uint8_t>(csid - 64)); }
        else { put8(out, static_cast<uint8_t>((fmt << 6) | 1)); put8(out, (csid - 64) & 0xff); put8(out, (csid - 64) >> 8); }
    };
    bool ext = hdr.timestamp >= 0xffffff;
    basic(0);
    put24(out, ext ? 0xffffff : hdr.timestamp);
    put24(out, static_cast<uint32_t>(len));
    put8(out, hdr.type);
    put32le(out, hdr.stream_id);
    if (ext) put32(out, hdr.timestamp);

    size_t off = 0;
    while (off < len) {
        size_t n = std::min<size_t>(chunk_size_, len - off);
        if (off) {
            basic(3);
            if (ext) put32(out, hdr.timestamp);
        }
        out.append(reinterpret_cast<const char*>(payload + off), n);
        off += n;
    }
}

} // namespace vsp::rtmp

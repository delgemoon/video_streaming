#include "live/rtmp/amf0.hpp"

#include <bit>
#include <cstring>

namespace vsp::amf0 {

namespace {

enum Marker : uint8_t {
    kNumber = 0x00, kBoolean = 0x01, kString = 0x02, kObject = 0x03, kNull = 0x05, kUndefined = 0x06,
    kReference = 0x07, kEcmaArray = 0x08, kObjectEnd = 0x09, kStrictArray = 0x0A, kDate = 0x0B,
    kLongString = 0x0C, kXmlDoc = 0x0F, kTypedObject = 0x10
};

void put8(std::string& o, uint8_t b) { o.push_back(static_cast<char>(b)); }
void put16(std::string& o, uint16_t v) { put8(o, v >> 8); put8(o, v & 0xff); }
void put32(std::string& o, uint32_t v) { put16(o, v >> 16); put16(o, v & 0xffff); }
void put_double(std::string& o, double d) {
    uint64_t bits;
    std::memcpy(&bits, &d, 8);
    if constexpr (std::endian::native == std::endian::little) bits = std::byteswap(bits);
    o.append(reinterpret_cast<const char*>(&bits), 8);
}
void put_str(std::string& o, std::string_view s) {
    if (s.size() > 0xffff) {
        put8(o, kLongString);
        put32(o, static_cast<uint32_t>(s.size()));
    } else {
        put8(o, kString);
        put16(o, static_cast<uint16_t>(s.size()));
    }
    o.append(s);
}

bool need(size_t pos, size_t n, size_t len) { return pos + n <= len; }
uint16_t rd16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }
uint32_t rd32(const uint8_t* p) { return (uint32_t{p[0]} << 24) | (uint32_t{p[1]} << 16) | (uint32_t{p[2]} << 8) | p[3]; }

std::optional<std::string> rd_str(const uint8_t* d, size_t len, size_t& pos, bool long_str) {
    size_t n;
    if (long_str) {
        if (!need(pos, 4, len)) return std::nullopt;
        n = rd32(d + pos);
        pos += 4;
    } else {
        if (!need(pos, 2, len)) return std::nullopt;
        n = rd16(d + pos);
        pos += 2;
    }
    if (!need(pos, n, len)) return std::nullopt;
    std::string s(reinterpret_cast<const char*>(d + pos), n);
    pos += n;
    return s;
}

std::optional<Object> rd_object_body(const uint8_t* d, size_t len, size_t& pos, int depth) {
    Object obj;
    for (;;) {
        // end marker: empty key followed by 0x09
        if (need(pos, 3, len) && d[pos] == 0 && d[pos + 1] == 0 && d[pos + 2] == kObjectEnd) {
            pos += 3;
            return obj;
        }
        auto key = rd_str(d, len, pos, false);
        if (!key) return std::nullopt;
        if (!need(pos, 1, len)) return std::nullopt;
        if (d[pos] == kObjectEnd && key->empty()) { ++pos; return obj; }
        size_t save = pos;
        auto val = decode(d, len, pos);
        if (!val) { pos = save; return std::nullopt; }
        if (depth > 32) return std::nullopt;
        obj.emplace_back(std::move(*key), std::move(*val));
    }
}

} // namespace

void encode(std::string& out, const Value& v) {
    std::visit(
        [&](const auto& x) {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, Null>) put8(out, kNull);
            else if constexpr (std::is_same_v<T, Undefined>) put8(out, kUndefined);
            else if constexpr (std::is_same_v<T, double>) { put8(out, kNumber); put_double(out, x); }
            else if constexpr (std::is_same_v<T, bool>) { put8(out, kBoolean); put8(out, x ? 1 : 0); }
            else if constexpr (std::is_same_v<T, std::string>) put_str(out, x);
            else if constexpr (std::is_same_v<T, std::shared_ptr<Object>>) {
                put8(out, kObject);
                if (x) for (const auto& [k, val] : *x) {
                    put16(out, static_cast<uint16_t>(k.size()));
                    out.append(k);
                    encode(out, val);
                }
                put16(out, 0);
                put8(out, kObjectEnd);
            } else if constexpr (std::is_same_v<T, std::shared_ptr<Array>>) {
                put8(out, kStrictArray);
                put32(out, x ? static_cast<uint32_t>(x->size()) : 0);
                if (x) for (const auto& val : *x) encode(out, val);
            }
        },
        v.v);
}

std::optional<Value> decode(const uint8_t* d, size_t len, size_t& pos) {
    if (!need(pos, 1, len)) return std::nullopt;
    uint8_t m = d[pos++];
    switch (m) {
    case kNumber: {
        if (!need(pos, 8, len)) return std::nullopt;
        uint64_t bits;
        std::memcpy(&bits, d + pos, 8);
        if constexpr (std::endian::native == std::endian::little) bits = std::byteswap(bits);
        double out;
        std::memcpy(&out, &bits, 8);
        pos += 8;
        return Value(out);
    }
    case kBoolean:
        if (!need(pos, 1, len)) return std::nullopt;
        return Value(d[pos++] != 0);
    case kString: {
        auto s = rd_str(d, len, pos, false);
        if (!s) return std::nullopt;
        return Value(std::move(*s));
    }
    case kLongString:
    case kXmlDoc: {
        auto s = rd_str(d, len, pos, true);
        if (!s) return std::nullopt;
        return Value(std::move(*s));
    }
    case kObject: {
        auto o = rd_object_body(d, len, pos, 0);
        if (!o) return std::nullopt;
        return Value(std::move(*o));
    }
    case kTypedObject: {
        auto cls = rd_str(d, len, pos, false);
        if (!cls) return std::nullopt;
        auto o = rd_object_body(d, len, pos, 0);
        if (!o) return std::nullopt;
        return Value(std::move(*o));
    }
    case kEcmaArray: {
        if (!need(pos, 4, len)) return std::nullopt;
        pos += 4;   // declared count is unreliable; parse until end marker
        auto o = rd_object_body(d, len, pos, 0);
        if (!o) return std::nullopt;
        return Value(std::move(*o));
    }
    case kStrictArray: {
        if (!need(pos, 4, len)) return std::nullopt;
        uint32_t n = rd32(d + pos);
        pos += 4;
        Array a;
        for (uint32_t i = 0; i < n; ++i) {
            auto v = decode(d, len, pos);
            if (!v) return std::nullopt;
            a.push_back(std::move(*v));
        }
        return Value(std::move(a));
    }
    case kDate:
        if (!need(pos, 10, len)) return std::nullopt;
        pos += 10;
        return Value(Null{});
    case kNull: return Value(Null{});
    case kUndefined: return Value(Undefined{});
    case kReference:
        if (!need(pos, 2, len)) return std::nullopt;
        pos += 2;
        return Value(Null{});
    case kObjectEnd: return Value(Null{});
    default: return std::nullopt;
    }
}

std::vector<Value> decode_all(const uint8_t* d, size_t len) {
    std::vector<Value> out;
    size_t pos = 0;
    while (pos < len) {
        auto v = decode(d, len, pos);
        if (!v) break;
        out.push_back(std::move(*v));
    }
    return out;
}

} // namespace vsp::amf0

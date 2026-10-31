#pragma once

// Minimal AMF0 encoder/decoder — only the subset RTMP control traffic uses
// (number, boolean, string, object, null, undefined, ECMA array, strict array, long string, date).

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace vsp::amf0 {

struct Value;
using Object = std::vector<std::pair<std::string, Value>>;   // ordered; ECMA arrays decode to this too
using Array = std::vector<Value>;

struct Null {};
struct Undefined {};

struct Value {
    std::variant<Null, Undefined, double, bool, std::string, std::shared_ptr<Object>, std::shared_ptr<Array>> v;

    Value() : v(Null{}) {}
    Value(Null) : v(Null{}) {}
    Value(Undefined) : v(Undefined{}) {}
    Value(double d) : v(d) {}
    Value(int i) : v(static_cast<double>(i)) {}
    Value(bool b) : v(b) {}
    Value(const char* s) : v(std::string(s)) {}
    Value(std::string s) : v(std::move(s)) {}
    Value(Object o) : v(std::make_shared<Object>(std::move(o))) {}
    Value(Array a) : v(std::make_shared<Array>(std::move(a))) {}

    bool is_null() const { return std::holds_alternative<Null>(v) || std::holds_alternative<Undefined>(v); }
    bool is_number() const { return std::holds_alternative<double>(v); }
    bool is_string() const { return std::holds_alternative<std::string>(v); }
    bool is_object() const { return std::holds_alternative<std::shared_ptr<Object>>(v); }
    double number(double def = 0) const { return is_number() ? std::get<double>(v) : def; }
    const std::string& str() const {
        static const std::string empty;
        return is_string() ? std::get<std::string>(v) : empty;
    }
    const Object* object() const { return is_object() ? std::get<std::shared_ptr<Object>>(v).get() : nullptr; }
    // Looks up a property on an object value; returns nullptr if absent or not an object.
    const Value* get(std::string_view key) const {
        if (auto* o = object())
            for (const auto& [k, val] : *o) if (k == key) return &val;
        return nullptr;
    }
};

// ---- encoding ----
void encode(std::string& out, const Value& v);
inline std::string encode_all(std::initializer_list<Value> vals) {
    std::string out;
    for (const auto& v : vals) encode(out, v);
    return out;
}

// ---- decoding ----
// Decodes one value starting at data[pos]; advances pos. Returns nullopt on malformed input.
std::optional<Value> decode(const uint8_t* data, size_t len, size_t& pos);
// Decodes all values in the buffer.
std::vector<Value> decode_all(const uint8_t* data, size_t len);

} // namespace vsp::amf0

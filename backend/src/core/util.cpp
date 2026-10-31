#include "core/util.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <ctime>
#include <format>
#include <random>

namespace vsp::util {

std::string generate_id() {
    static thread_local std::random_device rd;
    static thread_local std::mt19937_64 gen{(static_cast<uint64_t>(rd()) << 32) ^ rd()};
    std::uniform_int_distribution<uint64_t> dist;
    return std::format("{:016x}{:016x}", dist(gen), dist(gen));
}

int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

std::string iso8601(int64_t unix_ms) {
    std::time_t t = unix_ms / 1000;
    std::tm tm{};
    gmtime_r(&t, &tm);
    return std::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}.{:03}Z", tm.tm_year + 1900, tm.tm_mon + 1,
                       tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, unix_ms % 1000);
}

std::string sanitize_filename(std::string_view name) {
    std::string out;
    out.reserve(name.size());
    auto slash = name.find_last_of("/\\");
    if (slash != std::string_view::npos) name = name.substr(slash + 1);
    for (unsigned char c : name) {
        if (std::isalnum(c) || c == '.' || c == '-' || c == '_') out.push_back(static_cast<char>(c));
        else if (c == ' ') out.push_back('_');
    }
    while (!out.empty() && out.front() == '.') out.erase(out.begin());
    if (out.empty()) out = "source";
    if (out.size() > 120) out.resize(120);
    return out;
}

std::string file_extension_lower(std::string_view name) {
    auto dot = name.find_last_of('.');
    if (dot == std::string_view::npos) return {};
    return to_lower(std::string(name.substr(dot + 1)));
}

std::string mime_for_path(const std::filesystem::path& p) {
    auto ext = file_extension_lower(p.filename().string());
    if (ext == "m3u8") return "application/vnd.apple.mpegurl";
    if (ext == "m4s") return "video/iso.segment";
    if (ext == "mp4" || ext == "m4v") return "video/mp4";
    if (ext == "m4a") return "audio/mp4";
    if (ext == "ts") return "video/mp2t";
    if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
    if (ext == "png") return "image/png";
    if (ext == "webp") return "image/webp";
    if (ext == "json") return "application/json";
    if (ext == "vtt") return "text/vtt";
    return "application/octet-stream";
}

bool is_safe_relative_path(std::string_view rel) {
    if (rel.empty() || rel.front() == '/' || rel.find('\\') != std::string_view::npos) return false;
    if (rel.find('\0') != std::string_view::npos) return false;
    size_t start = 0;
    while (start <= rel.size()) {
        auto end = rel.find('/', start);
        auto seg = rel.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
        if (seg == ".." || seg == "." || seg.empty()) return false;
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return true;
}

std::string url_decode(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '%' && i + 2 < in.size() && std::isxdigit(static_cast<unsigned char>(in[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(in[i + 2]))) {
            out.push_back(static_cast<char>(std::stoi(std::string(in.substr(i + 1, 2)), nullptr, 16)));
            i += 2;
        } else if (in[i] == '+') {
            out.push_back(' ');
        } else {
            out.push_back(in[i]);
        }
    }
    return out;
}

std::string format_duration(uint64_t ms) {
    uint64_t s = ms / 1000;
    return std::format("{:02}:{:02}:{:02}.{:03}", s / 3600, (s / 60) % 60, s % 60, ms % 1000);
}

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

bool iequals(std::string_view a, std::string_view b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](unsigned char x, unsigned char y) {
               return std::tolower(x) == std::tolower(y);
           });
}

std::string trim(std::string_view s) {
    auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string_view::npos) return {};
    auto e = s.find_last_not_of(" \t\r\n");
    return std::string(s.substr(b, e - b + 1));
}

} // namespace vsp::util

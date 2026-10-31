#include "http/static_files.hpp"

#include "core/util.hpp"

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core/file.hpp>
#include <charconv>
#include <chrono>
#include <format>
#include <spdlog/spdlog.h>
#include <vector>

namespace vsp {

namespace asio = boost::asio;
namespace beast = boost::beast;
using asio::use_awaitable;
using asio::as_tuple;

namespace {

struct Range {
    uint64_t first = 0, last = 0;
    bool valid = false, present = false;
};

Range parse_range(std::string_view hdr, uint64_t size) {
    Range r;
    if (hdr.empty()) return r;
    r.present = true;
    if (!hdr.starts_with("bytes=")) return r;
    hdr.remove_prefix(6);
    if (hdr.find(',') != std::string_view::npos) return r;   // multi-range unsupported -> 416
    auto dash = hdr.find('-');
    if (dash == std::string_view::npos) return r;
    auto a = hdr.substr(0, dash), b = hdr.substr(dash + 1);
    auto parse = [](std::string_view s, uint64_t& out) {
        auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
        return ec == std::errc{} && p == s.data() + s.size();
    };
    if (a.empty()) {   // suffix range: last N bytes
        uint64_t n;
        if (!parse(b, n) || n == 0) return r;
        if (n > size) n = size;
        r.first = size - n;
        r.last = size - 1;
    } else {
        if (!parse(a, r.first)) return r;
        if (b.empty()) r.last = size ? size - 1 : 0;
        else if (!parse(b, r.last)) return r;
        if (r.last >= size) r.last = size ? size - 1 : 0;
    }
    r.valid = size > 0 && r.first <= r.last && r.first < size;
    return r;
}

std::string cache_control_for(const std::filesystem::path& p) {
    auto ext = util::file_extension_lower(p.filename().string());
    if (ext == "m3u8") return "no-cache, no-store, must-revalidate";
    if (ext == "m4s" || ext == "mp4") return "public, max-age=3600";
    return "public, max-age=300";
}

template <class Body>
void common_headers(http::response<Body>& res, const std::string& cors) {
    res.set(http::field::server, "vsp/" VSP_VERSION);
    res.set(http::field::access_control_allow_origin, cors);
    res.set(http::field::access_control_expose_headers, "Content-Length, Content-Range, Accept-Ranges");
    res.set(http::field::accept_ranges, "bytes");
}

bool header_keep_alive(const http::request_header<>& req) {
    auto conn = req[http::field::connection];
    if (req.version() >= 11) return !beast::http::token_list(conn).exists("close");
    return beast::http::token_list(conn).exists("keep-alive");
}

} // namespace

asio::awaitable<ServeResult> serve_file(beast::tcp_stream& stream, const http::request_header<>& req,
                                        const std::filesystem::path& file, const std::string& cors_origin) {
    const bool keep_alive = header_keep_alive(req);
    const bool head = req.method() == http::verb::head;

    beast::file f;
    beast::error_code ec;
    f.open(file.c_str(), beast::file_mode::scan, ec);
    std::error_code fec;
    if (ec || !std::filesystem::is_regular_file(file, fec)) {
        http::response<http::string_body> res{http::status::not_found, req.version()};
        common_headers(res, cors_origin);
        res.set(http::field::content_type, "application/json");
        res.set(http::field::cache_control, "no-store");
        res.keep_alive(keep_alive);
        res.body() = R"({"error":"not found"})";
        res.prepare_payload();
        co_await http::async_write(stream, res, use_awaitable);
        co_return ServeResult{keep_alive, 404, 0};
    }
    const uint64_t size = f.size(ec);
    if (ec) co_return ServeResult{false, 500, 0};

    Range range = parse_range(std::string(req[http::field::range]), size);
    if (range.present && !range.valid) {
        http::response<http::empty_body> res{http::status::range_not_satisfiable, req.version()};
        common_headers(res, cors_origin);
        res.set(http::field::content_range, std::format("bytes */{}", size));
        res.keep_alive(keep_alive);
        co_await http::async_write(stream, res, use_awaitable);
        co_return ServeResult{keep_alive, 416, 0};
    }

    uint64_t first = 0, last = size ? size - 1 : 0;
    http::status status = http::status::ok;
    if (range.valid) {
        first = range.first;
        last = range.last;
        status = http::status::partial_content;
    }
    const uint64_t length = size ? last - first + 1 : 0;

    http::response<http::buffer_body> res{status, req.version()};
    common_headers(res, cors_origin);
    res.set(http::field::content_type, util::mime_for_path(file));
    res.set(http::field::cache_control, cache_control_for(file));
    if (status == http::status::partial_content)
        res.set(http::field::content_range, std::format("bytes {}-{}/{}", first, last, size));
    res.content_length(length);
    res.keep_alive(keep_alive);
    res.body().data = nullptr;
    res.body().size = 0;
    res.body().more = !head && length > 0;

    http::response_serializer<http::buffer_body> sr{res};
    stream.expires_after(std::chrono::seconds(30));
    co_await http::async_write_header(stream, sr, use_awaitable);
    const int status_code = static_cast<int>(status);
    if (head || length == 0) co_return ServeResult{keep_alive, status_code, 0};

    f.seek(first, ec);
    if (ec) co_return ServeResult{false, status_code, 0};

    std::vector<char> chunk(std::min<uint64_t>(length, 512 * 1024));
    uint64_t remaining = length;
    uint64_t sent = 0;
    while (remaining > 0) {
        size_t want = static_cast<size_t>(std::min<uint64_t>(chunk.size(), remaining));
        size_t n = f.read(chunk.data(), want, ec);
        if (ec || n == 0) co_return ServeResult{false, status_code, sent};   // file truncated under us
        remaining -= n;
        sent += n;
        res.body().data = chunk.data();
        res.body().size = n;
        res.body().more = remaining > 0;
        stream.expires_after(std::chrono::seconds(30));
        auto [wec, _] = co_await http::async_write(stream, sr, as_tuple(use_awaitable));
        if (wec == http::error::need_buffer) wec = {};
        if (wec) co_return ServeResult{false, status_code, sent};
    }
    co_return ServeResult{keep_alive, status_code, sent};
}

} // namespace vsp

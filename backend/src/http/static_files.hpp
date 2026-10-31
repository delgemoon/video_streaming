#pragma once

#include <boost/asio/awaitable.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/http.hpp>
#include <cstdint>
#include <filesystem>
#include <string>

namespace vsp {

namespace http = boost::beast::http;

struct ServeResult {
    bool keep_alive = false;   // whether the connection may be reused
    int status = 0;
    uint64_t bytes = 0;        // body bytes written
};

// Serves a file with single-range support (RFC 7233), HEAD, conditional caching headers tuned
// for HLS (playlists never cached, segments cached).
boost::asio::awaitable<ServeResult> serve_file(boost::beast::tcp_stream& stream, const http::request_header<>& req,
                                               const std::filesystem::path& file, const std::string& cors_origin);

} // namespace vsp

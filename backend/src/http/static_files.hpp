#pragma once

#include <boost/asio/awaitable.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/http.hpp>
#include <filesystem>
#include <string>

namespace vsp {

namespace http = boost::beast::http;

// Serves a file with single-range support (RFC 7233), HEAD, conditional caching headers tuned
// for HLS (playlists never cached, segments cached). Returns whether the connection may be reused.
boost::asio::awaitable<bool> serve_file(boost::beast::tcp_stream& stream, const http::request_header<>& req,
                                        const std::filesystem::path& file, const std::string& cors_origin);

} // namespace vsp

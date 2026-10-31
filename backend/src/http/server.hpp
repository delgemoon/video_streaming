#pragma once

#include "core/config.hpp"
#include "http/events.hpp"
#include "http/router.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <memory>
#include <thread>
#include <vector>

namespace vsp {

// Coroutine-based Boost.Beast HTTP/1.1 server. One io_context, N threads, keep-alive, streaming
// request bodies (uploads never buffer in memory), range-capable static serving and SSE.
class HttpServer {
public:
    HttpServer(const Config& cfg, Router& router, EventBroker& broker, boost::asio::io_context& ioc);

    void start();
    void stop();

private:
    boost::asio::awaitable<void> listen();
    boost::asio::awaitable<void> session(boost::beast::tcp_stream stream);
    boost::asio::awaitable<void> heartbeat();

    const Config& cfg_;
    Router& router_;
    EventBroker& broker_;
    boost::asio::io_context& ioc_;
};

} // namespace vsp

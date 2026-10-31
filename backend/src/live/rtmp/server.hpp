#pragma once

#include "live/rtmp/session.hpp"

#include <boost/asio/io_context.hpp>
#include <cstdint>
#include <memory>
#include <string>

namespace vsp::rtmp {

// Accepts RTMP publishers on the shared io_context. Each connection runs as one coroutine.
class Server {
public:
    Server(boost::asio::io_context& ioc, std::string host, uint16_t port, PublishHandler& handler, ServerConfig cfg = {});
    void start();

private:
    boost::asio::awaitable<void> listen();

    boost::asio::io_context& ioc_;
    std::string host_;
    uint16_t port_;
    PublishHandler& handler_;
    ServerConfig cfg_;
    std::unique_ptr<boost::asio::ip::tcp::acceptor> acceptor_;
};

} // namespace vsp::rtmp

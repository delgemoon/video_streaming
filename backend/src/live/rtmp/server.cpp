#include "live/rtmp/server.hpp"

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <spdlog/spdlog.h>

namespace vsp::rtmp {

namespace asio = boost::asio;
using tcp = asio::ip::tcp;

Server::Server(asio::io_context& ioc, std::string host, uint16_t port, PublishHandler& handler, ServerConfig cfg)
    : ioc_(ioc), host_(std::move(host)), port_(port), handler_(handler), cfg_(cfg) {}

void Server::start() { asio::co_spawn(ioc_, listen(), asio::detached); }

asio::awaitable<void> Server::listen() {
    auto ex = co_await asio::this_coro::executor;
    tcp::endpoint ep{asio::ip::make_address(host_), port_};
    tcp::acceptor acceptor(ex);
    try {
        acceptor.open(ep.protocol());
        acceptor.set_option(asio::socket_base::reuse_address(true));
        acceptor.bind(ep);
        acceptor.listen(asio::socket_base::max_listen_connections);
    } catch (const std::exception& e) {
        spdlog::error("RTMP listen on {}:{} failed: {}", host_, port_, e.what());
        co_return;
    }
    spdlog::info("RTMP ingest listening on rtmp://{}:{}/<app>/<stream-key>", host_, port_);

    for (;;) {
        auto [ec, socket] = co_await acceptor.async_accept(asio::as_tuple(asio::use_awaitable));
        if (ec) {
            if (ec == asio::error::operation_aborted) co_return;
            spdlog::warn("rtmp accept: {}", ec.message());
            continue;
        }
        boost::system::error_code sec;
        socket.set_option(tcp::no_delay(true), sec);
        socket.set_option(asio::socket_base::receive_buffer_size(4 * 1024 * 1024), sec);
        auto session = std::make_shared<Session>(std::move(socket), handler_, cfg_);
        asio::co_spawn(ex, session->run(), asio::detached);
    }
}

} // namespace vsp::rtmp

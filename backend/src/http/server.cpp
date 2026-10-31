#include "http/server.hpp"

#include "core/util.hpp"
#include "http/static_files.hpp"
#include "observability/tracing.hpp"
#include "observability/vsp_metrics.hpp"

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <chrono>
#include <spdlog/spdlog.h>
#include <vector>

namespace vsp {

namespace asio = boost::asio;
namespace beast = boost::beast;
using tcp = asio::ip::tcp;
using asio::use_awaitable;
using asio::as_tuple;
using namespace std::chrono_literals;

namespace {

constexpr size_t kJsonBodyLimit = 16 * 1024 * 1024;

template <class Body>
void common_headers(http::response<Body>& res, const Config& cfg) {
    res.set(http::field::server, "vsp/" VSP_VERSION);
    res.set(http::field::access_control_allow_origin, cfg.cors_origin);
    res.set(http::field::access_control_allow_methods, "GET, HEAD, POST, PUT, PATCH, DELETE, OPTIONS");
    res.set(http::field::access_control_allow_headers, "Content-Type, Range, X-Requested-With");
    res.set(http::field::access_control_expose_headers, "Content-Length, Content-Range, Accept-Ranges");
}

http::response<http::string_body> json_response(const JsonResponse& jr, unsigned version, bool keep_alive,
                                                const Config& cfg) {
    http::response<http::string_body> res{jr.status, version};
    common_headers(res, cfg);
    res.set(http::field::content_type, "application/json");
    res.set(http::field::cache_control, "no-store");
    res.keep_alive(keep_alive);
    res.body() = jr.body.dump();
    res.prepare_payload();
    return res;
}

} // namespace

HttpServer::HttpServer(const Config& cfg, Router& router, EventBroker& broker, PlaylistRegistry& playlists,
                       asio::io_context& ioc)
    : cfg_(cfg), router_(router), broker_(broker), playlists_(playlists), ioc_(ioc) {}

void HttpServer::start() {
    tcp::endpoint ep{asio::ip::make_address(cfg_.host), cfg_.port};
    acceptor_ = std::make_unique<tcp::acceptor>(ioc_);
    acceptor_->open(ep.protocol());
    acceptor_->set_option(asio::socket_base::reuse_address(true));
    acceptor_->bind(ep);   // throws on conflict -> main() exits with a clear error
    acceptor_->listen(asio::socket_base::max_listen_connections);
    spdlog::info("HTTP listening on http://{}:{}", cfg_.host, cfg_.port);
    asio::co_spawn(ioc_, listen(), asio::detached);
    asio::co_spawn(ioc_, heartbeat(), asio::detached);
}

void HttpServer::stop() { ioc_.stop(); }

asio::awaitable<void> HttpServer::heartbeat() {
    asio::steady_timer timer(co_await asio::this_coro::executor);
    for (;;) {
        timer.expires_after(15s);
        auto [ec] = co_await timer.async_wait(as_tuple(use_awaitable));
        if (ec) co_return;
        broker_.heartbeat();
    }
}

asio::awaitable<void> HttpServer::listen() {
    auto ex = co_await asio::this_coro::executor;
    tcp::acceptor& acceptor = *acceptor_;

    for (;;) {
        auto [ec, socket] = co_await acceptor.async_accept(as_tuple(use_awaitable));
        if (ec) {
            if (ec == asio::error::operation_aborted) co_return;
            spdlog::warn("accept: {}", ec.message());
            continue;
        }
        socket.set_option(tcp::no_delay(true));
        asio::co_spawn(ex, session(beast::tcp_stream(std::move(socket))), asio::detached);
    }
}

namespace {
// Records one request's metrics when it goes out of scope (covers every exit path).
struct RequestRecord {
    std::string route, method;
    int status = 0;
    uint64_t bytes = 0;
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    vsp::trace::SpanPtr span;
    ~RequestRecord() {
        using namespace vsp::metrics;
        double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::string cls = std::to_string(status / 100) + "xx";
        M().http_request_duration->with({route, method, cls})->observe(secs);
        M().http_requests_total->with({route, method, std::to_string(status)})->inc();
        if (bytes) M().http_response_bytes_total->with({route})->inc(static_cast<double>(bytes));
        if (span) {
            span->set_attribute("http.response.status_code", static_cast<int64_t>(status));
            if (status >= 500) span->set_status(vsp::trace::Status::Error);
            span->end();
        }
    }
};
} // namespace

asio::awaitable<void> HttpServer::session(beast::tcp_stream stream) {
    using vsp::metrics::M;
    M().http_connections->get()->inc();
    struct ConnGuard { ~ConnGuard() { M().http_connections->get()->dec(); } } conn_guard;
    beast::flat_buffer buffer;
    buffer.reserve(64 * 1024);
    std::string remote;
    {
        beast::error_code ec;
        auto ep = stream.socket().remote_endpoint(ec);
        if (!ec) remote = ep.address().to_string();
    }

    try {
        for (;;) {
            http::request_parser<http::empty_body> hp;
            hp.body_limit(boost::none);
            stream.expires_after(30s);
            auto [ec, n] = co_await http::async_read_header(stream, buffer, hp, as_tuple(use_awaitable));
            if (ec) break;   // closed or timeout

            const auto& hdr = hp.get();
            const bool keep_alive = hdr.keep_alive();
            std::string target(hdr.target());
            std::string path = target, query;
            if (auto q = target.find('?'); q != std::string::npos) {
                path = target.substr(0, q);
                query = target.substr(q + 1);
            }
            path = util::url_decode(path);

            auto rec = std::make_unique<RequestRecord>();
            rec->route = vsp::metrics::route_label(path);
            rec->method = std::string(hdr.method_string());
            if (path.starts_with("/api/") && path != "/api/events" && vsp::trace::T().enabled()) {
                auto parent = vsp::trace::SpanContext::from_traceparent(std::string(hdr["traceparent"]));
                rec->span = vsp::trace::T().start("HTTP " + rec->method + " " + rec->route, vsp::trace::SpanKind::Server,
                                                   parent.valid() ? &parent : nullptr);
                rec->span->set_attribute("http.request.method", rec->method);
                rec->span->set_attribute("url.path", path);
                rec->span->set_attribute("client.address", remote);
            }

            // CORS preflight
            if (hdr.method() == http::verb::options) {
                http::response<http::empty_body> res{http::status::no_content, hdr.version()};
                common_headers(res, cfg_);
                res.set(http::field::access_control_max_age, "86400");
                res.keep_alive(keep_alive);
                rec->status = 204;
                co_await http::async_write(stream, res, use_awaitable);
                if (!keep_alive) break;
                continue;
            }

            // Prometheus exposition
            if (path == "/metrics" && (hdr.method() == http::verb::get || hdr.method() == http::verb::head)) {
                http::response<http::string_body> res{http::status::ok, hdr.version()};
                res.set(http::field::server, "vsp/" VSP_VERSION);
                res.set(http::field::content_type, "text/plain; version=0.0.4; charset=utf-8");
                res.set(http::field::cache_control, "no-store");
                res.keep_alive(keep_alive);
                res.body() = vsp::metrics::Registry::global().expose();
                res.prepare_payload();
                rec->status = 200;
                rec->bytes = res.body().size();
                co_await http::async_write(stream, res, use_awaitable);
                if (!keep_alive) break;
                continue;
            }

            RouteMatch m = router_.match(hdr.method(), path);
            if (!m.route) {
                auto jr = JsonResponse::error(m.path_matched ? http::status::method_not_allowed : http::status::not_found,
                                              m.path_matched ? "method not allowed" : "not found");
                // we did not consume the body: only reuse the connection if there was none
                const bool reusable = keep_alive && hp.is_done();
                rec->status = static_cast<int>(jr.status);
                co_await http::async_write(stream, json_response(jr, hdr.version(), reusable, cfg_), use_awaitable);
                if (!reusable) break;
                continue;
            }

            RequestContext ctx{hdr, path, std::move(m.params), Router::parse_query(query), {}, remote};
            const Route& route = *m.route;

            switch (route.kind) {
            case HandlerKind::Json: {
                auto cl = hdr[http::field::content_length];
                if (!cl.empty() && std::stoull(std::string(cl)) > kJsonBodyLimit) {
                    rec->status = 413;
                    co_await http::async_write(
                        stream, json_response(JsonResponse::error(http::status::payload_too_large, "body too large"),
                                              hdr.version(), false, cfg_),
                        use_awaitable);
                    co_return;
                }
                http::request_parser<http::string_body> p{std::move(hp)};
                p.body_limit(kJsonBodyLimit);
                if (!p.is_done()) {
                    stream.expires_after(30s);
                    co_await http::async_read(stream, buffer, p, use_awaitable);
                }
                ctx.body = std::move(p.get().body());
                JsonResponse jr;
                try {
                    jr = route.json(ctx);
                } catch (const std::invalid_argument& e) {
                    jr = JsonResponse::error(http::status::bad_request, e.what());
                } catch (const std::out_of_range& e) {
                    jr = JsonResponse::error(http::status::bad_request, std::string("bad request: ") + e.what());
                } catch (const nlohmann::json::exception& e) {
                    jr = JsonResponse::error(http::status::bad_request, std::string("bad request: ") + e.what());
                } catch (const std::exception& e) {
                    spdlog::error("handler error {} {}: {}", std::string(hdr.method_string()), path, e.what());
                    jr = JsonResponse::error(http::status::internal_server_error, e.what());
                }
                stream.expires_after(30s);
                rec->status = static_cast<int>(jr.status);
                {
                    auto res = json_response(jr, hdr.version(), keep_alive, cfg_);
                    rec->bytes = res.body().size();
                    co_await http::async_write(stream, res, use_awaitable);
                }
                break;
            }

            case HandlerKind::Upload: {
                std::expected<UploadSink, JsonResponse> sink_or;
                try {
                    sink_or = route.upload(ctx);
                } catch (const std::exception& e) {
                    sink_or = std::unexpected(JsonResponse::error(http::status::internal_server_error, e.what()));
                }
                auto cl = hdr[http::field::content_length];
                uint64_t declared = cl.empty() ? 0 : std::stoull(std::string(cl));
                if (sink_or && sink_or->max_bytes && declared > sink_or->max_bytes)
                    sink_or = std::unexpected(JsonResponse::error(http::status::payload_too_large, "body too large"));
                if (!sink_or) {
                    // Can't cheaply skip a huge body: answer and close.
                    rec->status = static_cast<int>(sink_or.error().status);
                    co_await http::async_write(stream, json_response(sink_or.error(), hdr.version(), false, cfg_),
                                               use_awaitable);
                    co_return;
                }
                UploadSink sink = std::move(*sink_or);
                vsp::metrics::Timer upload_timer(M().upload_duration->get());

                if (beast::iequals(hdr[http::field::expect], "100-continue")) {
                    http::response<http::empty_body> cont{http::status::continue_, hdr.version()};
                    co_await http::async_write(stream, cont, use_awaitable);
                }

                http::request_parser<http::buffer_body> p{std::move(hp)};
                p.body_limit(boost::none);
                std::vector<char> chunk(1 << 20);
                uint64_t total = 0;
                bool ok = true;
                while (!p.is_done()) {
                    p.get().body().data = chunk.data();
                    p.get().body().size = chunk.size();
                    stream.expires_after(120s);
                    auto [rec, rn] = co_await http::async_read(stream, buffer, p, as_tuple(use_awaitable));
                    if (rec && rec != http::error::need_buffer) {
                        ok = false;
                        spdlog::warn("upload aborted from {}: {}", remote, rec.message());
                        break;
                    }
                    size_t got = chunk.size() - p.get().body().size;
                    total += got;
                    if (got) M().upload_bytes_total->get()->inc(static_cast<double>(got));
                    if (sink.max_bytes && total > sink.max_bytes) { ok = false; break; }
                    if (got && !sink.write(chunk.data(), got)) {
                        ok = false;
                        spdlog::error("upload write failed (disk full?)");
                        break;
                    }
                }
                if (!ok) {
                    if (sink.abort) sink.abort();
                    upload_timer.cancel();
                    rec->status = 499;
                    co_return;
                }
                if (rec->span) rec->span->set_attribute("upload.bytes", static_cast<int64_t>(total));
                JsonResponse jr = sink.finish(total);
                stream.expires_after(30s);
                rec->status = static_cast<int>(jr.status);
                co_await http::async_write(stream, json_response(jr, hdr.version(), keep_alive, cfg_), use_awaitable);
                break;
            }

            case HandlerKind::Static: {
                auto file_or = route.static_file(ctx);
                if (!file_or) {
                    rec->status = static_cast<int>(file_or.error().status);
                    co_await http::async_write(stream, json_response(file_or.error(), hdr.version(), keep_alive, cfg_),
                                               use_awaitable);
                    break;
                }
                // LL-HLS blocking playlist reload
                if (auto msn = ctx.query.find("_HLS_msn"); msn != ctx.query.end() &&
                    file_or->extension() == ".m3u8") {
                    uint64_t n = std::strtoull(msn->second.c_str(), nullptr, 10);
                    int part = -1;
                    if (auto p = ctx.query.find("_HLS_part"); p != ctx.query.end()) part = std::atoi(p->second.c_str());
                    stream.expires_after(30s);
                    auto t0 = std::chrono::steady_clock::now();
                    co_await playlists_.wait_for(*file_or, n, part, std::chrono::milliseconds(cfg_.live_block_timeout_ms));
                    double held = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
                    M().hls_block_hold->get()->observe(held);
                    if (held >= cfg_.live_block_timeout_ms / 1000.0 - 0.01) M().hls_block_timeouts_total->get()->inc();
                }
                ServeResult sr = co_await serve_file(stream, hdr, *file_or, cfg_.cors_origin);
                rec->status = sr.status;
                rec->bytes = sr.bytes;
                {
                    auto kind = vsp::metrics::media_kind(path);
                    M().media_requests_total->with({kind})->inc();
                    if (sr.bytes) M().media_bytes_total->with({kind})->inc(static_cast<double>(sr.bytes));
                }
                if (!sr.keep_alive) co_return;
                break;
            }

            case HandlerKind::Sse: {
                std::string head = "HTTP/1.1 200 OK\r\n"
                                   "Content-Type: text/event-stream\r\n"
                                   "Cache-Control: no-cache\r\n"
                                   "Connection: keep-alive\r\n"
                                   "X-Accel-Buffering: no\r\n"
                                   "Access-Control-Allow-Origin: " + cfg_.cors_origin + "\r\n\r\n"
                                   "retry: 2000\n\n";
                stream.expires_never();
                co_await asio::async_write(stream, asio::buffer(head), use_awaitable);
                rec->status = 200;
                auto ch = broker_.subscribe();
                M().sse_clients->get()->inc();
                struct Unsub {
                    EventBroker& b; std::shared_ptr<EventBroker::Channel> c;
                    ~Unsub() { b.unsubscribe(c); M().sse_clients->get()->dec(); }
                } unsub{broker_, ch};
                for (;;) {
                    auto [rec, frame] = co_await ch->async_receive(as_tuple(use_awaitable));
                    if (rec) co_return;
                    auto [wec, wn] = co_await asio::async_write(stream, asio::buffer(frame), as_tuple(use_awaitable));
                    if (wec) co_return;
                }
            }
            }

            if (!keep_alive) break;
        }
    } catch (const boost::system::system_error& e) {
        if (e.code() != http::error::end_of_stream && e.code() != asio::error::operation_aborted &&
            e.code() != beast::error::timeout && e.code() != asio::error::connection_reset &&
            e.code() != asio::error::broken_pipe)
            spdlog::debug("session {}: {}", remote, e.what());
    } catch (const std::exception& e) {
        spdlog::warn("session {} error: {}", remote, e.what());
    }

    beast::error_code ec;
    stream.socket().shutdown(tcp::socket::shutdown_send, ec);
}

} // namespace vsp

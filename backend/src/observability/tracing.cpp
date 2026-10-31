#include "observability/tracing.hpp"

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <format>
#include <nlohmann/json.hpp>
#include <random>
#include <spdlog/spdlog.h>

namespace vsp::trace {

namespace {

int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string random_hex(size_t bytes) {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    std::string out;
    out.reserve(bytes * 2);
    while (out.size() < bytes * 2) out += std::format("{:016x}", rng());
    out.resize(bytes * 2);
    return out;
}

} // namespace

// ---------------------------------------------------------------------------------------------

std::string SpanContext::traceparent() const {
    return valid() ? std::format("00-{}-{}-01", trace_id, span_id) : std::string();
}

SpanContext SpanContext::from_traceparent(std::string_view h) {
    // 00-<32 hex>-<16 hex>-<2 hex>
    SpanContext c;
    if (h.size() < 55 || h[2] != '-' || h[35] != '-' || h[52] != '-') return c;
    auto is_hex = [](std::string_view s) {
        for (char ch : s) if (!std::isxdigit(static_cast<unsigned char>(ch))) return false;
        return true;
    };
    auto tid = h.substr(3, 32), sid = h.substr(36, 16);
    if (!is_hex(tid) || !is_hex(sid)) return c;
    c.trace_id = std::string(tid);
    c.span_id = std::string(sid);
    return c;
}

// ---------------------------------------------------------------------------------------------

Span::~Span() { end(); }

void Span::set_attribute(std::string_view key, std::string_view value) {
    if (!recording_) return;
    std::lock_guard lk(mu_);
    attrs_.push_back({std::string(key), 0, std::string(value)});
}
void Span::set_attribute(std::string_view key, int64_t value) {
    if (!recording_) return;
    std::lock_guard lk(mu_);
    Attr a{std::string(key), 1};
    a.i = value;
    attrs_.push_back(std::move(a));
}
void Span::set_attribute(std::string_view key, double value) {
    if (!recording_) return;
    std::lock_guard lk(mu_);
    Attr a{std::string(key), 2};
    a.d = value;
    attrs_.push_back(std::move(a));
}
void Span::set_attribute(std::string_view key, bool value) {
    if (!recording_) return;
    std::lock_guard lk(mu_);
    Attr a{std::string(key), 3};
    a.b = value;
    attrs_.push_back(std::move(a));
}
void Span::add_event(std::string_view name) {
    if (!recording_) return;
    std::lock_guard lk(mu_);
    events_.push_back({std::string(name), now_ns()});
}
void Span::set_status(Status s, std::string_view description) {
    if (!recording_) return;
    std::lock_guard lk(mu_);
    status_ = s;
    status_desc_ = std::string(description);
}
void Span::end() {
    if (!recording_ || ended_.exchange(true)) return;
    end_ns_ = now_ns();
    // Hand a copy to the exporter (the owner may still hold this shared_ptr).
    auto copy = std::make_shared<Span>();
    {
        std::lock_guard lk(mu_);
        copy->name_ = name_;
        copy->ctx_ = ctx_;
        copy->parent_span_id_ = parent_span_id_;
        copy->start_ns_ = start_ns_;
        copy->end_ns_ = end_ns_;
        copy->attrs_ = attrs_;
        copy->events_ = events_;
        copy->status_ = status_;
        copy->status_desc_ = status_desc_;
        copy->kind_ = kind_;
    }
    Tracer::global().enqueue(std::move(copy));
}

// ---------------------------------------------------------------------------------------------

Tracer& Tracer::global() {
    static Tracer t;
    return t;
}

void Tracer::configure(std::string endpoint, std::string service_name, std::string service_version) {
    service_name_ = std::move(service_name);
    service_version_ = std::move(service_version);
    if (endpoint.empty()) {
        enabled_ = false;
        return;
    }
    // parse http://host:port/path
    std::string rest = endpoint;
    if (rest.starts_with("http://")) rest = rest.substr(7);
    else if (rest.starts_with("https://")) {
        spdlog::warn("tracing: https endpoints are not supported, tracing disabled");
        return;
    }
    auto slash = rest.find('/');
    std::string hostport = slash == std::string::npos ? rest : rest.substr(0, slash);
    target_ = slash == std::string::npos ? "/v1/traces" : rest.substr(slash);
    auto colon = hostport.rfind(':');
    host_ = colon == std::string::npos ? hostport : hostport.substr(0, colon);
    port_ = colon == std::string::npos ? "4318" : hostport.substr(colon + 1);
    endpoint_ = std::move(endpoint);
    enabled_ = true;
    stopping_ = false;
    exporter_ = std::thread([this] { export_loop(); });
    spdlog::info("tracing: exporting OTLP/JSON to {}", endpoint_);
}

SpanPtr Tracer::start(std::string_view name, SpanKind kind, const SpanContext* parent) {
    auto s = std::make_shared<Span>();
    s->name_ = std::string(name);
    s->kind_ = static_cast<int>(kind);
    if (!enabled_) return s;   // non-recording
    s->recording_ = true;
    s->start_ns_ = now_ns();
    if (parent && parent->valid()) {
        s->ctx_.trace_id = parent->trace_id;
        s->parent_span_id_ = parent->span_id;
    } else {
        s->ctx_.trace_id = random_hex(16);
    }
    s->ctx_.span_id = random_hex(8);
    return s;
}

SpanPtr Tracer::start_child(std::string_view name, const SpanPtr& parent, SpanKind kind) {
    return start(name, kind, parent ? &parent->context() : nullptr);
}

void Tracer::enqueue(std::shared_ptr<Span> finished) {
    std::lock_guard lk(mu_);
    if (queue_.size() >= 10000) {
        dropped_.fetch_add(1);
        return;
    }
    queue_.push_back(std::move(finished));
}

void Tracer::shutdown() {
    if (!enabled_) return;
    stopping_ = true;
    if (exporter_.joinable()) exporter_.join();
    enabled_ = false;
}

void Tracer::export_loop() {
    using namespace std::chrono_literals;
    while (!stopping_) {
        std::this_thread::sleep_for(2s);
        std::vector<std::shared_ptr<Span>> batch;
        {
            std::lock_guard lk(mu_);
            if (queue_.empty()) continue;
            batch.swap(queue_);
        }
        if (post(to_otlp_json(batch))) exported_ += batch.size();
        else failed_ += batch.size();
    }
    // final flush
    std::vector<std::shared_ptr<Span>> batch;
    {
        std::lock_guard lk(mu_);
        batch.swap(queue_);
    }
    if (!batch.empty()) post(to_otlp_json(batch));
}

bool Tracer::post(const std::string& body) {
    namespace beast = boost::beast;
    namespace http = beast::http;
    namespace asio = boost::asio;
    try {
        asio::io_context ioc;
        asio::ip::tcp::resolver resolver(ioc);
        beast::tcp_stream stream(ioc);
        stream.expires_after(std::chrono::seconds(5));
        stream.connect(resolver.resolve(host_, port_));
        http::request<http::string_body> req{http::verb::post, target_, 11};
        req.set(http::field::host, host_);
        req.set(http::field::content_type, "application/json");
        req.set(http::field::user_agent, "vsp-server");
        req.body() = body;
        req.prepare_payload();
        http::write(stream, req);
        beast::flat_buffer buf;
        http::response<http::string_body> res;
        http::read(stream, buf, res);
        beast::error_code ec;
        stream.socket().shutdown(asio::ip::tcp::socket::shutdown_both, ec);
        if (res.result_int() / 100 != 2) {
            static int warned = 0;
            if (warned++ < 3) spdlog::warn("tracing: collector returned {} {}", res.result_int(), res.body().substr(0, 200));
            return false;
        }
        return true;
    } catch (const std::exception& e) {
        static int warned = 0;
        if (warned++ < 3) spdlog::warn("tracing: export failed: {}", e.what());
        return false;
    }
}

std::string Tracer::to_otlp_json(const std::vector<std::shared_ptr<Span>>& spans) const {
    using json = nlohmann::json;
    auto attr = [](const std::string& k, const json& v) { return json{{"key", k}, {"value", v}}; };
    json arr = json::array();
    for (const auto& s : spans) {
        json attrs = json::array();
        for (const auto& a : s->attrs_) {
            switch (a.kind) {
            case 0: attrs.push_back(attr(a.key, {{"stringValue", a.s}})); break;
            case 1: attrs.push_back(attr(a.key, {{"intValue", std::to_string(a.i)}})); break;
            case 2: attrs.push_back(attr(a.key, {{"doubleValue", a.d}})); break;
            case 3: attrs.push_back(attr(a.key, {{"boolValue", a.b}})); break;
            }
        }
        json events = json::array();
        for (const auto& e : s->events_) events.push_back({{"name", e.name}, {"timeUnixNano", std::to_string(e.time_ns)}});
        json sp{{"traceId", s->ctx_.trace_id},
                {"spanId", s->ctx_.span_id},
                {"name", s->name_},
                {"kind", s->kind_},
                {"startTimeUnixNano", std::to_string(s->start_ns_)},
                {"endTimeUnixNano", std::to_string(s->end_ns_)},
                {"attributes", attrs},
                {"events", events},
                {"status", {{"code", static_cast<int>(s->status_)}, {"message", s->status_desc_}}}};
        if (!s->parent_span_id_.empty()) sp["parentSpanId"] = s->parent_span_id_;
        arr.push_back(std::move(sp));
    }
    json doc{{"resourceSpans",
              {{{"resource",
                 {{"attributes",
                   {attr("service.name", {{"stringValue", service_name_}}),
                    attr("service.version", {{"stringValue", service_version_}}),
                    attr("telemetry.sdk.name", {{"stringValue", "vsp-minimal"}}),
                    attr("telemetry.sdk.language", {{"stringValue", "cpp"}})}}}},
                {"scopeSpans", {{{"scope", {{"name", "vsp"}}}, {"spans", arr}}}}}}}};
    return doc.dump();
}

} // namespace vsp::trace

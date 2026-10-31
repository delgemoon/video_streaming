#pragma once

// Minimal OpenTelemetry tracing: spans are recorded in-process and exported as OTLP/JSON over
// HTTP (POST /v1/traces) to a collector, batched on a background thread. No opentelemetry-cpp
// dependency — the OTLP JSON schema is small and stable. Disabled (no-op spans) when no endpoint
// is configured, so instrumentation can stay in the code unconditionally.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace vsp::trace {

struct SpanContext {
    std::string trace_id;   // 32 hex
    std::string span_id;    // 16 hex
    bool valid() const { return !trace_id.empty(); }
    // W3C traceparent header value, for propagation to/from clients.
    std::string traceparent() const;
    static SpanContext from_traceparent(std::string_view header);
};

enum class Status { Unset, Ok, Error };

class Span;
using SpanPtr = std::shared_ptr<Span>;

class Span {
public:
    ~Span();
    void set_attribute(std::string_view key, std::string_view value);
    void set_attribute(std::string_view key, int64_t value);
    void set_attribute(std::string_view key, double value);
    void set_attribute(std::string_view key, bool value);
    void add_event(std::string_view name);
    void set_status(Status s, std::string_view description = {});
    void end();   // idempotent; also called by the destructor
    const SpanContext& context() const { return ctx_; }
    bool recording() const { return recording_; }

private:
    friend class Tracer;
    struct Attr {
        std::string key;
        int kind;   // 0 string, 1 int, 2 double, 3 bool
        std::string s;
        int64_t i = 0;
        double d = 0;
        bool b = false;
    };
    struct Event {
        std::string name;
        int64_t time_ns;
    };

    std::string name_;
    SpanContext ctx_;
    std::string parent_span_id_;
    int64_t start_ns_ = 0, end_ns_ = 0;
    std::vector<Attr> attrs_;
    std::vector<Event> events_;
    Status status_ = Status::Unset;
    std::string status_desc_;
    int kind_ = 1;   // SPAN_KIND_INTERNAL
    bool recording_ = false;
    std::atomic<bool> ended_{false};
    std::mutex mu_;
};

enum class SpanKind { Internal = 1, Server = 2, Client = 3, Producer = 4, Consumer = 5 };

class Tracer {
public:
    static Tracer& global();

    // endpoint: e.g. "http://127.0.0.1:4318/v1/traces". Empty disables tracing.
    void configure(std::string endpoint, std::string service_name, std::string service_version);
    bool enabled() const { return enabled_.load(); }

    SpanPtr start(std::string_view name, SpanKind kind = SpanKind::Internal, const SpanContext* parent = nullptr);
    // Convenience: child of another span.
    SpanPtr start_child(std::string_view name, const SpanPtr& parent, SpanKind kind = SpanKind::Internal);

    void shutdown();   // flush and stop the exporter

private:
    friend class Span;
    void enqueue(std::shared_ptr<Span> finished);
    void export_loop();
    bool post(const std::string& body);
    std::string to_otlp_json(const std::vector<std::shared_ptr<Span>>& spans) const;

    std::atomic<bool> enabled_{false};
    std::string endpoint_, host_, port_, target_;
    std::string service_name_, service_version_;
    std::mutex mu_;
    std::vector<std::shared_ptr<Span>> queue_;
    std::thread exporter_;
    std::atomic<bool> stopping_{false};
    std::atomic<uint64_t> exported_{0}, dropped_{0}, failed_{0};
};

inline Tracer& T() { return Tracer::global(); }

// Scoped helper: ends the span when it goes out of scope.
class Scope {
public:
    explicit Scope(SpanPtr s) : s_(std::move(s)) {}
    ~Scope() { if (s_) s_->end(); }
    Span* operator->() const { return s_.get(); }
    const SpanPtr& span() const { return s_; }

private:
    SpanPtr s_;
};

} // namespace vsp::trace

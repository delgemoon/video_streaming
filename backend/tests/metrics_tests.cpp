// Tests for the metrics registry (exposition format, histogram buckets, label escaping,
// route normalisation) and the tracing client's W3C/OTLP encoding.
#include "observability/metrics.hpp"
#include "observability/tracing.hpp"
#include "observability/vsp_metrics.hpp"

#include <cstdio>
#include <string>
#include <thread>
#include <vector>

using namespace vsp;

namespace {
int failures = 0;
#define CHECK(c)                                                                                        \
    do {                                                                                                \
        if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; }              \
    } while (0)
bool contains(const std::string& h, const std::string& n) { return h.find(n) != std::string::npos; }
} // namespace

int main() {
    metrics::Registry reg;

    // counters / gauges / labels / escaping
    auto* c = reg.counter("t_requests_total", "Requests", {"route", "code"});
    c->with({"/a", "200"})->inc();
    c->with({"/a", "200"})->inc(2);
    c->with({"/b\"q\\", "500"})->inc();
    auto* g = reg.gauge("t_temp", "Temp");
    g->get()->set(21.5);
    g->get()->inc(0.5);

    // histogram
    auto* h = reg.histogram("t_latency_seconds", "Latency", {0.01, 0.1, 1}, {"op"});
    for (double v : {0.005, 0.05, 0.5, 5.0, 0.1}) h->with({"x"})->observe(v);

    std::string out = reg.expose();
    CHECK(contains(out, "# TYPE t_requests_total counter"));
    CHECK(contains(out, "t_requests_total{route=\"/a\",code=\"200\"} 3"));
    CHECK(contains(out, "t_requests_total{route=\"/b\\\"q\\\\\",code=\"500\"} 1"));
    CHECK(contains(out, "t_temp 22"));
    CHECK(contains(out, "t_latency_seconds_bucket{op=\"x\",le=\"0.01\"} 1"));
    CHECK(contains(out, "t_latency_seconds_bucket{op=\"x\",le=\"0.1\"} 3"));   // 0.1 is inclusive
    CHECK(contains(out, "t_latency_seconds_bucket{op=\"x\",le=\"1\"} 4"));
    CHECK(contains(out, "t_latency_seconds_bucket{op=\"x\",le=\"+Inf\"} 5"));
    CHECK(contains(out, "t_latency_seconds_count{op=\"x\"} 5"));
    CHECK(contains(out, "t_latency_seconds_sum{op=\"x\"} 5.655"));

    // concurrent increments are not lost
    auto* cc = reg.counter("t_conc_total", "Concurrent");
    std::vector<std::thread> ts;
    for (int i = 0; i < 8; ++i) ts.emplace_back([&] { for (int k = 0; k < 10000; ++k) cc->get()->inc(); });
    for (auto& t : ts) t.join();
    CHECK(cc->get()->value() == 80000);

    // same family returned for same name; series removal
    CHECK(reg.counter("t_requests_total", "x") == c);
    c->remove({"/a", "200"});
    CHECK(!contains(reg.expose(), "route=\"/a\""));

    // process metrics present
    metrics::register_process_metrics(reg);
    out = reg.expose();
    CHECK(contains(out, "process_cpu_seconds_total"));
    CHECK(contains(out, "process_resident_memory_bytes"));
    CHECK(contains(out, "process_open_fds"));

    // timer
    {
        auto* th = reg.histogram("t_timer_seconds", "Timer", {0.001, 1});
        { metrics::Timer t(th->get()); std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
        CHECK(th->get()->count() == 1);
        CHECK(th->get()->sum() >= 0.002);
    }

    // route normalisation
    CHECK(metrics::route_label("/media/abc123/v_720p/seg_000012.m4s") == "/media/:id/:rendition/segment");
    CHECK(metrics::route_label("/media/abc123/v_720p/index.m3u8") == "/media/:id/:rendition/index.m3u8");
    CHECK(metrics::route_label("/media/abc123/master.m3u8") == "/media/:id/master.m3u8");
    CHECK(metrics::route_label("/api/videos/abc/source") == "/api/videos/:id/source");
    CHECK(metrics::route_label("/api/live/abc") == "/api/live/:id");
    CHECK(metrics::route_label("/api/health") == "/api/health");
    CHECK(metrics::route_label("/whatever") == "other");
    CHECK(metrics::media_kind("/media/x/a/seg_1.m4s") == "segment");
    CHECK(metrics::media_kind("/media/x/a/init.mp4") == "init");

    // global registry: all VSP families resolve and expose without throwing
    auto& M = metrics::M();
    M.http_requests_total->with({"/api/health", "GET", "200"})->inc();
    CHECK(contains(metrics::Registry::global().expose(), "vsp_http_requests_total{route=\"/api/health\",method=\"GET\",status=\"200\"} 1"));

    // tracing: disabled tracer gives non-recording spans; traceparent parsing
    auto s = trace::T().start("noop");
    CHECK(!s->recording());
    s->set_attribute("k", "v");
    s->end();
    auto ctx = trace::SpanContext::from_traceparent("00-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331-01");
    CHECK(ctx.valid() && ctx.trace_id == "0af7651916cd43dd8448eb211c80319c" && ctx.span_id == "b7ad6b7169203331");
    CHECK(ctx.traceparent() == "00-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331-01");
    CHECK(!trace::SpanContext::from_traceparent("garbage").valid());
    CHECK(!trace::SpanContext::from_traceparent("00-zz-yy-01").valid());

    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("all metrics tests passed\n");
    return 0;
}

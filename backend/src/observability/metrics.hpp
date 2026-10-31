#pragma once

// Prometheus-style metrics with a lock-free hot path.
//
// Design: metric *families* are created once at startup (or lazily, under a mutex) and return
// stable pointers to per-label-set *series*. Observing a value on a series is a relaxed atomic
// add/store — no locks, no allocation — so it is safe to call from GStreamer streaming threads,
// the RTMP I/O threads and HTTP coroutines. Exposition walks the registry under the mutex.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace vsp::metrics {

class Counter {
public:
    void inc(double v = 1.0) {
        double cur = value_.load(std::memory_order_relaxed);
        while (!value_.compare_exchange_weak(cur, cur + v, std::memory_order_relaxed)) {}
    }
    double value() const { return value_.load(std::memory_order_relaxed); }

private:
    std::atomic<double> value_{0};
};

class Gauge {
public:
    void set(double v) { value_.store(v, std::memory_order_relaxed); }
    void inc(double v = 1.0) {
        double cur = value_.load(std::memory_order_relaxed);
        while (!value_.compare_exchange_weak(cur, cur + v, std::memory_order_relaxed)) {}
    }
    void dec(double v = 1.0) { inc(-v); }
    double value() const { return value_.load(std::memory_order_relaxed); }

private:
    std::atomic<double> value_{0};
};

// Fixed-bucket histogram. Buckets are chosen per family at creation.
class Histogram {
public:
    explicit Histogram(const std::vector<double>& bounds);
    void observe(double v);
    const std::vector<double>& bounds() const { return bounds_; }
    std::vector<uint64_t> bucket_counts() const;   // cumulative, same size as bounds()
    uint64_t count() const { return count_.load(std::memory_order_relaxed); }
    double sum() const { return sum_.load(std::memory_order_relaxed); }

private:
    std::vector<double> bounds_;
    std::unique_ptr<std::atomic<uint64_t>[]> buckets_;   // non-cumulative, bounds_.size()+1 (last = +Inf)
    std::atomic<uint64_t> count_{0};
    std::atomic<double> sum_{0};
};

// RAII timer: observes elapsed seconds into a histogram on destruction.
class Timer {
public:
    explicit Timer(Histogram* h);
    ~Timer();
    Timer(const Timer&) = delete;
    Timer& operator=(const Timer&) = delete;
    void cancel() { h_ = nullptr; }
    double elapsed() const;

private:
    Histogram* h_;
    std::chrono::steady_clock::time_point t0_;
};

template <typename T>
class Family {
public:
    Family(std::string name, std::string help, std::vector<std::string> label_names, std::vector<double> bounds = {})
        : name_(std::move(name)), help_(std::move(help)), label_names_(std::move(label_names)), bounds_(std::move(bounds)) {}

    // Returns the series for the given label values (must match label_names order). Creates on first use.
    T* with(std::initializer_list<std::string_view> values);
    T* with(const std::vector<std::string>& values);
    T* get() { return with({}); }   // unlabelled
    void remove(const std::vector<std::string>& values);   // drop a series (e.g. a finished live stream)

    const std::string& name() const { return name_; }
    const std::string& help() const { return help_; }
    const std::vector<std::string>& label_names() const { return label_names_; }
    std::vector<std::pair<std::vector<std::string>, T*>> series() const;

private:
    std::string name_, help_;
    std::vector<std::string> label_names_;
    std::vector<double> bounds_;
    mutable std::mutex mu_;
    std::map<std::vector<std::string>, std::unique_ptr<T>> series_;
};

using CounterFamily = Family<Counter>;
using GaugeFamily = Family<Gauge>;
using HistogramFamily = Family<Histogram>;

class Registry {
public:
    static Registry& global();

    CounterFamily* counter(const std::string& name, const std::string& help, std::vector<std::string> labels = {});
    GaugeFamily* gauge(const std::string& name, const std::string& help, std::vector<std::string> labels = {});
    HistogramFamily* histogram(const std::string& name, const std::string& help, std::vector<double> bounds,
                               std::vector<std::string> labels = {});

    // Callback-driven output for values that are cheaper to compute at scrape time.
    using Collect = std::function<void(std::string& out)>;
    void add_collector(Collect fn);

    // Prometheus text exposition format 0.0.4.
    std::string expose() const;

private:
    mutable std::mutex mu_;
    std::map<std::string, std::unique_ptr<CounterFamily>> counters_;
    std::map<std::string, std::unique_ptr<GaugeFamily>> gauges_;
    std::map<std::string, std::unique_ptr<HistogramFamily>> histograms_;
    std::vector<Collect> collectors_;
};

// Common bucket layouts
inline std::vector<double> latency_buckets_s() {   // 1 ms .. 30 s
    return {0.001, 0.0025, 0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1, 2.5, 5, 10, 30};
}
inline std::vector<double> duration_buckets_s() {   // 1 s .. 1 h, for jobs
    return {1, 2, 5, 10, 20, 30, 60, 120, 300, 600, 1200, 1800, 3600};
}
inline std::vector<double> bytes_buckets() {
    return {1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10};
}
inline std::vector<double> ratio_buckets() {   // realtime factor etc.
    return {0.25, 0.5, 0.75, 1, 1.5, 2, 3, 5, 8, 12, 20};
}

std::string escape_label_value(std::string_view v);

// Appends process_* metrics; call once at startup.
void register_process_metrics(Registry& r);

} // namespace vsp::metrics

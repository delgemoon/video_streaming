#include "observability/metrics.hpp"

#include <algorithm>
#include <cmath>
#include <dirent.h>
#include <format>
#include <fstream>
#include <sstream>
#include <sys/resource.h>
#include <unistd.h>

namespace vsp::metrics {

// ---------------------------------------------------------------------------------------------
// Histogram

Histogram::Histogram(const std::vector<double>& bounds) : bounds_(bounds) {
    std::sort(bounds_.begin(), bounds_.end());
    buckets_ = std::make_unique<std::atomic<uint64_t>[]>(bounds_.size() + 1);
    for (size_t i = 0; i <= bounds_.size(); ++i) buckets_[i].store(0, std::memory_order_relaxed);
}

void Histogram::observe(double v) {
    size_t i = static_cast<size_t>(std::lower_bound(bounds_.begin(), bounds_.end(), v) - bounds_.begin());
    buckets_[i].fetch_add(1, std::memory_order_relaxed);
    count_.fetch_add(1, std::memory_order_relaxed);
    double cur = sum_.load(std::memory_order_relaxed);
    while (!sum_.compare_exchange_weak(cur, cur + v, std::memory_order_relaxed)) {}
}

std::vector<uint64_t> Histogram::bucket_counts() const {
    std::vector<uint64_t> out(bounds_.size());
    uint64_t acc = 0;
    for (size_t i = 0; i < bounds_.size(); ++i) {
        acc += buckets_[i].load(std::memory_order_relaxed);
        out[i] = acc;
    }
    return out;
}

Timer::Timer(Histogram* h) : h_(h), t0_(std::chrono::steady_clock::now()) {}
Timer::~Timer() {
    if (h_) h_->observe(elapsed());
}
double Timer::elapsed() const {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0_).count();
}

// ---------------------------------------------------------------------------------------------
// Family

template <typename T>
T* Family<T>::with(std::initializer_list<std::string_view> values) {
    std::vector<std::string> v;
    v.reserve(values.size());
    for (auto s : values) v.emplace_back(s);
    return with(v);
}

template <typename T>
T* Family<T>::with(const std::vector<std::string>& values) {
    std::lock_guard lk(mu_);
    auto it = series_.find(values);
    if (it == series_.end()) {
        std::unique_ptr<T> s;
        if constexpr (std::is_same_v<T, Histogram>) s = std::make_unique<Histogram>(bounds_);
        else s = std::make_unique<T>();
        it = series_.emplace(values, std::move(s)).first;
    }
    return it->second.get();
}

template <typename T>
void Family<T>::remove(const std::vector<std::string>& values) {
    std::lock_guard lk(mu_);
    series_.erase(values);
}

template <typename T>
std::vector<std::pair<std::vector<std::string>, T*>> Family<T>::series() const {
    std::lock_guard lk(mu_);
    std::vector<std::pair<std::vector<std::string>, T*>> out;
    out.reserve(series_.size());
    for (const auto& [k, v] : series_) out.emplace_back(k, v.get());
    return out;
}

template class Family<Counter>;
template class Family<Gauge>;
template class Family<Histogram>;

// ---------------------------------------------------------------------------------------------
// Registry

Registry& Registry::global() {
    static Registry r;
    return r;
}

CounterFamily* Registry::counter(const std::string& name, const std::string& help, std::vector<std::string> labels) {
    std::lock_guard lk(mu_);
    auto it = counters_.find(name);
    if (it == counters_.end())
        it = counters_.emplace(name, std::make_unique<CounterFamily>(name, help, std::move(labels))).first;
    return it->second.get();
}

GaugeFamily* Registry::gauge(const std::string& name, const std::string& help, std::vector<std::string> labels) {
    std::lock_guard lk(mu_);
    auto it = gauges_.find(name);
    if (it == gauges_.end()) it = gauges_.emplace(name, std::make_unique<GaugeFamily>(name, help, std::move(labels))).first;
    return it->second.get();
}

HistogramFamily* Registry::histogram(const std::string& name, const std::string& help, std::vector<double> bounds,
                                     std::vector<std::string> labels) {
    std::lock_guard lk(mu_);
    auto it = histograms_.find(name);
    if (it == histograms_.end())
        it = histograms_.emplace(name, std::make_unique<HistogramFamily>(name, help, std::move(labels), std::move(bounds))).first;
    return it->second.get();
}

void Registry::add_collector(Collect fn) {
    std::lock_guard lk(mu_);
    collectors_.push_back(std::move(fn));
}

std::string escape_label_value(std::string_view v) {
    std::string out;
    out.reserve(v.size() + 2);
    for (char c : v) {
        if (c == '\\') out += "\\\\";
        else if (c == '"') out += "\\\"";
        else if (c == '\n') out += "\\n";
        else out += c;
    }
    return out;
}

namespace {

std::string fmt_num(double v) {
    if (std::isnan(v)) return "NaN";
    if (std::isinf(v)) return v > 0 ? "+Inf" : "-Inf";
    if (v == std::floor(v) && std::fabs(v) < 1e15) return std::format("{}", static_cast<long long>(v));
    return std::format("{:.6g}", v);
}

std::string label_str(const std::vector<std::string>& names, const std::vector<std::string>& values,
                      std::string_view extra = {}) {
    if (names.empty() && extra.empty()) return {};
    std::string s = "{";
    for (size_t i = 0; i < names.size(); ++i) {
        if (i) s += ",";
        s += names[i] + "=\"" + escape_label_value(values[i]) + "\"";
    }
    if (!extra.empty()) {
        if (!names.empty()) s += ",";
        s += extra;
    }
    s += "}";
    return s;
}

} // namespace

std::string Registry::expose() const {
    std::string out;
    out.reserve(32 * 1024);
    std::lock_guard lk(mu_);
    for (const auto& [name, f] : counters_) {
        out += std::format("# HELP {} {}\n# TYPE {} counter\n", name, f->help(), name);
        for (const auto& [labels, s] : f->series())
            out += std::format("{}{} {}\n", name, label_str(f->label_names(), labels), fmt_num(s->value()));
    }
    for (const auto& [name, f] : gauges_) {
        out += std::format("# HELP {} {}\n# TYPE {} gauge\n", name, f->help(), name);
        for (const auto& [labels, s] : f->series())
            out += std::format("{}{} {}\n", name, label_str(f->label_names(), labels), fmt_num(s->value()));
    }
    for (const auto& [name, f] : histograms_) {
        out += std::format("# HELP {} {}\n# TYPE {} histogram\n", name, f->help(), name);
        for (const auto& [labels, s] : f->series()) {
            auto counts = s->bucket_counts();
            const auto& bounds = s->bounds();
            for (size_t i = 0; i < bounds.size(); ++i)
                out += std::format("{}_bucket{} {}\n", name,
                                   label_str(f->label_names(), labels, std::format("le=\"{}\"", fmt_num(bounds[i]))),
                                   counts[i]);
            out += std::format("{}_bucket{} {}\n", name, label_str(f->label_names(), labels, "le=\"+Inf\""), s->count());
            out += std::format("{}_sum{} {}\n", name, label_str(f->label_names(), labels), fmt_num(s->sum()));
            out += std::format("{}_count{} {}\n", name, label_str(f->label_names(), labels), s->count());
        }
    }
    for (const auto& c : collectors_) c(out);
    return out;
}

// ---------------------------------------------------------------------------------------------
// process metrics (Linux /proc)

void register_process_metrics(Registry& r) {
    static const auto start = std::chrono::system_clock::now();
    static const long clk_tck = sysconf(_SC_CLK_TCK);
    static const long page = sysconf(_SC_PAGESIZE);
    r.add_collector([](std::string& out) {
        std::ifstream st("/proc/self/stat");
        std::string line;
        std::getline(st, line);
        auto rp = line.rfind(')');
        if (rp == std::string::npos) return;
        std::istringstream ss(line.substr(rp + 2));
        std::string state;
        long long ppid, pgrp, sess, tty, tpgid;
        unsigned long long flags, minflt, cminflt, majflt, cmajflt, utime, stime;
        long long cutime, cstime, prio, nice, nthreads, itreal;
        unsigned long long starttime, vsize;
        long long rss;
        ss >> state >> ppid >> pgrp >> sess >> tty >> tpgid >> flags >> minflt >> cminflt >> majflt >> cmajflt >>
            utime >> stime >> cutime >> cstime >> prio >> nice >> nthreads >> itreal >> starttime >> vsize >> rss;

        out += "# HELP process_cpu_seconds_total Total user and system CPU time spent in seconds.\n# TYPE process_cpu_seconds_total counter\n";
        out += std::format("process_cpu_seconds_total {:.3f}\n", static_cast<double>(utime + stime) / clk_tck);
        out += "# HELP process_resident_memory_bytes Resident memory size in bytes.\n# TYPE process_resident_memory_bytes gauge\n";
        out += std::format("process_resident_memory_bytes {}\n", rss * page);
        out += "# HELP process_virtual_memory_bytes Virtual memory size in bytes.\n# TYPE process_virtual_memory_bytes gauge\n";
        out += std::format("process_virtual_memory_bytes {}\n", vsize);
        out += "# HELP process_threads Number of OS threads in the process.\n# TYPE process_threads gauge\n";
        out += std::format("process_threads {}\n", nthreads);
        out += "# HELP process_major_page_faults_total Major page faults.\n# TYPE process_major_page_faults_total counter\n";
        out += std::format("process_major_page_faults_total {}\n", majflt);
        out += "# HELP process_start_time_seconds Start time of the process since unix epoch in seconds.\n# TYPE process_start_time_seconds gauge\n";
        out += std::format("process_start_time_seconds {}\n",
                           std::chrono::duration_cast<std::chrono::seconds>(start.time_since_epoch()).count());

        size_t fds = 0;
        if (DIR* d = opendir("/proc/self/fd")) {
            while (readdir(d)) ++fds;
            closedir(d);
            fds = fds > 2 ? fds - 2 : 0;
        }
        out += "# HELP process_open_fds Number of open file descriptors.\n# TYPE process_open_fds gauge\n";
        out += std::format("process_open_fds {}\n", fds);
        rlimit rl{};
        if (getrlimit(RLIMIT_NOFILE, &rl) == 0) {
            out += "# HELP process_max_fds Maximum number of open file descriptors.\n# TYPE process_max_fds gauge\n";
            out += std::format("process_max_fds {}\n", rl.rlim_cur);
        }
    });
}

} // namespace vsp::metrics

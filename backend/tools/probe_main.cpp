// vsp-probe: scans a directory tree and reports which media files the platform can ingest.
//
// For every candidate file it runs the same GstDiscoverer probe the server uses, classifies the
// result, and optionally performs a real decode test (uridecodebin3 -> fakesink for N media
// seconds) to catch files whose metadata parses but whose essence cannot actually be decoded.

#include "core/util.hpp"
#include "media/gst_init.hpp"
#include "media/mxf_fixup.hpp"
#include "media/probe.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <gst/gst.h>
#include <iostream>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace vsp;

namespace {

enum class Verdict { Supported, Partial, Unsupported, NotVideo };

const char* to_string(Verdict v) {
    switch (v) {
    case Verdict::Supported: return "supported";
    case Verdict::Partial: return "partial";
    case Verdict::Unsupported: return "unsupported";
    case Verdict::NotVideo: return "not-video";
    }
    return "?";
}

struct Entry {
    fs::path path;
    uintmax_t size = 0;
    Verdict verdict = Verdict::Unsupported;
    std::string reason;
    std::optional<ProbeResult> probe;
    double decode_fps = 0;   // frames/s achieved in the decode test (0 = not run)
    int probe_ms = 0;
    bool preclassified = false;
};

struct Options {
    std::vector<fs::path> roots;
    bool decode_test = false;
    int decode_seconds = 3;
    int timeout_s = 30;
    unsigned jobs = 0;
    bool all_files = false;
    bool quiet = false;
    bool only_problems = false;
    std::string json_out, csv_out;
    fs::path worker_file;   // internal: set by --worker
};

// Single-frame raw camera formats (image sequences): reported as not-video without probing.
const std::set<std::string> kFrameSeqExt = {"ari", "dpx", "exr", "cin", "dng", "r3d-frame"};

const std::set<std::string> kVideoExt = {
    "mp4", "m4v", "mov", "qt",  "mkv", "webm", "avi",  "wmv",  "asf", "flv",  "f4v",  "mxf",  "ts",  "m2ts",
    "mts", "mp2ts", "ps", "mp2ps", "mpg", "mpeg", "m2v", "vob", "dv",  "3gp",  "3g2",  "ogv",  "ogm", "rm",
    "rmvb", "mp2raw", "m2p", "mod", "tod", "gxf", "lxf", "y4m", "mjpeg", "mjpg", "h264", "264", "hevc", "265",
    "ivf", "braw", "r3d", "divx", "xvid", "nut", "yuv"};

bool is_hidden(const fs::path& p) { return p.filename().string().starts_with("."); }

std::string fmt_size(uintmax_t b) {
    const char* u[] = {"B", "KB", "MB", "GB", "TB"};
    double v = static_cast<double>(b);
    int i = 0;
    while (v >= 1024 && i < 4) { v /= 1024; ++i; }
    return i == 0 ? std::format("{} B", b) : std::format("{:.1f} {}", v, u[i]);
}

// Decodes `seconds` of media time through the real decoder chain; reports decoded video fps.
// Pads are linked dynamically so video-only / audio-only sources don't stall waiting for a
// stream that never appears.
struct DecodeCtx {
    GstElement* pipeline = nullptr;
    std::atomic<uint64_t> frames{0};
};

GstPadProbeReturn count_buffer(GstPad*, GstPadProbeInfo*, gpointer u) {
    static_cast<DecodeCtx*>(u)->frames.fetch_add(1, std::memory_order_relaxed);
    return GST_PAD_PROBE_OK;
}

void decode_pad_added(GstElement*, GstPad* pad, gpointer u) {
    auto* ctx = static_cast<DecodeCtx*>(u);
    GstCaps* caps = gst_pad_get_current_caps(pad);
    if (!caps) caps = gst_pad_query_caps(pad, nullptr);
    const gchar* name = caps ? gst_structure_get_name(gst_caps_get_structure(caps, 0)) : "";
    bool is_video = g_str_has_prefix(name, "video/");
    if (caps) gst_caps_unref(caps);

    GstElement* q = gst_element_factory_make("queue", nullptr);
    GstElement* sink = gst_element_factory_make("fakesink", nullptr);
    g_object_set(sink, "sync", FALSE, "async", FALSE, nullptr);
    gst_bin_add_many(GST_BIN(ctx->pipeline), q, sink, nullptr);
    gst_element_link(q, sink);
    gst_element_sync_state_with_parent(q);
    gst_element_sync_state_with_parent(sink);
    GstPad* qsink = gst_element_get_static_pad(q, "sink");
    if (is_video) gst_pad_add_probe(qsink, GST_PAD_PROBE_TYPE_BUFFER, count_buffer, ctx, nullptr);
    gst_pad_link(pad, qsink);
    gst_object_unref(qsink);
}

std::pair<bool, std::string> decode_test(const fs::path& path, int seconds, double& out_fps) {
    gchar* uri = gst_filename_to_uri(path.c_str(), nullptr);
    if (!uri) return {false, "bad path"};
    DecodeCtx ctx;
    ctx.pipeline = gst_pipeline_new("decode-test");
    install_mxf_h264_fixup(ctx.pipeline);
    GstElement* dec = gst_element_factory_make("uridecodebin3", "d");
    g_object_set(dec, "uri", uri, nullptr);
    g_free(uri);
    gst_bin_add(GST_BIN(ctx.pipeline), dec);
    g_signal_connect(dec, "pad-added", G_CALLBACK(decode_pad_added), &ctx);

    GstBus* bus = gst_element_get_bus(ctx.pipeline);
    auto t0 = std::chrono::steady_clock::now();
    bool ok = true;
    std::string why;
    if (gst_element_set_state(ctx.pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        ok = false;
        why = "cannot start pipeline";
    }
    uint64_t last_frames = 0;
    auto last_progress = t0;
    while (ok) {
        GstMessage* msg = gst_bus_timed_pop_filtered(
            bus, 200 * GST_MSECOND, static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
        if (msg) {
            if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
                GError* e = nullptr;
                gst_message_parse_error(msg, &e, nullptr);
                why = e ? e->message : "decode error";
                if (e) g_error_free(e);
                ok = false;
            }
            gst_message_unref(msg);
            break;
        }
        gint64 pos = 0;
        if (gst_element_query_position(ctx.pipeline, GST_FORMAT_TIME, &pos) && pos >= seconds * GST_SECOND) break;
        uint64_t f = ctx.frames.load();
        auto now = std::chrono::steady_clock::now();
        if (f != last_frames) { last_frames = f; last_progress = now; }
        if (now - last_progress > std::chrono::seconds(20)) {
            why = f == 0 ? "no video frames decoded within 20s" : "decoder stalled";
            ok = false;
            break;
        }
    }
    gst_element_set_state(ctx.pipeline, GST_STATE_NULL);
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    out_fps = secs > 0 ? static_cast<double>(ctx.frames.load()) / secs : 0;
    if (ok && ctx.frames.load() == 0) {
        ok = false;
        why = "no video frames decoded";
    }
    gst_object_unref(bus);
    gst_object_unref(ctx.pipeline);
    return {ok, why};
}

nlohmann::json probe_to_json(const ProbeResult& p) {
    return {{"container", p.container},         {"duration_ms", p.duration_ms},
            {"width", p.width},                 {"height", p.height},
            {"fps_n", p.fps_n},                 {"fps_d", p.fps_d},
            {"interlaced", p.interlaced},       {"depth", p.video_depth},
            {"video_codec", p.video_codec},     {"video_bitrate", p.video_bitrate},
            {"video_streams", p.video_streams}, {"audio_codec", p.audio_codec},
            {"audio_channels", p.audio_channels}, {"audio_rate", p.audio_rate},
            {"audio_streams", p.audio_streams}, {"has_video", p.has_video},
            {"has_audio", p.has_audio}};
}

ProbeResult probe_from_json(const nlohmann::json& j) {
    ProbeResult p;
    p.container = j.value("container", "");
    p.duration_ms = j.value("duration_ms", int64_t{0});
    p.width = j.value("width", 0);
    p.height = j.value("height", 0);
    p.fps_n = j.value("fps_n", 0);
    p.fps_d = j.value("fps_d", 1);
    p.interlaced = j.value("interlaced", false);
    p.video_depth = j.value("depth", 0u);
    p.video_codec = j.value("video_codec", "");
    p.video_bitrate = j.value("video_bitrate", 0u);
    p.video_streams = j.value("video_streams", 0u);
    p.audio_codec = j.value("audio_codec", "");
    p.audio_channels = j.value("audio_channels", 0);
    p.audio_rate = j.value("audio_rate", 0);
    p.audio_streams = j.value("audio_streams", 0u);
    p.has_video = j.value("has_video", false);
    p.has_audio = j.value("has_audio", false);
    return p;
}

// Child process: probe (and optionally decode-test) one file, print a single JSON line.
// Runs in its own process so a crashing demuxer/decoder cannot take the scan down.
int worker_main(const fs::path& file, const Options& opt) {
    // libav decoders print "# no audio" etc. straight to stderr; silence everything but our JSON.
    int devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0) { dup2(devnull, STDERR_FILENO); close(devnull); }
    gst_platform_init();

    nlohmann::json out;
    auto pr = probe_file(file, opt.timeout_s);
    if (!pr) {
        out["error"] = pr.error();
    } else {
        out["probe"] = probe_to_json(*pr);
        if (opt.decode_test) {
            double fps = 0;
            auto [ok, why] = decode_test(file, opt.decode_seconds, fps);
            out["decode_ok"] = ok;
            out["decode_fps"] = fps;
            if (!ok) out["decode_error"] = why;
        }
    }
    std::string line = out.dump() + "\n";
    if (write(STDOUT_FILENO, line.data(), line.size()) < 0) return 3;
    _exit(0);
}

// Parent side: fork a worker for `e`, collect its JSON line, translate crashes/timeouts.
struct WorkerResult {
    nlohmann::json json;
    std::string failure;   // non-empty if the worker itself died
};

WorkerResult run_isolated(const Entry& e, const Options& opt, const char* self) {
    int fds[2];
    if (pipe(fds) != 0) return {{}, "pipe failed"};
    pid_t pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); return {{}, "fork failed"}; }
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        close(fds[1]);
        std::vector<std::string> args = {self, "--worker", e.path.string(), "--timeout", std::to_string(opt.timeout_s),
                                         "--decode-seconds", std::to_string(opt.decode_seconds)};
        if (opt.decode_test) args.push_back("--decode");
        std::vector<char*> argv;
        for (auto& a : args) argv.push_back(a.data());
        argv.push_back(nullptr);
        execv(self, argv.data());
        _exit(127);
    }
    close(fds[1]);

    // Read with an overall deadline: probe timeout + decode budget + slack.
    const int budget_s = opt.timeout_s + (opt.decode_test ? opt.decode_seconds * 10 + 20 : 0) + 15;
    std::string buf;
    char tmp[4096];
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(budget_s);
    bool timed_out = false;
    for (;;) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fds[0], &rfds);
        timeval tv{1, 0};
        int r = select(fds[0] + 1, &rfds, nullptr, nullptr, &tv);
        if (r > 0) {
            ssize_t n = read(fds[0], tmp, sizeof tmp);
            if (n <= 0) break;
            buf.append(tmp, static_cast<size_t>(n));
        }
        if (std::chrono::steady_clock::now() > deadline) { timed_out = true; break; }
    }
    close(fds[0]);
    if (timed_out) kill(pid, SIGKILL);
    int status = 0;
    waitpid(pid, &status, 0);

    if (timed_out) return {{}, std::format("worker timed out after {}s (hung demuxer/decoder)", budget_s)};
    if (WIFSIGNALED(status)) {
        int sig = WTERMSIG(status);
        return {{}, std::format("GStreamer crashed while probing ({}{})", strsignal(sig),
                                sig == SIGABRT ? ": internal assertion in a demuxer/decoder" : "")};
    }
    if (buf.empty()) return {{}, std::format("worker exited with status {} and no output", WEXITSTATUS(status))};
    auto j = nlohmann::json::parse(buf, nullptr, false);
    if (j.is_discarded()) return {{}, "worker produced invalid output"};
    return {j, {}};
}

void classify(Entry& e, const Options& opt, const char* self) {
    auto t0 = std::chrono::steady_clock::now();
    WorkerResult wr = run_isolated(e, opt, self);
    e.probe_ms = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
    if (!wr.failure.empty()) {
        e.verdict = Verdict::Unsupported;
        e.reason = wr.failure;
        return;
    }
    std::expected<ProbeResult, std::string> pr;
    if (wr.json.contains("error")) pr = std::unexpected(wr.json["error"].get<std::string>());
    else pr = probe_from_json(wr.json["probe"]);
    if (!pr) {
        const std::string& m = pr.error();
        bool not_video = m.find("still image") != std::string::npos || m.find("audio-only") != std::string::npos ||
                         m.find("no video stream") != std::string::npos;
        e.verdict = not_video ? Verdict::NotVideo : Verdict::Unsupported;
        e.reason = m;
        return;
    }
    e.probe = *pr;

    if (opt.decode_test) {
        e.decode_fps = wr.json.value("decode_fps", 0.0);
        if (!wr.json.value("decode_ok", false)) {
            e.verdict = Verdict::Unsupported;
            e.reason = "decode failed: " + wr.json.value("decode_error", std::string("unknown"));
            return;
        }
    }

    std::vector<std::string> notes;
    if (!pr->has_audio) notes.push_back("no audio");
    if (pr->interlaced) notes.push_back("interlaced (output stays interlaced-as-progressive)");
    if (pr->fps() > 60) notes.push_back(std::format("{:.3g} fps will be capped to 60", pr->fps()));
    if (pr->video_depth > 8) notes.push_back(std::format("{}-bit source → 8-bit H.264 output", pr->video_depth));
    if (pr->video_streams > 1) notes.push_back(std::format("{} video streams, first used", pr->video_streams));
    if (pr->audio_streams > 1) notes.push_back(std::format("{} audio streams, first used", pr->audio_streams));
    if (pr->height < 240) notes.push_back("very low resolution");

    if (notes.empty()) {
        e.verdict = Verdict::Supported;
    } else {
        e.verdict = Verdict::Partial;
        for (auto& n : notes) e.reason += (e.reason.empty() ? "" : "; ") + n;
    }
}

void usage() {
    std::cout <<
R"(vsp-probe — check which media files the platform can ingest

usage: vsp-probe [options] <file-or-directory>...

  -d, --decode            also decode the first seconds of each file (slower; catches broken essence)
      --decode-seconds N  media seconds to decode in the test (default 3)
  -t, --timeout S         probe timeout per file (default 30)
  -j, --jobs N            parallel probes (default 2-6 depending on cores; probing is disk-bound,
                          raise only for SSDs). Each probe runs in its own process, so a crashing
                          demuxer only fails that one file
  -a, --all               probe every file, not only known video extensions
  -p, --problems          only list files that are not fully supported
  -q, --quiet             no per-file progress, just the report
      --json FILE         write full results as JSON
      --csv FILE          write results as CSV

verdicts:
  supported    will transcode as-is
  partial      will transcode, with caveats (interlaced, >60 fps, >8-bit, no audio, …)
  unsupported  probe or decode failed (missing plugin, corrupt file, unknown codec)
  not-video    audio-only, still image, or not a media file

exit status: 0 if nothing is unsupported, 1 otherwise.
)";
}

Options parse_args(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&] {
            if (i + 1 >= argc) { std::cerr << "missing value for " << a << "\n"; std::exit(2); }
            return std::string(argv[++i]);
        };
        if (a == "--decode" || a == "-d") o.decode_test = true;
        else if (a == "--decode-seconds") o.decode_seconds = std::stoi(next());
        else if (a == "--timeout" || a == "-t") o.timeout_s = std::stoi(next());
        else if (a == "--jobs" || a == "-j") o.jobs = static_cast<unsigned>(std::stoul(next()));
        else if (a == "--all" || a == "-a") o.all_files = true;
        else if (a == "--problems" || a == "-p") o.only_problems = true;
        else if (a == "--quiet" || a == "-q") o.quiet = true;
        else if (a == "--json") o.json_out = next();
        else if (a == "--csv") o.csv_out = next();
        else if (a == "--worker") o.worker_file = next();
        else if (a == "--help" || a == "-h") { usage(); std::exit(0); }
        else if (!a.empty() && a[0] == '-') { std::cerr << "unknown option " << a << "\n"; std::exit(2); }
        else o.roots.emplace_back(a);
    }
    if (o.roots.empty() && o.worker_file.empty()) { usage(); std::exit(2); }
    return o;
}

} // namespace

int main(int argc, char** argv) {
    Options opt = parse_args(argc, argv);
    g_setenv("GST_DEBUG", "0", FALSE);
    if (!opt.worker_file.empty()) return worker_main(opt.worker_file, opt);

    // Resolve our own executable so workers can re-exec it.
    char self_buf[4096];
    ssize_t n = readlink("/proc/self/exe", self_buf, sizeof self_buf - 1);
    if (n <= 0) { std::cerr << "cannot resolve own executable path\n"; return 2; }
    self_buf[n] = 0;
    const char* self = self_buf;
    gst_platform_init();

    // ---- enumerate candidates
    std::vector<Entry> entries;
    std::map<std::string, size_t> skipped_ext;
    size_t hidden = 0;
    for (const auto& root : opt.roots) {
        std::error_code ec;
        if (fs::is_regular_file(root, ec)) {
            entries.push_back(Entry{root, fs::file_size(root, ec)});
            continue;
        }
        if (!fs::is_directory(root, ec)) {
            std::cerr << "not found: " << root << "\n";
            continue;
        }
        for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) { ec.clear(); continue; }
            if (!it->is_regular_file(ec)) continue;
            if (is_hidden(it->path())) { ++hidden; continue; }   // includes AppleDouble "._x" shadows
            std::string ext = util::file_extension_lower(it->path().filename().string());
            if (kFrameSeqExt.contains(ext)) {
                Entry e{it->path(), it->file_size(ec)};
                e.verdict = Verdict::NotVideo;
                e.reason = "raw camera frame (image sequence), not a video container";
                e.preclassified = true;
                entries.push_back(std::move(e));
                continue;
            }
            if (!opt.all_files && !kVideoExt.contains(ext)) {
                ++skipped_ext[ext.empty() ? "(none)" : ext];
                continue;
            }
            entries.push_back(Entry{it->path(), it->file_size(ec)});
        }
    }
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.path < b.path; });
    if (entries.empty()) {
        std::cout << "no candidate media files found\n";
        return 0;
    }

    const fs::path base = opt.roots.size() == 1 && fs::is_directory(opt.roots[0]) ? opt.roots[0] : fs::path();
    auto rel = [&](const fs::path& p) {
        if (base.empty()) return p.string();
        std::error_code ec;
        auto r = fs::relative(p, base, ec);
        return ec ? p.string() : r.string();
    };

    // Probing is I/O-bound (demuxers seek around the file); too many parallel readers on one
    // disk just thrash and cause false timeouts, so stay modest unless told otherwise.
    const unsigned jobs = opt.jobs ? opt.jobs : std::clamp(std::thread::hardware_concurrency() / 8, 2u, 6u);
    std::cerr << std::format("probing {} files with {} parallel workers{}…\n", entries.size(), jobs,
                             opt.decode_test ? " (+decode test)" : "");

    // ---- probe in parallel
    std::atomic<size_t> next{0}, done{0};
    std::mutex out_mu;
    auto t_start = std::chrono::steady_clock::now();
    auto worker = [&] {
        for (;;) {
            size_t i = next.fetch_add(1);
            if (i >= entries.size()) return;
            if (!entries[i].preclassified) classify(entries[i], opt, self);
            size_t d = ++done;
            if (!opt.quiet) {
                std::lock_guard lk(out_mu);
                std::cerr << std::format("\r[{}/{}] {:<12} {:.90}\033[K", d, entries.size(),
                                         to_string(entries[i].verdict), rel(entries[i].path));
                if (d == entries.size()) std::cerr << "\n";
            }
        }
    };
    std::vector<std::thread> pool;
    for (unsigned i = 0; i < jobs; ++i) pool.emplace_back(worker);
    for (auto& t : pool) t.join();
    double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count();

    // ---- aggregate
    std::map<Verdict, size_t> counts;
    std::map<std::string, std::pair<size_t, size_t>> by_codec, by_container;   // name -> (ok, total)
    std::map<std::string, size_t> reasons;
    for (const auto& e : entries) {
        ++counts[e.verdict];
        bool ok = e.verdict == Verdict::Supported || e.verdict == Verdict::Partial;
        if (e.probe) {
            auto& c = by_codec[e.probe->video_codec.empty() ? "(unknown)" : e.probe->video_codec];
            ++c.second;
            if (ok) ++c.first;
            auto& k = by_container[e.probe->container.empty() ? "(unknown)" : e.probe->container];
            ++k.second;
            if (ok) ++k.first;
        }
        if (e.verdict == Verdict::Unsupported) ++reasons[e.reason.substr(0, 110)];
    }

    // ---- per-file listing
    std::cout << "\n";
    for (const auto& e : entries) {
        if (opt.only_problems && e.verdict == Verdict::Supported) continue;
        std::cout << std::format("{:<12} {:>9}  {}\n", to_string(e.verdict), fmt_size(e.size), rel(e.path));
        if (e.probe) {
            const auto& p = *e.probe;
            std::string info = std::format(
                "{}x{}{} {:.4g}fps {} | {} | {}", p.width, p.height, p.interlaced ? "i" : "p", p.fps(),
                util::format_duration(static_cast<uint64_t>(p.duration_ms)).substr(0, 8), p.video_codec,
                p.has_audio ? std::format("{} {}ch {}Hz", p.audio_codec, p.audio_channels, p.audio_rate) : "no audio");
            if (p.video_depth > 8) info += std::format(" | {}-bit", p.video_depth);
            if (e.decode_fps > 0) info += std::format(" | decode {:.0f} fps", e.decode_fps);
            std::cout << std::format("{:<22}  └ {}\n", "", info);
        }
        if (!e.reason.empty()) std::cout << std::format("{:<22}  └ {}\n", "", e.reason);
    }

    // ---- summary
    size_t skipped_total = hidden;
    for (auto& [k, v] : skipped_ext) skipped_total += v;
    std::cout << std::format("\n==== Summary: {} ({:.1f}s) ====\n",
                             opt.roots.size() == 1 ? opt.roots[0].string() : std::format("{} roots", opt.roots.size()),
                             elapsed);
    std::cout << std::format("  files probed : {}\n", entries.size());
    std::cout << std::format("  supported    : {}\n", counts[Verdict::Supported]);
    std::cout << std::format("  partial      : {}   (transcodes, with caveats)\n", counts[Verdict::Partial]);
    std::cout << std::format("  unsupported  : {}\n", counts[Verdict::Unsupported]);
    std::cout << std::format("  not video    : {}   (audio-only sidecars / raw frames / stills)\n", counts[Verdict::NotVideo]);
    std::cout << std::format("  skipped      : {}   ({} hidden, {} by extension — use --all to probe those)\n",
                             skipped_total, hidden, skipped_total - hidden);

    auto print_table = [&](const char* title, auto& m) {
        if (m.empty()) return;
        std::cout << "\n  " << title << "\n";
        std::vector<std::pair<std::string, std::pair<size_t, size_t>>> rows(m.begin(), m.end());
        std::sort(rows.begin(), rows.end(), [](auto& a, auto& b) { return a.second.second > b.second.second; });
        for (auto& [name, c] : rows)
            std::cout << std::format("    {:<52.52} {:>4}/{:<4} {}\n", name, c.first, c.second,
                                     c.first == c.second ? "ok" : c.first == 0 ? "NONE" : "partial");
    };
    print_table("By video codec (usable/total):", by_codec);
    print_table("By container (usable/total):", by_container);
    if (!reasons.empty()) {
        std::cout << "\n  Failure reasons:\n";
        std::vector<std::pair<std::string, size_t>> rows(reasons.begin(), reasons.end());
        std::sort(rows.begin(), rows.end(), [](auto& a, auto& b) { return a.second > b.second; });
        for (auto& [r, n] : rows) std::cout << std::format("    {:>4}  {}\n", n, r);
    }

    // ---- exports
    if (!opt.json_out.empty()) {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& e : entries) {
            nlohmann::json j{{"path", e.path.string()}, {"size_bytes", e.size}, {"verdict", to_string(e.verdict)},
                             {"reason", e.reason}, {"probe_ms", e.probe_ms}, {"decode_fps", e.decode_fps}};
            if (e.probe) {
                const auto& p = *e.probe;
                j["media"] = {{"container", p.container},      {"duration_ms", p.duration_ms},
                              {"width", p.width},              {"height", p.height},
                              {"fps", p.fps()},                {"interlaced", p.interlaced},
                              {"depth", p.video_depth},        {"video_codec", p.video_codec},
                              {"video_bitrate", p.video_bitrate}, {"video_streams", p.video_streams},
                              {"audio_codec", p.audio_codec},  {"audio_channels", p.audio_channels},
                              {"audio_rate", p.audio_rate},    {"audio_streams", p.audio_streams}};
            }
            arr.push_back(std::move(j));
        }
        nlohmann::json out{{"roots", nlohmann::json::array()},
                           {"generated_at", util::iso8601(util::now_ms())},
                           {"decode_test", opt.decode_test},
                           {"summary",
                            {{"probed", entries.size()},
                             {"supported", counts[Verdict::Supported]},
                             {"partial", counts[Verdict::Partial]},
                             {"unsupported", counts[Verdict::Unsupported]},
                             {"not_video", counts[Verdict::NotVideo]},
                             {"skipped", skipped_total}}},
                           {"files", arr}};
        for (auto& r : opt.roots) out["roots"].push_back(r.string());
        std::ofstream(opt.json_out) << out.dump(2) << "\n";
        std::cout << "\nJSON written to " << opt.json_out << "\n";
    }
    if (!opt.csv_out.empty()) {
        std::ofstream f(opt.csv_out);
        f << "verdict,path,size_bytes,container,video_codec,width,height,fps,interlaced,depth,duration_ms,"
             "audio_codec,audio_channels,audio_rate,decode_fps,reason\n";
        auto q = [](std::string s) {
            for (auto& c : s) if (c == '"') c = '\'';
            return "\"" + s + "\"";
        };
        for (const auto& e : entries) {
            const ProbeResult p = e.probe.value_or(ProbeResult{});
            f << to_string(e.verdict) << ',' << q(e.path.string()) << ',' << e.size << ',' << q(p.container) << ','
              << q(p.video_codec) << ',' << p.width << ',' << p.height << ',' << std::format("{:.3f}", p.fps()) << ','
              << (p.interlaced ? 1 : 0) << ',' << p.video_depth << ',' << p.duration_ms << ',' << q(p.audio_codec)
              << ',' << p.audio_channels << ',' << p.audio_rate << ',' << std::format("{:.1f}", e.decode_fps) << ','
              << q(e.reason) << "\n";
        }
        std::cout << "CSV written to " << opt.csv_out << "\n";
    }
    return counts[Verdict::Unsupported] ? 1 : 0;
}

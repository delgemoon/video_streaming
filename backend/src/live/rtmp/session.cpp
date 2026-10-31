#include "live/rtmp/session.hpp"

#include "core/util.hpp"
#include "observability/vsp_metrics.hpp"

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/write.hpp>
#include <format>
#include <random>
#include <spdlog/spdlog.h>

namespace vsp::rtmp {

namespace asio = boost::asio;
using asio::use_awaitable;
using asio::as_tuple;
using namespace std::chrono_literals;

namespace {

constexpr size_t kHandshakeSize = 1536;
constexpr uint32_t kControlCsid = 2;
constexpr uint32_t kCommandCsid = 3;
constexpr uint32_t kStreamCsid = 5;

uint32_t rd32(const uint8_t* p) { return (uint32_t{p[0]} << 24) | (uint32_t{p[1]} << 16) | (uint32_t{p[2]} << 8) | p[3]; }
uint16_t rd16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }
void put16(std::string& o, uint16_t v) { o.push_back(static_cast<char>(v >> 8)); o.push_back(static_cast<char>(v & 0xff)); }
void put32(std::string& o, uint32_t v) { put16(o, static_cast<uint16_t>(v >> 16)); put16(o, static_cast<uint16_t>(v & 0xffff)); }

// Heap storage for non-media messages (commands/control): tiny and rare.
struct HeapBuf {
    std::vector<uint8_t> bytes;
};

} // namespace

Session::Session(asio::ip::tcp::socket socket, PublishHandler& handler, const ServerConfig& cfg)
    : socket_(std::move(socket)), handler_(handler), cfg_(cfg), id_(util::generate_id().substr(0, 12)),
      reader_([this](const MessageHeader& h) { return allocate(h); },
              [this](const MessageHeader& h, MessageBuffer b) { on_message(h, b); }) {
    boost::system::error_code ec;
    auto ep = socket_.remote_endpoint(ec);
    if (!ec) remote_ = std::format("{}:{}", ep.address().to_string(), ep.port());
    in_.resize(cfg_.read_buffer_size);
    rate_t0_ = std::chrono::steady_clock::now();
}

Session::~Session() {
    if (sink_) sink_->on_end("session destroyed");
}

void Session::close() {
    auto self = shared_from_this();
    asio::post(socket_.get_executor(), [self] {
        self->closing_ = true;
        boost::system::error_code ec;
        self->socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
        self->socket_.close(ec);
    });
}

SessionStats Session::stats() const {
    SessionStats s;
    s.bytes_received = stat_bytes_.load();
    s.media_messages = stat_msgs_.load();
    s.last_video_ts = stat_vts_.load();
    s.last_audio_ts = stat_ats_.load();
    s.bitrate_bps = stat_bps_.load();
    return s;
}

// ---------------------------------------------------------------------------------------------
// handshake (simple/plain variant; accepted by OBS, ffmpeg, GStreamer, librtmp)

asio::awaitable<bool> Session::handshake() {
    std::vector<uint8_t> c0c1(1 + kHandshakeSize);
    auto [ec, n] = co_await asio::async_read(socket_, asio::buffer(c0c1), as_tuple(use_awaitable));
    if (ec) co_return false;
    if (c0c1[0] != 3) {
        spdlog::warn("rtmp[{}] unsupported RTMP version {} from {}", id_, c0c1[0], remote_);
        co_return false;
    }

    std::string s0s1s2;
    s0s1s2.reserve(1 + 2 * kHandshakeSize);
    s0s1s2.push_back(3);
    // S1: time(4) zero(4) random(1528)
    put32(s0s1s2, 0);
    put32(s0s1s2, 0);
    {
        std::mt19937_64 rng{std::random_device{}()};
        for (size_t i = 0; i < kHandshakeSize - 8; i += 8) {
            uint64_t r = rng();
            s0s1s2.append(reinterpret_cast<const char*>(&r), 8);
        }
    }
    // S2: echo of C1 (time, time2, random)
    s0s1s2.append(reinterpret_cast<const char*>(c0c1.data() + 1), kHandshakeSize);
    auto [wec, wn] = co_await asio::async_write(socket_, asio::buffer(s0s1s2), as_tuple(use_awaitable));
    if (wec) co_return false;

    std::vector<uint8_t> c2(kHandshakeSize);
    auto [ec2, n2] = co_await asio::async_read(socket_, asio::buffer(c2), as_tuple(use_awaitable));
    if (ec2) co_return false;
    co_return true;
}

// ---------------------------------------------------------------------------------------------

asio::awaitable<void> Session::run() {
    auto self = shared_from_this();
    spdlog::info("rtmp[{}] connection from {}", id_, remote_);
    metrics::M().rtmp_connections->get()->inc();
    struct ConnGauge { ~ConnGauge() { metrics::M().rtmp_connections->get()->dec(); } } conn_gauge;

    asio::steady_timer timer(socket_.get_executor());
    bool ok = false;
    {
        // handshake with timeout
        bool timed_out = false;
        timer.expires_after(cfg_.handshake_timeout);
        timer.async_wait([this, &timed_out](const boost::system::error_code& e) {
            if (!e) { timed_out = true; boost::system::error_code ec; socket_.close(ec); }
        });
        ok = co_await handshake();
        timer.cancel();
        if (!ok) {
            metrics::M().rtmp_errors_total->with({timed_out ? "handshake_timeout" : "handshake"})->inc();
            spdlog::info("rtmp[{}] handshake failed{}", id_, timed_out ? " (timeout)" : "");
            co_return;
        }
    }
    spdlog::debug("rtmp[{}] handshake complete", id_);

    std::string error;
    while (!closing_) {
        // idle timeout
        timer.expires_after(cfg_.idle_timeout);
        timer.async_wait([this](const boost::system::error_code& e) {
            if (!e) { boost::system::error_code ec; socket_.close(ec); }
        });
        auto [ec, n] = co_await socket_.async_read_some(asio::buffer(in_.data() + in_len_, in_.size() - in_len_),
                                                        as_tuple(use_awaitable));
        timer.cancel();
        if (ec) {
            if (ec != asio::error::eof && ec != asio::error::operation_aborted && !closing_)
                spdlog::debug("rtmp[{}] read: {}", id_, ec.message());
            break;
        }
        in_len_ += n;
        bytes_in_ += n;
        stat_bytes_.store(bytes_in_, std::memory_order_relaxed);
        metrics::M().rtmp_bytes_total->get()->inc(static_cast<double>(n));

        size_t consumed = reader_.feed(in_.data(), in_len_, error);
        if (!error.empty()) {
            metrics::M().rtmp_errors_total->with({"protocol"})->inc();
            spdlog::warn("rtmp[{}] protocol error: {}", id_, error);
            break;
        }
        if (consumed < in_len_) std::memmove(in_.data(), in_.data() + consumed, in_len_ - consumed);
        in_len_ -= consumed;
        if (in_len_ == in_.size()) {
            // A single chunk larger than our buffer: grow (bounded by the reader's 64 MiB limit).
            in_.resize(in_.size() * 2);
        }

        // acknowledgements
        if (bytes_in_ - last_ack_at_ >= peer_window_) {
            send_ack(static_cast<uint32_t>(bytes_in_));
            last_ack_at_ = bytes_in_;
        }

        // bitrate (1 s window)
        auto now = std::chrono::steady_clock::now();
        auto dt = std::chrono::duration<double>(now - rate_t0_).count();
        if (dt >= 1.0) {
            stat_bps_.store(static_cast<double>(bytes_in_ - rate_bytes0_) * 8 / dt);
            rate_t0_ = now;
            rate_bytes0_ = bytes_in_;
        }

        if (!out_.empty()) {
            std::string pending;
            pending.swap(out_);
            auto [wec, wn] = co_await asio::async_write(socket_, asio::buffer(pending), as_tuple(use_awaitable));
            if (wec) break;
        }
    }

    end_publish(closing_ ? "closed by server" : "publisher disconnected");
    boost::system::error_code ec;
    socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
    socket_.close(ec);
    spdlog::info("rtmp[{}] closed ({} bytes)", id_, bytes_in_);
}

// ---------------------------------------------------------------------------------------------
// inbound

MessageBuffer Session::allocate(const MessageHeader& hdr) {
    if ((hdr.type == kAudio || hdr.type == kVideo) && sink_ && publishing_) return sink_->allocate(hdr.type, hdr.length);
    auto* hb = new HeapBuf;
    hb->bytes.resize(hdr.length);
    return MessageBuffer{hb->bytes.data(), hdr.length, hb};
}

void Session::on_message(const MessageHeader& hdr, MessageBuffer buf) {
    if ((hdr.type == kAudio || hdr.type == kVideo) && sink_ && publishing_) {
        stat_msgs_.fetch_add(1, std::memory_order_relaxed);
        metrics::M().rtmp_messages_total->with({hdr.type == kVideo ? "video" : "audio"})->inc();
        if (hdr.type == kVideo) stat_vts_.store(hdr.timestamp, std::memory_order_relaxed);
        else stat_ats_.store(hdr.timestamp, std::memory_order_relaxed);
        sink_->on_media(hdr.type, hdr.timestamp, buf);
        return;
    }
    std::unique_ptr<HeapBuf> hb(static_cast<HeapBuf*>(buf.user));
    const uint8_t* d = buf.data;
    size_t len = buf.length;
    switch (hdr.type) {
    case kSetChunkSize:
    case kAbort:
    case kAck:
    case kUserControl:
    case kWindowAckSize:
    case kSetPeerBandwidth:
        metrics::M().rtmp_messages_total->with({"control"})->inc();
        on_control(hdr, d, len);
        break;
    case kCommandAmf0:
        metrics::M().rtmp_messages_total->with({"command"})->inc();
        on_command(hdr, d, len);
        break;
    case kCommandAmf3:
        if (len > 0) on_command(hdr, d + 1, len - 1);   // AMF3 command messages start with a 0x00 AMF0 marker
        break;
    case kDataAmf0:
        metrics::M().rtmp_messages_total->with({"data"})->inc();
        on_data_message(hdr, d, len);
        break;
    case kDataAmf3:
        if (len > 0) on_data_message(hdr, d + 1, len - 1);
        break;
    case kAudio:
    case kVideo:
        break;   // media before publish(): ignore
    default:
        spdlog::debug("rtmp[{}] ignoring message type {} ({} bytes)", id_, hdr.type, len);
    }
}

void Session::on_control(const MessageHeader& hdr, const uint8_t* d, size_t len) {
    switch (hdr.type) {
    case kSetChunkSize:
        if (len >= 4) {
            uint32_t n = rd32(d) & 0x7fffffff;
            if (n >= 1 && n <= 0xffffff) {
                reader_.set_chunk_size(n);
                spdlog::debug("rtmp[{}] peer chunk size {}", id_, n);
            }
        }
        break;
    case kWindowAckSize:
        if (len >= 4) peer_window_ = std::max<uint32_t>(rd32(d), 4096);
        break;
    case kSetPeerBandwidth:
        if (len >= 4) send_window_ack_size(rd32(d));
        break;
    case kUserControl:
        if (len >= 6) {
            uint16_t ev = rd16(d);
            if (ev == 6) send_user_control(7, rd32(d + 2));   // PingRequest -> PingResponse
        }
        break;
    default: break;
    }
}

void Session::on_data_message(const MessageHeader& hdr, const uint8_t* d, size_t len) {
    if (!sink_ || !publishing_) return;
    size_t pos = 0;
    auto first = amf0::decode(d, len, pos);
    if (first && first->is_string() && first->str() == "@setDataFrame") {
        sink_->on_data(hdr.timestamp, d + pos, len - pos);
    } else {
        sink_->on_data(hdr.timestamp, d, len);
    }
}

void Session::on_command(const MessageHeader& hdr, const uint8_t* d, size_t len) {
    auto vals = amf0::decode_all(d, len);
    if (vals.empty() || !vals[0].is_string()) return;
    const std::string& name = vals[0].str();
    double txid = vals.size() > 1 ? vals[1].number() : 0;
    const amf0::Value* cmd = vals.size() > 2 ? &vals[2] : nullptr;
    spdlog::debug("rtmp[{}] command '{}' tx={}", id_, name, txid);

    if (name == "connect") {
        if (cmd) {
            if (auto* a = cmd->get("app")) app_ = a->str();
            if (auto* t = cmd->get("tcUrl")) tc_url_ = t->str();
            if (auto* f = cmd->get("flashVer")) flash_version_ = f->str();
        }
        // strip trailing slash / instance from app
        while (!app_.empty() && app_.back() == '/') app_.pop_back();
        send_window_ack_size(cfg_.window_ack_size);
        send_set_peer_bandwidth(cfg_.window_ack_size, 2);
        send_set_chunk_size(cfg_.out_chunk_size);
        send_user_control(0, 0);   // StreamBegin 0
        send_command(kCommandCsid, 0,
                     {"_result", txid,
                      amf0::Object{{"fmsVer", "FMS/3,5,7,7009"}, {"capabilities", 31.0}, {"mode", 1.0}},
                      amf0::Object{{"level", "status"},
                                   {"code", "NetConnection.Connect.Success"},
                                   {"description", "Connection succeeded."},
                                   {"objectEncoding", 0.0}}});
        connected_ = true;
        spdlog::info("rtmp[{}] connect app='{}' client='{}'", id_, app_, flash_version_);
    } else if (name == "createStream") {
        send_command(kCommandCsid, 0, {"_result", txid, amf0::Null{}, 1.0});
    } else if (name == "releaseStream" || name == "FCPublish" || name == "FCUnpublish") {
        if (name == "FCPublish") send_command(kCommandCsid, 0, {"onFCPublish", 0.0, amf0::Null{},
                                                                 amf0::Object{{"code", "NetStream.Publish.Start"},
                                                                              {"description", "FCPublish"}}});
        else if (name == "FCUnpublish") end_publish("FCUnpublish");
        send_command(kCommandCsid, 0, {"_result", txid, amf0::Null{}, amf0::Undefined{}});
    } else if (name == "publish") {
        std::string stream = vals.size() > 3 ? vals[3].str() : "";
        std::string type = vals.size() > 4 ? vals[4].str() : "live";
        if (publishing_) {
            send_on_status(hdr.stream_id, "error", "NetStream.Publish.BadConnection", "Already publishing");
            return;
        }
        PublishRequest req;
        req.app = app_;
        // Some clients append librtmp-style options ("key live=1") to the stream name.
        if (auto sp = stream.find(' '); sp != std::string::npos) stream.resize(sp);
        auto q = stream.find('?');
        req.stream_name = q == std::string::npos ? stream : stream.substr(0, q);
        req.query = q == std::string::npos ? "" : stream.substr(q + 1);
        req.tc_url = tc_url_;
        req.flash_version = flash_version_;
        req.remote = remote_;
        req.session_id = id_;
        std::weak_ptr<Session> weak = shared_from_this();
        req.close = [weak] { if (auto s = weak.lock()) s->close(); };

        std::string reason;
        auto sink = handler_.on_publish(req, reason);
        if (!sink) {
            spdlog::warn("rtmp[{}] publish '{}' rejected: {}", id_, req.stream_name, reason);
            send_on_status(hdr.stream_id, "error", "NetStream.Publish.BadName", reason.empty() ? "Rejected" : reason);
            closing_ = true;
            return;
        }
        sink_ = std::move(sink);
        publishing_ = true;
        publish_stream_id_ = hdr.stream_id;
        send_user_control(0, hdr.stream_id);   // StreamBegin
        send_on_status(hdr.stream_id, "status", "NetStream.Publish.Start", "Publishing " + req.stream_name);
        spdlog::info("rtmp[{}] publishing '{}/{}' ({}) from {}", id_, app_, req.stream_name, type, remote_);
    } else if (name == "deleteStream" || name == "closeStream") {
        end_publish(name);
    } else if (name == "play") {
        send_on_status(hdr.stream_id, "error", "NetStream.Play.Failed", "This server is ingest-only");
        closing_ = true;
    } else if (name == "_checkbw" || name == "_result" || name == "_error" || name == "onBWDone" ||
               name == "getStreamLength") {
        // ignore
    } else {
        spdlog::debug("rtmp[{}] unhandled command '{}'", id_, name);
    }
}

void Session::end_publish(const std::string& reason) {
    if (!publishing_) return;
    publishing_ = false;
    spdlog::info("rtmp[{}] publish ended: {}", id_, reason);
    if (sink_) {
        sink_->on_end(reason);
        sink_.reset();
    }
    send_on_status(publish_stream_id_, "status", "NetStream.Unpublish.Success", "Stopped");
}

// ---------------------------------------------------------------------------------------------
// outbound

void Session::send_command(uint32_t csid, uint32_t stream_id, std::initializer_list<amf0::Value> values) {
    MessageHeader h;
    h.type = kCommandAmf0;
    h.stream_id = stream_id;
    writer_.write(out_, csid, h, amf0::encode_all(values));
}

void Session::send_control(uint8_t type, const std::string& payload) {
    MessageHeader h;
    h.type = type;
    writer_.write(out_, kControlCsid, h, payload);
}

void Session::send_window_ack_size(uint32_t n) {
    std::string p;
    put32(p, n);
    send_control(kWindowAckSize, p);
}

void Session::send_set_peer_bandwidth(uint32_t n, uint8_t limit_type) {
    std::string p;
    put32(p, n);
    p.push_back(static_cast<char>(limit_type));
    send_control(kSetPeerBandwidth, p);
}

void Session::send_set_chunk_size(uint32_t n) {
    std::string p;
    put32(p, n);
    send_control(kSetChunkSize, p);
    writer_.set_chunk_size(n);
}

void Session::send_user_control(uint16_t event, uint32_t value) {
    std::string p;
    put16(p, event);
    put32(p, value);
    send_control(kUserControl, p);
}

void Session::send_ack(uint32_t sequence) {
    std::string p;
    put32(p, sequence);
    send_control(kAck, p);
}

void Session::send_on_status(uint32_t stream_id, const char* level, const char* code, const std::string& description) {
    send_command(kStreamCsid, stream_id,
                 {"onStatus", 0.0, amf0::Null{},
                  amf0::Object{{"level", level}, {"code", code}, {"description", description}}});
}

} // namespace vsp::rtmp

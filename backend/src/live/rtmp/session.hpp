#pragma once

#include "live/rtmp/chunk.hpp"
#include "live/rtmp/sink.hpp"

#include <atomic>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace vsp::rtmp {

struct ServerConfig {
    uint32_t out_chunk_size = 4096;
    uint32_t window_ack_size = 2500000;
    std::chrono::seconds idle_timeout{30};      // no bytes from the publisher
    std::chrono::seconds handshake_timeout{10};
    size_t read_buffer_size = 256 * 1024;
};

// One RTMP publishing connection. Ingest only: play requests are refused.
class Session : public std::enable_shared_from_this<Session> {
public:
    Session(boost::asio::ip::tcp::socket socket, PublishHandler& handler, const ServerConfig& cfg);
    ~Session();

    boost::asio::awaitable<void> run();
    void close();                                      // thread-safe
    const std::string& id() const { return id_; }
    SessionStats stats() const;

private:
    boost::asio::awaitable<bool> handshake();
    void on_message(const MessageHeader& hdr, MessageBuffer buf);
    MessageBuffer allocate(const MessageHeader& hdr);
    void on_command(const MessageHeader& hdr, const uint8_t* data, size_t len);
    void on_control(const MessageHeader& hdr, const uint8_t* data, size_t len);
    void on_data_message(const MessageHeader& hdr, const uint8_t* data, size_t len);

    void send_command(uint32_t csid, uint32_t stream_id, std::initializer_list<amf0::Value> values);
    void send_control(uint8_t type, const std::string& payload);
    void send_window_ack_size(uint32_t n);
    void send_set_peer_bandwidth(uint32_t n, uint8_t limit_type);
    void send_set_chunk_size(uint32_t n);
    void send_user_control(uint16_t event, uint32_t value);
    void send_ack(uint32_t sequence);
    void send_on_status(uint32_t stream_id, const char* level, const char* code, const std::string& description);

    void end_publish(const std::string& reason);

    boost::asio::ip::tcp::socket socket_;
    PublishHandler& handler_;
    ServerConfig cfg_;
    std::string id_;
    std::string remote_;

    ChunkReader reader_;
    ChunkWriter writer_;
    std::string out_;                     // pending bytes to send after processing a read
    std::vector<uint8_t> in_;
    size_t in_len_ = 0;

    std::string app_, tc_url_, flash_version_;
    bool connected_ = false;
    bool publishing_ = false;
    bool closing_ = false;
    uint32_t publish_stream_id_ = 0;
    std::shared_ptr<MediaSink> sink_;

    uint32_t peer_window_ = 2500000;
    uint64_t bytes_in_ = 0;
    uint64_t last_ack_at_ = 0;

    mutable std::atomic<uint64_t> stat_bytes_{0};
    std::atomic<uint64_t> stat_msgs_{0};
    std::atomic<uint32_t> stat_vts_{0}, stat_ats_{0};
    std::atomic<double> stat_bps_{0};
    std::chrono::steady_clock::time_point rate_t0_;
    uint64_t rate_bytes0_ = 0;
};

} // namespace vsp::rtmp

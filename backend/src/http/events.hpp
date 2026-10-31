#pragma once

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/experimental/concurrent_channel.hpp>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace vsp {

// Fan-out of server-sent events to any number of connected SSE clients. publish() is safe to call
// from worker threads; slow clients simply miss events (bounded per-client queue, never blocks).
class EventBroker {
public:
    using Channel = boost::asio::experimental::concurrent_channel<void(boost::system::error_code, std::string)>;

    explicit EventBroker(boost::asio::any_io_executor ex);

    std::shared_ptr<Channel> subscribe();
    void unsubscribe(const std::shared_ptr<Channel>& ch);
    void publish(const std::string& event, const nlohmann::json& data);
    void heartbeat();
    size_t subscriber_count() const;

private:
    void broadcast(const std::string& frame);

    boost::asio::any_io_executor ex_;
    mutable std::mutex mu_;
    std::vector<std::shared_ptr<Channel>> subs_;
};

} // namespace vsp

#include "http/events.hpp"

#include <algorithm>

namespace vsp {

EventBroker::EventBroker(boost::asio::any_io_executor ex) : ex_(std::move(ex)) {}

std::shared_ptr<EventBroker::Channel> EventBroker::subscribe() {
    auto ch = std::make_shared<Channel>(ex_, 256);
    std::lock_guard lk(mu_);
    subs_.push_back(ch);
    return ch;
}

void EventBroker::unsubscribe(const std::shared_ptr<Channel>& ch) {
    std::lock_guard lk(mu_);
    subs_.erase(std::remove(subs_.begin(), subs_.end(), ch), subs_.end());
    ch->close();
}

size_t EventBroker::subscriber_count() const {
    std::lock_guard lk(mu_);
    return subs_.size();
}

void EventBroker::publish(const std::string& event, const nlohmann::json& data) {
    std::string frame = "event: " + event + "\ndata: " + data.dump() + "\n\n";
    broadcast(frame);
}

void EventBroker::heartbeat() { broadcast(": ping\n\n"); }

void EventBroker::broadcast(const std::string& frame) {
    std::vector<std::shared_ptr<Channel>> targets;
    {
        std::lock_guard lk(mu_);
        targets = subs_;
    }
    for (auto& ch : targets) ch->try_send(boost::system::error_code{}, frame);
}

} // namespace vsp

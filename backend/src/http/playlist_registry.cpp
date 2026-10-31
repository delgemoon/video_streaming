#include "http/playlist_registry.hpp"

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

namespace vsp {

namespace asio = boost::asio;

void PlaylistRegistry::publish(const std::filesystem::path& playlist, const PlaylistUpdate& u) {
    std::vector<std::shared_ptr<asio::steady_timer>> wake;
    {
        std::lock_guard lk(mu_);
        auto& e = entries_[playlist.string()];
        e.latest = u;
        wake.swap(e.waiters);
    }
    // Timers are not thread-safe; cancel them on their own executor.
    for (auto& t : wake) asio::post(t->get_executor(), [t] { t->cancel(); });
}

void PlaylistRegistry::remove(const std::filesystem::path& playlist) {
    std::vector<std::shared_ptr<asio::steady_timer>> wake;
    {
        std::lock_guard lk(mu_);
        auto it = entries_.find(playlist.string());
        if (it == entries_.end()) return;
        wake.swap(it->second.waiters);
        entries_.erase(it);
    }
    for (auto& t : wake) asio::post(t->get_executor(), [t] { t->cancel(); });
}

bool PlaylistRegistry::is_live(const std::filesystem::path& playlist) const {
    std::lock_guard lk(mu_);
    return entries_.contains(playlist.string());
}

bool PlaylistRegistry::satisfied(const PlaylistUpdate& u, uint64_t msn, int part) {
    if (u.final) return true;
    if (u.last_segment > msn) return true;
    if (u.last_segment < msn) return false;
    // same segment: need the requested part, or (no part given) at least one part published
    return part < 0 ? true : u.last_part >= static_cast<unsigned>(part);
}

asio::awaitable<void> PlaylistRegistry::wait_for(const std::filesystem::path& playlist, uint64_t msn, int part,
                                                 std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    const std::string key = playlist.string();
    auto ex = co_await asio::this_coro::executor;
    for (;;) {
        std::shared_ptr<asio::steady_timer> timer;
        {
            std::lock_guard lk(mu_);
            auto it = entries_.find(key);
            if (it == entries_.end()) co_return;               // not a live playlist
            if (satisfied(it->second.latest, msn, part)) co_return;
            auto now = std::chrono::steady_clock::now();
            if (now >= deadline) co_return;                    // give up: serve what we have
            timer = std::make_shared<asio::steady_timer>(ex, deadline);
            it->second.waiters.push_back(timer);
        }
        auto [ec] = co_await timer->async_wait(asio::as_tuple(asio::use_awaitable));
        if (!ec) co_return;                                    // deadline reached
        // cancelled => playlist updated, re-check
    }
}

} // namespace vsp

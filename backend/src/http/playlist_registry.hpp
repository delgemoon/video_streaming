#pragma once

// LL-HLS blocking playlist reload (RFC 8216bis §6.2.5.2). Players ask for
// index.m3u8?_HLS_msn=N&_HLS_part=M and the server holds the response until that segment/part
// exists. Packagers publish their state here; HTTP handlers await it. Lock-free on the serving path
// except for a short mutex hold; wake-ups are delivered by cancelling per-waiter Asio timers.

#include "media/cmaf_packager.hpp"

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/steady_timer.hpp>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace vsp {

class PlaylistRegistry {
public:
    // Called from packager threads after each playlist write.
    void publish(const std::filesystem::path& playlist, const PlaylistUpdate& u);
    // Forget a playlist (stream ended and was removed, or became plain VOD).
    void remove(const std::filesystem::path& playlist);
    bool is_live(const std::filesystem::path& playlist) const;

    // Waits until segment `msn` (and part `part`, if >= 0) is present or `timeout` elapses.
    // Returns immediately for playlists that are not registered (plain VOD files).
    boost::asio::awaitable<void> wait_for(const std::filesystem::path& playlist, uint64_t msn, int part,
                                          std::chrono::milliseconds timeout);

private:
    struct Entry {
        PlaylistUpdate latest;
        std::vector<std::shared_ptr<boost::asio::steady_timer>> waiters;
    };
    static bool satisfied(const PlaylistUpdate& u, uint64_t msn, int part);

    mutable std::mutex mu_;
    std::unordered_map<std::string, Entry> entries_;
};

} // namespace vsp

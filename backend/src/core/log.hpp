#pragma once

#include <spdlog/spdlog.h>

namespace vsp::log {

inline void init(const std::string& level) {
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%t] %v");
    spdlog::set_level(spdlog::level::from_str(level));
    spdlog::flush_on(spdlog::level::warn);
}

} // namespace vsp::log

#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>

namespace vsp {

// Extracts a JPEG poster frame at `position_ms`, scaled to `width` pixels wide.
std::expected<void, std::string> generate_thumbnail(const std::filesystem::path& source,
                                                    const std::filesystem::path& out_jpeg, int64_t position_ms,
                                                    int width = 640);

} // namespace vsp

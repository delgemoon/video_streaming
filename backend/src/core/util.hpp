#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace vsp::util {

std::string generate_id();                 // 32 hex chars, cryptographically random
int64_t now_ms();
std::string iso8601(int64_t unix_ms);
std::string sanitize_filename(std::string_view name);
std::string file_extension_lower(std::string_view name);
std::string mime_for_path(const std::filesystem::path& p);
bool is_safe_relative_path(std::string_view rel);   // no "..", no absolute, no backslashes
std::string url_decode(std::string_view in);
std::string format_duration(uint64_t ms);
std::string to_lower(std::string s);
bool iequals(std::string_view a, std::string_view b);
std::string trim(std::string_view s);

} // namespace vsp::util

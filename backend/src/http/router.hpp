#pragma once

#include <boost/beast/http.hpp>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <nlohmann/json.hpp>
#include <regex>
#include <string>
#include <vector>

namespace vsp {

namespace http = boost::beast::http;

struct RequestContext {
    const http::request_header<>& header;
    std::string path;                          // decoded, without query
    std::map<std::string, std::string> params; // path params
    std::map<std::string, std::string> query;
    std::string body;                          // only for JSON handlers
    std::string remote;

    std::string query_or(const std::string& k, const std::string& def = "") const {
        auto it = query.find(k);
        return it == query.end() ? def : it->second;
    }
};

struct JsonResponse {
    http::status status = http::status::ok;
    nlohmann::json body;

    static JsonResponse ok(nlohmann::json b) { return {http::status::ok, std::move(b)}; }
    static JsonResponse created(nlohmann::json b) { return {http::status::created, std::move(b)}; }
    static JsonResponse error(http::status s, const std::string& msg) {
        return {s, nlohmann::json{{"error", msg}}};
    }
};

// Streaming upload: the handler validates the request and returns a sink; the server pumps the
// body into `write` chunk by chunk and calls `finish` with the total byte count.
struct UploadSink {
    std::function<bool(const char* data, size_t len)> write;
    std::function<JsonResponse(uint64_t total_bytes)> finish;
    std::function<void()> abort;
    uint64_t max_bytes = 0;   // 0 = unlimited
};

enum class HandlerKind { Json, Upload, Static, Sse };

using JsonHandler = std::function<JsonResponse(const RequestContext&)>;
using UploadHandler = std::function<std::expected<UploadSink, JsonResponse>(const RequestContext&)>;
using StaticHandler = std::function<std::expected<std::filesystem::path, JsonResponse>(const RequestContext&)>;

struct Route {
    http::verb method;
    std::regex pattern;
    std::vector<std::string> param_names;
    HandlerKind kind;
    JsonHandler json;
    UploadHandler upload;
    StaticHandler static_file;
};

struct RouteMatch {
    const Route* route = nullptr;
    std::map<std::string, std::string> params;
    bool path_matched = false;   // path matched but method didn't -> 405
};

class Router {
public:
    void add_json(http::verb m, const std::string& pattern, JsonHandler h);
    void add_upload(http::verb m, const std::string& pattern, UploadHandler h);
    void add_static(const std::string& pattern, StaticHandler h);
    void add_sse(const std::string& pattern);

    RouteMatch match(http::verb m, const std::string& path) const;

    // "/api/videos/{id}/source" -> regex with named captures
    static std::pair<std::regex, std::vector<std::string>> compile(const std::string& pattern);
    static std::map<std::string, std::string> parse_query(std::string_view q);

private:
    void add(Route r, const std::string& pattern);
    std::vector<Route> routes_;
};

} // namespace vsp

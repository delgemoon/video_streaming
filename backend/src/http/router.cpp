#include "http/router.hpp"

#include "core/util.hpp"

namespace vsp {

std::pair<std::regex, std::vector<std::string>> Router::compile(const std::string& pattern) {
    std::string re = "^";
    std::vector<std::string> names;
    for (size_t i = 0; i < pattern.size(); ++i) {
        char c = pattern[i];
        if (c == '{') {
            auto end = pattern.find('}', i);
            std::string name = pattern.substr(i + 1, end - i - 1);
            if (name.ends_with("...")) {
                names.push_back(name.substr(0, name.size() - 3));
                re += "(.+)";
            } else {
                names.push_back(name);
                re += "([^/]+)";
            }
            i = end;
        } else if (std::string(".^$|()[]*+?\\").find(c) != std::string::npos) {
            re += '\\';
            re += c;
        } else {
            re += c;
        }
    }
    re += "/?$";
    return {std::regex(re), names};
}

std::map<std::string, std::string> Router::parse_query(std::string_view q) {
    std::map<std::string, std::string> out;
    size_t start = 0;
    while (start < q.size()) {
        auto end = q.find('&', start);
        auto kv = q.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
        auto eq = kv.find('=');
        if (eq == std::string_view::npos) out[util::url_decode(kv)] = "";
        else out[util::url_decode(kv.substr(0, eq))] = util::url_decode(kv.substr(eq + 1));
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return out;
}

void Router::add(Route r, const std::string& pattern) {
    auto [re, names] = compile(pattern);
    r.pattern = std::move(re);
    r.param_names = std::move(names);
    routes_.push_back(std::move(r));
}

void Router::add_json(http::verb m, const std::string& pattern, JsonHandler h) {
    Route r;
    r.method = m;
    r.kind = HandlerKind::Json;
    r.json = std::move(h);
    add(std::move(r), pattern);
}

void Router::add_upload(http::verb m, const std::string& pattern, UploadHandler h) {
    Route r;
    r.method = m;
    r.kind = HandlerKind::Upload;
    r.upload = std::move(h);
    add(std::move(r), pattern);
}

void Router::add_static(const std::string& pattern, StaticHandler h) {
    Route r;
    r.method = http::verb::get;
    r.kind = HandlerKind::Static;
    r.static_file = std::move(h);
    add(std::move(r), pattern);
}

void Router::add_sse(const std::string& pattern) {
    Route r;
    r.method = http::verb::get;
    r.kind = HandlerKind::Sse;
    add(std::move(r), pattern);
}

RouteMatch Router::match(http::verb m, const std::string& path) const {
    RouteMatch result;
    http::verb effective = m == http::verb::head ? http::verb::get : m;
    for (const auto& r : routes_) {
        std::smatch sm;
        if (!std::regex_match(path, sm, r.pattern)) continue;
        result.path_matched = true;
        if (r.method != effective) continue;
        result.route = &r;
        for (size_t i = 0; i < r.param_names.size() && i + 1 < sm.size(); ++i)
            result.params[r.param_names[i]] = util::url_decode(sm[i + 1].str());
        return result;
    }
    return result;
}

} // namespace vsp

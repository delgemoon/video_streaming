#pragma once

#include "storage/models.hpp"

#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

struct sqlite3;

namespace vsp {

// Thin, mutex-serialized SQLite repository. Hot paths (segment serving) never touch the DB,
// so a single serialized connection is more than enough here.
class Database {
public:
    explicit Database(const std::filesystem::path& path);
    ~Database();
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    void insert_video(const Video& v);
    void update_video(const Video& v);
    std::optional<Video> get_video(const std::string& id);
    std::vector<Video> list_videos(std::optional<VideoStatus> status = std::nullopt, int limit = 200);
    bool delete_video(const std::string& id);

    void insert_job(const Job& j);
    void update_job(const Job& j);
    std::optional<Job> get_job(const std::string& id);
    std::vector<Job> list_jobs(int limit = 200);
    std::vector<Job> unfinished_jobs();

    void insert_live(const LiveStream& s);
    void update_live(const LiveStream& s);
    std::optional<LiveStream> get_live(const std::string& id);
    std::optional<LiveStream> get_live_by_key(const std::string& key);
    std::vector<LiveStream> list_live();
    bool delete_live(const std::string& id);

private:
    void exec(const char* sql);
    void migrate();

    sqlite3* db_ = nullptr;
    std::mutex mu_;
};

} // namespace vsp

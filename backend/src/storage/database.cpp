#include "storage/database.hpp"

#include "core/util.hpp"

#include <spdlog/spdlog.h>
#include <sqlite3.h>
#include <stdexcept>

namespace vsp {

// ---------- model JSON ----------

nlohmann::json TranscodeOptions::to_json() const {
    return {{"trim_start_ms", trim_start_ms},
            {"trim_end_ms", trim_end_ms},
            {"renditions", renditions},
            {"audio_bitrate_kbps", audio_bitrate_kbps},
            {"keep_source", keep_source}};
}

TranscodeOptions TranscodeOptions::from_json(const nlohmann::json& j) {
    TranscodeOptions o;
    if (!j.is_object()) return o;
    o.trim_start_ms = j.value("trim_start_ms", int64_t{0});
    o.trim_end_ms = j.value("trim_end_ms", int64_t{0});
    if (j.contains("renditions") && j["renditions"].is_array())
        for (const auto& r : j["renditions"]) if (r.is_string()) o.renditions.push_back(r.get<std::string>());
    o.audio_bitrate_kbps = j.value("audio_bitrate_kbps", 0u);
    o.keep_source = j.value("keep_source", true);
    if (o.trim_start_ms < 0) o.trim_start_ms = 0;
    if (o.trim_end_ms < 0) o.trim_end_ms = 0;
    return o;
}

nlohmann::json Rendition::to_json() const {
    return {{"name", name},   {"kind", kind},
            {"width", width}, {"height", height},
            {"bitrate_kbps", bitrate_kbps},
            {"measured_bandwidth", measured_bandwidth},
            {"measured_avg_bandwidth", measured_avg_bandwidth},
            {"codecs", codecs}, {"path", path}};
}

Rendition Rendition::from_json(const nlohmann::json& j) {
    Rendition r;
    r.name = j.value("name", "");
    r.kind = j.value("kind", "video");
    r.width = j.value("width", 0);
    r.height = j.value("height", 0);
    r.bitrate_kbps = j.value("bitrate_kbps", 0u);
    r.measured_bandwidth = j.value("measured_bandwidth", 0u);
    r.measured_avg_bandwidth = j.value("measured_avg_bandwidth", 0u);
    r.codecs = j.value("codecs", "");
    r.path = j.value("path", "");
    return r;
}

std::string to_string(VideoStatus s) {
    switch (s) {
    case VideoStatus::Created: return "created";
    case VideoStatus::Uploading: return "uploading";
    case VideoStatus::Uploaded: return "uploaded";
    case VideoStatus::Queued: return "queued";
    case VideoStatus::Processing: return "processing";
    case VideoStatus::Ready: return "ready";
    case VideoStatus::Failed: return "failed";
    }
    return "unknown";
}

VideoStatus video_status_from_string(const std::string& s) {
    if (s == "uploading") return VideoStatus::Uploading;
    if (s == "uploaded") return VideoStatus::Uploaded;
    if (s == "queued") return VideoStatus::Queued;
    if (s == "processing") return VideoStatus::Processing;
    if (s == "ready") return VideoStatus::Ready;
    if (s == "failed") return VideoStatus::Failed;
    return VideoStatus::Created;
}

nlohmann::json Video::to_json() const {
    nlohmann::json rends = nlohmann::json::array();
    for (const auto& r : renditions) rends.push_back(r.to_json());
    return {{"id", id},
            {"title", title},
            {"description", description},
            {"original_filename", original_filename},
            {"size_bytes", size_bytes},
            {"status", to_string(status)},
            {"duration_ms", duration_ms},
            {"width", width},
            {"height", height},
            {"fps", fps},
            {"video_codec", video_codec},
            {"audio_codec", audio_codec},
            {"options", options.to_json()},
            {"renditions", rends},
            {"has_thumbnail", has_thumbnail},
            {"encoder_used", encoder_used},
            {"error", error},
            {"progress", progress},
            {"playback_url", status == VideoStatus::Ready || status == VideoStatus::Processing
                                 ? "/media/" + id + "/master.m3u8"
                                 : ""},
            {"thumbnail_url", has_thumbnail ? "/media/" + id + "/thumbnail.jpg" : ""},
            {"created_at", util::iso8601(created_at)},
            {"updated_at", util::iso8601(updated_at)}};
}

std::string to_string(JobStatus s) {
    switch (s) {
    case JobStatus::Queued: return "queued";
    case JobStatus::Running: return "running";
    case JobStatus::Done: return "done";
    case JobStatus::Failed: return "failed";
    case JobStatus::Cancelled: return "cancelled";
    }
    return "unknown";
}

JobStatus job_status_from_string(const std::string& s) {
    if (s == "running") return JobStatus::Running;
    if (s == "done") return JobStatus::Done;
    if (s == "failed") return JobStatus::Failed;
    if (s == "cancelled") return JobStatus::Cancelled;
    return JobStatus::Queued;
}

nlohmann::json Job::to_json() const {
    return {{"id", id},
            {"video_id", video_id},
            {"type", type},
            {"status", to_string(status)},
            {"progress", progress},
            {"stage", stage},
            {"fps", fps},
            {"encoder", encoder},
            {"error", error},
            {"created_at", util::iso8601(created_at)},
            {"started_at", started_at ? util::iso8601(started_at) : ""},
            {"finished_at", finished_at ? util::iso8601(finished_at) : ""}};
}

// ---------- sqlite helpers ----------

namespace {

struct Stmt {
    sqlite3_stmt* s = nullptr;
    Stmt(sqlite3* db, const char* sql) {
        if (sqlite3_prepare_v2(db, sql, -1, &s, nullptr) != SQLITE_OK)
            throw std::runtime_error(std::string("sqlite prepare: ") + sqlite3_errmsg(db));
    }
    ~Stmt() { sqlite3_finalize(s); }
    void bind(int i, const std::string& v) { sqlite3_bind_text(s, i, v.c_str(), -1, SQLITE_TRANSIENT); }
    void bind(int i, int64_t v) { sqlite3_bind_int64(s, i, v); }
    void bind(int i, double v) { sqlite3_bind_double(s, i, v); }
    bool step() {
        int rc = sqlite3_step(s);
        if (rc == SQLITE_ROW) return true;
        if (rc == SQLITE_DONE) return false;
        throw std::runtime_error(std::string("sqlite step: ") + sqlite3_errstr(rc));
    }
    std::string text(int i) {
        const unsigned char* t = sqlite3_column_text(s, i);
        return t ? reinterpret_cast<const char*>(t) : "";
    }
    int64_t i64(int i) { return sqlite3_column_int64(s, i); }
    double dbl(int i) { return sqlite3_column_double(s, i); }
};

constexpr const char* kVideoCols =
    "id,title,description,original_filename,source_path,size_bytes,status,duration_ms,width,height,fps,"
    "video_codec,audio_codec,options_json,renditions_json,has_thumbnail,encoder_used,error,progress,"
    "created_at,updated_at";

Video read_video(Stmt& st) {
    Video v;
    int c = 0;
    v.id = st.text(c++);
    v.title = st.text(c++);
    v.description = st.text(c++);
    v.original_filename = st.text(c++);
    v.source_path = st.text(c++);
    v.size_bytes = st.i64(c++);
    v.status = video_status_from_string(st.text(c++));
    v.duration_ms = st.i64(c++);
    v.width = static_cast<int>(st.i64(c++));
    v.height = static_cast<int>(st.i64(c++));
    v.fps = st.dbl(c++);
    v.video_codec = st.text(c++);
    v.audio_codec = st.text(c++);
    v.options = TranscodeOptions::from_json(nlohmann::json::parse(st.text(c++), nullptr, false));
    auto rj = nlohmann::json::parse(st.text(c++), nullptr, false);
    if (rj.is_array()) for (const auto& r : rj) v.renditions.push_back(Rendition::from_json(r));
    v.has_thumbnail = st.i64(c++) != 0;
    v.encoder_used = st.text(c++);
    v.error = st.text(c++);
    v.progress = st.dbl(c++);
    v.created_at = st.i64(c++);
    v.updated_at = st.i64(c++);
    return v;
}

void bind_video(Stmt& st, const Video& v, int start) {
    nlohmann::json rends = nlohmann::json::array();
    for (const auto& r : v.renditions) rends.push_back(r.to_json());
    int i = start;
    st.bind(i++, v.title);
    st.bind(i++, v.description);
    st.bind(i++, v.original_filename);
    st.bind(i++, v.source_path);
    st.bind(i++, v.size_bytes);
    st.bind(i++, to_string(v.status));
    st.bind(i++, v.duration_ms);
    st.bind(i++, int64_t{v.width});
    st.bind(i++, int64_t{v.height});
    st.bind(i++, v.fps);
    st.bind(i++, v.video_codec);
    st.bind(i++, v.audio_codec);
    st.bind(i++, v.options.to_json().dump());
    st.bind(i++, rends.dump());
    st.bind(i++, int64_t{v.has_thumbnail ? 1 : 0});
    st.bind(i++, v.encoder_used);
    st.bind(i++, v.error);
    st.bind(i++, v.progress);
    st.bind(i++, v.created_at);
    st.bind(i++, v.updated_at);
}

constexpr const char* kJobCols =
    "id,video_id,type,status,progress,stage,fps,encoder,error,created_at,started_at,finished_at";

Job read_job(Stmt& st) {
    Job j;
    int c = 0;
    j.id = st.text(c++);
    j.video_id = st.text(c++);
    j.type = st.text(c++);
    j.status = job_status_from_string(st.text(c++));
    j.progress = st.dbl(c++);
    j.stage = st.text(c++);
    j.fps = st.dbl(c++);
    j.encoder = st.text(c++);
    j.error = st.text(c++);
    j.created_at = st.i64(c++);
    j.started_at = st.i64(c++);
    j.finished_at = st.i64(c++);
    return j;
}

} // namespace

Database::Database(const std::filesystem::path& path) {
    std::filesystem::create_directories(path.parent_path());
    if (sqlite3_open_v2(path.c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                        nullptr) != SQLITE_OK)
        throw std::runtime_error(std::string("sqlite open: ") + sqlite3_errmsg(db_));
    exec("PRAGMA journal_mode=WAL;");
    exec("PRAGMA synchronous=NORMAL;");
    exec("PRAGMA busy_timeout=5000;");
    migrate();
    spdlog::info("database opened at {}", path.string());
}

Database::~Database() {
    if (db_) sqlite3_close(db_);
}

void Database::exec(const char* sql) {
    char* err = nullptr;
    if (sqlite3_exec(db_, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        std::string msg = err ? err : "unknown";
        sqlite3_free(err);
        throw std::runtime_error("sqlite exec: " + msg);
    }
}

void Database::migrate() {
    exec(R"sql(
CREATE TABLE IF NOT EXISTS videos (
  id TEXT PRIMARY KEY,
  title TEXT NOT NULL,
  description TEXT NOT NULL DEFAULT '',
  original_filename TEXT NOT NULL DEFAULT '',
  source_path TEXT NOT NULL DEFAULT '',
  size_bytes INTEGER NOT NULL DEFAULT 0,
  status TEXT NOT NULL,
  duration_ms INTEGER NOT NULL DEFAULT 0,
  width INTEGER NOT NULL DEFAULT 0,
  height INTEGER NOT NULL DEFAULT 0,
  fps REAL NOT NULL DEFAULT 0,
  video_codec TEXT NOT NULL DEFAULT '',
  audio_codec TEXT NOT NULL DEFAULT '',
  options_json TEXT NOT NULL DEFAULT '{}',
  renditions_json TEXT NOT NULL DEFAULT '[]',
  has_thumbnail INTEGER NOT NULL DEFAULT 0,
  encoder_used TEXT NOT NULL DEFAULT '',
  error TEXT NOT NULL DEFAULT '',
  progress REAL NOT NULL DEFAULT 0,
  created_at INTEGER NOT NULL,
  updated_at INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_videos_created ON videos(created_at DESC);
CREATE TABLE IF NOT EXISTS jobs (
  id TEXT PRIMARY KEY,
  video_id TEXT NOT NULL,
  type TEXT NOT NULL,
  status TEXT NOT NULL,
  progress REAL NOT NULL DEFAULT 0,
  stage TEXT NOT NULL DEFAULT '',
  fps REAL NOT NULL DEFAULT 0,
  encoder TEXT NOT NULL DEFAULT '',
  error TEXT NOT NULL DEFAULT '',
  created_at INTEGER NOT NULL,
  started_at INTEGER NOT NULL DEFAULT 0,
  finished_at INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX IF NOT EXISTS idx_jobs_video ON jobs(video_id);
CREATE INDEX IF NOT EXISTS idx_jobs_created ON jobs(created_at DESC);
)sql");
}

void Database::insert_video(const Video& v) {
    std::lock_guard lk(mu_);
    Stmt st(db_, "INSERT INTO videos (id,title,description,original_filename,source_path,size_bytes,status,"
                 "duration_ms,width,height,fps,video_codec,audio_codec,options_json,renditions_json,"
                 "has_thumbnail,encoder_used,error,progress,created_at,updated_at) VALUES "
                 "(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
    st.bind(1, v.id);
    bind_video(st, v, 2);
    st.step();
}

void Database::update_video(const Video& v) {
    std::lock_guard lk(mu_);
    Stmt st(db_, "UPDATE videos SET title=?,description=?,original_filename=?,source_path=?,size_bytes=?,"
                 "status=?,duration_ms=?,width=?,height=?,fps=?,video_codec=?,audio_codec=?,options_json=?,"
                 "renditions_json=?,has_thumbnail=?,encoder_used=?,error=?,progress=?,created_at=?,"
                 "updated_at=? WHERE id=?");
    bind_video(st, v, 1);
    st.bind(21, v.id);
    st.step();
}

std::optional<Video> Database::get_video(const std::string& id) {
    std::lock_guard lk(mu_);
    std::string sql = std::string("SELECT ") + kVideoCols + " FROM videos WHERE id=?";
    Stmt st(db_, sql.c_str());
    st.bind(1, id);
    if (!st.step()) return std::nullopt;
    return read_video(st);
}

std::vector<Video> Database::list_videos(std::optional<VideoStatus> status, int limit) {
    std::lock_guard lk(mu_);
    std::string sql = std::string("SELECT ") + kVideoCols + " FROM videos";
    if (status) sql += " WHERE status=?";
    sql += " ORDER BY created_at DESC LIMIT ?";
    Stmt st(db_, sql.c_str());
    int i = 1;
    if (status) st.bind(i++, to_string(*status));
    st.bind(i, int64_t{limit});
    std::vector<Video> out;
    while (st.step()) out.push_back(read_video(st));
    return out;
}

bool Database::delete_video(const std::string& id) {
    std::lock_guard lk(mu_);
    {
        Stmt st(db_, "DELETE FROM jobs WHERE video_id=?");
        st.bind(1, id);
        st.step();
    }
    Stmt st(db_, "DELETE FROM videos WHERE id=?");
    st.bind(1, id);
    st.step();
    return sqlite3_changes(db_) > 0;
}

void Database::insert_job(const Job& j) {
    std::lock_guard lk(mu_);
    Stmt st(db_, "INSERT INTO jobs (id,video_id,type,status,progress,stage,fps,encoder,error,created_at,"
                 "started_at,finished_at) VALUES (?,?,?,?,?,?,?,?,?,?,?,?)");
    st.bind(1, j.id);
    st.bind(2, j.video_id);
    st.bind(3, j.type);
    st.bind(4, to_string(j.status));
    st.bind(5, j.progress);
    st.bind(6, j.stage);
    st.bind(7, j.fps);
    st.bind(8, j.encoder);
    st.bind(9, j.error);
    st.bind(10, j.created_at);
    st.bind(11, j.started_at);
    st.bind(12, j.finished_at);
    st.step();
}

void Database::update_job(const Job& j) {
    std::lock_guard lk(mu_);
    Stmt st(db_, "UPDATE jobs SET status=?,progress=?,stage=?,fps=?,encoder=?,error=?,started_at=?,"
                 "finished_at=? WHERE id=?");
    st.bind(1, to_string(j.status));
    st.bind(2, j.progress);
    st.bind(3, j.stage);
    st.bind(4, j.fps);
    st.bind(5, j.encoder);
    st.bind(6, j.error);
    st.bind(7, j.started_at);
    st.bind(8, j.finished_at);
    st.bind(9, j.id);
    st.step();
}

std::optional<Job> Database::get_job(const std::string& id) {
    std::lock_guard lk(mu_);
    std::string sql = std::string("SELECT ") + kJobCols + " FROM jobs WHERE id=?";
    Stmt st(db_, sql.c_str());
    st.bind(1, id);
    if (!st.step()) return std::nullopt;
    return read_job(st);
}

std::vector<Job> Database::list_jobs(int limit) {
    std::lock_guard lk(mu_);
    std::string sql = std::string("SELECT ") + kJobCols + " FROM jobs ORDER BY created_at DESC LIMIT ?";
    Stmt st(db_, sql.c_str());
    st.bind(1, int64_t{limit});
    std::vector<Job> out;
    while (st.step()) out.push_back(read_job(st));
    return out;
}

std::vector<Job> Database::unfinished_jobs() {
    std::lock_guard lk(mu_);
    std::string sql = std::string("SELECT ") + kJobCols +
                      " FROM jobs WHERE status IN ('queued','running') ORDER BY created_at ASC";
    Stmt st(db_, sql.c_str());
    std::vector<Job> out;
    while (st.step()) out.push_back(read_job(st));
    return out;
}

} // namespace vsp

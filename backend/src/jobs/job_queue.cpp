#include "jobs/job_queue.hpp"

#include "core/util.hpp"
#include "media/ladder.hpp"
#include "media/probe.hpp"
#include "media/thumbnail.hpp"
#include "media/transcoder.hpp"
#include "observability/tracing.hpp"
#include "observability/vsp_metrics.hpp"

#include <chrono>
#include <spdlog/spdlog.h>

namespace vsp {

JobQueue::JobQueue(const Config& cfg, Database& db, EventBroker& broker, const EncoderDetection& enc)
    : cfg_(cfg), db_(db), broker_(broker), enc_(enc) {}

JobQueue::~JobQueue() { stop(); }

void JobQueue::start(unsigned workers) {
    stopping_ = false;
    for (unsigned i = 0; i < workers; ++i) workers_.emplace_back([this] { worker_loop(); });
    spdlog::info("job queue started with {} worker(s)", workers);
}

void JobQueue::stop() {
    if (stopping_.exchange(true)) return;
    {
        std::lock_guard lk(mu_);
        for (auto& [id, flag] : cancel_flags_) flag->store(true);
    }
    cv_.notify_all();
    for (auto& t : workers_) if (t.joinable()) t.join();
    workers_.clear();
}

size_t JobQueue::pending() const {
    std::lock_guard lk(mu_);
    return queue_.size();
}

size_t JobQueue::running() const { return running_.load(); }

Job JobQueue::enqueue_transcode(const std::string& video_id) {
    Job job;
    job.id = util::generate_id();
    job.video_id = video_id;
    job.type = "transcode";
    job.status = JobStatus::Queued;
    job.stage = "queued";
    job.encoder = enc_.video.element;
    job.created_at = util::now_ms();
    db_.insert_job(job);

    if (auto v = db_.get_video(video_id)) {
        v->status = VideoStatus::Queued;
        v->progress = 0;
        v->error.clear();
        v->updated_at = util::now_ms();
        db_.update_video(*v);
        publish_video(*v);
    }
    {
        std::lock_guard lk(mu_);
        queue_.push_back(job.id);
        cancel_flags_[job.id] = std::make_shared<std::atomic<bool>>(false);
        metrics::M().jobs_queued->get()->set(static_cast<double>(queue_.size()));
    }
    cv_.notify_one();
    broker_.publish("job.updated", job.to_json());
    return job;
}

bool JobQueue::cancel(const std::string& job_id) {
    std::lock_guard lk(mu_);
    auto it = cancel_flags_.find(job_id);
    if (it == cancel_flags_.end()) return false;
    it->second->store(true);
    auto q = std::find(queue_.begin(), queue_.end(), job_id);
    if (q != queue_.end()) {
        queue_.erase(q);
        metrics::M().jobs_queued->get()->set(static_cast<double>(queue_.size()));
        metrics::M().jobs_total->with({"cancelled"})->inc();
        if (auto job = db_.get_job(job_id)) {
            job->status = JobStatus::Cancelled;
            job->finished_at = util::now_ms();
            db_.update_job(*job);
            broker_.publish("job.updated", job->to_json());
            if (auto v = db_.get_video(job->video_id)) {
                v->status = VideoStatus::Uploaded;
                v->updated_at = util::now_ms();
                db_.update_video(*v);
                publish_video(*v);
            }
        }
    }
    return true;
}

void JobQueue::recover_unfinished() {
    auto jobs = db_.unfinished_jobs();
    for (auto& job : jobs) {
        auto v = db_.get_video(job.video_id);
        if (!v || v->source_path.empty() || !std::filesystem::exists(v->source_path)) {
            job.status = JobStatus::Failed;
            job.error = "source missing after restart";
            job.finished_at = util::now_ms();
            db_.update_job(job);
            if (v) {
                v->status = VideoStatus::Failed;
                v->error = job.error;
                db_.update_video(*v);
            }
            continue;
        }
        job.status = JobStatus::Queued;
        job.stage = "queued";
        job.progress = 0;
        db_.update_job(job);
        v->status = VideoStatus::Queued;
        db_.update_video(*v);
        std::lock_guard lk(mu_);
        queue_.push_back(job.id);
        cancel_flags_[job.id] = std::make_shared<std::atomic<bool>>(false);
        spdlog::info("re-queued interrupted job {} for video {}", job.id, job.video_id);
    }
    cv_.notify_all();
}

void JobQueue::publish_video(const Video& v) { broker_.publish("video.updated", v.to_json()); }

void JobQueue::worker_loop() {
    while (!stopping_) {
        std::string id;
        {
            std::unique_lock lk(mu_);
            cv_.wait(lk, [&] { return stopping_ || !queue_.empty(); });
            if (stopping_) return;
            id = std::move(queue_.front());
            queue_.pop_front();
            metrics::M().jobs_queued->get()->set(static_cast<double>(queue_.size()));
        }
        ++running_;
        metrics::M().jobs_running->get()->set(static_cast<double>(running_.load()));
        try {
            run_job(id);
        } catch (const std::exception& e) {
            spdlog::error("job {} crashed: {}", id, e.what());
            if (auto job = db_.get_job(id)) {
                auto v = db_.get_video(job->video_id);
                fail_job(*job, v ? &*v : nullptr, e.what());
            }
        }
        --running_;
        metrics::M().jobs_running->get()->set(static_cast<double>(running_.load()));
        metrics::M().job_encode_fps->remove({id});
        std::lock_guard lk(mu_);
        cancel_flags_.erase(id);
    }
}

void JobQueue::fail_job(Job& job, Video* video, const std::string& error) {
    job.status = error == "cancelled" ? JobStatus::Cancelled : JobStatus::Failed;
    metrics::M().jobs_total->with({job.status == JobStatus::Cancelled ? "cancelled" : "failed"})->inc();
    if (job.status == JobStatus::Failed) {
        // bucket the reason: text before the first ':' keeps cardinality low
        std::string reason = error.substr(0, error.find(':'));
        if (reason.size() > 60) reason.resize(60);
        metrics::M().job_failures_total->with({reason})->inc();
    }
    job.error = error;
    job.finished_at = util::now_ms();
    db_.update_job(job);
    broker_.publish("job.updated", job.to_json());
    if (video) {
        video->status = job.status == JobStatus::Cancelled ? VideoStatus::Uploaded : VideoStatus::Failed;
        video->error = error;
        video->updated_at = util::now_ms();
        db_.update_video(*video);
        publish_video(*video);
    }
    spdlog::error("job {} failed: {}", job.id, error);
}

void JobQueue::run_job(const std::string& job_id) {
    auto job_opt = db_.get_job(job_id);
    if (!job_opt) return;
    Job job = *job_opt;
    auto video_opt = db_.get_video(job.video_id);
    if (!video_opt) {
        fail_job(job, nullptr, "video no longer exists");
        return;
    }
    Video video = *video_opt;
    std::shared_ptr<std::atomic<bool>> cancel;
    {
        std::lock_guard lk(mu_);
        cancel = cancel_flags_[job_id];
        if (!cancel) cancel = cancel_flags_[job_id] = std::make_shared<std::atomic<bool>>(false);
    }

    auto job_span = trace::T().start("transcode.job", trace::SpanKind::Internal);
    job_span->set_attribute("job.id", job.id);
    job_span->set_attribute("video.id", video.id);
    job_span->set_attribute("video.filename", video.original_filename);
    trace::Scope job_scope(job_span);
    metrics::Timer total_timer(metrics::M().job_duration->with({"total"}));

    job.status = JobStatus::Running;
    job.stage = "probing";
    job.started_at = util::now_ms();
    db_.update_job(job);
    broker_.publish("job.updated", job.to_json());
    video.status = VideoStatus::Processing;
    video.progress = 0;
    video.error.clear();
    video.updated_at = util::now_ms();
    db_.update_video(video);
    publish_video(video);
    spdlog::info("job {} started for video {} ({})", job.id, video.id, video.original_filename);

    // ---- probe
    std::expected<ProbeResult, std::string> probe;
    {
        auto sp = trace::T().start_child("transcode.probe", job_span);
        trace::Scope sc(sp);
        metrics::Timer t(metrics::M().job_duration->with({"probe"}));
        probe = probe_file(video.source_path);
        if (!probe) sp->set_status(trace::Status::Error, probe.error());
    }
    if (!probe) {
        job_span->set_status(trace::Status::Error, "probe failed");
        fail_job(job, &video, "probe failed: " + probe.error());
        return;
    }
    job_span->set_attribute("source.width", static_cast<int64_t>(probe->width));
    job_span->set_attribute("source.height", static_cast<int64_t>(probe->height));
    job_span->set_attribute("source.duration_s", probe->duration_ms / 1000.0);
    job_span->set_attribute("source.codec", probe->video_codec);
    job_span->set_attribute("source.container", probe->container);
    metrics::M().job_source_duration->get()->observe(probe->duration_ms / 1000.0);
    metrics::M().job_source_bytes->get()->observe(static_cast<double>(video.size_bytes));
    video.duration_ms = probe->duration_ms;
    video.width = probe->width;
    video.height = probe->height;
    video.fps = probe->fps();
    video.video_codec = probe->video_codec;
    video.audio_codec = probe->audio_codec;
    db_.update_video(video);
    publish_video(video);

    // ---- output dir (keep a client-provided thumbnail)
    const auto out_dir = cfg_.media_dir() / video.id;
    std::error_code ec;
    std::filesystem::create_directories(out_dir, ec);
    for (const auto& entry : std::filesystem::directory_iterator(out_dir, ec)) {
        if (entry.path().filename() == "thumbnail.jpg") continue;
        std::filesystem::remove_all(entry.path(), ec);
    }

    TranscodeRequest req;
    req.source = video.source_path;
    req.out_dir = out_dir;
    req.probe = *probe;
    req.options = video.options;
    req.renditions = build_renditions(*probe, video.options, cfg_.audio_bitrate_kbps);
    req.encoder = enc_.video;
    req.audio_encoder = enc_.audio_element;
    req.segment_ms = cfg_.segment_duration_ms;
    req.part_ms = cfg_.part_duration_ms;

    video.renditions = req.renditions;
    video.encoder_used = enc_.video.element;
    db_.update_video(video);

    job.stage = "transcoding";
    db_.update_job(job);
    broker_.publish("job.updated", job.to_json());

    auto last_db = std::chrono::steady_clock::now();
    bool announced_playable = false;
    auto on_progress = [&](const TranscodeProgress& p) {
        job.progress = p.fraction;
        job.fps = p.fps;
        video.progress = p.fraction;
        metrics::M().job_encode_fps->with({job.id})->set(p.fps);
        broker_.publish("job.progress", {{"job_id", job.id},
                                         {"video_id", video.id},
                                         {"progress", p.fraction},
                                         {"fps", p.fps},
                                         {"position_ms", p.position_ms},
                                         {"playable", p.master_ready}});
        if (p.master_ready && !announced_playable) {
            announced_playable = true;
            publish_video(video);   // status processing + playback_url now resolvable
        }
        auto now = std::chrono::steady_clock::now();
        if (now - last_db > std::chrono::seconds(2)) {
            last_db = now;
            db_.update_job(job);
            video.updated_at = util::now_ms();
            db_.update_video(video);
        }
    };

    std::expected<TranscodeResult, std::string> result;
    {
        auto sp = trace::T().start_child("transcode.encode", job_span);
        sp->set_attribute("encoder", enc_.video.element);
        sp->set_attribute("renditions", static_cast<int64_t>(req.renditions.size()));
        trace::Scope sc(sp);
        metrics::Timer t(metrics::M().job_duration->with({"transcode"}));
        result = run_transcode(req, on_progress, *cancel);
        double wall = t.elapsed();
        int64_t span_ms = (video.options.trim_end_ms > 0 ? video.options.trim_end_ms : probe->duration_ms) -
                          video.options.trim_start_ms;
        if (result && wall > 0 && span_ms > 0) {
            double rtf = span_ms / 1000.0 / wall;
            metrics::M().job_realtime_factor->get()->observe(rtf);
            sp->set_attribute("realtime_factor", rtf);
        }
        if (!result) sp->set_status(trace::Status::Error, result.error());
    }
    if (!result) {
        std::filesystem::remove(out_dir / "master.m3u8", ec);
        job_span->set_status(trace::Status::Error, result.error());
        fail_job(job, &video, result.error());
        return;
    }

    // ---- finalize
    job.stage = "finalizing";
    db_.update_job(job);
    video.renditions = result->renditions;
    video.duration_ms = result->duration_ms > 0 ? result->duration_ms : video.duration_ms;

    if (!std::filesystem::exists(out_dir / "thumbnail.jpg")) {
        auto sp = trace::T().start_child("transcode.thumbnail", job_span);
        trace::Scope sc(sp);
        metrics::Timer t(metrics::M().job_duration->with({"thumbnail"}));
        int64_t start = video.options.trim_start_ms;
        int64_t end = video.options.trim_end_ms > 0 ? video.options.trim_end_ms : probe->duration_ms;
        int64_t pos = start + std::max<int64_t>(0, (end - start) / 10);
        auto th = generate_thumbnail(video.source_path, out_dir / "thumbnail.jpg", pos);
        if (!th) spdlog::warn("thumbnail generation failed for {}: {}", video.id, th.error());
    }
    video.has_thumbnail = std::filesystem::exists(out_dir / "thumbnail.jpg");

    if (!video.options.keep_source && !video.source_path.empty()) {
        std::filesystem::remove(video.source_path, ec);
    }

    video.status = VideoStatus::Ready;
    video.progress = 1.0;
    video.updated_at = util::now_ms();
    db_.update_video(video);
    publish_video(video);

    metrics::M().jobs_total->with({"done"})->inc();
    job_span->set_status(trace::Status::Ok);
    job.status = JobStatus::Done;
    job.progress = 1.0;
    job.stage = "done";
    job.finished_at = util::now_ms();
    db_.update_job(job);
    broker_.publish("job.updated", job.to_json());
    spdlog::info("job {} done: video {} ready ({} renditions)", job.id, video.id, video.renditions.size());
}

} // namespace vsp

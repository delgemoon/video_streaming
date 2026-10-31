#pragma once

#include "core/config.hpp"
#include "http/events.hpp"
#include "media/encoder_detect.hpp"
#include "storage/database.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace vsp {

// Simple in-process worker pool. Each worker runs one GStreamer transcode pipeline at a time
// (one pipeline already fans out to all renditions, so parallelism across jobs is capped low
// for hardware encoders that have session limits).
class JobQueue {
public:
    JobQueue(const Config& cfg, Database& db, EventBroker& broker, const EncoderDetection& enc);
    ~JobQueue();

    void start(unsigned workers);
    void stop();

    Job enqueue_transcode(const std::string& video_id);
    bool cancel(const std::string& job_id);
    void recover_unfinished();   // called at startup to re-queue interrupted jobs
    size_t pending() const;
    size_t running() const;

private:
    void worker_loop();
    void run_job(const std::string& job_id);
    void fail_job(Job& job, Video* video, const std::string& error);
    void publish_video(const Video& v);

    const Config& cfg_;
    Database& db_;
    EventBroker& broker_;
    const EncoderDetection& enc_;

    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::string> queue_;
    std::unordered_map<std::string, std::shared_ptr<std::atomic<bool>>> cancel_flags_;
    std::vector<std::thread> workers_;
    std::atomic<bool> stopping_{false};
    std::atomic<size_t> running_{0};
};

} // namespace vsp

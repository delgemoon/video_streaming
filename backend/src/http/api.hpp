#pragma once

#include "core/config.hpp"
#include "http/events.hpp"
#include "http/router.hpp"
#include "jobs/job_queue.hpp"
#include "live/live_manager.hpp"
#include "media/encoder_detect.hpp"
#include "storage/database.hpp"

namespace vsp {

struct App {
    const Config& cfg;
    Database& db;
    EventBroker& broker;
    JobQueue& jobs;
    const EncoderDetection& encoders;
    LiveManager* live = nullptr;   // null when RTMP is disabled
};

void register_routes(Router& router, App& app);

} // namespace vsp

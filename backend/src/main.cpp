#include "core/config.hpp"
#include "core/log.hpp"
#include "http/api.hpp"
#include "http/events.hpp"
#include "http/router.hpp"
#include "http/server.hpp"
#include "jobs/job_queue.hpp"
#include "media/encoder_detect.hpp"
#include "storage/database.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>
#include <gst/gst.h>
#include <spdlog/spdlog.h>
#include <thread>
#include <vector>

int main(int argc, char** argv) {
    using namespace vsp;
    Config cfg = Config::load(argc, argv);
    log::init(cfg.log_level);
    spdlog::info("vsp-server {} starting (data dir: {})", VSP_VERSION, cfg.data_dir.string());

    gst_init(nullptr, nullptr);
    spdlog::info("GStreamer {}", gst_version_string());

    try {
        std::filesystem::create_directories(cfg.uploads_dir());
        std::filesystem::create_directories(cfg.media_dir());

        EncoderDetection encoders = detect_encoders(cfg.encoder);
        Database db(cfg.db_path());

        const unsigned threads = cfg.effective_http_threads();
        boost::asio::io_context ioc(static_cast<int>(threads));
        EventBroker broker(ioc.get_executor());

        JobQueue jobs(cfg, db, broker, encoders);
        jobs.recover_unfinished();
        jobs.start(cfg.effective_jobs(encoders.video.hardware));

        Router router;
        App app{cfg, db, broker, jobs, encoders};
        register_routes(router, app);

        HttpServer server(cfg, router, broker, ioc);
        server.start();

        boost::asio::signal_set signals(ioc, SIGINT, SIGTERM);
        signals.async_wait([&](const boost::system::error_code&, int sig) {
            spdlog::info("signal {} received, shutting down", sig);
            ioc.stop();
        });

        std::vector<std::thread> pool;
        for (unsigned i = 1; i < threads; ++i) pool.emplace_back([&] { ioc.run(); });
        ioc.run();
        for (auto& t : pool) t.join();

        jobs.stop();
        spdlog::info("bye");
    } catch (const std::exception& e) {
        spdlog::critical("fatal: {}", e.what());
        return 1;
    }
    return 0;
}

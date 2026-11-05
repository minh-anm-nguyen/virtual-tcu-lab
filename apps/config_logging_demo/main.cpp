// Project 04 demonstration: spec section 21, the six scenarios of section 12.
//
// Wiring, in the order spec section 11 requires:
//   1. stderr diagnostics, before anything else exists
//   2. a bootstrap read, so the logger can be built FROM the configuration
//      rather than guessing at it (the restart-only logging keys are only
//      knowable once the file has been validated)
//   3. the real logger, the real configuration service, the worker threads
//   4. shutdown: producers, then the config service, then the scheduler it
//      borrows, then the logger both were writing to

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "tcu/config/configuration_service.hpp"
#include "tcu/eventbus/event_bus.hpp"
#include "tcu/logging/async_logger.hpp"
#include "tcu/logging/console_sink.hpp"
#include "tcu/logging/file_sink.hpp"
#include "tcu/scheduler/timer_scheduler.hpp"

namespace {

using namespace std::chrono_literals;
namespace cfg = tcu::config;
namespace log = tcu::logging;
namespace sch = tcu::scheduler;
namespace bus = tcu::eventbus;

std::atomic<bool> g_stop{false};

extern "C" void onSignal(int) { g_stop.store(true); }

struct Options {
  std::string config_path = "configs/tcu.json";
  std::string log_path = "logs/tcu.log";
  // Scenario E: producers x records per producer.
  int flood_producers = 0;
  int flood_records = 0;
  int duration_seconds = 0;  // 0 means run until SIGINT
  bool ok = true;
};

Options parseArgs(int argc, char** argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    const bool has_value = (i + 1) < argc;
    if (arg == "--config" && has_value) {
      options.config_path = argv[++i];
    } else if (arg == "--log" && has_value) {
      options.log_path = argv[++i];
    } else if (arg == "--duration" && has_value) {
      options.duration_seconds = std::atoi(argv[++i]);
    } else if (arg == "--flood" && has_value) {
      // NxM, as in 8x50000.
      const std::string value = argv[++i];
      const std::size_t x = value.find('x');
      if (x == std::string::npos) {
        options.ok = false;
      } else {
        options.flood_producers = std::atoi(value.substr(0, x).c_str());
        options.flood_records = std::atoi(value.substr(x + 1).c_str());
      }
    } else {
      std::fprintf(stderr, "usage: config_logging_demo [--config PATH] "
                           "[--log PATH] [--flood NxM] [--duration SECONDS]\n");
      options.ok = false;
    }
  }
  return options;
}

// Scenario F and T061: the logger cannot log its own sink failures or drops
// without re-entering itself, so it reports them through metrics and the
// application turns the deltas into the events of contracts/metrics.md section
// 6. One line per drain in which something happened, never one per record.
class LoggerWatch {
 public:
  LoggerWatch(log::AsyncLogger& logger, log::FileSink& file_sink)
      : logger_(logger), file_sink_(file_sink) {}

  void poll() {
    const log::LoggerMetrics m = logger_.metrics();

    const std::uint64_t dropped = m.droppedTotal();
    if (dropped > dropped_) {
      logger_.log(log::Severity::Warn, "logger", "queue_drops_detected", "",
                  {log::f("count", static_cast<std::int64_t>(dropped - dropped_))});
      dropped_ = dropped;
    }

    const std::size_t file = log::sinkIndex(log::SinkKind::File);
    if (m.sinkErrorsTotal[file] > sink_errors_) {
      sink_errors_ = m.sinkErrorsTotal[file];
      logger_.log(log::Severity::Error, "logger", "sink_error", "",
                  {log::f("sink", std::string("file"))});
      file_unhealthy_ = true;
    } else if (file_unhealthy_ && m.sinkHealth[file] == log::SinkHealth::Healthy) {
      file_unhealthy_ = false;
      logger_.log(log::Severity::Info, "logger", "sink_recovered", "",
                  {log::f("sink", std::string("file"))});
    }

    const std::uint64_t rotations = file_sink_.rotations();
    if (rotations > rotations_) {
      rotations_ = rotations;
      logger_.log(log::Severity::Info, "logger", "rotated", "",
                  {log::f("sink", std::string("file"))});
    }
  }

 private:
  log::AsyncLogger& logger_;
  log::FileSink& file_sink_;
  std::uint64_t dropped_ = 0;
  std::uint64_t sink_errors_ = 0;
  std::uint64_t rotations_ = 0;
  bool file_unhealthy_ = false;
};

// The health line of contracts/metrics.md section 5. Flags and a generation
// only: no configuration value appears here.
void logHealth(log::AsyncLogger& logger,
               const cfg::ConfigMetrics& config_metrics,
               const log::LoggerMetrics& logger_metrics,
               std::uint64_t telemetry_generation,
               std::uint64_t connectivity_generation) {
  logger.log(log::Severity::Info, "demo", "health", "",
             {log::f("config_valid", config_metrics.configValid),
              log::f("generation", static_cast<std::int64_t>(config_metrics.generation)),
              log::f("notify_degraded", config_metrics.notifyDegraded),
              log::f("logger_healthy", logger_metrics.allSinksHealthy()),
              log::f("queue_depth", static_cast<std::int64_t>(logger_metrics.queueDepth)),
              log::f("dropped_total", static_cast<std::int64_t>(logger_metrics.droppedTotal())),
              log::f("file_sink", std::string(
                  logger_metrics.sinkHealth[log::sinkIndex(log::SinkKind::File)] ==
                          log::SinkHealth::Healthy
                      ? "healthy"
                      : "unhealthy")),
              log::f("telemetry_generation_applied",
                     static_cast<std::int64_t>(telemetry_generation)),
              log::f("connectivity_generation_applied",
                     static_cast<std::int64_t>(connectivity_generation))});
}

}  // namespace

int main(int argc, char** argv) {
  const Options options = parseArgs(argc, argv);
  if (!options.ok) {
    return 2;
  }

  // Step 1 of scenario A: minimal stderr diagnostics, before any library
  // exists to say it for us.
  std::fprintf(stderr, "config_logging_demo: starting with config %s\n",
               options.config_path.c_str());

  std::signal(SIGINT, onSignal);
  std::signal(SIGTERM, onSignal);

  sch::Config scheduler_config;
  // One worker: ordered configuration notification depends on it
  // (contracts/config_api.md section 2).
  scheduler_config.workerThreads = 1;
  scheduler_config.maxTimers = 256;
  scheduler_config.maxReadyTasks = 256;
  sch::TimerScheduler timers(scheduler_config);
  if (timers.start() != sch::Error::None) {
    std::fprintf(stderr, "config_logging_demo: scheduler failed to start\n");
    return 1;
  }

  // Step 2: a bootstrap read with a console-only logger, so the real logger can
  // be built from validated configuration rather than from a guess. The
  // restart-only logging keys - queue capacity, file size, file count - are
  // only knowable after the file has been read and validated.
  cfg::LoggingConfig logging_config;
  {
    log::LoggerOptions bootstrap_options;
    bootstrap_options.minLevel = log::Severity::Info;
    log::AsyncLogger bootstrap_logger(bootstrap_options,
                                      {std::make_shared<log::ConsoleSink>()});
    bootstrap_logger.start();

    cfg::ServiceOptions bootstrap_service_options;
    bootstrap_service_options.path = options.config_path;
    bootstrap_service_options.pollInterval = 0ms;  // read once, no watching
    cfg::ConfigurationService bootstrap(bootstrap_service_options, timers,
                                        bootstrap_logger);
    const cfg::LoadError error = bootstrap.start();
    if (error != cfg::LoadError::None) {
      std::fprintf(stderr,
                   "config_logging_demo: configuration is not usable (%s); "
                   "refusing to start on defaults\n",
                   std::string(cfg::loadErrorName(error)).c_str());
      bootstrap.stop();
      bootstrap_logger.stop();
      timers.stop(sch::StopMode::Drain);
      return 1;
    }
    logging_config = bootstrap.snapshot()->logging;
    bootstrap.stop();
    bootstrap_logger.stop();
  }

  // Step 3: the real logger, from the file.
  log::FileSinkOptions file_options;
  file_options.path = options.log_path;
  file_options.fileMaxBytes = static_cast<std::size_t>(logging_config.fileMaxBytes);
  file_options.fileCount = static_cast<std::size_t>(logging_config.fileCount);
  auto file_sink = std::make_shared<log::FileSink>(file_options);
  if (!file_sink->open()) {
    // BC-27: an unusable log path does not stop the service. The sink starts
    // unhealthy and falls back to stderr.
    std::fprintf(stderr,
                 "config_logging_demo: log file not writable (errno %d); "
                 "continuing with stderr fallback\n",
                 file_sink->lastError());
  }

  log::LoggerOptions logger_options;
  logger_options.minLevel = logging_config.level;
  logger_options.consoleEnabled = logging_config.consoleEnabled;
  logger_options.queueCapacity = static_cast<std::size_t>(logging_config.queueCapacity);
  log::AsyncLogger logger(logger_options,
                          {std::make_shared<log::ConsoleSink>(), file_sink});
  logger.start();

  bus::BusConfig bus_config;
  bus_config.queue_capacity = 256;
  bus::EventBus event_bus(bus_config);
  event_bus.start();

  cfg::ServiceOptions service_options;
  service_options.path = options.config_path;
  service_options.pollInterval = 500ms;  // research R-03
  cfg::ConfigurationService service(service_options, timers, logger);
  if (service.start() != cfg::LoadError::None) {
    std::fprintf(stderr, "config_logging_demo: configuration became unusable\n");
    logger.stop();
    timers.stop(sch::StopMode::Drain);
    return 1;
  }

  // The two consumers. Each applies what it owns and reports the generation it
  // has applied, which is the distinction spec section 13 draws between a
  // committed generation and an applied one.
  std::atomic<std::uint64_t> telemetry_generation{0};
  std::atomic<std::uint64_t> connectivity_generation{0};
  std::atomic<std::int64_t> telemetry_period_ms{0};
  std::atomic<std::int64_t> retry_base_ms{0};
  sch::TimerId telemetry_timer = 0;

  const auto applyTelemetry = [&](const cfg::ConfigPtr& snapshot) {
    if (snapshot->telemetry.periodMs != telemetry_period_ms.load()) {
      if (telemetry_timer != 0) {
        timers.cancel(telemetry_timer);
      }
      const auto scheduled = timers.scheduleEvery(
          std::chrono::milliseconds(snapshot->telemetry.periodMs),
          sch::PeriodicMode::FixedDelay, [&logger, &telemetry_generation] {
            logger.log(log::Severity::Info, "telemetry", "sample", "",
                       {log::f("generation",
                               static_cast<std::int64_t>(telemetry_generation.load()))});
          });
      telemetry_timer = scheduled ? scheduled.id : 0;
      telemetry_period_ms.store(snapshot->telemetry.periodMs);
      logger.log(log::Severity::Info, "telemetry", "telemetry_rescheduled", "",
                 {log::f("generation", static_cast<std::int64_t>(snapshot->generation)),
                  log::f("period_ms", snapshot->telemetry.periodMs)});
    }
    telemetry_generation.store(snapshot->generation);
  };

  const auto applyConnectivity = [&](const cfg::ConfigPtr& snapshot) {
    retry_base_ms.store(snapshot->connectivity.retryBaseMs);
    connectivity_generation.store(snapshot->generation);
  };

  // The configuration observer: the two hot logger settings, the two modules,
  // and the generation on the bus (R-20 - the generation only, never a value).
  service.subscribe([&](cfg::ConfigPtr current, cfg::ConfigPtr) {
    if (current == nullptr) {
      return;
    }
    logger.setLevel(current->logging.level);
    logger.setConsoleEnabled(current->logging.consoleEnabled);
    applyTelemetry(current);
    applyConnectivity(current);

    bus::Event event;
    event.type = bus::event_type_of<bus::ConfigChanged>();
    event.created_at = std::chrono::steady_clock::now();
    event.payload = bus::ConfigChanged{current->generation};
    event_bus.publish(std::move(event));
  });

  // Startup application: the first snapshot is read, not notified about.
  applyTelemetry(service.snapshot());
  applyConnectivity(service.snapshot());

  LoggerWatch watch(logger, *file_sink);
  logHealth(logger, service.metrics(), logger.metrics(),
            telemetry_generation.load(), connectivity_generation.load());

  // Scenario E: many producers, faster than any disk.
  std::vector<std::thread> flood;
  for (int producer = 0; producer < options.flood_producers; ++producer) {
    const int records = options.flood_records;
    flood.emplace_back([&logger, producer, records] {
      for (int i = 0; i < records; ++i) {
        // Info, not Debug: the shipped level is INFO, and a flood that is
        // filtered out demonstrates nothing about queue pressure.
        logger.log(log::Severity::Info, "flood", "burst", "",
                   {log::f("producer", static_cast<std::int64_t>(producer)),
                    log::f("index", static_cast<std::int64_t>(i))});
      }
    });
  }

  const auto started = std::chrono::steady_clock::now();
  while (!g_stop.load()) {
    std::this_thread::sleep_for(500ms);
    watch.poll();
    logHealth(logger, service.metrics(), logger.metrics(),
              telemetry_generation.load(), connectivity_generation.load());
    if (options.duration_seconds > 0 &&
        std::chrono::steady_clock::now() - started >=
            std::chrono::seconds(options.duration_seconds)) {
      break;
    }
  }

  // Shutdown, in the order of spec section 11: producers first, then the
  // configuration service, then the scheduler it borrows, then the logger.
  for (std::thread& thread : flood) {
    thread.join();
  }
  watch.poll();
  if (telemetry_timer != 0) {
    timers.cancel(telemetry_timer);
  }
  service.stop();
  timers.stop(sch::StopMode::Drain);
  event_bus.stop(bus::StopMode::Drain);

  const log::LoggerMetrics final_metrics = logger.metrics();
  logger.log(log::Severity::Info, "demo", "shutdown_drained", "",
             {log::f("written", static_cast<std::int64_t>(final_metrics.writtenTotal())),
              log::f("dropped", static_cast<std::int64_t>(final_metrics.droppedTotal()))});
  logger.stop();

  std::fprintf(stderr,
               "config_logging_demo: stopped. written=%llu dropped=%llu "
               "not_drained=%llu rotations=%llu\n",
               static_cast<unsigned long long>(final_metrics.writtenTotal()),
               static_cast<unsigned long long>(final_metrics.droppedTotal()),
               static_cast<unsigned long long>(logger.metrics().notDrainedAtStopTotal),
               static_cast<unsigned long long>(file_sink->rotations()));
  return 0;
}

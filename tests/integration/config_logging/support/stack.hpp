#pragma once

// The whole project 04 stack wired the way apps/config_logging_demo wires it:
// a one-worker scheduler, a logger with a recording sink, and the
// configuration service that borrows both. Shared by every integration case so
// the wiring - including the shutdown order of spec section 11 - is stated once.

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "support/recording_sink.hpp"
#include "support/temp_config_dir.hpp"
#include "tcu/config/configuration_service.hpp"
#include "tcu/logging/async_logger.hpp"
#include "tcu/scheduler/timer_scheduler.hpp"

namespace tcu::itest {

struct Stack {
  config::test::TempConfigDir dir;
  std::shared_ptr<logging::test::RecordingSink> sink =
      std::make_shared<logging::test::RecordingSink>(logging::SinkKind::Console);
  std::unique_ptr<scheduler::TimerScheduler> timers;
  std::unique_ptr<logging::AsyncLogger> logger;
  std::unique_ptr<config::ConfigurationService> service;

  explicit Stack(std::chrono::milliseconds poll_interval = std::chrono::milliseconds{0},
                 std::size_t max_pending = 64,
                 const std::string& fixture = "tcu.json") {
    scheduler::Config scheduler_config;
    scheduler_config.workerThreads = 1;  // ordered notification (R-02)
    scheduler_config.maxTimers = max_pending;
    scheduler_config.maxReadyTasks = max_pending;
    timers = std::make_unique<scheduler::TimerScheduler>(scheduler_config);
    timers->start();

    logging::LoggerOptions logger_options;
    logger_options.minLevel = logging::Severity::Debug;
    logger = std::make_unique<logging::AsyncLogger>(
        logger_options, std::vector<std::shared_ptr<logging::ILogSink>>{sink});
    logger->start();

    dir.writeAtomic(config::test::TempConfigDir::readFixture(fixture));
    config::ServiceOptions options;
    options.path = dir.path();
    options.pollInterval = poll_interval;
    service = std::make_unique<config::ConfigurationService>(options, *timers, *logger);
  }

  // Spec section 11: the config service first, then the scheduler it borrows,
  // then the logger both were writing to.
  ~Stack() {
    service.reset();
    timers->stop(scheduler::StopMode::Drain);
    logger->stop();
  }

  Stack(const Stack&) = delete;
  Stack& operator=(const Stack&) = delete;

  void writeFixture(const std::string& name) {
    dir.writeAtomic(config::test::TempConfigDir::readFixture(name));
  }

  void writeWith(const std::string& from, const std::string& to) {
    std::string text = config::test::TempConfigDir::readFixture("tcu.json");
    const std::size_t at = text.find(from);
    if (at != std::string::npos) {
      text.replace(at, from.size(), to);
    }
    dir.writeAtomic(text);
  }
};

}  // namespace tcu::itest

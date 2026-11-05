#pragma once

// Everything a configuration test needs: a temp directory holding a real file,
// a scheduler configured the way contracts/config_api.md section 2 requires
// (workerThreads == 1, so notification order is publication order), and a
// logger with no sink - the records are accounted for in metrics, which is what
// these tests assert on.

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "support/temp_config_dir.hpp"
#include "tcu/config/configuration_service.hpp"
#include "tcu/logging/async_logger.hpp"
#include "tcu/scheduler/timer_scheduler.hpp"

namespace tcu::config::test {

class ServiceFixture {
 public:
  explicit ServiceFixture(std::size_t max_pending = 64,
                          std::chrono::milliseconds poll_interval =
                              std::chrono::milliseconds{0}) {
    tcu::scheduler::Config scheduler_config;
    scheduler_config.workerThreads = 1;  // ordered notification (R-02, N1)
    // Both caps come from the same knob. The notification queue is bounded by
    // maxTimers rather than maxReadyTasks: project 03 guarantees a scheduled
    // one-shot is never lost to ready-queue pressure (its IT-01), so
    // CapacityExceeded - the overflow signal of spec section 9 rule 7 - is what
    // a full timer registry returns.
    scheduler_config.maxTimers = max_pending;
    scheduler_config.maxReadyTasks = max_pending;
    scheduler_ = std::make_unique<tcu::scheduler::TimerScheduler>(scheduler_config);
    scheduler_->start();

    tcu::logging::LoggerOptions logger_options;
    logger_options.minLevel = tcu::logging::Severity::Trace;
    logger_ = std::make_unique<tcu::logging::AsyncLogger>(
        logger_options, std::vector<std::shared_ptr<tcu::logging::ILogSink>>{});
    logger_->start();

    dir_.writeAtomic(TempConfigDir::readFixture("tcu.json"));

    ServiceOptions options;
    options.path = dir_.path();
    options.pollInterval = poll_interval;
    service_ = std::make_unique<ConfigurationService>(options, *scheduler_, *logger_);
  }

  // Shutdown order of spec section 11: the service first, then the scheduler it
  // borrows, then the logger.
  ~ServiceFixture() {
    service_.reset();
    scheduler_->stop(tcu::scheduler::StopMode::Drain);
    logger_->stop();
  }

  ServiceFixture(const ServiceFixture&) = delete;
  ServiceFixture& operator=(const ServiceFixture&) = delete;

  ConfigurationService& service() { return *service_; }
  TempConfigDir& dir() { return dir_; }
  tcu::scheduler::TimerScheduler& scheduler() { return *scheduler_; }
  tcu::logging::AsyncLogger& logger() { return *logger_; }

  void writeFixture(const std::string& name) {
    dir_.writeAtomic(TempConfigDir::readFixture(name));
  }

  // The baseline with one fragment replaced, for the single-field changes the
  // reload cases need.
  void writeWith(const std::string& from, const std::string& to) {
    std::string text = TempConfigDir::readFixture("tcu.json");
    const std::size_t at = text.find(from);
    if (at != std::string::npos) {
      text.replace(at, from.size(), to);
    }
    dir_.writeAtomic(text);
  }

 private:
  TempConfigDir dir_;
  std::unique_ptr<tcu::scheduler::TimerScheduler> scheduler_;
  std::unique_ptr<tcu::logging::AsyncLogger> logger_;
  std::unique_ptr<ConfigurationService> service_;
};

}  // namespace tcu::config::test

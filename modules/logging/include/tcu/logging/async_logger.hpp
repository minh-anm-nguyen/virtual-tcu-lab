#pragma once

// The bounded asynchronous logger of spec section 10: one writer thread that
// alone owns the sinks, a bounded queue with a severity-aware overload policy,
// redaction at ingestion, and a shutdown that drains or reports.

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <string_view>
#include <vector>

#include "tcu/logging/log_sink.hpp"
#include "tcu/logging/log_types.hpp"
#include "tcu/logging/logger.hpp"
#include "tcu/logging/logger_metrics.hpp"

namespace tcu::logging {

struct LoggerOptions {
  // Hot: /logging/level (research R-15).
  Severity minLevel{Severity::Info};
  // Hot: /logging/console_enabled.
  bool consoleEnabled{true};

  // Restart-only, read once in start().
  std::size_t queueCapacity{8192};
  std::size_t maxRecordBytes{16 * 1024};
  std::chrono::milliseconds highPriorityWaitMs{20};
  std::chrono::milliseconds shutdownDeadline{2000};
  std::chrono::seconds recoveryProbeInterval{30};

  // Injectable wall clock, so a formatter assertion can be a golden line
  // (research R-17). Defaults to std::chrono::system_clock::now.
  std::function<std::chrono::system_clock::time_point()> clock{};
};

class AsyncLogger final : public ILogger {
 public:
  AsyncLogger(LoggerOptions options,
              std::vector<std::shared_ptr<ILogSink>> sinks);
  ~AsyncLogger() override;

  AsyncLogger(const AsyncLogger&) = delete;
  AsyncLogger& operator=(const AsyncLogger&) = delete;

  // Opens the sinks and starts the writer thread. A sink that cannot open does
  // not fail start(): it starts Unhealthy and the application still runs
  // (BC-27). Returns false only when the logger was already started or
  // stopped.
  bool start();

  // Hot settings. Safe from any thread; take effect on the next record.
  void setLevel(Severity level);
  void setConsoleEnabled(bool enabled);

  LoggerMetrics metrics() const;

  void log(Severity level,
           std::string_view component,
           std::string_view event,
           std::string_view message) noexcept override;

  void log(Severity level,
           std::string_view component,
           std::string_view event,
           std::string_view message,
           std::vector<Field> fields) noexcept override;

  bool enabled(Severity level) const noexcept override;
  void flush() override;
  void stop() override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace tcu::logging

#include "log.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>

namespace tcu::scheduler::detail {
namespace {

const char* levelName(LogLevel level) {
  switch (level) {
    case LogLevel::Debug: return "DEBUG";
    case LogLevel::Info: return "INFO";
    case LogLevel::Warn: return "WARN";
    case LogLevel::Error: return "ERROR";
  }
  return "INFO";
}

const char* eventName(LogEvent event) {
  switch (event) {
    case LogEvent::Start: return "start";
    case LogEvent::Schedule: return "schedule";
    case LogEvent::Cancel: return "cancel";
    case LogEvent::Backpressure: return "backpressure";
    case LogEvent::Callback: return "callback";
    case LogEvent::Stop: return "stop";
  }
  return "start";
}

const char* reasonName(LogReason reason) {
  switch (reason) {
    case LogReason::None: return "-";
    case LogReason::Cancelled: return "cancelled";
    case LogReason::CancelRequested: return "cancel_requested";
    case LogReason::Exception: return "exception";
    case LogReason::PeriodicDisabled: return "periodic_disabled";
    case LogReason::Drain: return "drain";
    case LogReason::Discard: return "discard";
    case LogReason::CalledFromSchedulerThread: return "called_from_scheduler_thread";
  }
  return "-";
}

const char* modeName(LogMode mode) {
  switch (mode) {
    case LogMode::None: return "-";
    case LogMode::Once: return "once";
    case LogMode::FixedDelay: return "fixed_delay";
    case LogMode::FixedRate: return "fixed_rate";
  }
  return "-";
}

// The only place a wall clock is read, and it is for humans: no deadline uses
// it (contracts/timing_semantics.md, guarantee G3).
std::string timestamp() {
  using namespace std::chrono;
  const auto now = system_clock::now();
  const std::time_t secs = system_clock::to_time_t(now);
  const auto millis =
      duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000;

  std::tm utc{};
#if defined(_WIN32)
  gmtime_s(&utc, &secs);
#else
  gmtime_r(&secs, &utc);
#endif

  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday, utc.tm_hour,
                utc.tm_min, utc.tm_sec, static_cast<int>(millis));
  return buffer;
}

// Minimum level, read once from TCU_SCHEDULER_LOG_LEVEL
// (debug|info|warn|error). Default info, so the per-schedule DEBUG line is
// off unless an operator asks for it: a 10k-timer run would otherwise emit
// 10k lines (contracts/metrics.md, Volume).
LogLevel minimumLevel() {
  static const LogLevel level = [] {
    const char* raw = std::getenv("TCU_SCHEDULER_LOG_LEVEL");
    if (raw == nullptr) return LogLevel::Info;
    const std::string value = raw;
    if (value == "debug") return LogLevel::Debug;
    if (value == "warn") return LogLevel::Warn;
    if (value == "error") return LogLevel::Error;
    return LogLevel::Info;
  }();
  return level;
}

const char* kSpace = " ";

std::string orDash(const std::optional<std::uint64_t>& value) {
  return value ? std::to_string(*value) : std::string("-");
}

}  // namespace

std::string format(const LogFields& fields) {
  std::string line = timestamp();
  line += kSpace;
  line += levelName(fields.level);
  line += kSpace;
  line += "scheduler";
  line += kSpace;
  line += orDash(fields.timer_id);
  line += kSpace;
  line += modeName(fields.mode);
  line += kSpace;
  line += eventName(fields.event);
  line += kSpace;
  line += reasonName(fields.reason);
  line += kSpace;
  line += orDash(fields.value);
  return line;
}

void emit(const LogFields& fields) {
  // shortcut: the level threshold is an environment variable and the sink is
  // always stderr; upgrade when project 04 (Configuration & Logging Service)
  // owns both (research R-16).
  if (static_cast<int>(fields.level) < static_cast<int>(minimumLevel())) return;
  const std::string line = format(fields) + "\n";
  std::fputs(line.c_str(), stderr);
}

}  // namespace tcu::scheduler::detail

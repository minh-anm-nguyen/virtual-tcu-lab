#pragma once

// Structured logging, one line per call.
//
// The field set is closed and comes from spec section 15 /
// contracts/metrics.md section 3:
//   timestamp level service timer_id mode event reason value
//
// Every field is an enum, an id or a number. There is deliberately no string
// field at all, so callback state, captured values and secrets have no path
// into a log line - Principle V enforced by the type rather than by review.

#include <cstdint>
#include <optional>
#include <string>

#include "tcu/scheduler/scheduler_types.hpp"

namespace tcu::scheduler::detail {

enum class LogLevel { Debug, Info, Warn, Error };

/// Closed vocabulary. Spec section 15 names exactly these six.
enum class LogEvent { Start, Schedule, Cancel, Backpressure, Callback, Stop };

enum class LogReason {
  None,
  Cancelled,
  CancelRequested,
  Exception,
  PeriodicDisabled,
  Drain,
  Discard,
  CalledFromSchedulerThread,
};

enum class LogMode { None, Once, FixedDelay, FixedRate };

/// Absent optionals and the None enumerators render as "-".
struct LogFields {
  LogLevel level{LogLevel::Info};
  LogEvent event{LogEvent::Start};
  LogReason reason{LogReason::None};
  LogMode mode{LogMode::None};
  std::optional<TimerId> timer_id;
  /// One numeric detail whose meaning the event fixes: worker count, delay in
  /// ms, queue depth, in-flight callback count.
  std::optional<std::uint64_t> value;
};

/// Renders the line without a trailing newline. Exposed for the unit test.
std::string format(const LogFields& fields);

/// Renders and writes the line to stderr, newline terminated.
void emit(const LogFields& fields);


}  // namespace tcu::scheduler::detail

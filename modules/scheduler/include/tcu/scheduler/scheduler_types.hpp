#pragma once

// Public vocabulary of the timer scheduler.
//
// These declarations are fixed by spec section 9 and
// specs/003-timer-task-scheduler/contracts/scheduler_api.md section 1. Adding
// an enum value here is a spec change, not an implementation detail
// (Constitution Principle I).

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>

namespace tcu::scheduler {

/// Monotonic by construction: wall-clock, timezone and NTP changes cannot move
/// a deadline (spec section 11.3).
using Clock = std::chrono::steady_clock;

/// Strictly increasing and never reused. 0 is never issued and means
/// "no timer" (research R-07).
using TimerId = std::uint64_t;

using Callback = std::function<void()>;

enum class PeriodicMode {
  /// next deadline = completion time + interval. Drifts, never overlaps.
  FixedDelay,
  /// next deadline stays aligned to the original phase; missed slots are
  /// counted and skipped, never replayed as a burst.
  FixedRateSkipMissed,
};

enum class StopMode {
  /// Run what was already due or queued when stop() was entered; cancel the
  /// future. Bounded by a single clock reading (research R-13).
  Drain,
  /// Cancel everything not already running; let in-flight callbacks finish.
  Discard,
};

enum class Error {
  None,
  InvalidArgument,
  InvalidState,
  CapacityExceeded,
};

enum class CancelResult {
  /// The callback had not started and now never will.
  Cancelled,
  /// The callback is executing; it is not interrupted, and a periodic timer
  /// will not be rearmed.
  RunningCancelRequested,
  /// The id was issued and its timer is gone or already cancel-requested.
  AlreadyFinished,
  /// The id was never issued (0, or not yet handed out).
  NotFound,
};

struct Config {
  std::size_t workerThreads{2};
  std::size_t maxTimers{10000};
  std::size_t maxReadyTasks{1024};
};

struct ScheduleResult {
  TimerId id{0};
  Error error{Error::None};

  explicit operator bool() const { return error == Error::None; }
};

}  // namespace tcu::scheduler

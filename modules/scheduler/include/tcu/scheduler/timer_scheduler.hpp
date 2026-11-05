#pragma once

// The public scheduler. Pre- and postconditions for every method are in
// specs/003-timer-task-scheduler/contracts/scheduler_api.md section 2.

#include <memory>

#include "tcu/scheduler/clock_adapter.hpp"
#include "tcu/scheduler/scheduler_metrics.hpp"
#include "tcu/scheduler/scheduler_types.hpp"

namespace tcu::scheduler {

class TimerScheduler final {
 public:
  explicit TimerScheduler(Config config);
  /// A null clock is replaced by a SteadyClockAdapter, so the argument cannot
  /// make construction fail (research R-05).
  TimerScheduler(Config config, std::shared_ptr<ClockAdapter> clock);

  /// Calls stop(StopMode::Discard) when still running, then joins.
  /// Precondition: no other thread is calling any method on this object.
  ~TimerScheduler();

  TimerScheduler(const TimerScheduler&) = delete;
  TimerScheduler& operator=(const TimerScheduler&) = delete;

  /// Created -> Running. InvalidArgument on a zero Config field; InvalidState
  /// in any other state - the scheduler is single-use (spec section 8.1).
  Error start();

  ScheduleResult scheduleAfter(Clock::duration delay, Callback cb);
  ScheduleResult scheduleAt(Clock::time_point deadline, Callback cb);
  ScheduleResult scheduleEvery(Clock::duration interval, PeriodicMode mode,
                               Callback cb);

  /// Never blocks waiting for a callback to finish. Idempotent and crash-free
  /// for any input, including 0 and UINT64_MAX.
  CancelResult cancel(TimerId id);

  /// Blocks until every thread joins. No timeout, no force-kill (R-04).
  /// Called from a scheduler thread it logs an error and returns (R-12).
  void stop(StopMode mode);

  /// Each counter is read atomically; the set is not read under one lock, so
  /// the conservation equations hold at a quiescent point.
  SchedulerMetrics metrics() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace tcu::scheduler

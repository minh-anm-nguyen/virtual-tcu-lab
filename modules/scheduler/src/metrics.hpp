#pragma once

// Internal counter storage behind the public SchedulerMetrics snapshot.
//
// Definitions, increment points and the M1-M6 conservation equations are in
// specs/003-timer-task-scheduler/contracts/metrics.md. The three rejection
// reasons are three counters rather than a keyed map, because the reason set is
// closed (research R-15).

#include <array>
#include <atomic>
#include <cstdint>

#include "tcu/scheduler/scheduler_metrics.hpp"
#include "tcu/scheduler/scheduler_types.hpp"

namespace tcu::scheduler::detail {

class AtomicHistogram {
 public:
  /// Samples are milliseconds. A would-be-negative sample is clamped by the
  /// caller, so the <=1ms bucket absorbing it is the visible sign of invariant
  /// T2 holding rather than an unsigned wrap.
  void record(std::uint64_t ms);
  Histogram snapshot() const;

 private:
  std::array<std::atomic<std::uint64_t>, Histogram::kBucketCount> buckets_{};
  std::atomic<std::uint64_t> count_{0};
  std::atomic<std::uint64_t> sum_ms_{0};
};

struct Metrics {
  std::atomic<std::uint64_t> timers_active{0};
  std::atomic<std::uint64_t> ready_queue_depth{0};

  std::atomic<std::uint64_t> timers_scheduled_total{0};
  std::atomic<std::uint64_t> callbacks_started_total{0};
  std::atomic<std::uint64_t> callbacks_completed_total{0};
  std::atomic<std::uint64_t> callbacks_failed_total{0};
  std::atomic<std::uint64_t> timers_cancelled_total{0};
  std::atomic<std::uint64_t> rejected_invalid_argument_total{0};
  std::atomic<std::uint64_t> rejected_invalid_state_total{0};
  std::atomic<std::uint64_t> rejected_capacity_total{0};
  std::atomic<std::uint64_t> periods_skipped_total{0};
  std::atomic<std::uint64_t> callbacks_skipped_cancelled_total{0};

  AtomicHistogram start_lateness_ms;
  AtomicHistogram callback_duration_ms;

  /// Clamps at 0: the coordinator never dispatches early (invariant T2).
  void recordLateness(Clock::duration lateness);
  void recordDuration(Clock::duration duration);

  SchedulerMetrics snapshot() const;
};

}  // namespace tcu::scheduler::detail

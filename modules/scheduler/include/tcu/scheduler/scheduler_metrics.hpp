#pragma once

// Public metrics snapshot. Field set and semantics are fixed by spec section 15
// and specs/003-timer-task-scheduler/contracts/metrics.md.

#include <array>
#include <cstddef>
#include <cstdint>

namespace tcu::scheduler {

/// Fixed-boundary histogram. Buckets are exclusive counts, not cumulative.
/// Upper bounds in milliseconds: 1, 5, 10, 25, 50, 100, 250, 1000, +inf.
/// Percentiles are derived outside the library (research R-15, R-18).
struct Histogram {
  static constexpr std::size_t kBucketCount = 9;
  static constexpr std::array<std::uint64_t, kBucketCount - 1> kUpperBoundsMs{
      1, 5, 10, 25, 50, 100, 250, 1000};

  std::array<std::uint64_t, kBucketCount> buckets{};
  std::uint64_t count{0};
  std::uint64_t sum_ms{0};
};

struct SchedulerMetrics {
  // Gauges.
  std::uint64_t timers_active{0};
  std::uint64_t ready_queue_depth{0};

  // Counters.
  std::uint64_t timers_scheduled_total{0};
  std::uint64_t callbacks_started_total{0};
  std::uint64_t callbacks_completed_total{0};
  std::uint64_t callbacks_failed_total{0};
  std::uint64_t timers_cancelled_total{0};
  std::uint64_t rejected_invalid_argument_total{0};
  std::uint64_t rejected_invalid_state_total{0};
  std::uint64_t rejected_capacity_total{0};
  std::uint64_t periods_skipped_total{0};
  std::uint64_t callbacks_skipped_cancelled_total{0};

  // Histograms.
  Histogram start_lateness_ms{};
  Histogram callback_duration_ms{};
};

}  // namespace tcu::scheduler

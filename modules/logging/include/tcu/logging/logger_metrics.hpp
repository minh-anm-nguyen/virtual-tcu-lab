#pragma once

// A by-value snapshot of the logger counters, following the SchedulerMetrics
// pattern of project 03: atomics inside, a plain struct out, no registry and no
// runtime label strings.
//
// Histogram is declared HERE rather than included from
// tcu/scheduler/scheduler_metrics.hpp, even though that header is public. The
// shape is the same so the stress report percentile code works on both, but
// including it would make tcu_logging depend on the timer library, and that
// dependency runs the wrong way: project 03 is a future consumer of this logger
// (research R-19, contracts/metrics.md section 2).

#include <array>
#include <cstdint>

#include "tcu/logging/log_sink.hpp"
#include "tcu/logging/log_types.hpp"

namespace tcu::logging {

struct Histogram {
  static constexpr std::size_t kBucketCount = 9;
  static constexpr std::array<std::uint64_t, kBucketCount - 1> kUpperBoundsMs{
      1, 5, 10, 25, 50, 100, 250, 1000};

  std::array<std::uint64_t, kBucketCount> buckets{};
  std::uint64_t count{0};
  std::uint64_t sum_ms{0};
};

struct LoggerMetrics {
  // Gauges.
  std::uint64_t queueDepth{0};
  std::array<SinkHealth, kSinkCount> sinkHealth{SinkHealth::Healthy,
                                                SinkHealth::Healthy};

  // Counters.
  std::uint64_t recordsSubmittedTotal{0};
  std::uint64_t recordsFilteredTotal{0};
  std::uint64_t recordsEnqueuedTotal{0};
  std::array<std::uint64_t, kSeverityCount> recordsDroppedTotal{};
  std::uint64_t highPriorityDropsTotal{0};
  std::uint64_t recordsTruncatedTotal{0};
  std::uint64_t formatFailuresTotal{0};
  std::uint64_t fatalEmergencyTotal{0};
  std::uint64_t reentrantEmergencyTotal{0};
  std::uint64_t rejectedAfterStopTotal{0};
  std::uint64_t notDrainedAtStopTotal{0};
  std::array<std::uint64_t, kSinkCount> recordsWrittenTotal{};
  std::array<std::uint64_t, kSinkCount> sinkErrorsTotal{};
  std::uint64_t rotationsTotal{0};
  Histogram writeLatencyMs{};

  std::uint64_t droppedTotal() const {
    std::uint64_t sum = 0;
    for (const std::uint64_t n : recordsDroppedTotal) { sum += n; }
    return sum;
  }

  std::uint64_t writtenTotal() const {
    std::uint64_t sum = 0;
    for (const std::uint64_t n : recordsWrittenTotal) { sum += n; }
    return sum;
  }

  bool allSinksHealthy() const {
    for (const SinkHealth h : sinkHealth) {
      if (h != SinkHealth::Healthy) { return false; }
    }
    return true;
  }
};

}  // namespace tcu::logging

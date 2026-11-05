#pragma once

// By-value snapshot of the configuration counters. Same pattern as
// LoggerMetrics and project 03 SchedulerMetrics.
//
// config_generation_applied{component} is deliberately NOT here: the library
// cannot know when a consumer finished applying a change, so that metric
// belongs to the consumer (contracts/metrics.md section 1).

#include <array>
#include <cstdint>

#include "tcu/config/config_types.hpp"

namespace tcu::config {

struct ConfigMetrics {
  // Gauges.
  std::uint64_t generation{0};
  std::uint64_t notificationQueueDepth{0};
  bool notifyDegraded{false};
  bool configValid{false};

  // Counters.
  std::uint64_t reloadAttemptsTotal{0};
  std::uint64_t reloadSuccessChangedTotal{0};
  std::uint64_t reloadSuccessNoopTotal{0};
  std::array<std::uint64_t, kLoadErrorCount> reloadFailuresTotal{};
  std::uint64_t notifyFailuresTotal{0};
  std::uint64_t notifyDeliveredTotal{0};
  std::uint64_t resyncNotificationsTotal{0};

  std::uint64_t reloadFailuresSum() const {
    std::uint64_t sum = 0;
    for (const std::uint64_t n : reloadFailuresTotal) { sum += n; }
    return sum;
  }

  std::uint64_t failures(LoadError reason) const {
    return reloadFailuresTotal[loadErrorIndex(reason)];
  }
};

}  // namespace tcu::config

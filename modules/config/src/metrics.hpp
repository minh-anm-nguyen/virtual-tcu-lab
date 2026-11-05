#pragma once

// Internal counter storage for the configuration service. Same pattern as the
// logging side: atomics in, a plain ConfigMetrics out, failure reasons as a
// fixed array indexed by LoadError so the label set cannot grow at runtime
// (invariant M4).

#include <atomic>

#include "tcu/config/config_metrics.hpp"

namespace tcu::config::internal {

class Metrics {
 public:
  void addAttempt() { attempts_.fetch_add(1, std::memory_order_relaxed); }
  void addSuccessChanged() { success_changed_.fetch_add(1, std::memory_order_relaxed); }
  void addSuccessNoop() { success_noop_.fetch_add(1, std::memory_order_relaxed); }
  void addFailure(LoadError reason) {
    failures_[loadErrorIndex(reason)].fetch_add(1, std::memory_order_relaxed);
  }
  void addNotifyFailure() { notify_failures_.fetch_add(1, std::memory_order_relaxed); }
  void addNotifyDelivered() { notify_delivered_.fetch_add(1, std::memory_order_relaxed); }
  void addResync() { resyncs_.fetch_add(1, std::memory_order_relaxed); }

  void setGeneration(std::uint64_t g) { generation_.store(g, std::memory_order_relaxed); }
  void setQueueDepth(std::uint64_t d) { queue_depth_.store(d, std::memory_order_relaxed); }
  void setConfigValid(bool valid) { config_valid_.store(valid, std::memory_order_relaxed); }
  // Sticky: once degraded, it stays degraded for the life of the service, which
  // is what spec section 9 rule 7 means by marking degraded health.
  void latchNotifyDegraded() { notify_degraded_.store(true, std::memory_order_relaxed); }

  ConfigMetrics snapshot() const {
    ConfigMetrics out;
    out.generation = generation_.load(std::memory_order_relaxed);
    out.notificationQueueDepth = queue_depth_.load(std::memory_order_relaxed);
    out.notifyDegraded = notify_degraded_.load(std::memory_order_relaxed);
    out.configValid = config_valid_.load(std::memory_order_relaxed);
    out.reloadAttemptsTotal = attempts_.load(std::memory_order_relaxed);
    out.reloadSuccessChangedTotal = success_changed_.load(std::memory_order_relaxed);
    out.reloadSuccessNoopTotal = success_noop_.load(std::memory_order_relaxed);
    for (std::size_t i = 0; i < kLoadErrorCount; ++i) {
      out.reloadFailuresTotal[i] = failures_[i].load(std::memory_order_relaxed);
    }
    out.notifyFailuresTotal = notify_failures_.load(std::memory_order_relaxed);
    out.notifyDeliveredTotal = notify_delivered_.load(std::memory_order_relaxed);
    out.resyncNotificationsTotal = resyncs_.load(std::memory_order_relaxed);
    return out;
  }

 private:
  std::atomic<std::uint64_t> generation_{0};
  std::atomic<std::uint64_t> queue_depth_{0};
  std::atomic<bool> notify_degraded_{false};
  std::atomic<bool> config_valid_{false};

  std::atomic<std::uint64_t> attempts_{0};
  std::atomic<std::uint64_t> success_changed_{0};
  std::atomic<std::uint64_t> success_noop_{0};
  std::array<std::atomic<std::uint64_t>, kLoadErrorCount> failures_{};
  std::atomic<std::uint64_t> notify_failures_{0};
  std::atomic<std::uint64_t> notify_delivered_{0};
  std::atomic<std::uint64_t> resyncs_{0};
};

}  // namespace tcu::config::internal

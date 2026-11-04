#pragma once

// Internal counter block behind IpcMetricsSnapshot.
//
// Each counter is atomic; the snapshot is deliberately NOT an atomic view
// across counters (contracts/ipc_api.md), so the conservation equations of
// contracts/metrics.md hold only while no request is outstanding.

#include <atomic>
#include <cstdint>
#include <mutex>

#include "tcu/ipc/ipc_types.hpp"

namespace tcu::ipc::detail {

class Histogram {
 public:
  void observe(std::uint64_t ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::size_t i = 0;
    while (i < LatencyHistogram::kBucketCount - 1 && ms > LatencyHistogram::kUpperBoundsMs[i]) {
      ++i;
    }
    ++data_.buckets[i];
    ++data_.count;
    data_.sum_ms += ms;
  }

  LatencyHistogram snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return data_;
  }

 private:
  // A mutex rather than 14 atomics: an observation is off the hot path (one per
  // resolved request) and the snapshot must be self-consistent to be readable.
  mutable std::mutex mutex_;
  LatencyHistogram data_{};
};

struct Metrics {
  std::atomic<std::uint64_t> connections_active{0};
  std::atomic<std::uint64_t> connect_attempts_total{0};
  std::atomic<std::uint64_t> reconnect_total{0};
  std::atomic<std::uint64_t> requests_created_total{0};
  std::atomic<std::uint64_t> requests_sent_total{0};
  std::atomic<std::uint64_t> requests_completed_total{0};
  std::atomic<std::uint64_t> requests_failed_total{0};
  std::atomic<std::uint64_t> request_timeouts_total{0};
  std::atomic<std::uint64_t> queue_rejects_total{0};
  std::atomic<std::uint64_t> protocol_errors_total{0};
  std::atomic<std::uint64_t> late_responses_total{0};
  std::atomic<std::uint64_t> tx_queue_bytes{0};
  Histogram request_latency_ms{};

  /// Records the terminal counter for one request. Called exactly once per
  /// request, from the thread that erased it from the pending map (invariant
  /// M2, a consequence of S1).
  void recordTerminal(IpcError error, std::uint64_t latency_ms, bool observe_latency) {
    switch (error) {
      case IpcError::None:
      case IpcError::RemoteError:
        requests_completed_total.fetch_add(1, std::memory_order_relaxed);
        break;
      case IpcError::Timeout:
        request_timeouts_total.fetch_add(1, std::memory_order_relaxed);
        break;
      default:
        requests_failed_total.fetch_add(1, std::memory_order_relaxed);
        break;
    }
    if (observe_latency) {
      request_latency_ms.observe(latency_ms);
    }
  }

  IpcMetricsSnapshot snapshot() const {
    IpcMetricsSnapshot s;
    s.connections_active = connections_active.load(std::memory_order_relaxed);
    s.connect_attempts_total = connect_attempts_total.load(std::memory_order_relaxed);
    s.reconnect_total = reconnect_total.load(std::memory_order_relaxed);
    s.requests_created_total = requests_created_total.load(std::memory_order_relaxed);
    s.requests_sent_total = requests_sent_total.load(std::memory_order_relaxed);
    s.requests_completed_total = requests_completed_total.load(std::memory_order_relaxed);
    s.requests_failed_total = requests_failed_total.load(std::memory_order_relaxed);
    s.request_timeouts_total = request_timeouts_total.load(std::memory_order_relaxed);
    s.queue_rejects_total = queue_rejects_total.load(std::memory_order_relaxed);
    s.protocol_errors_total = protocol_errors_total.load(std::memory_order_relaxed);
    s.late_responses_total = late_responses_total.load(std::memory_order_relaxed);
    s.tx_queue_bytes = tx_queue_bytes.load(std::memory_order_relaxed);
    s.request_latency_ms = request_latency_ms.snapshot();
    return s;
  }
};

}  // namespace tcu::ipc::detail

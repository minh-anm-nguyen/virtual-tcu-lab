#pragma once

// The live counters behind CanMetricsSnapshot.
//
// Relaxed ordering throughout: these are observability, not synchronization.
// Nothing in the service makes a control decision by reading a counter, so a
// counter read that trails reality by a few instructions costs nothing and a
// stronger ordering would put a barrier on the RX hot path (research R-23).

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>

#include "tcu/can/can_metrics.hpp"

namespace tcu::can {

class CanMetricsStore {
 public:
  // One place to bump a counter, so no increment site invents its own ordering.
  static void bump(std::atomic<std::uint64_t>& counter, std::uint64_t by = 1) noexcept {
    counter.fetch_add(by, std::memory_order_relaxed);
  }

  /// Bucket index for a receipt-to-decode duration. Powers of two in
  /// milliseconds, so bucket i covers [2^(i-1), 2^i) ms and the last bucket is
  /// an open-ended overflow. Monotonic in the duration, which is what makes a
  /// percentile from the buckets meaningful.
  static std::size_t latencyBucket(std::chrono::nanoseconds d) noexcept {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(d).count();
    if (ms <= 0) {
      return 0;
    }
    std::size_t bucket = 1;
    std::int64_t bound = 1;
    while (bucket + 1 < kLatencyBucketCount && ms >= bound) {
      bound *= 2;
      ++bucket;
    }
    return bucket;
  }

  void recordDecodeLatency(std::chrono::nanoseconds d) noexcept {
    bump(decode_latency_buckets[latencyBucket(d)]);
  }

  CanMetricsSnapshot snapshot() const noexcept {
    CanMetricsSnapshot out;
    auto read = [](const std::atomic<std::uint64_t>& c) {
      return c.load(std::memory_order_relaxed);
    };

    out.can_rx_read_total = read(rx_read_total);
    out.can_rx_enqueued_total = read(rx_enqueued_total);
    out.can_rx_queue_drop_total = read(rx_queue_drop_total);
    out.can_rx_invalid_total = read(rx_invalid_total);
    out.can_unknown_id_total = read(unknown_id_total);
    out.can_reserved_nonzero_total = read(reserved_nonzero_total);
    out.can_decoded_total = read(decoded_total);

    out.can_reject_flags_total = read(reject_flags_total);
    out.can_reject_length_total = read(reject_length_total);
    out.can_reject_range_total = read(reject_range_total);

    out.signal_update_total = read(signal_update_total);
    out.signal_stale_transitions_total = read(signal_stale_transitions_total);
    out.signal_recovered_total = read(signal_recovered_total);
    out.signal_invalid_transitions_total = read(signal_invalid_transitions_total);

    out.can_tx_attempt_total = read(tx_attempt_total);
    out.can_tx_success_total = read(tx_success_total);
    out.can_ack_matched_total = read(ack_matched_total);
    out.can_ack_unmatched_total = read(ack_unmatched_total);
    out.can_ack_timeout_total = read(ack_timeout_total);

    out.can_link_error_total = read(link_error_total);
    out.can_link_reconnect_total = read(link_reconnect_total);
    out.can_shutdown_discarded_total = read(shutdown_discarded_total);

    out.event_publish_total = read(event_publish_total);
    out.event_publish_failed_total = read(event_publish_failed_total);

    for (std::size_t i = 0; i < kLatencyBucketCount; ++i) {
      out.can_decode_latency_buckets[i] = read(decode_latency_buckets[i]);
    }
    return out;
  }

  std::atomic<std::uint64_t> rx_read_total{0};
  std::atomic<std::uint64_t> rx_enqueued_total{0};
  std::atomic<std::uint64_t> rx_queue_drop_total{0};
  std::atomic<std::uint64_t> rx_invalid_total{0};
  std::atomic<std::uint64_t> unknown_id_total{0};
  std::atomic<std::uint64_t> reserved_nonzero_total{0};
  std::atomic<std::uint64_t> decoded_total{0};

  std::atomic<std::uint64_t> reject_flags_total{0};
  std::atomic<std::uint64_t> reject_length_total{0};
  std::atomic<std::uint64_t> reject_range_total{0};

  std::atomic<std::uint64_t> signal_update_total{0};
  std::atomic<std::uint64_t> signal_stale_transitions_total{0};
  std::atomic<std::uint64_t> signal_recovered_total{0};
  std::atomic<std::uint64_t> signal_invalid_transitions_total{0};

  std::atomic<std::uint64_t> tx_attempt_total{0};
  std::atomic<std::uint64_t> tx_success_total{0};
  std::atomic<std::uint64_t> ack_matched_total{0};
  std::atomic<std::uint64_t> ack_unmatched_total{0};
  std::atomic<std::uint64_t> ack_timeout_total{0};

  std::atomic<std::uint64_t> link_error_total{0};
  std::atomic<std::uint64_t> link_reconnect_total{0};
  std::atomic<std::uint64_t> shutdown_discarded_total{0};

  std::atomic<std::uint64_t> event_publish_total{0};
  std::atomic<std::uint64_t> event_publish_failed_total{0};

  std::array<std::atomic<std::uint64_t>, kLatencyBucketCount> decode_latency_buckets{};
};

}  // namespace tcu::can

#pragma once

// The observability surface. contracts/metrics.md holds the meaning, the
// caveat and the invariant for every field; this is only the shape.
//
// NFR-08 is the rule the split counters exist for: "any drop or reject path
// must increment a distinguishable metric". No cause shares a counter with a
// cause an operator would need to tell apart.
//
// Shape follows the repo convention set by SchedulerMetricsSnapshot and
// IpcMetricsSnapshot: one POD read through one noexcept accessor (R-23).

#include <array>
#include <cstddef>
#include <cstdint>

namespace tcu::can {

/// Buckets for can_decode_latency: a histogram, not a mean, because NFR-05
/// asks for p50/p95/p99 and forbids inventing a latency target.
inline constexpr std::size_t kLatencyBucketCount = 16;

struct CanMetricsSnapshot {
  // Receive path
  std::uint64_t can_rx_read_total{0};
  std::uint64_t can_rx_enqueued_total{0};
  std::uint64_t can_rx_queue_drop_total{0};
  std::uint64_t can_rx_invalid_total{0};
  std::uint64_t can_unknown_id_total{0};
  std::uint64_t can_reserved_nonzero_total{0};
  std::uint64_t can_decoded_total{0};

  // Reject reasons, split so one cause is never hidden behind another.
  std::uint64_t can_reject_flags_total{0};
  std::uint64_t can_reject_length_total{0};
  std::uint64_t can_reject_range_total{0};

  // Signals
  std::uint64_t signal_update_total{0};
  std::uint64_t signal_stale_transitions_total{0};
  std::uint64_t signal_recovered_total{0};
  std::uint64_t signal_invalid_transitions_total{0};

  // Transmit and command correlation
  std::uint64_t can_tx_attempt_total{0};
  std::uint64_t can_tx_success_total{0};
  std::uint64_t can_ack_matched_total{0};
  std::uint64_t can_ack_unmatched_total{0};
  std::uint64_t can_ack_timeout_total{0};

  // Link lifecycle
  std::uint64_t can_link_error_total{0};
  std::uint64_t can_link_reconnect_total{0};
  std::uint64_t can_shutdown_discarded_total{0};

  // Event Bus adapter
  std::uint64_t event_publish_total{0};
  std::uint64_t event_publish_failed_total{0};

  // Gauges and distribution
  std::size_t can_queue_depth{0};
  std::size_t can_queue_capacity{0};
  std::array<std::uint64_t, kLatencyBucketCount> can_decode_latency_buckets{};
};

}  // namespace tcu::can

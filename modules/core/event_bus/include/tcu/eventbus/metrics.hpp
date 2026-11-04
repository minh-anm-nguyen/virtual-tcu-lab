#pragma once

#include <cstddef>
#include <cstdint>

namespace tcu::eventbus {

// Counter semantics and the conservation equations live in
// specs/001-thread-safe-event-bus/contracts/metrics.md. A snapshot is a
// consistent read of each field but not atomic across fields (invariant M2).
struct MetricsSnapshot {
  std::uint64_t accepted_total{0};
  std::uint64_t rejected_queue_full_total{0};
  std::uint64_t publish_timeout_total{0};
  std::uint64_t dispatched_total{0};
  std::uint64_t callback_invocations_total{0};
  std::uint64_t callback_failures_total{0};
  std::uint64_t discarded_on_stop_total{0};
  std::size_t queue_depth{0};
  std::size_t queue_high_watermark{0};
};

}  // namespace tcu::eventbus

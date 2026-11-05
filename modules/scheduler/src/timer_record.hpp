#pragma once

// Internal per-timer state. Fields and transitions are fixed by
// specs/003-timer-task-scheduler/data-model.md sections 1.2 and 2.2.

#include <cstdint>
#include <memory>
#include <unordered_map>

#include "tcu/scheduler/scheduler_types.hpp"

namespace tcu::scheduler::detail {

enum class TimerState {
  Scheduled,
  Queued,
  Running,
  /// cancel() arrived while the callback was executing. The callback is not
  /// interrupted, and a periodic timer is not rearmed (flow 12.5).
  CancelRequested,
};

enum class TimerKind { OneShot, Periodic };

struct TimerRecord {
  TimerId id{0};
  TimerState state{TimerState::Scheduled};
  /// Bumped on every rearm and on cancel; invalidates heap entries (R-11).
  std::uint64_t generation{0};
  Clock::time_point deadline{};
  Callback callback;
  TimerKind kind{TimerKind::OneShot};
  PeriodicMode mode{PeriodicMode::FixedDelay};
  /// > 0 only when kind == Periodic; zero otherwise.
  Clock::duration interval{Clock::duration::zero()};
  /// t0, the first deadline: the fixed-rate alignment base (R-14).
  Clock::time_point phase{};
  /// The cadence index k this timer last fired for; 0 before the first fire.
  std::uint64_t periodIndex{0};
};

using Registry = std::unordered_map<TimerId, std::shared_ptr<TimerRecord>>;

}  // namespace tcu::scheduler::detail

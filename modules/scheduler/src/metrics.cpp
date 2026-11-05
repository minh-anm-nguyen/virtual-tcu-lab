#include "metrics.hpp"

#include <chrono>

namespace tcu::scheduler::detail {
namespace {

std::uint64_t toMillis(Clock::duration d) {
  if (d.count() <= 0) return 0;
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(d).count());
}

}  // namespace

void AtomicHistogram::record(std::uint64_t ms) {
  std::size_t index = Histogram::kBucketCount - 1;  // the +inf bucket
  for (std::size_t i = 0; i < Histogram::kUpperBoundsMs.size(); ++i) {
    if (ms <= Histogram::kUpperBoundsMs[i]) {
      index = i;
      break;
    }
  }
  buckets_[index].fetch_add(1, std::memory_order_relaxed);
  count_.fetch_add(1, std::memory_order_relaxed);
  sum_ms_.fetch_add(ms, std::memory_order_relaxed);
}

Histogram AtomicHistogram::snapshot() const {
  Histogram out;
  for (std::size_t i = 0; i < Histogram::kBucketCount; ++i) {
    out.buckets[i] = buckets_[i].load(std::memory_order_relaxed);
  }
  out.count = count_.load(std::memory_order_relaxed);
  out.sum_ms = sum_ms_.load(std::memory_order_relaxed);
  return out;
}

void Metrics::recordLateness(Clock::duration lateness) {
  start_lateness_ms.record(toMillis(lateness));
}

void Metrics::recordDuration(Clock::duration duration) {
  callback_duration_ms.record(toMillis(duration));
}

SchedulerMetrics Metrics::snapshot() const {
  SchedulerMetrics out;
  out.timers_active = timers_active.load(std::memory_order_relaxed);
  out.ready_queue_depth = ready_queue_depth.load(std::memory_order_relaxed);
  out.timers_scheduled_total =
      timers_scheduled_total.load(std::memory_order_relaxed);
  out.callbacks_started_total =
      callbacks_started_total.load(std::memory_order_relaxed);
  out.callbacks_completed_total =
      callbacks_completed_total.load(std::memory_order_relaxed);
  out.callbacks_failed_total =
      callbacks_failed_total.load(std::memory_order_relaxed);
  out.timers_cancelled_total =
      timers_cancelled_total.load(std::memory_order_relaxed);
  out.rejected_invalid_argument_total =
      rejected_invalid_argument_total.load(std::memory_order_relaxed);
  out.rejected_invalid_state_total =
      rejected_invalid_state_total.load(std::memory_order_relaxed);
  out.rejected_capacity_total =
      rejected_capacity_total.load(std::memory_order_relaxed);
  out.periods_skipped_total =
      periods_skipped_total.load(std::memory_order_relaxed);
  out.callbacks_skipped_cancelled_total =
      callbacks_skipped_cancelled_total.load(std::memory_order_relaxed);
  out.start_lateness_ms = start_lateness_ms.snapshot();
  out.callback_duration_ms = callback_duration_ms.snapshot();
  return out;
}

}  // namespace tcu::scheduler::detail

#pragma once

// Internal counter storage. Atomics in here, a plain LoggerMetrics out, so a
// reader never sees a half-updated struct and never takes a lock.
//
// Every per-severity and per-sink counter is a fixed array indexed by the enum:
// a label cannot be invented at runtime, which is what keeps metric cardinality
// bounded by construction (invariant M4).

#include <algorithm>
#include <atomic>
#include <chrono>

#include "tcu/logging/logger_metrics.hpp"

namespace tcu::logging::internal {

struct AtomicHistogram {
  std::array<std::atomic<std::uint64_t>, Histogram::kBucketCount> buckets{};
  std::atomic<std::uint64_t> count{0};
  std::atomic<std::uint64_t> sum_ms{0};

  void record(std::uint64_t value_ms) {
    std::size_t index = Histogram::kBucketCount - 1;
    for (std::size_t i = 0; i < Histogram::kUpperBoundsMs.size(); ++i) {
      if (value_ms <= Histogram::kUpperBoundsMs[i]) {
        index = i;
        break;
      }
    }
    buckets[index].fetch_add(1, std::memory_order_relaxed);
    count.fetch_add(1, std::memory_order_relaxed);
    sum_ms.fetch_add(value_ms, std::memory_order_relaxed);
  }

  Histogram snapshot() const {
    Histogram out;
    for (std::size_t i = 0; i < Histogram::kBucketCount; ++i) {
      out.buckets[i] = buckets[i].load(std::memory_order_relaxed);
    }
    out.count = count.load(std::memory_order_relaxed);
    out.sum_ms = sum_ms.load(std::memory_order_relaxed);
    return out;
  }
};

class Metrics {
 public:
  void addSubmitted() { submitted_.fetch_add(1, std::memory_order_relaxed); }
  void addFiltered() { filtered_.fetch_add(1, std::memory_order_relaxed); }
  void addEnqueued() { enqueued_.fetch_add(1, std::memory_order_relaxed); }
  void addDropped(Severity level) {
    dropped_[static_cast<std::size_t>(level)].fetch_add(1, std::memory_order_relaxed);
  }
  void addHighPriorityDrop() {
    high_priority_drops_.fetch_add(1, std::memory_order_relaxed);
  }
  void addTruncated() { truncated_.fetch_add(1, std::memory_order_relaxed); }
  void addFormatFailure() { format_failures_.fetch_add(1, std::memory_order_relaxed); }
  void addFatalEmergency() { fatal_emergency_.fetch_add(1, std::memory_order_relaxed); }
  void addReentrantEmergency() {
    reentrant_emergency_.fetch_add(1, std::memory_order_relaxed);
  }
  void addRejectedAfterStop() {
    rejected_after_stop_.fetch_add(1, std::memory_order_relaxed);
  }
  void addNotDrainedAtStop(std::uint64_t n) {
    not_drained_.fetch_add(n, std::memory_order_relaxed);
  }
  void addWritten(SinkKind kind) {
    written_[sinkIndex(kind)].fetch_add(1, std::memory_order_relaxed);
  }
  void addSinkError(SinkKind kind) {
    sink_errors_[sinkIndex(kind)].fetch_add(1, std::memory_order_relaxed);
  }
  void addRotation() { rotations_.fetch_add(1, std::memory_order_relaxed); }

  void recordWriteLatency(std::chrono::steady_clock::duration d) {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(d).count();
    write_latency_.record(static_cast<std::uint64_t>(std::max<std::int64_t>(0, ms)));
  }

  void setQueueDepth(std::uint64_t depth) {
    queue_depth_.store(depth, std::memory_order_relaxed);
  }
  void setSinkHealth(SinkKind kind, SinkHealth health) {
    sink_health_[sinkIndex(kind)].store(health == SinkHealth::Healthy,
                                       std::memory_order_relaxed);
  }

  LoggerMetrics snapshot() const {
    LoggerMetrics out;
    out.queueDepth = queue_depth_.load(std::memory_order_relaxed);
    for (std::size_t i = 0; i < kSinkCount; ++i) {
      out.sinkHealth[i] = sink_health_[i].load(std::memory_order_relaxed)
                              ? SinkHealth::Healthy
                              : SinkHealth::Unhealthy;
      out.recordsWrittenTotal[i] = written_[i].load(std::memory_order_relaxed);
      out.sinkErrorsTotal[i] = sink_errors_[i].load(std::memory_order_relaxed);
    }
    out.recordsSubmittedTotal = submitted_.load(std::memory_order_relaxed);
    out.recordsFilteredTotal = filtered_.load(std::memory_order_relaxed);
    out.recordsEnqueuedTotal = enqueued_.load(std::memory_order_relaxed);
    for (std::size_t i = 0; i < kSeverityCount; ++i) {
      out.recordsDroppedTotal[i] = dropped_[i].load(std::memory_order_relaxed);
    }
    out.highPriorityDropsTotal = high_priority_drops_.load(std::memory_order_relaxed);
    out.recordsTruncatedTotal = truncated_.load(std::memory_order_relaxed);
    out.formatFailuresTotal = format_failures_.load(std::memory_order_relaxed);
    out.fatalEmergencyTotal = fatal_emergency_.load(std::memory_order_relaxed);
    out.reentrantEmergencyTotal = reentrant_emergency_.load(std::memory_order_relaxed);
    out.rejectedAfterStopTotal = rejected_after_stop_.load(std::memory_order_relaxed);
    out.notDrainedAtStopTotal = not_drained_.load(std::memory_order_relaxed);
    out.rotationsTotal = rotations_.load(std::memory_order_relaxed);
    out.writeLatencyMs = write_latency_.snapshot();
    return out;
  }

 private:
  std::atomic<std::uint64_t> queue_depth_{0};
  std::array<std::atomic<bool>, kSinkCount> sink_health_{};

  std::atomic<std::uint64_t> submitted_{0};
  std::atomic<std::uint64_t> filtered_{0};
  std::atomic<std::uint64_t> enqueued_{0};
  std::array<std::atomic<std::uint64_t>, kSeverityCount> dropped_{};
  std::atomic<std::uint64_t> high_priority_drops_{0};
  std::atomic<std::uint64_t> truncated_{0};
  std::atomic<std::uint64_t> format_failures_{0};
  std::atomic<std::uint64_t> fatal_emergency_{0};
  std::atomic<std::uint64_t> reentrant_emergency_{0};
  std::atomic<std::uint64_t> rejected_after_stop_{0};
  std::atomic<std::uint64_t> not_drained_{0};
  std::array<std::atomic<std::uint64_t>, kSinkCount> written_{};
  std::array<std::atomic<std::uint64_t>, kSinkCount> sink_errors_{};
  std::atomic<std::uint64_t> rotations_{0};
  AtomicHistogram write_latency_;

 public:
  // Sink health starts Healthy; std::atomic<bool> has no initializer in an
  // array, so it is set once at construction.
  Metrics() {
    for (std::size_t i = 0; i < kSinkCount; ++i) {
      sink_health_[i].store(true, std::memory_order_relaxed);
    }
  }
};

}  // namespace tcu::logging::internal

#pragma once

// Bounded FIFO of callbacks ready to run. Strict FIFO: no priorities in v1
// (research R-02). Invariants Q1 and Q4 of data-model.md.

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>

#include "tcu/scheduler/scheduler_types.hpp"
#include "timer_record.hpp"

namespace tcu::scheduler::detail {

struct ReadyJob {
  std::shared_ptr<TimerRecord> record;
  /// Generation at dispatch time; a mismatch at the worker means the timer was
  /// cancelled while queued.
  std::uint64_t generation{0};
  Callback callback;
  /// The deadline this job is for: the lateness baseline.
  Clock::time_point deadline{};
};

class ReadyQueue {
 public:
  explicit ReadyQueue(std::size_t capacity) : capacity_(capacity) {}

  std::size_t capacity() const { return capacity_; }
  std::size_t size() const { return jobs_.size(); }
  bool empty() const { return jobs_.empty(); }
  bool full() const { return jobs_.size() >= capacity_; }

  /// Returns false when full, storing nothing: the caller leaves the timer
  /// scheduled and retries (research R-10).
  bool push(ReadyJob&& job);

  /// Precondition: not empty.
  ReadyJob pop();

  /// Drops every queued job without running it. Used by stop(Discard); the
  /// caller counts what it dropped.
  void clear() { jobs_.clear(); }

 private:
  std::deque<ReadyJob> jobs_;
  std::size_t capacity_;
};

}  // namespace tcu::scheduler::detail

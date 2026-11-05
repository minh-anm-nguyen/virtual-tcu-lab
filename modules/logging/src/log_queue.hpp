#pragma once

// The bounded MPSC queue of spec section 10.3.
//
// shortcut: a mutex and two condition variables, not a lock-free ring. Spec
// section 25.5 fixes this for v1 ("bounded mutex/CV queue first; optimize only
// after profiling"). Upgrade when a profile shows this mutex is the bottleneck
// at the 10,000 records/s target, not before (research R-08).
//
// The one optimisation taken up front is the batch drain: the writer swaps the
// whole deque out under the lock and formats and writes with the lock released,
// so formatting never blocks a producer.

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>

#include "tcu/logging/log_types.hpp"

namespace tcu::logging::internal {

class LogQueue {
 public:
  explicit LogQueue(std::size_t capacity);

  // Enqueues the record, waiting at most waitBudget for space. A zero budget
  // means no wait at all, which is the policy for Trace, Debug and Info; the
  // bounded budget is the policy for Warn and Error (BC-17, BC-18).
  // Returns false when the record was not admitted - a full queue, or
  // admission closed by stop().
  bool push(LogRecord&& record, std::chrono::milliseconds waitBudget);

  // Waits up to timeout for work and takes everything queued in one swap.
  // An empty result means the timeout expired, which is the writer chance to
  // run periodic work such as a sink recovery probe.
  std::deque<LogRecord> drain(std::chrono::milliseconds timeout);

  // Takes whatever is queued without waiting. Used by the shutdown drain.
  std::deque<LogRecord> drainNow();

  // After this, push() always fails. Idempotent.
  void closeAdmission();

  // Wakes the writer even with an empty queue (stop, flush).
  void wake();

  std::size_t depth() const;

  // Sequence numbers, so flush() can wait for exactly the records that were
  // already queued when it was called.
  std::uint64_t enqueuedCount() const;
  void markProcessed(std::size_t count);
  bool waitProcessed(std::uint64_t target, std::chrono::milliseconds timeout);

 private:
  mutable std::mutex mutex_;
  std::condition_variable writer_cv_;
  std::condition_variable space_cv_;
  std::condition_variable processed_cv_;

  std::deque<LogRecord> queue_;
  const std::size_t capacity_;
  bool closed_{false};
  std::uint64_t enqueued_{0};
  std::uint64_t processed_{0};
};

}  // namespace tcu::logging::internal

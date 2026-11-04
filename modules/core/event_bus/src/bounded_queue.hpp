#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>

#include "tcu/eventbus/event.hpp"

namespace tcu::eventbus::detail {

enum class EnqueueStatus { Accepted, Full, TimedOut, Closed };

struct EnqueueResult {
  EnqueueStatus status = EnqueueStatus::Closed;
  std::uint64_t id = 0;
};

// Private to src/. Publishing this header would widen the contract that
// projects 05-30 depend on (plan.md Structure Decision 2).
class BoundedQueue {
 public:
  explicit BoundedQueue(std::size_t capacity);

  // The queue is the enqueue authority: it assigns the event id under its own
  // mutex, so ids follow enqueue order and a refused publish consumes none
  // (invariant E3).

  // Reject policy: never waits.
  EnqueueResult push(Event event);

  // BlockUntil policy: waits for a free slot until an absolute deadline.
  // A deadline already in the past degenerates to a poll (research R-11).
  EnqueueResult push_until(Event event, std::chrono::steady_clock::time_point deadline);

  // Blocks until an event is available, or until the queue is closed and
  // empty. Returning items after close() is what makes Drain work.
  std::optional<Event> pop();

  // Sets the closed flag and wakes every waiter, so blocked publishers return
  // Closed rather than hanging (guarantee G16). Queued items are kept, which
  // is what lets the worker finish the backlog under Drain.
  void close();

  // Discard: closes, drops everything still queued, and reports how many were
  // dropped so they can be counted in discarded_on_stop_total.
  std::size_t close_and_discard();

  std::size_t depth() const;
  std::size_t high_watermark() const;
  std::size_t capacity() const noexcept { return capacity_; }

 private:
  // Caller must hold mutex_ and have checked for room and closed state.
  std::uint64_t enqueue_locked(Event event);

  mutable std::mutex mutex_;
  std::condition_variable not_empty_;
  std::condition_variable not_full_;
  std::deque<Event> items_;
  const std::size_t capacity_;
  std::uint64_t next_id_ = 0;
  std::size_t high_watermark_ = 0;
  bool closed_ = false;
};

}  // namespace tcu::eventbus::detail

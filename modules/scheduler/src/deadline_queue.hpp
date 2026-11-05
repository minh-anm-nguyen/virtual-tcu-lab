#pragma once

// Min-heap of deadlines with lazy invalidation by generation counter, plus
// threshold compaction (research R-11). Invariants H1-H3 of data-model.md.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "tcu/scheduler/scheduler_types.hpp"
#include "timer_record.hpp"

namespace tcu::scheduler::detail {

struct HeapEntry {
  Clock::time_point deadline;
  TimerId id;
  std::uint64_t generation;
};

class DeadlineQueue {
 public:
  void push(const HeapEntry& entry);

  bool empty() const { return heap_.empty(); }
  std::size_t size() const { return heap_.size(); }

  /// Precondition: not empty, and pruneAndCompact has been called since the
  /// last mutation, so the head is live.
  const HeapEntry& peek() const { return heap_.front(); }
  void pop();

  /// A cancel or a rearm just invalidated one entry somewhere in the heap.
  void noteStale() { ++staleEstimate_; }

  /// Drops every stale entry from the head, then rebuilds the heap when stale
  /// entries dominate - the churn fault of spec section 14 leaves them buried
  /// where head pruning can never reach them.
  void pruneAndCompact(const Registry& registry);

 private:
  /// Ordered by (deadline, id) ascending, so equal deadlines dispatch in id
  /// order and a fake-clock test can assert a sequence, not just a set.
  static bool later(const HeapEntry& a, const HeapEntry& b);
  static bool isStale(const HeapEntry& entry, const Registry& registry);
  void compact(const Registry& registry);

  /// Internal, not Config: spec section 9 fixes Config at three fields and no
  /// requirement names a compaction knob (Principle I).
  static constexpr std::size_t kCompactionFloor = 64;

  std::vector<HeapEntry> heap_;
  std::size_t staleEstimate_{0};
};

}  // namespace tcu::scheduler::detail

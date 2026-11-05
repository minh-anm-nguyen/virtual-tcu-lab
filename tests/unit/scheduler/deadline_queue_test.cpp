// Invariants H1, H2, H3 against the private DeadlineQueue.

#include <gtest/gtest.h>

#include <memory>

#include "deadline_queue.hpp"
#include "timer_record.hpp"

using tcu::scheduler::Clock;
using tcu::scheduler::TimerId;
using tcu::scheduler::detail::DeadlineQueue;
using tcu::scheduler::detail::HeapEntry;
using tcu::scheduler::detail::Registry;
using tcu::scheduler::detail::TimerRecord;
using namespace std::chrono_literals;

namespace {

std::shared_ptr<TimerRecord> liveRecord(Registry& reg, TimerId id,
                                        std::uint64_t generation) {
  auto rec = std::make_shared<TimerRecord>();
  rec->id = id;
  rec->generation = generation;
  reg[id] = rec;
  return rec;
}

}  // namespace

TEST(DeadlineQueue, EarliestWins) {  // H1
  const Clock::time_point base{};
  Registry reg;
  DeadlineQueue q;
  for (TimerId id : {TimerId{1}, TimerId{2}, TimerId{3}}) liveRecord(reg, id, 1);
  q.push({base + 300ms, 1, 1});
  q.push({base + 100ms, 2, 1});
  q.push({base + 200ms, 3, 1});

  q.pruneAndCompact(reg);
  EXPECT_EQ(q.peek().id, 2u);
  q.pop();
  q.pruneAndCompact(reg);
  EXPECT_EQ(q.peek().id, 3u);
  q.pop();
  q.pruneAndCompact(reg);
  EXPECT_EQ(q.peek().id, 1u);
}

TEST(DeadlineQueue, TieBreaksOnId) {  // deterministic dispatch order
  const Clock::time_point base{};
  Registry reg;
  DeadlineQueue q;
  for (TimerId id : {TimerId{7}, TimerId{3}, TimerId{5}}) liveRecord(reg, id, 1);
  q.push({base + 100ms, 7, 1});
  q.push({base + 100ms, 3, 1});
  q.push({base + 100ms, 5, 1});

  q.pruneAndCompact(reg);
  EXPECT_EQ(q.peek().id, 3u);
  q.pop();
  q.pruneAndCompact(reg);
  EXPECT_EQ(q.peek().id, 5u);
  q.pop();
  q.pruneAndCompact(reg);
  EXPECT_EQ(q.peek().id, 7u);
}

TEST(DeadlineQueue, StaleEntryIgnored) {  // H2
  const Clock::time_point base{};
  Registry reg;
  DeadlineQueue q;

  // id 1: record erased (cancelled). id 2: generation bumped (rearmed).
  liveRecord(reg, 1, 1);
  auto rearmed = liveRecord(reg, 2, 1);
  auto live = liveRecord(reg, 3, 1);
  q.push({base + 10ms, 1, 1});
  q.push({base + 20ms, 2, 1});
  q.push({base + 30ms, 3, 1});

  reg.erase(1);
  rearmed->generation = 2;
  q.noteStale();
  q.noteStale();

  q.pruneAndCompact(reg);
  ASSERT_FALSE(q.empty());
  EXPECT_EQ(q.peek().id, 3u) << "both stale heads must be discarded";
  EXPECT_EQ(live->generation, 1u);
}

TEST(DeadlineQueue, EmptyAfterEveryEntryGoesStale) {  // H2
  const Clock::time_point base{};
  Registry reg;
  DeadlineQueue q;
  for (TimerId id = 1; id <= 5; ++id) {
    liveRecord(reg, id, 1);
    q.push({base + 10ms * static_cast<int>(id), id, 1});
  }
  reg.clear();
  for (int i = 0; i < 5; ++i) q.noteStale();

  q.pruneAndCompact(reg);
  EXPECT_TRUE(q.empty());
  EXPECT_EQ(q.size(), 0u);
}

TEST(DeadlineQueue, CompactionBoundsSize) {  // H3
  // The churn fault of spec section 14: stale entries that are not at the head
  // are never pruned lazily, so without compaction the heap grows without
  // bound.
  const Clock::time_point base{};
  Registry reg;
  DeadlineQueue q;

  constexpr int kLive = 100;
  for (TimerId id = 1; id <= kLive; ++id) {
    liveRecord(reg, id, 1);
    // Soonest, so a live entry is always at the head and head pruning can
    // never reach the stale ones piling up behind it.
    q.push({base + 10ms, id, 1});
  }

  TimerId next = kLive + 1;
  for (int cycle = 0; cycle < 10000; ++cycle) {
    liveRecord(reg, next, 1);
    q.push({base + 1h, next, 1});
    reg.erase(next);
    q.noteStale();
    ++next;
    q.pruneAndCompact(reg);
  }

  EXPECT_LE(q.size(), static_cast<std::size_t>(2 * kLive + 64))
      << "heap size must stay O(live timers)";
}

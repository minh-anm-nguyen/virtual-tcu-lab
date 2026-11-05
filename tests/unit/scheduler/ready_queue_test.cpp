// Invariants Q1 and Q4 against the private bounded ready queue.

#include <gtest/gtest.h>

#include <memory>
#include <vector>

#include "ready_queue.hpp"
#include "timer_record.hpp"

using tcu::scheduler::Clock;
using tcu::scheduler::detail::ReadyJob;
using tcu::scheduler::detail::ReadyQueue;
using tcu::scheduler::detail::TimerRecord;

namespace {

ReadyJob job(int tag, std::vector<int>* sink) {
  auto rec = std::make_shared<TimerRecord>();
  rec->id = static_cast<tcu::scheduler::TimerId>(tag);
  rec->generation = 1;
  ReadyJob j;
  j.record = rec;
  j.generation = 1;
  j.deadline = Clock::time_point{};
  j.callback = [tag, sink] { sink->push_back(tag); };
  return j;
}

}  // namespace

TEST(ReadyQueue, Fifo) {  // Q4
  std::vector<int> ran;
  ReadyQueue q(8);
  for (int tag = 1; tag <= 4; ++tag) ASSERT_TRUE(q.push(job(tag, &ran)));

  while (!q.empty()) {
    ReadyJob j = q.pop();
    j.callback();
  }
  EXPECT_EQ(ran, (std::vector<int>{1, 2, 3, 4}));
}

TEST(ReadyQueue, NeverExceedsCapacity) {  // Q1
  std::vector<int> ran;
  ReadyQueue q(3);
  EXPECT_EQ(q.capacity(), 3u);
  for (int tag = 1; tag <= 3; ++tag) ASSERT_TRUE(q.push(job(tag, &ran)));
  EXPECT_TRUE(q.full());
  EXPECT_EQ(q.size(), 3u);
}

TEST(ReadyQueue, TryPushReportsFullWithoutGrowing) {
  std::vector<int> ran;
  ReadyQueue q(2);
  ASSERT_TRUE(q.push(job(1, &ran)));
  ASSERT_TRUE(q.push(job(2, &ran)));
  EXPECT_FALSE(q.push(job(3, &ran)));
  EXPECT_EQ(q.size(), 2u) << "a refused push must store nothing";

  ReadyJob first = q.pop();
  first.callback();
  EXPECT_EQ(q.size(), 1u);
  EXPECT_FALSE(q.full());
  EXPECT_TRUE(q.push(job(4, &ran)));
  EXPECT_EQ(ran, (std::vector<int>{1}));
}

TEST(ReadyQueue, ClearDropsEverything) {
  std::vector<int> ran;
  ReadyQueue q(4);
  for (int tag = 1; tag <= 4; ++tag) ASSERT_TRUE(q.push(job(tag, &ran)));
  q.clear();
  EXPECT_TRUE(q.empty());
  EXPECT_EQ(q.size(), 0u);
  EXPECT_TRUE(ran.empty()) << "clear() must not run the callbacks it drops";
}

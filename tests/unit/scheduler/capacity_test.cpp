// UT-11 (B-11) and invariants Q1, Q3.

#include <gtest/gtest.h>

#include "support/fixture.hpp"

using namespace std::chrono_literals;
using tcu::scheduler::Config;
using tcu::scheduler::Error;
using tcu::scheduler::StopMode;
using Capacity = tcu::test::FakeClockTest;

TEST_F(Capacity, RejectsBeyondMaxTimers) {  // UT-11, B-11, Q3
  auto s = started(Config{1, 4, 8});
  for (int i = 0; i < 4; ++i) {
    ASSERT_TRUE(s->scheduleAfter(1h, [this] { rec_.record(); })) << "timer " << i;
  }
  const auto r = s->scheduleAfter(1h, [this] { rec_.record(); });
  EXPECT_EQ(r.error, Error::CapacityExceeded);
  EXPECT_EQ(r.id, 0u) << "a refused schedule must register no timer";

  const auto m = s->metrics();
  EXPECT_EQ(m.timers_active, 4u);
  EXPECT_EQ(m.timers_scheduled_total, 4u);
  EXPECT_EQ(m.rejected_capacity_total, 1u);
  EXPECT_EQ(m.rejected_invalid_argument_total, 0u);
  s->stop(StopMode::Discard);
}

TEST_F(Capacity, SlotReleasedAfterCallbackCompletes) {
  auto s = started(Config{1, 1, 8});
  ASSERT_TRUE(s->scheduleAfter(10ms, [this] { rec_.record(); }));
  EXPECT_EQ(s->scheduleAfter(10ms, [this] { rec_.record(); }).error,
            Error::CapacityExceeded);

  clock_->advance(10ms);
  rec_.waitFor(1);
  // The slot is free once the callback finished, not when it was dispatched.
  ASSERT_TRUE(waitForActive(*s, 0));
  EXPECT_TRUE(s->scheduleAfter(10ms, [this] { rec_.record(); }));
  s->stop(StopMode::Discard);
}

TEST_F(Capacity, ReadyQueueDepthNeverExceedsMaxReadyTasks) {  // Q1
  auto s = started(Config{1, 64, 2});
  for (int i = 0; i < 16; ++i) {
    ASSERT_TRUE(s->scheduleAfter(10ms, [this] { rec_.record(); }));
  }
  clock_->advance(10ms);
  rec_.waitFor(16);
  EXPECT_LE(s->metrics().ready_queue_depth, 2u);
  s->stop(StopMode::Drain);
  EXPECT_EQ(s->metrics().ready_queue_depth, 0u);
}

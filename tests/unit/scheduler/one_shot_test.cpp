// UT-03 (B-03), FR-02 and invariants T1, T2.

#include <gtest/gtest.h>

#include <vector>

#include "support/fixture.hpp"

using namespace std::chrono_literals;
using tcu::scheduler::Config;
using tcu::scheduler::StopMode;
using OneShot = tcu::test::FakeClockTest;

TEST_F(OneShot, ZeroDelayIsImmediate) {  // UT-03, B-03
  auto s = started();
  ASSERT_TRUE(s->scheduleAfter(0ms, [this] { rec_.record(); }));
  rec_.waitFor(1);  // no clock advance needed
  EXPECT_EQ(rec_.count(), 1u);
  s->stop(StopMode::Discard);
}

TEST_F(OneShot, StartsExactlyOnce) {  // T1
  auto s = started();
  ASSERT_TRUE(s->scheduleAfter(100ms, [this] { rec_.record(); }));
  clock_->advance(100ms);
  rec_.waitFor(1);
  clock_->advance(10s);  // repeated advances must not re-fire it
  EXPECT_TRUE(rec_.stayedBelow(2));
  EXPECT_EQ(rec_.count(), 1u);
  s->stop(StopMode::Discard);
}

TEST_F(OneShot, NotBeforeDeadline) {  // T2
  auto s = started();
  ASSERT_TRUE(s->scheduleAfter(100ms, [this] { rec_.record(); }));
  clock_->advance(99ms);
  EXPECT_TRUE(rec_.stayedBelow(1)) << "a callback must not start before its deadline";
  clock_->advance(1ms);
  rec_.waitFor(1);
  EXPECT_EQ(rec_.count(), 1u);
  s->stop(StopMode::Discard);
}

TEST_F(OneShot, ScheduleAtFutureDeadline) {  // FR-02
  auto s = started();
  const auto deadline = clock_->now() + 250ms;
  ASSERT_TRUE(s->scheduleAt(deadline, [this] { rec_.record(); }));
  clock_->advance(249ms);
  EXPECT_TRUE(rec_.stayedBelow(1));
  clock_->advanceTo(deadline);
  rec_.waitFor(1);
  s->stop(StopMode::Discard);
}

TEST_F(OneShot, ScheduleAtPastDeadlineIsImmediate) {  // FR-02
  auto s = started();
  ASSERT_TRUE(s->scheduleAt(clock_->now() - 5s, [this] { rec_.record(); }));
  rec_.waitFor(1);
  EXPECT_EQ(rec_.count(), 1u);
  s->stop(StopMode::Discard);
}

TEST_F(OneShot, EqualDeadlinesRunInIdOrder) {  // H1's tiebreak
  // One worker, so dispatch order is observable as completion order.
  auto s = started(Config{1, 100, 16});
  for (int tag = 1; tag <= 4; ++tag) {
    ASSERT_TRUE(s->scheduleAfter(50ms, [this, tag] { rec_.record(tag); }));
  }
  clock_->advance(50ms);
  rec_.waitFor(4);
  EXPECT_EQ(rec_.tags(), (std::vector<int>{1, 2, 3, 4}));
  s->stop(StopMode::Discard);
}

TEST_F(OneShot, RecordReleasedAfterCompletion) {  // research R-03 accounting
  auto s = started();
  ASSERT_TRUE(s->scheduleAfter(10ms, [this] { rec_.record(); }));
  EXPECT_EQ(s->metrics().timers_active, 1u);
  clock_->advance(10ms);
  rec_.waitFor(1);
  // The gauge drops once the callback has finished, not when it was dispatched.
  EXPECT_TRUE(waitForActive(*s, 0));
  s->stop(StopMode::Discard);
}

TEST_F(OneShot, LatenessIsSampledOncePerStart) {  // M3 for the one-shot path
  auto s = started();
  ASSERT_TRUE(s->scheduleAfter(10ms, [this] { rec_.record(); }));
  clock_->advance(10ms);
  rec_.waitFor(1);
  s->stop(StopMode::Drain);
  const auto m = s->metrics();
  EXPECT_EQ(m.callbacks_started_total, 1u);
  EXPECT_EQ(m.start_lateness_ms.count, 1u);
}

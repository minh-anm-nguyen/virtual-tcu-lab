// UT-05, UT-06, UT-07, UT-13 (B-05, B-06, B-07, B-16) and invariants T3, T5.
// The resolution table under test is research R-07 /
// contracts/scheduler_api.md section 2.5.

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <limits>
#include <vector>

#include "support/fixture.hpp"
#include "support/test_gate.hpp"

using namespace std::chrono_literals;
using tcu::scheduler::CancelResult;
using tcu::scheduler::Config;
using tcu::scheduler::PeriodicMode;
using tcu::scheduler::StopMode;
using tcu::scheduler::TimerId;
using Cancel = tcu::test::FakeClockTest;

TEST_F(Cancel, Scheduled) {  // UT-05, B-05, T3
  auto s = started();
  const TimerId id = s->scheduleAfter(1h, [this] { rec_.record(); }).id;
  ASSERT_NE(id, 0u);
  ASSERT_EQ(s->metrics().timers_active, 1u);

  EXPECT_EQ(s->cancel(id), CancelResult::Cancelled);
  EXPECT_EQ(s->metrics().timers_active, 0u) << "the capacity slot is released";
  EXPECT_EQ(s->metrics().timers_cancelled_total, 1u);

  clock_->advance(2h);
  EXPECT_TRUE(rec_.stayedBelow(1)) << "a cancelled timer never starts";
  s->stop(StopMode::Discard);
}

TEST_F(Cancel, Queued) {  // UT-06, B-06, T3
  // One worker, held by a gate, so the second timer sits in the ready queue
  // with its callback not yet claimed.
  auto s = started(Config{1, 8, 4});
  tcu::test::TestGate gate;
  ASSERT_TRUE(s->scheduleAfter(0ms, [&] {
    gate.wait_until_opened();
    rec_.record(1);
  }));
  gate.wait_until_entered();

  const TimerId queued = s->scheduleAfter(0ms, [this] { rec_.record(2); }).id;
  ASSERT_NE(queued, 0u);
  ASSERT_TRUE(waitForMetric([&] { return s->metrics().ready_queue_depth; }, 1));

  EXPECT_EQ(s->cancel(queued), CancelResult::Cancelled);
  gate.open();
  rec_.waitFor(1);
  ASSERT_TRUE(waitForMetric(
      [&] { return s->metrics().callbacks_skipped_cancelled_total; }, 1));

  EXPECT_TRUE(rec_.stayedBelow(2));
  EXPECT_EQ(rec_.tags(), (std::vector<int>{1}));
  s->stop(StopMode::Discard);
}

TEST_F(Cancel, Running) {  // UT-07, B-07
  auto s = started(Config{1, 8, 4});
  tcu::test::TestGate gate;
  const TimerId id = s->scheduleAfter(0ms, [&] {
                          gate.wait_until_opened();
                          rec_.record();
                        }).id;
  ASSERT_NE(id, 0u);
  gate.wait_until_entered();

  EXPECT_EQ(s->cancel(id), CancelResult::RunningCancelRequested);
  EXPECT_EQ(s->cancel(id), CancelResult::AlreadyFinished)
      << "a repeated cancel of a cancel-requested timer is stable";

  gate.open();
  rec_.waitFor(1);  // the callback is not interrupted; it runs to completion
  ASSERT_TRUE(waitForFinished(*s, 1));
  EXPECT_EQ(s->metrics().callbacks_completed_total, 1u);
  EXPECT_EQ(s->cancel(id), CancelResult::AlreadyFinished);
  s->stop(StopMode::Discard);
}

TEST_F(Cancel, RunningPeriodicIsNotRearmed) {  // flow 12.5
  auto s = started(Config{1, 8, 4});
  tcu::test::TestGate gate;
  const TimerId id = s->scheduleEvery(10ms, PeriodicMode::FixedDelay, [&] {
                          gate.wait_until_opened();
                          rec_.record();
                        }).id;
  ASSERT_NE(id, 0u);
  clock_->advance(10ms);
  gate.wait_until_entered();

  EXPECT_EQ(s->cancel(id), CancelResult::RunningCancelRequested);
  gate.open();
  rec_.waitFor(1);
  ASSERT_TRUE(waitForFinished(*s, 1));
  EXPECT_TRUE(waitForActive(*s, 0));

  clock_->advance(1h);
  EXPECT_TRUE(rec_.stayedBelow(2)) << "a cancel-requested periodic timer is not rearmed";
  s->stop(StopMode::Discard);
}

TEST_F(Cancel, FromInsideOwnCallbackStopsPeriodic) {
  auto s = started(Config{1, 8, 4});
  std::atomic<TimerId> self{0};
  std::atomic<int> result{-1};
  const TimerId id = s->scheduleEvery(1h, PeriodicMode::FixedDelay, [&] {
                          result = static_cast<int>(s->cancel(self.load()));
                          rec_.record();
                        }).id;
  ASSERT_NE(id, 0u);
  self = id;

  clock_->advance(1h);
  rec_.waitFor(1);
  ASSERT_TRUE(waitForFinished(*s, 1));
  EXPECT_EQ(result.load(), static_cast<int>(CancelResult::RunningCancelRequested));

  clock_->advance(10h);
  EXPECT_TRUE(rec_.stayedBelow(2)) << "cancelling itself is how a periodic callback stops";
  s->stop(StopMode::Discard);
}

TEST_F(Cancel, IdSemantics) {  // UT-13, B-16, research R-07
  auto s = started();
  const TimerId cancelled = s->scheduleAfter(1h, [this] { rec_.record(); }).id;
  ASSERT_NE(cancelled, 0u);
  EXPECT_EQ(s->cancel(cancelled), CancelResult::Cancelled);
  EXPECT_EQ(s->cancel(cancelled), CancelResult::AlreadyFinished);
  EXPECT_EQ(s->cancel(cancelled), CancelResult::AlreadyFinished)
      << "the answer never changes with time; there is no retention window";

  const TimerId completed = s->scheduleAfter(0ms, [this] { rec_.record(); }).id;
  rec_.waitFor(1);
  ASSERT_TRUE(waitForFinished(*s, 1));
  EXPECT_EQ(s->cancel(completed), CancelResult::AlreadyFinished);
  s->stop(StopMode::Discard);
}

TEST_F(Cancel, ZeroAndNeverIssuedAreNotFound) {
  auto s = started();
  EXPECT_EQ(s->cancel(0), CancelResult::NotFound) << "0 is never issued";
  EXPECT_EQ(s->cancel(std::numeric_limits<TimerId>::max()), CancelResult::NotFound);
  const TimerId id = s->scheduleAfter(1h, [this] { rec_.record(); }).id;
  EXPECT_EQ(s->cancel(id + 1), CancelResult::NotFound) << "not yet handed out";
  s->stop(StopMode::Discard);
}

TEST_F(Cancel, IdsAreStrictlyIncreasingAndNeverReused) {  // T5
  auto s = started();
  TimerId previous = 0;
  for (int i = 0; i < 20; ++i) {
    const TimerId id = s->scheduleAfter(1h, [this] { rec_.record(); }).id;
    ASSERT_NE(id, 0u);
    EXPECT_GT(id, previous);
    previous = id;
    EXPECT_EQ(s->cancel(id), CancelResult::Cancelled);
  }
  EXPECT_EQ(s->metrics().timers_cancelled_total, 20u);
  s->stop(StopMode::Discard);
}

TEST_F(Cancel, AfterStopIsStillDefined) {
  auto s = started();
  const TimerId id = s->scheduleAfter(1h, [this] { rec_.record(); }).id;
  s->stop(StopMode::Discard);
  EXPECT_EQ(s->cancel(id), CancelResult::AlreadyFinished)
      << "stop() cancelled it, so the id is issued-but-gone";
  EXPECT_EQ(s->cancel(0), CancelResult::NotFound);
}

// FR-11, invariant S1 and flow 12.15: a callback runs with no scheduler lock
// held, so it may call back into the scheduler. Every assertion here is "the
// test finishes" - a nested lock shows up as the CTest timeout.

#include <gtest/gtest.h>

#include <atomic>

#include "support/fixture.hpp"
#include "support/test_gate.hpp"

using namespace std::chrono_literals;
using tcu::scheduler::CancelResult;
using tcu::scheduler::Config;
using tcu::scheduler::StopMode;
using tcu::scheduler::TimerId;
using Lock = tcu::test::FakeClockTest;

TEST_F(Lock, CallbackCanScheduleAndCancel) {  // FR-11, S1, flow 12.15
  auto s = started();
  std::atomic<bool> nested_scheduled{false};
  std::atomic<int> cancel_result{-1};

  TimerId victim = s->scheduleAfter(1h, [this] { rec_.record(99); }).id;
  ASSERT_NE(victim, 0u);

  ASSERT_TRUE(s->scheduleAfter(10ms, [&] {
    nested_scheduled = static_cast<bool>(s->scheduleAfter(0ms, [this] { rec_.record(2); }));
    cancel_result = static_cast<int>(s->cancel(victim));
    rec_.record(1);
  }));

  clock_->advance(10ms);
  rec_.waitFor(2);
  EXPECT_TRUE(nested_scheduled.load());
  EXPECT_EQ(cancel_result.load(), static_cast<int>(CancelResult::Cancelled));
  s->stop(StopMode::Discard);
}

TEST_F(Lock, CallbackCanReadMetrics) {
  auto s = started();
  std::atomic<std::uint64_t> seen{0};
  ASSERT_TRUE(s->scheduleAfter(10ms, [&] {
    seen = s->metrics().callbacks_started_total;
    rec_.record();
  }));
  clock_->advance(10ms);
  rec_.waitFor(1);
  EXPECT_GE(seen.load(), 1u);
  s->stop(StopMode::Discard);
}

TEST_F(Lock, StopFromCallbackDoesNotDeadlock) {  // research R-12
  auto s = started();
  ASSERT_TRUE(s->scheduleAfter(10ms, [&] {
    s->stop(StopMode::Discard);  // precondition violation: logged no-op
    rec_.record();
  }));
  clock_->advance(10ms);
  rec_.waitFor(1);

  // The scheduler is untouched, so a normal stop from the owning thread still
  // works and still joins.
  EXPECT_TRUE(s->scheduleAfter(0ms, [this] { rec_.record(); }));
  rec_.waitFor(2);
  s->stop(StopMode::Discard);
}

TEST_F(Lock, SlowCallbackDoesNotBlockAnotherWorker) {  // flow 12.14
  auto s = started(Config{2, 16, 8});
  tcu::test::TestGate gate;
  ASSERT_TRUE(s->scheduleAfter(10ms, [&] {
    gate.wait_until_opened();
    rec_.record(1);
  }));
  clock_->advance(10ms);
  gate.wait_until_entered();

  ASSERT_TRUE(s->scheduleAfter(0ms, [this] { rec_.record(2); }));
  rec_.waitFor(1);  // the second worker runs while the first is blocked
  EXPECT_EQ(rec_.tags().front(), 2);

  gate.open();
  rec_.waitFor(2);
  s->stop(StopMode::Discard);
}

// UT-01, UT-02, UT-12 (B-01, B-02, B-15) and invariants S2, S3, S6.

#include <gtest/gtest.h>

#include "support/fixture.hpp"

using namespace std::chrono_literals;
using tcu::scheduler::Config;
using tcu::scheduler::Error;
using tcu::scheduler::StopMode;
using Lifecycle = tcu::test::FakeClockTest;

TEST_F(Lifecycle, StartOnce) {  // UT-01, B-01
  auto s = make();
  EXPECT_EQ(s->start(), Error::None);
  EXPECT_TRUE(s->scheduleAfter(1s, [this] { rec_.record(); }));
  s->stop(StopMode::Discard);
}

TEST_F(Lifecycle, StartTwiceIsInvalidState) {  // UT-02, B-02
  auto s = started();
  EXPECT_EQ(s->start(), Error::InvalidState);
  EXPECT_EQ(s->start(), Error::InvalidState);
  s->stop(StopMode::Discard);
}

TEST_F(Lifecycle, StartWithZeroConfigFieldIsInvalidArgument) {
  for (const Config cfg : {Config{0, 10, 10}, Config{1, 0, 10}, Config{1, 10, 0}}) {
    auto s = make(cfg);
    EXPECT_EQ(s->start(), Error::InvalidArgument);
    // Still Created, so a corrected start would be possible; no thread exists.
    EXPECT_EQ(s->metrics().timers_active, 0u);
  }
}

TEST_F(Lifecycle, ScheduleAfterStop) {  // UT-12, B-15
  auto s = started();
  s->stop(StopMode::Discard);
  const auto r = s->scheduleAfter(1s, [this] { rec_.record(); });
  EXPECT_FALSE(r);
  EXPECT_EQ(r.error, Error::InvalidState);
  EXPECT_EQ(r.id, 0u);
  EXPECT_EQ(s->metrics().rejected_invalid_state_total, 1u);
}

TEST_F(Lifecycle, StopIdempotent) {
  auto s = started();
  s->stop(StopMode::Discard);
  s->stop(StopMode::Discard);
  s->stop(StopMode::Drain);
  EXPECT_EQ(s->metrics().ready_queue_depth, 0u);
}

TEST_F(Lifecycle, StopOnCreatedIsNoOp) {
  auto s = make();
  s->stop(StopMode::Drain);  // nothing was ever started; must not hang
  EXPECT_EQ(s->metrics().timers_active, 0u);
}

TEST_F(Lifecycle, StartAfterStopIsInvalidState) {
  // The scheduler is single-use (spec section 8.1 V1 policy).
  auto s = started();
  s->stop(StopMode::Discard);
  EXPECT_EQ(s->start(), Error::InvalidState);
}

TEST_F(Lifecycle, StopJoinsEveryThread) {  // S2
  // The assertion is that stop() returns at all: a detached or unjoined thread
  // shows up as the CTest timeout, and a callback outliving stop() is caught by
  // NoCallbackAfterStop below.
  auto s = started(Config{4, 100, 16});
  for (int i = 0; i < 8; ++i) {
    ASSERT_TRUE(s->scheduleAfter(10ms * (i + 1), [this] { rec_.record(); }));
  }
  s->stop(StopMode::Discard);
  SUCCEED();
}

TEST_F(Lifecycle, NoCallbackAfterStop) {  // S6
  auto s = started();
  for (int i = 0; i < 10; ++i) {
    ASSERT_TRUE(s->scheduleAfter(1s, [this] { rec_.record(); }));
  }
  s->stop(StopMode::Discard);
  clock_->advance(1h);  // nothing is listening any more
  EXPECT_TRUE(rec_.stayedBelow(1));
  EXPECT_EQ(rec_.count(), 0u);
}

TEST_F(Lifecycle, DestructorStopsRunningScheduler) {
  {
    auto s = started();
    ASSERT_TRUE(s->scheduleAfter(1h, [this] { rec_.record(); }));
  }  // destructor must stop and join without hanging
  clock_->advance(2h);
  EXPECT_TRUE(rec_.stayedBelow(1));
}

TEST_F(Lifecycle, ClockOutlivesSchedulerSafely) {
  // Regression, found by ThreadSanitizer: attach() hands the clock the
  // coordinator mutex and condition variable, and an injected clock normally
  // outlives the scheduler. Without a matching detach(), an advance() after
  // destruction locks a freed mutex and notifies a freed condition variable.
  {
    auto s = started();
    ASSERT_TRUE(s->scheduleAfter(1h, [this] { rec_.record(); }));
    s->stop(StopMode::Discard);
  }
  clock_->advance(10h);
  EXPECT_TRUE(rec_.stayedBelow(1));

  // And again without an explicit stop, so the destructor path is covered too.
  {
    auto s = started();
    ASSERT_TRUE(s->scheduleAfter(1h, [this] { rec_.record(); }));
  }
  clock_->advance(10h);
  EXPECT_TRUE(rec_.stayedBelow(1));
}

TEST_F(Lifecycle, MetricsReadableInEveryState) {
  auto s = make();
  EXPECT_EQ(s->metrics().callbacks_started_total, 0u);
  ASSERT_EQ(s->start(), Error::None);
  EXPECT_EQ(s->metrics().timers_active, 0u);
  s->stop(StopMode::Drain);
  EXPECT_EQ(s->metrics().ready_queue_depth, 0u);
}

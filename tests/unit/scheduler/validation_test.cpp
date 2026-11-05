// UT-04 (B-04) and the five input rules of spec section 9.1.

#include <gtest/gtest.h>

#include "support/fixture.hpp"

using namespace std::chrono_literals;
using tcu::scheduler::Callback;
using tcu::scheduler::Error;
using tcu::scheduler::PeriodicMode;
using tcu::scheduler::StopMode;
using Validation = tcu::test::FakeClockTest;

TEST_F(Validation, EmptyCallbackIsInvalidArgument) {
  auto s = started();
  const auto r = s->scheduleAfter(1s, Callback{});
  EXPECT_EQ(r.error, Error::InvalidArgument);
  EXPECT_EQ(r.id, 0u);
  EXPECT_EQ(s->metrics().rejected_invalid_argument_total, 1u);
  EXPECT_EQ(s->metrics().timers_active, 0u);
  s->stop(StopMode::Discard);
}

TEST_F(Validation, NegativeDelayIsInvalidArgument) {  // UT-04, B-04
  auto s = started();
  const auto r = s->scheduleAfter(-1ms, [this] { rec_.record(); });
  EXPECT_EQ(r.error, Error::InvalidArgument);
  EXPECT_EQ(r.id, 0u);
  EXPECT_EQ(s->metrics().timers_scheduled_total, 0u);
  s->stop(StopMode::Discard);
}

TEST_F(Validation, PastAbsoluteDeadlineIsNotAnError) {
  // FR-02: an overdue deadline is immediately eligible, not invalid.
  auto s = started();
  EXPECT_TRUE(s->scheduleAt(clock_->now() - 1h, [this] { rec_.record(); }));
  rec_.waitFor(1);
  s->stop(StopMode::Discard);
}

TEST_F(Validation, NonPositiveIntervalIsInvalidArgument) {
  auto s = started();
  for (const auto interval : {tcu::scheduler::Clock::duration::zero(),
                              std::chrono::duration_cast<tcu::scheduler::Clock::duration>(-5ms)}) {
    const auto r = s->scheduleEvery(interval, PeriodicMode::FixedDelay,
                                    [this] { rec_.record(); });
    EXPECT_EQ(r.error, Error::InvalidArgument);
  }
  EXPECT_EQ(s->metrics().rejected_invalid_argument_total, 2u);
  s->stop(StopMode::Discard);
}

TEST_F(Validation, EmptyCallbackWinsOverStoppedState) {
  // Check order of contracts/scheduler_api.md section 2.2, so exactly one
  // rejection counter moves per call (M6).
  auto s = started();
  s->stop(StopMode::Discard);
  const auto r = s->scheduleAfter(1s, Callback{});
  EXPECT_EQ(r.error, Error::InvalidArgument);
  const auto m = s->metrics();
  EXPECT_EQ(m.rejected_invalid_argument_total, 1u);
  EXPECT_EQ(m.rejected_invalid_state_total, 0u);
}

TEST_F(Validation, NegativeDelayWinsOverStoppedState) {
  auto s = started();
  s->stop(StopMode::Discard);
  EXPECT_EQ(s->scheduleAfter(-1s, [this] { rec_.record(); }).error,
            Error::InvalidArgument);
  EXPECT_EQ(s->metrics().rejected_invalid_state_total, 0u);
}

TEST_F(Validation, ScheduleBeforeStartIsInvalidState) {
  auto s = make();
  EXPECT_EQ(s->scheduleAfter(1s, [this] { rec_.record(); }).error,
            Error::InvalidState);
  EXPECT_EQ(s->metrics().rejected_invalid_state_total, 1u);
}

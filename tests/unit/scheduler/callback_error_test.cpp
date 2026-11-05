// UT-10 (B-10), FR-09 and invariant T6: a throwing callback is caught, counted
// and isolated, and a periodic timer that throws is disabled rather than
// retried (research R-09).

#include <gtest/gtest.h>

#include <stdexcept>

#include "support/fixture.hpp"

using namespace std::chrono_literals;
using tcu::scheduler::Config;
using tcu::scheduler::PeriodicMode;
using tcu::scheduler::StopMode;
using CallbackError = tcu::test::FakeClockTest;

TEST_F(CallbackError, ThrowIsCountedAndWorkerSurvives) {  // UT-10, B-10
  auto s = started(Config{1, 16, 8});
  ASSERT_TRUE(s->scheduleAfter(10ms, [this] {
    rec_.record(1);
    throw std::runtime_error("callback failure");
  }));
  clock_->advance(10ms);
  rec_.waitFor(1);
  ASSERT_TRUE(waitForFinished(*s, 1));

  const auto m = s->metrics();
  EXPECT_EQ(m.callbacks_failed_total, 1u);
  EXPECT_EQ(m.callbacks_completed_total, 0u);
  EXPECT_EQ(m.callbacks_started_total, 1u);

  // The same worker must still serve the next timer. Recorder only proves the
  // callback started, so wait for the outcome before reading the counter -
  // found flaky under ThreadSanitizer, which widens that window.
  ASSERT_TRUE(s->scheduleAfter(0ms, [this] { rec_.record(2); }));
  rec_.waitFor(2);
  ASSERT_TRUE(waitForFinished(*s, 2));
  EXPECT_EQ(s->metrics().callbacks_completed_total, 1u);
  s->stop(StopMode::Discard);
}

TEST_F(CallbackError, NonStdExceptionIsAlsoCaught) {
  auto s = started(Config{1, 16, 8});
  ASSERT_TRUE(s->scheduleAfter(10ms, [this] {
    rec_.record();
    throw 42;  // not derived from std::exception
  }));
  clock_->advance(10ms);
  rec_.waitFor(1);
  ASSERT_TRUE(waitForFinished(*s, 1));
  EXPECT_EQ(s->metrics().callbacks_failed_total, 1u);
  s->stop(StopMode::Discard);
}

TEST_F(CallbackError, ThrowOnOneShotOnlyCounts) {
  auto s = started(Config{1, 16, 8});
  ASSERT_TRUE(s->scheduleAfter(10ms, [this] {
    rec_.record();
    throw std::runtime_error("boom");
  }));
  clock_->advance(10ms);
  rec_.waitFor(1);
  EXPECT_TRUE(waitForActive(*s, 0)) << "a failed one-shot releases its slot";
  clock_->advance(1h);
  EXPECT_TRUE(rec_.stayedBelow(2));
  s->stop(StopMode::Discard);
}

TEST_F(CallbackError, ThrowDisablesPeriodic) {  // T6, research R-09
  auto s = started(Config{1, 16, 8});
  ASSERT_TRUE(s->scheduleEvery(10ms, PeriodicMode::FixedDelay, [this] {
    rec_.record();
    throw std::runtime_error("periodic failure");
  }));
  clock_->advance(10ms);
  rec_.waitFor(1);
  ASSERT_TRUE(waitForFinished(*s, 1));

  EXPECT_TRUE(waitForActive(*s, 0)) << "the periodic record is removed, not rearmed";
  clock_->advance(1h);
  EXPECT_TRUE(rec_.stayedBelow(2)) << "a throwing periodic timer is disabled, not retried";
  EXPECT_EQ(s->metrics().callbacks_failed_total, 1u);
  s->stop(StopMode::Discard);
}

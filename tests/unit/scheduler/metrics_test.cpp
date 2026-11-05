// FR-10 and the M1-M6 conservation equations of contracts/metrics.md section 2.
// These turn "no timer was lost" and "no callback went uncounted" into
// arithmetic rather than review.

#include <gtest/gtest.h>

#include <stdexcept>

#include "metrics.hpp"
#include "support/fixture.hpp"
#include "support/test_gate.hpp"

using namespace std::chrono_literals;
using tcu::scheduler::CancelResult;
using tcu::scheduler::Config;
using tcu::scheduler::Error;
using tcu::scheduler::Histogram;
using tcu::scheduler::PeriodicMode;
using tcu::scheduler::StopMode;
using tcu::scheduler::TimerId;
using Metrics = tcu::test::FakeClockTest;

TEST_F(Metrics, StartedEqualsCompletedPlusFailed) {  // M1
  auto s = started(Config{2, 64, 16});
  for (int i = 0; i < 6; ++i) {
    ASSERT_TRUE(s->scheduleAfter(10ms, [this] { rec_.record(); }));
  }
  ASSERT_TRUE(s->scheduleAfter(10ms, [this] {
    rec_.record();
    throw std::runtime_error("boom");
  }));
  clock_->advance(10ms);
  rec_.waitFor(7);
  ASSERT_TRUE(waitForFinished(*s, 7));
  s->stop(StopMode::Drain);

  const auto m = s->metrics();
  EXPECT_EQ(m.callbacks_started_total,
            m.callbacks_completed_total + m.callbacks_failed_total);
  EXPECT_EQ(m.callbacks_failed_total, 1u);
  EXPECT_EQ(m.callbacks_completed_total, 6u);
}

TEST_F(Metrics, ConservationAfterMixedOutcomes) {  // M2
  // Terms count timers, not invocations. The test owns the classification
  // because the library does not expose "one-shot" or "periodic" totals.
  auto s = started(Config{1, 64, 16});
  tcu::test::TestGate gate;

  std::uint64_t finished_one_shots = 0;
  std::uint64_t disabled_periodics = 0;

  // Hold the only worker so a queued timer can be cancelled before it starts.
  ASSERT_TRUE(s->scheduleAfter(0ms, [&] { gate.wait_until_opened(); }));
  gate.wait_until_entered();
  const TimerId queued = s->scheduleAfter(0ms, [this] { rec_.record(); }).id;
  ASSERT_NE(queued, 0u);
  ASSERT_TRUE(waitForMetric([&] { return s->metrics().ready_queue_depth; }, 1));
  ASSERT_EQ(s->cancel(queued), CancelResult::Cancelled);
  gate.open();
  ASSERT_TRUE(waitForFinished(*s, 1));
  ++finished_one_shots;  // the gate timer

  // Three one-shots that complete, one that throws.
  for (int i = 0; i < 3; ++i) {
    ASSERT_TRUE(s->scheduleAfter(0ms, [this] { rec_.record(); }));
  }
  ASSERT_TRUE(s->scheduleAfter(0ms, [this] {
    rec_.record();
    throw std::runtime_error("boom");
  }));
  ASSERT_TRUE(waitForFinished(*s, 5));
  finished_one_shots += 4;

  // Two one-shots cancelled while merely scheduled.
  for (int i = 0; i < 2; ++i) {
    const TimerId id = s->scheduleAfter(1h, [this] { rec_.record(); }).id;
    ASSERT_NE(id, 0u);
    ASSERT_EQ(s->cancel(id), CancelResult::Cancelled);
  }

  // A periodic timer whose callback throws: disabled, not rearmed.
  ASSERT_TRUE(s->scheduleEvery(10ms, PeriodicMode::FixedDelay, [this] {
    rec_.record();
    throw std::runtime_error("periodic failure");
  }));
  clock_->advance(10ms);
  ASSERT_TRUE(waitForFinished(*s, 6));
  ++disabled_periodics;

  // Two one-shots left alive.
  for (int i = 0; i < 2; ++i) {
    ASSERT_TRUE(s->scheduleAfter(1h, [this] { rec_.record(); }));
  }
  ASSERT_TRUE(waitForActive(*s, 2));

  const auto m = s->metrics();
  // 1 gate + 1 cancelled-while-queued + 3 completing + 1 throwing
  // + 2 cancelled-while-scheduled + 1 periodic + 2 left alive.
  EXPECT_EQ(m.timers_scheduled_total, 11u);
  EXPECT_EQ(m.timers_cancelled_total, 3u);
  EXPECT_EQ(m.callbacks_skipped_cancelled_total, 1u);
  EXPECT_EQ(m.timers_active, 2u);
  EXPECT_EQ(m.timers_scheduled_total, finished_one_shots + disabled_periodics +
                                          m.timers_cancelled_total + m.timers_active)
      << "a timer vanished without an outcome";
  s->stop(StopMode::Discard);
}

TEST_F(Metrics, LatenessAndDurationSampledPerCallback) {  // M3, M4
  auto s = started(Config{2, 64, 16});
  for (int i = 0; i < 5; ++i) {
    ASSERT_TRUE(s->scheduleAfter(10ms, [this] { rec_.record(); }));
  }
  ASSERT_TRUE(s->scheduleAfter(10ms, [this] {
    rec_.record();
    throw std::runtime_error("boom");
  }));
  clock_->advance(10ms);
  rec_.waitFor(6);
  ASSERT_TRUE(waitForFinished(*s, 6));
  s->stop(StopMode::Drain);

  const auto m = s->metrics();
  EXPECT_EQ(m.start_lateness_ms.count, m.callbacks_started_total);  // M3
  EXPECT_EQ(m.callback_duration_ms.count,
            m.callbacks_completed_total + m.callbacks_failed_total);  // M4
}

TEST_F(Metrics, QueueEmptyAfterStop) {  // M5
  auto s = started(Config{1, 64, 8});
  for (int i = 0; i < 20; ++i) {
    ASSERT_TRUE(s->scheduleAfter(0ms, [this] { rec_.record(); }));
  }
  s->stop(StopMode::Discard);
  EXPECT_EQ(s->metrics().ready_queue_depth, 0u);
}

TEST_F(Metrics, RejectionReasonsDisjoint) {  // M6
  auto s = started(Config{1, 1, 8});
  ASSERT_TRUE(s->scheduleAfter(1h, [this] { rec_.record(); }));

  EXPECT_EQ(s->scheduleAfter(-1s, [this] { rec_.record(); }).error,
            Error::InvalidArgument);
  EXPECT_EQ(s->scheduleAfter(1h, [this] { rec_.record(); }).error,
            Error::CapacityExceeded);
  s->stop(StopMode::Discard);
  EXPECT_EQ(s->scheduleAfter(1h, [this] { rec_.record(); }).error,
            Error::InvalidState);

  const auto m = s->metrics();
  EXPECT_EQ(m.rejected_invalid_argument_total, 1u);
  EXPECT_EQ(m.rejected_capacity_total, 1u);
  EXPECT_EQ(m.rejected_invalid_state_total, 1u);
}

TEST_F(Metrics, ActiveGaugeTracksRegistry) {  // S4
  auto s = started(Config{1, 64, 8});
  EXPECT_EQ(s->metrics().timers_active, 0u);
  TimerId ids[4];
  for (int i = 0; i < 4; ++i) {
    ids[i] = s->scheduleAfter(1h, [this] { rec_.record(); }).id;
    ASSERT_NE(ids[i], 0u);
    EXPECT_EQ(s->metrics().timers_active, static_cast<std::uint64_t>(i + 1));
  }
  for (int i = 0; i < 4; ++i) {
    ASSERT_EQ(s->cancel(ids[i]), CancelResult::Cancelled);
    EXPECT_EQ(s->metrics().timers_active, static_cast<std::uint64_t>(3 - i));
  }
  s->stop(StopMode::Discard);
}

TEST(MetricsStorage, HistogramBucketBoundaries) {
  tcu::scheduler::detail::AtomicHistogram h;
  h.record(0);
  h.record(1);
  h.record(2);
  h.record(1000);
  h.record(1001);

  const Histogram snap = h.snapshot();
  EXPECT_EQ(snap.count, 5u);
  EXPECT_EQ(snap.sum_ms, 0u + 1u + 2u + 1000u + 1001u);
  EXPECT_EQ(snap.buckets[0], 2u) << "0 and 1 both land in the <=1ms bucket";
  EXPECT_EQ(snap.buckets[1], 1u) << "2 lands in the <=5ms bucket";
  EXPECT_EQ(snap.buckets[7], 1u) << "1000 lands in the <=1000ms bucket";
  EXPECT_EQ(snap.buckets[8], 1u) << "1001 lands in the +inf bucket";
}

TEST(MetricsStorage, LatenessClampedAtZero) {
  tcu::scheduler::detail::Metrics m;
  m.recordLateness(-5ms);
  m.recordLateness(0ms);

  const auto snap = m.snapshot();
  EXPECT_EQ(snap.start_lateness_ms.count, 2u);
  EXPECT_EQ(snap.start_lateness_ms.sum_ms, 0u)
      << "a callback cannot start before its deadline (T2)";
  EXPECT_EQ(snap.start_lateness_ms.buckets[0], 2u);
}

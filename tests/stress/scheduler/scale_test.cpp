// PT-01 (B-20): ten thousand timers, all due at once.
//
// This asserts boundedness and loss-freedom, never a latency number: spec
// section 6 calls the p99 target a benchmark and section 19.4 forbids reading a
// workstation run as a guarantee (research R-18). The measurement lives in
// benchmarks/scheduler_bench.

#include <gtest/gtest.h>

#include <chrono>
#include <random>
#include <vector>

#include "support/fixture.hpp"
#include "tcu/scheduler/timer_scheduler.hpp"

using namespace std::chrono_literals;
using tcu::scheduler::Config;
using tcu::scheduler::StopMode;
using Scale = tcu::test::FakeClockTest;

TEST_F(Scale, TenThousandTimersBounded) {  // PT-01, B-20
  constexpr int kTimers = 10000;
  constexpr std::size_t kMaxReady = 1024;
  auto s = started(Config{4, 20000, kMaxReady});

  std::mt19937 rng(20261104u);
  std::uniform_int_distribution<int> seconds(1, 60);
  for (int i = 0; i < kTimers; ++i) {
    ASSERT_TRUE(s->scheduleAfter(std::chrono::seconds(seconds(rng)),
                                 [this] { rec_.record(); }))
        << "timer " << i;
  }
  ASSERT_EQ(s->metrics().timers_active, static_cast<std::uint64_t>(kTimers));

  // One jump makes every deadline overdue: the burst row of spec section 14.
  clock_->advance(61s);
  rec_.waitFor(kTimers);
  ASSERT_TRUE(waitForFinished(*s, kTimers));
  s->stop(StopMode::Drain);

  const auto m = s->metrics();
  EXPECT_EQ(rec_.count(), static_cast<std::size_t>(kTimers)) << "no timer may be lost";
  EXPECT_EQ(m.callbacks_started_total, static_cast<std::uint64_t>(kTimers));
  EXPECT_EQ(m.callbacks_started_total,
            m.callbacks_completed_total + m.callbacks_failed_total);  // M1
  EXPECT_EQ(m.callbacks_failed_total, 0u);
  EXPECT_LE(m.ready_queue_depth, kMaxReady);
  EXPECT_EQ(m.timers_active, 0u);
  EXPECT_EQ(m.start_lateness_ms.count, static_cast<std::uint64_t>(kTimers));  // M3
}

TEST_F(Scale, TenThousandTimersCancelledWithoutRunning) {
  constexpr int kTimers = 10000;
  auto s = started(Config{4, 20000, 1024});

  std::vector<tcu::scheduler::TimerId> ids;
  ids.reserve(kTimers);
  for (int i = 0; i < kTimers; ++i) {
    const auto r = s->scheduleAfter(10h, [this] { rec_.record(); });
    ASSERT_TRUE(r);
    ids.push_back(r.id);
  }
  for (const auto id : ids) {
    ASSERT_EQ(s->cancel(id), tcu::scheduler::CancelResult::Cancelled);
  }

  EXPECT_EQ(s->metrics().timers_active, 0u);
  clock_->advance(20h);
  EXPECT_TRUE(rec_.stayedBelow(1));
  s->stop(StopMode::Drain);
  EXPECT_EQ(s->metrics().timers_cancelled_total,
            static_cast<std::uint64_t>(kTimers));
}

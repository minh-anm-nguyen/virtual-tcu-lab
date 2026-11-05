// IT-01 (B-12) and invariants Q1, Q2, S5: a full ready queue costs lateness,
// never correctness. Real clock, because the point is that the wiring works
// outside the fake one.

#include <gtest/gtest.h>

#include <memory>

#include "support/counting_clock.hpp"
#include "support/fixture.hpp"
#include "support/recorder.hpp"
#include "support/test_gate.hpp"
#include "tcu/scheduler/timer_scheduler.hpp"

using namespace std::chrono_literals;
using tcu::scheduler::Config;
using tcu::scheduler::Error;
using tcu::scheduler::PeriodicMode;
using tcu::scheduler::StopMode;
using tcu::scheduler::TimerScheduler;
using tcu::test::FakeClockTest;
using tcu::test::Recorder;
using tcu::test::TestGate;

TEST(Backpressure, OneShotNotLost) {  // IT-01, B-12, Q1, Q2
  // One worker, a ready queue of two. Ten due one-shots cannot all be queued,
  // so eight of them stay registered and must still run later.
  TimerScheduler s(Config{1, 64, 2});
  ASSERT_EQ(s.start(), Error::None);
  Recorder rec;
  TestGate gate;

  ASSERT_TRUE(s.scheduleAfter(0ms, [&] { gate.wait_until_opened(); }));
  gate.wait_until_entered();

  for (int i = 0; i < 10; ++i) {
    ASSERT_TRUE(s.scheduleAfter(0ms, [&rec, i] { rec.record(i); })) << "timer " << i;
  }

  // Sample the depth while the worker is held: it must never exceed capacity.
  for (int probe = 0; probe < 50; ++probe) {
    ASSERT_LE(s.metrics().ready_queue_depth, 2u);
  }
  EXPECT_TRUE(rec.stayedBelow(1)) << "the only worker is blocked";

  gate.open();
  rec.waitFor(10);
  EXPECT_EQ(rec.count(), 10u) << "no due one-shot may be dropped silently";
  s.stop(StopMode::Drain);
  EXPECT_EQ(s.metrics().ready_queue_depth, 0u);
}

TEST(Backpressure, QueueDepthNeverExceedsCapacityUnderChurn) {  // Q1
  TimerScheduler s(Config{2, 256, 4});
  ASSERT_EQ(s.start(), Error::None);
  Recorder rec;
  for (int i = 0; i < 200; ++i) {
    ASSERT_TRUE(s.scheduleAfter(0ms, [&rec] { rec.record(); }));
    ASSERT_LE(s.metrics().ready_queue_depth, 4u);
  }
  rec.waitFor(200);
  s.stop(StopMode::Drain);
}

TEST(Backpressure, FixedRateSkipsWhenQueueFull) {  // research R-10
  // A fixed-rate tick that cannot be queued is skipped and counted; it does not
  // wait, and it does not pile up.
  TimerScheduler s(Config{1, 64, 1});
  ASSERT_EQ(s.start(), Error::None);
  Recorder rec;
  TestGate gate;

  // Two fillers: the first occupies the only worker, the second fills the
  // one-slot ready queue. Without the second the queue would be empty, because
  // the running job was already popped out of it.
  ASSERT_TRUE(s.scheduleAfter(0ms, [&] { gate.wait_until_opened(); }));
  gate.wait_until_entered();
  ASSERT_TRUE(s.scheduleAfter(0ms, [&] { gate.wait_until_opened(); }));
  ASSERT_TRUE(FakeClockTest::waitForMetric(
      [&s] { return s.metrics().ready_queue_depth; }, 1));

  ASSERT_TRUE(s.scheduleEvery(2ms, PeriodicMode::FixedRateSkipMissed,
                              [&rec] { rec.record(); }));

  EXPECT_TRUE(FakeClockTest::waitForMetric(
      [&s] { return s.metrics().periods_skipped_total; }, 1))
      << "ticks that cannot be queued must be counted as skipped";

  gate.open();
  s.stop(StopMode::Discard);
}

TEST(Backpressure, RegistryFillsThenScheduleIsRejected) {  // research R-10 chain
  // The documented consequence of sustained overload: memory stays bounded, so
  // the registry fills and schedule* starts refusing.
  TimerScheduler s(Config{1, 4, 1});
  ASSERT_EQ(s.start(), Error::None);
  Recorder rec;
  TestGate gate;

  ASSERT_TRUE(s.scheduleAfter(0ms, [&] { gate.wait_until_opened(); }));
  gate.wait_until_entered();

  int accepted = 0;
  Error last = Error::None;
  for (int i = 0; i < 20; ++i) {
    const auto r = s.scheduleAfter(0ms, [&rec] { rec.record(); });
    if (r) {
      ++accepted;
    } else {
      last = r.error;
      break;
    }
  }
  EXPECT_EQ(last, Error::CapacityExceeded);
  EXPECT_LE(accepted, 4);
  EXPECT_GE(s.metrics().rejected_capacity_total, 1u);

  gate.open();
  rec.waitFor(static_cast<std::size_t>(accepted));
  s.stop(StopMode::Drain);
}

TEST(Backpressure, CoordinatorDoesNotSpin) {  // S5
  // A busy loop and a correct wait look identical from the outside, so the
  // assertion is on the number of times the coordinator blocked: O(dispatches),
  // not O(iterations).
  auto clock = std::make_shared<tcu::test::CountingClock>();
  TimerScheduler s(Config{1, 64, 8}, clock);
  ASSERT_EQ(s.start(), Error::None);
  Recorder rec;

  constexpr int kTimers = 20;
  for (int i = 1; i <= kTimers; ++i) {
    ASSERT_TRUE(s.scheduleAfter(std::chrono::milliseconds(i), [&rec] { rec.record(); }));
  }
  for (int i = 1; i <= kTimers; ++i) {
    clock->advanceTo(clock->now() + 1ms);
  }
  rec.waitFor(kTimers);
  s.stop(StopMode::Drain);

  // Each dispatch costs at most a couple of waits; a spin would cost thousands.
  EXPECT_LT(clock->waits(), static_cast<std::size_t>(20 * kTimers))
      << "the coordinator must block, not poll";
}

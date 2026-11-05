// IT-02, IT-03 (B-13, B-14) and invariants S6, M5: the two shutdown policies,
// on the real clock.

#include <gtest/gtest.h>

#include <atomic>
#include <thread>

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

TEST(StopDrain, RunsDueCancelsFuture) {  // IT-02, B-13
  TimerScheduler s(Config{1, 64, 8});
  ASSERT_EQ(s.start(), Error::None);
  Recorder rec;
  TestGate gate;

  // Occupy the only worker so the due timer is still waiting when stop begins.
  ASSERT_TRUE(s.scheduleAfter(0ms, [&] { gate.wait_until_opened(); }));
  gate.wait_until_entered();

  ASSERT_TRUE(s.scheduleAfter(0ms, [&rec] { rec.record(1); }));   // due
  ASSERT_TRUE(s.scheduleAfter(1h, [&rec] { rec.record(2); }));    // future
  ASSERT_TRUE(FakeClockTest::waitForMetric(
      [&s] { return s.metrics().ready_queue_depth; }, 1));

  std::thread opener([&] { gate.open(); });
  s.stop(StopMode::Drain);
  opener.join();

  EXPECT_EQ(rec.count(), 1u);
  EXPECT_EQ(rec.tags().front(), 1) << "due work finishes; the future is cancelled";
  EXPECT_GE(s.metrics().timers_cancelled_total, 1u);
  EXPECT_EQ(s.metrics().ready_queue_depth, 0u);  // M5
}

TEST(StopDrain, WaitsForInFlightCallback) {
  TimerScheduler s(Config{1, 64, 8});
  ASSERT_EQ(s.start(), Error::None);
  TestGate gate;
  std::atomic<bool> finished{false};

  ASSERT_TRUE(s.scheduleAfter(0ms, [&] {
    gate.wait_until_opened();
    finished = true;
  }));
  gate.wait_until_entered();

  std::thread opener([&] { gate.open(); });
  s.stop(StopMode::Drain);
  opener.join();

  EXPECT_TRUE(finished.load()) << "stop() must not return while a callback runs";
}

TEST(StopDrain, PeriodicIsNotRearmedDuringDrain) {
  TimerScheduler s(Config{1, 64, 8});
  ASSERT_EQ(s.start(), Error::None);
  Recorder rec;
  ASSERT_TRUE(s.scheduleEvery(1ms, PeriodicMode::FixedDelay, [&rec] { rec.record(); }));
  rec.waitFor(3);

  s.stop(StopMode::Drain);
  const std::size_t after_stop = rec.count();
  EXPECT_TRUE(rec.stayedBelow(after_stop + 1)) << "no callback runs after stop() returns";
  EXPECT_EQ(rec.count(), after_stop);
}

TEST(StopDrain, TerminatesWithAOneMillisecondPeriodicTimer) {
  // The reason research R-13 reads the clock exactly once: without that, a
  // fast periodic timer would keep producing due work and the drain would
  // never finish. A hang here surfaces as the CTest timeout.
  TimerScheduler s(Config{2, 64, 8});
  ASSERT_EQ(s.start(), Error::None);
  Recorder rec;
  for (int i = 0; i < 4; ++i) {
    ASSERT_TRUE(s.scheduleEvery(1ms, PeriodicMode::FixedRateSkipMissed,
                                [&rec] { rec.record(); }));
  }
  rec.waitFor(8);
  s.stop(StopMode::Drain);
  SUCCEED();
}

TEST(StopDiscard, DropsPendingFinishesInFlight) {  // IT-03, B-14
  TimerScheduler s(Config{1, 64, 16});
  ASSERT_EQ(s.start(), Error::None);
  Recorder rec;
  TestGate gate;
  std::atomic<bool> in_flight_finished{false};

  ASSERT_TRUE(s.scheduleAfter(0ms, [&] {
    gate.wait_until_opened();
    in_flight_finished = true;
  }));
  gate.wait_until_entered();

  for (int i = 0; i < 5; ++i) {
    ASSERT_TRUE(s.scheduleAfter(0ms, [&rec] { rec.record(); }));
  }
  ASSERT_TRUE(FakeClockTest::waitForMetric(
      [&s] { return s.metrics().ready_queue_depth; }, 5));

  std::thread opener([&] { gate.open(); });
  s.stop(StopMode::Discard);
  opener.join();

  EXPECT_TRUE(in_flight_finished.load()) << "an in-flight callback is allowed to finish";
  EXPECT_EQ(rec.count(), 0u) << "nothing not yet started may run";
  EXPECT_GE(s.metrics().timers_cancelled_total, 5u);
  EXPECT_EQ(s.metrics().ready_queue_depth, 0u);  // M5
}

TEST(StopDiscard, ClearedJobsCountedAsCancelled) {
  TimerScheduler s(Config{1, 64, 16});
  ASSERT_EQ(s.start(), Error::None);
  Recorder rec;
  for (int i = 0; i < 8; ++i) {
    ASSERT_TRUE(s.scheduleAfter(1h, [&rec] { rec.record(); }));
  }
  s.stop(StopMode::Discard);

  const auto m = s.metrics();
  EXPECT_EQ(m.timers_cancelled_total, 8u) << "nothing vanishes without an outcome";
  EXPECT_EQ(m.timers_active, 0u);
  EXPECT_EQ(rec.count(), 0u);
}

TEST(Stop, RepeatedIsNoOp) {  // B-15
  TimerScheduler s(Config{2, 64, 8});
  ASSERT_EQ(s.start(), Error::None);
  s.stop(StopMode::Drain);
  s.stop(StopMode::Drain);
  s.stop(StopMode::Discard);
  EXPECT_EQ(s.metrics().ready_queue_depth, 0u);
  EXPECT_EQ(s.scheduleAfter(0ms, [] {}).error, Error::InvalidState);
}

TEST(Stop, ConcurrentStopsBothReturn) {
  TimerScheduler s(Config{2, 64, 8});
  ASSERT_EQ(s.start(), Error::None);
  Recorder rec;
  for (int i = 0; i < 16; ++i) {
    ASSERT_TRUE(s.scheduleAfter(0ms, [&rec] { rec.record(); }));
  }

  std::thread other([&] { s.stop(StopMode::Discard); });
  s.stop(StopMode::Drain);
  other.join();
  EXPECT_EQ(s.metrics().ready_queue_depth, 0u);
}

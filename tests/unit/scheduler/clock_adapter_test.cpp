// UT-14 (B-18) and invariant S5: the injectable clock, and the wake-up that
// makes a fake-clock deadline test terminate instead of hanging.
//
// Contract under test: specs/003-timer-task-scheduler/contracts/scheduler_api.md
// section 3.

#include <gtest/gtest.h>

#include <condition_variable>
#include <mutex>
#include <thread>

#include "tcu/scheduler/clock_adapter.hpp"

using tcu::scheduler::Clock;
using tcu::scheduler::FakeClockAdapter;
using tcu::scheduler::SteadyClockAdapter;
using namespace std::chrono_literals;

TEST(ClockAdapter, FakeClock_NowStartsAtEpochAndAdvances) {
  FakeClockAdapter clock;
  const Clock::time_point start = clock.now();
  clock.advance(500ms);
  EXPECT_EQ(clock.now() - start, 500ms);
  clock.advance(1s);
  EXPECT_EQ(clock.now() - start, 1500ms);
}

TEST(ClockAdapter, FakeClock_AdvanceToNeverGoesBackwards) {
  FakeClockAdapter clock;
  clock.advance(1s);
  const Clock::time_point after = clock.now();
  clock.advanceTo(after - 500ms);
  EXPECT_EQ(clock.now(), after) << "a clock that goes backwards breaks T2";
  clock.advanceTo(after + 250ms);
  EXPECT_EQ(clock.now(), after + 250ms);
}

// The failure this guards against is a lost wake-up: the coordinator blocks on
// a fake deadline that only advance() can reach, so without the notification
// every fake-clock test would hang.
TEST(ClockAdapter, FakeClock_AdvanceWakesCoordinator) {
  FakeClockAdapter clock;
  std::mutex mu;
  std::condition_variable cv;
  std::condition_variable entered_cv;
  bool entered = false;
  bool woke = false;
  const Clock::time_point deadline = clock.now() + 10s;

  std::thread waiter([&] {
    std::unique_lock<std::mutex> lk(mu);
    entered = true;
    entered_cv.notify_all();
    clock.waitUntil(lk, cv, deadline);
    woke = true;
  });

  {
    std::unique_lock<std::mutex> lk(mu);
    entered_cv.wait(lk, [&] { return entered; });
  }
  clock.advance(20s);
  waiter.join();

  EXPECT_TRUE(woke);
  EXPECT_GE(clock.now(), deadline);
}

TEST(ClockAdapter, FakeClock_WaitUntilReturnsImmediatelyWhenDeadlinePassed) {
  FakeClockAdapter clock;
  std::mutex mu;
  std::condition_variable cv;
  const Clock::time_point deadline = clock.now();
  clock.advance(1s);

  std::unique_lock<std::mutex> lk(mu);
  clock.waitUntil(lk, cv, deadline);  // would hang if rule 2 were not honoured
  EXPECT_TRUE(lk.owns_lock()) << "waitUntil must return with the lock held";
}

TEST(ClockAdapter, SteadyClock_WaitUntilReturnsAtDeadline) {
  SteadyClockAdapter clock;
  std::mutex mu;
  std::condition_variable cv;
  const Clock::time_point start = clock.now();
  const Clock::time_point deadline = start + 20ms;

  std::unique_lock<std::mutex> lk(mu);
  while (clock.now() < deadline) {
    clock.waitUntil(lk, cv, deadline);
  }
  EXPECT_GE(clock.now() - start, 20ms);
  EXPECT_TRUE(lk.owns_lock());
}

TEST(ClockAdapter, SteadyClock_AttachIsANoOp) {
  SteadyClockAdapter clock;
  std::mutex mu;
  std::condition_variable cv;
  clock.attach(mu, cv);  // must not throw and must not change behaviour
  EXPECT_GE(clock.now(), Clock::time_point{});
}

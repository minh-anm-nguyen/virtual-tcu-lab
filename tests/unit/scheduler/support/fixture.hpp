#pragma once

// Shared setup: a scheduler on an injected fake clock, plus a recorder.
// Every deadline assertion in the unit suite runs on the fake clock
// (research R-17), so none of them depends on real time passing.

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <thread>

#include "support/recorder.hpp"
#include "tcu/scheduler/clock_adapter.hpp"
#include "tcu/scheduler/timer_scheduler.hpp"

namespace tcu::test {

class FakeClockTest : public ::testing::Test {
 public:
  using Scheduler = tcu::scheduler::TimerScheduler;

  /// Builds (but does not start) a scheduler sharing this test's fake clock.
  std::unique_ptr<Scheduler> make(tcu::scheduler::Config cfg = {}) {
    return std::make_unique<Scheduler>(cfg, clock_);
  }

  /// Builds and starts, failing the test if start() does not succeed.
  std::unique_ptr<Scheduler> started(tcu::scheduler::Config cfg = {}) {
    auto s = make(cfg);
    EXPECT_EQ(s->start(), tcu::scheduler::Error::None);
    return s;
  }

  /// Milliseconds of fake time since `base`, for use as a Recorder tag: it
  /// turns "when did this callback start" into an assertable integer.
  static int msSince(tcu::scheduler::Clock::time_point base,
                     tcu::scheduler::Clock::time_point t) {
    return static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(t - base).count());
  }

  /// Bounded wait on a counter the test cannot be notified about. This is a
  /// timeout on an atomic, not a sleep used to create ordering.
  static bool waitForMetric(const std::function<std::uint64_t()>& read,
                            std::uint64_t target,
                            std::chrono::milliseconds budget = std::chrono::seconds(5)) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
      if (read() >= target) return true;
      std::this_thread::yield();
    }
    return read() >= target;
  }

  /// Waits until `n` callbacks have finished, success or throw. Needed where a
  /// test asserts post-completion state (a rearm, a released capacity slot),
  /// because Recorder only proves the callback started.
  static bool waitForFinished(const Scheduler& s, std::uint64_t n) {
    return waitForMetric(
        [&s] {
          const auto m = s.metrics();
          return m.callbacks_completed_total + m.callbacks_failed_total;
        },
        n);
  }

  /// Waits for the active-timer gauge to equal `n` exactly (it both rises and
  /// falls, so this is an equality wait rather than a threshold).
  static bool waitForActive(const Scheduler& s, std::uint64_t n,
                            std::chrono::milliseconds budget = std::chrono::seconds(5)) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
      if (s.metrics().timers_active == n) return true;
      std::this_thread::yield();
    }
    return s.metrics().timers_active == n;
  }

 protected:
  std::shared_ptr<tcu::scheduler::FakeClockAdapter> clock_ =
      std::make_shared<tcu::scheduler::FakeClockAdapter>();
  Recorder rec_;
};

}  // namespace tcu::test

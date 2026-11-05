#pragma once

// The injectable clock. Spec section 6 requires "deterministic deadline tests
// using an injected clock plus a mechanism to wake coordinator after fake clock
// advances"; this is that mechanism (research R-05).
//
// FakeClockAdapter is deliberately public rather than test-only: projects 10,
// 13 and 15 all have timeouts to test, and a private fake would make each of
// them write it again (Constitution Principle II).

#include <condition_variable>
#include <mutex>

#include "tcu/scheduler/scheduler_types.hpp"

namespace tcu::scheduler {

/// Implementer contract (contracts/scheduler_api.md section 3):
///  1. waitUntil is entered with `lk` held and must return with `lk` held.
///  2. It must return when now() >= deadline, and when `cv` is notified.
///  3. Spurious returns are permitted; the caller re-checks its own predicate.
///  4. It must not return early otherwise, or the coordinator busy-spins (S5).
///  5. now() must be non-decreasing.
class ClockAdapter {
 public:
  virtual ~ClockAdapter() = default;

  virtual Clock::time_point now() const = 0;

  virtual void waitUntil(std::unique_lock<std::mutex>& lk,
                         std::condition_variable& cv,
                         Clock::time_point deadline) = 0;

  /// Called once by TimerScheduler::start() with the mutex and condition
  /// variable the coordinator waits on, so a clock that drives time from
  /// outside can wake it. A real clock needs nothing and ignores this.
  virtual void attach(std::mutex& mu, std::condition_variable& cv);

  /// Called by TimerScheduler once every thread has joined, and before the
  /// mutex and condition variable attach() was given can be destroyed. An
  /// adapter that stored them MUST forget them here: the clock can outlive the
  /// scheduler, and a later advance() would otherwise lock freed memory.
  virtual void detach();
};

class SteadyClockAdapter final : public ClockAdapter {
 public:
  Clock::time_point now() const override;
  void waitUntil(std::unique_lock<std::mutex>& lk, std::condition_variable& cv,
                 Clock::time_point deadline) override;
};

/// Time moves only when a test says so.
///
/// `advance` takes the scheduler mutex, which is safe from anywhere a caller
/// can reach - including from inside a callback, because no scheduler lock is
/// ever held across one (FR-11). A callback that advances the clock is how a
/// test makes a slow callback deterministic.
class FakeClockAdapter : public ClockAdapter {
 public:
  Clock::time_point now() const override;
  void waitUntil(std::unique_lock<std::mutex>& lk, std::condition_variable& cv,
                 Clock::time_point deadline) override;
  void attach(std::mutex& mu, std::condition_variable& cv) override;
  void detach() override;

  void advance(Clock::duration d);
  /// Never moves backwards: an earlier target is ignored (rule 5).
  void advanceTo(Clock::time_point t);

 private:
  mutable std::mutex own_;
  Clock::time_point now_{};
  std::mutex* sched_mu_{nullptr};
  std::condition_variable* sched_cv_{nullptr};
};

}  // namespace tcu::scheduler

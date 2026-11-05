#include "tcu/scheduler/clock_adapter.hpp"

namespace tcu::scheduler {

void ClockAdapter::attach(std::mutex&, std::condition_variable&) {}

void ClockAdapter::detach() {}

Clock::time_point SteadyClockAdapter::now() const { return Clock::now(); }

void SteadyClockAdapter::waitUntil(std::unique_lock<std::mutex>& lk,
                                   std::condition_variable& cv,
                                   Clock::time_point deadline) {
  cv.wait_until(lk, deadline);
}

Clock::time_point FakeClockAdapter::now() const {
  std::lock_guard<std::mutex> g(own_);
  return now_;
}

void FakeClockAdapter::attach(std::mutex& mu, std::condition_variable& cv) {
  std::lock_guard<std::mutex> g(own_);
  sched_mu_ = &mu;
  sched_cv_ = &cv;
}

void FakeClockAdapter::detach() {
  // Found by ThreadSanitizer: without this, a test that advances the clock
  // after the scheduler has been destroyed locks a freed mutex and notifies a
  // freed condition variable. The clock outlives the scheduler whenever it is
  // injected as a shared_ptr, which is the normal case.
  std::lock_guard<std::mutex> g(own_);
  sched_mu_ = nullptr;
  sched_cv_ = nullptr;
}

void FakeClockAdapter::waitUntil(std::unique_lock<std::mutex>& lk,
                                 std::condition_variable& cv,
                                 Clock::time_point deadline) {
  {
    std::lock_guard<std::mutex> g(own_);
    // Lazy registration for a clock used standalone, e.g. by its own unit
    // test. TimerScheduler::start() calls attach() before any coordinator
    // exists, so the registration is already done by the time it matters.
    if (sched_mu_ == nullptr) {
      sched_mu_ = lk.mutex();
      sched_cv_ = &cv;
    }
    if (now_ >= deadline) return;  // rule 2
  }
  // No timeout: only advance() can reach a fake deadline, and advance()
  // notifies this very cv while holding the mutex `lk` owns.
  cv.wait(lk);
}

void FakeClockAdapter::advance(Clock::duration d) {
  advanceTo(now() + d);
}

void FakeClockAdapter::advanceTo(Clock::time_point t) {
  std::mutex* sched_mu = nullptr;
  std::condition_variable* sched_cv = nullptr;
  {
    std::lock_guard<std::mutex> g(own_);
    sched_mu = sched_mu_;
    sched_cv = sched_cv_;
  }

  if (sched_mu == nullptr) {
    std::lock_guard<std::mutex> g(own_);
    if (t > now_) now_ = t;
    return;
  }

  // The waiter re-evaluates its deadline under this mutex, so the time change
  // and the notification must not slip between its predicate and its wait.
  {
    std::lock_guard<std::mutex> sched(*sched_mu);
    std::lock_guard<std::mutex> g(own_);
    if (t > now_) now_ = t;
  }
  sched_cv->notify_all();
}

}  // namespace tcu::scheduler

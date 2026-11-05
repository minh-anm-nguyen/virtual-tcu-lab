#pragma once

// A FakeClockAdapter that counts how often the coordinator blocks.
//
// Invariant S5 ("the coordinator never spins") is otherwise unobservable: a
// busy loop and a correct wait produce identical output. Counting waitUntil
// entries turns it into an assertion - N dispatches must cost O(N) waits, not
// O(iterations).

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <mutex>

#include "tcu/scheduler/clock_adapter.hpp"

namespace tcu::test {

class CountingClock final : public tcu::scheduler::FakeClockAdapter {
 public:
  void waitUntil(std::unique_lock<std::mutex>& lk, std::condition_variable& cv,
                 tcu::scheduler::Clock::time_point deadline) override {
    ++waits_;
    FakeClockAdapter::waitUntil(lk, cv, deadline);
  }

  std::size_t waits() const { return waits_.load(); }

 private:
  std::atomic<std::size_t> waits_{0};
};

}  // namespace tcu::test

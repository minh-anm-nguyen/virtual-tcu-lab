#pragma once

#include <condition_variable>
#include <mutex>

namespace tcu::test {

// Holds a callback inside the dispatcher until the test opens the gate.
// Spec section 9.1 forbids sleep-based synchronization; no test may use
// std::this_thread::sleep_for to create ordering.
class TestGate {
 public:
  // Called from inside the handler: announces arrival, then blocks.
  void wait_until_opened() {
    std::unique_lock<std::mutex> lock(mutex_);
    entered_ = true;
    entered_cv_.notify_all();
    opened_cv_.wait(lock, [this] { return opened_; });
  }

  // Called from the test: releases the handler.
  void open() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      opened_ = true;
    }
    opened_cv_.notify_all();
  }

  // Called from the test: waits until the handler is actually blocked, so the
  // test knows the dispatcher is occupied.
  void wait_until_entered() {
    std::unique_lock<std::mutex> lock(mutex_);
    entered_cv_.wait(lock, [this] { return entered_; });
  }

 private:
  std::mutex mutex_;
  std::condition_variable opened_cv_;
  std::condition_variable entered_cv_;
  bool opened_ = false;
  bool entered_ = false;
};

}  // namespace tcu::test

#pragma once

// Records callback invocations and lets the test block until the n-th arrives,
// so no test needs a sleep to order itself against a worker thread
// (contracts/timing_semantics.md section 7, rule 3).

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <thread>
#include <vector>

namespace tcu::test {

/// Budget for a negative assertion. A negative has no event to wait for, so it
/// needs a bounded wait; that is a timeout, not a synchronisation device.
constexpr std::chrono::milliseconds kNegativeWait{80};

class Recorder {
 public:
  /// Called from inside a callback.
  void record(int tag = 0) {
    {
      std::lock_guard<std::mutex> g(mu_);
      tags_.push_back(tag);
      threads_.push_back(std::this_thread::get_id());
    }
    cv_.notify_all();
  }

  /// Blocks until at least `n` invocations have been recorded. Unbounded on
  /// purpose: a lost wake-up must surface as the CTest timeout, loudly.
  void waitFor(std::size_t n) {
    std::unique_lock<std::mutex> lk(mu_);
    cv_.wait(lk, [&] { return tags_.size() >= n; });
  }

  bool waitForFor(std::size_t n, std::chrono::milliseconds budget) {
    std::unique_lock<std::mutex> lk(mu_);
    return cv_.wait_for(lk, budget, [&] { return tags_.size() >= n; });
  }

  /// True when fewer than `n` invocations arrived within the budget.
  bool stayedBelow(std::size_t n, std::chrono::milliseconds budget = kNegativeWait) {
    return !waitForFor(n, budget);
  }

  std::size_t count() const {
    std::lock_guard<std::mutex> g(mu_);
    return tags_.size();
  }

  std::vector<int> tags() const {
    std::lock_guard<std::mutex> g(mu_);
    return tags_;
  }

  /// How many distinct threads ran callbacks - used to show that a slow
  /// callback does not monopolise the pool.
  std::size_t distinctThreads() const {
    std::lock_guard<std::mutex> g(mu_);
    std::vector<std::thread::id> ids = threads_;
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids.size();
  }

  bool ranOn(std::thread::id id) const {
    std::lock_guard<std::mutex> g(mu_);
    return std::find(threads_.begin(), threads_.end(), id) != threads_.end();
  }

 private:
  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::vector<int> tags_;
  std::vector<std::thread::id> threads_;
};

}  // namespace tcu::test

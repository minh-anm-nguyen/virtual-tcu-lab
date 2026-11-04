#pragma once

// A bounded task queue served by one or more worker threads.
//
// Spec section 4.1 forbids running a handler or a callback on the I/O loop, and
// section 7 asks for a "callback executor 1-N threads" on the client and a
// "worker executor N threads" on the server. The thread count is a constructor
// argument for exactly that reason:
//
//   client: 1 thread, so notification callbacks for one session stay in the
//           order the frames arrived (spec section 8.7).
//   server: several threads, so one slow handler cannot starve the handlers of
//           other sessions - measured: with a single thread, a 3 s handler made
//           an unrelated client's 2 s request time out.
//
// The bound on the queue is the part that matters either way: an unbounded
// queue would undo the backpressure of FR-07.
//
// Project 01's tcu_eventbus was evaluated first and does not fit: its payload is
// a closed std::variant fixed by project 01's spec, so carrying a std::function
// through it would mean widening the API that projects 05-30 depend on. See
// research R-05.

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace tcu::ipc::detail {

/// What stop() does with tasks that have not started yet.
///
/// `Discard` is the default because shutdown must be bounded (spec section 3.2
/// gives it 3 s) and a queued handler's result is being thrown away anyway -
/// its session is closing. `Drain` exists for the caller that wants every
/// queued task to run first.
enum class DrainPolicy { Discard, Drain };

class Executor {
 public:
  using Task = std::function<void()>;

  explicit Executor(std::size_t capacity, std::size_t threads = 1)
      : capacity_(capacity == 0 ? 1 : capacity), thread_count_(threads == 0 ? 1 : threads) {}

  ~Executor() { stop(); }

  Executor(const Executor&) = delete;
  Executor& operator=(const Executor&) = delete;

  void start();

  /// Returns false when the queue is at capacity or the executor is stopping,
  /// so a caller learns the task will not run instead of assuming it will.
  bool post(Task task);

  /// Stops accepting work, lets the running tasks finish, and joins.
  /// Idempotent. Must not be called from a worker thread - that would be a
  /// self-join (spec section 8.9 step 4).
  void stop(DrainPolicy policy = DrainPolicy::Discard);

  std::uint64_t taskFailures() const noexcept {
    return task_failures_.load(std::memory_order_relaxed);
  }

  /// Tasks that stop(Discard) threw away without running.
  std::uint64_t tasksDiscarded() const noexcept {
    return tasks_discarded_.load(std::memory_order_relaxed);
  }

  /// True while called from a worker, so a caller can refuse to self-join.
  bool onWorkerThread() const;

 private:
  void run();

  const std::size_t capacity_;
  const std::size_t thread_count_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<Task> queue_;
  bool stopping_{false};
  bool started_{false};
  bool drain_on_stop_{false};
  std::vector<std::thread> workers_;
  std::vector<std::thread::id> worker_ids_;
  std::atomic<std::uint64_t> task_failures_{0};
  std::atomic<std::uint64_t> tasks_discarded_{0};
};

}  // namespace tcu::ipc::detail

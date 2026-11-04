#include "executor.hpp"

#include <algorithm>
#include <utility>

#include "log.hpp"

namespace tcu::ipc::detail {

void Executor::start() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (started_) {
    return;
  }
  stopping_ = false;
  drain_on_stop_ = false;
  workers_.reserve(thread_count_);
  worker_ids_.reserve(thread_count_);
  for (std::size_t i = 0; i < thread_count_; ++i) {
    workers_.emplace_back([this] { run(); });
    worker_ids_.push_back(workers_.back().get_id());
  }
  started_ = true;
}

bool Executor::post(Task task) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_ || !started_) {
      return false;
    }
    if (queue_.size() >= capacity_) {
      return false;
    }
    queue_.push_back(std::move(task));
  }
  cv_.notify_one();
  return true;
}

void Executor::stop(DrainPolicy policy) {
  std::vector<std::thread> to_join;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!started_) {
      return;
    }
    stopping_ = true;
    drain_on_stop_ = policy == DrainPolicy::Drain;
    if (!drain_on_stop_) {
      // Bounded shutdown: a task that has not started is not worth waiting
      // for, and it is counted rather than silently dropped (Principle V).
      tasks_discarded_.fetch_add(queue_.size(), std::memory_order_relaxed);
      queue_.clear();
    }
    to_join.swap(workers_);
    worker_ids_.clear();
    started_ = false;
  }
  cv_.notify_all();
  for (std::thread& worker : to_join) {
    if (worker.joinable()) {
      worker.join();
    }
  }
}

bool Executor::onWorkerThread() const {
  const std::thread::id self = std::this_thread::get_id();
  std::lock_guard<std::mutex> lock(mutex_);
  return std::find(worker_ids_.begin(), worker_ids_.end(), self) != worker_ids_.end();
}

void Executor::run() {
  for (;;) {
    Task task;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
      if (queue_.empty()) {
        // Only reachable when stopping and nothing is left: exit.
        return;
      }
      if (stopping_ && !drain_on_stop_) {
        return;
      }
      task = std::move(queue_.front());
      queue_.pop_front();
    }
    // Deliberately outside the lock: spec section 7 forbids holding a mutex
    // across a user callback.
    try {
      task();
    } catch (const std::exception& e) {
      task_failures_.fetch_add(1, std::memory_order_relaxed);
      LogFields f;
      f.level = LogLevel::Error;
      f.service = "ipc";
      f.event = "request_failed";
      f.reason = e.what();
      emit(f);
    } catch (...) {
      task_failures_.fetch_add(1, std::memory_order_relaxed);
      LogFields f;
      f.level = LogLevel::Error;
      f.service = "ipc";
      f.event = "request_failed";
      f.reason = "unknown_exception";
      emit(f);
    }
  }
}

}  // namespace tcu::ipc::detail

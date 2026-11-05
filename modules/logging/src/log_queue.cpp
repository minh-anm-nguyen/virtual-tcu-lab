#include "log_queue.hpp"

#include <utility>

namespace tcu::logging::internal {

LogQueue::LogQueue(std::size_t capacity)
    : capacity_(capacity == 0 ? 1 : capacity) {}

bool LogQueue::push(LogRecord&& record, std::chrono::milliseconds waitBudget) {
  std::unique_lock<std::mutex> lock(mutex_);
  if (closed_) {
    return false;
  }

  if (queue_.size() >= capacity_ && waitBudget.count() > 0) {
    // Bounded wait, never indefinite: a business thread may be held for the
    // budget and not one millisecond longer (BC-18).
    space_cv_.wait_for(lock, waitBudget, [this] {
      return queue_.size() < capacity_ || closed_;
    });
  }

  if (closed_ || queue_.size() >= capacity_) {
    return false;
  }

  queue_.push_back(std::move(record));
  ++enqueued_;
  lock.unlock();
  writer_cv_.notify_one();
  return true;
}

std::deque<LogRecord> LogQueue::drain(std::chrono::milliseconds timeout) {
  std::unique_lock<std::mutex> lock(mutex_);
  if (queue_.empty()) {
    writer_cv_.wait_for(lock, timeout, [this] { return !queue_.empty() || closed_; });
  }
  std::deque<LogRecord> batch;
  batch.swap(queue_);
  lock.unlock();
  if (!batch.empty()) {
    space_cv_.notify_all();
  }
  return batch;
}

std::deque<LogRecord> LogQueue::drainNow() {
  std::deque<LogRecord> batch;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    batch.swap(queue_);
  }
  if (!batch.empty()) {
    space_cv_.notify_all();
  }
  return batch;
}

void LogQueue::closeAdmission() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    closed_ = true;
  }
  // Everyone has to learn about it: a producer waiting for space must stop
  // waiting, and the writer must come back to finish the drain.
  space_cv_.notify_all();
  writer_cv_.notify_all();
  processed_cv_.notify_all();
}

void LogQueue::wake() { writer_cv_.notify_all(); }

std::size_t LogQueue::depth() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return queue_.size();
}

std::uint64_t LogQueue::enqueuedCount() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return enqueued_;
}

void LogQueue::markProcessed(std::size_t count) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    processed_ += count;
  }
  processed_cv_.notify_all();
}

bool LogQueue::waitProcessed(std::uint64_t target,
                             std::chrono::milliseconds timeout) {
  std::unique_lock<std::mutex> lock(mutex_);
  return processed_cv_.wait_for(lock, timeout, [this, target] {
    // closed_ is a release condition too: once admission is shut and the queue
    // is empty, nothing more will ever be processed, so waiting is pointless.
    return processed_ >= target || (closed_ && queue_.empty());
  });
}

}  // namespace tcu::logging::internal

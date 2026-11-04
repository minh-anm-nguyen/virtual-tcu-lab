#include "bounded_queue.hpp"

#include <utility>

namespace tcu::eventbus::detail {

BoundedQueue::BoundedQueue(std::size_t capacity) : capacity_(capacity) {}

std::uint64_t BoundedQueue::enqueue_locked(Event event) {
  const std::uint64_t id = ++next_id_;
  event.id = id;
  items_.push_back(std::move(event));
  if (items_.size() > high_watermark_) {
    high_watermark_ = items_.size();
  }
  not_empty_.notify_one();
  return id;
}

EnqueueResult BoundedQueue::push(Event event) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (closed_) {
    return {EnqueueStatus::Closed, 0};
  }
  if (items_.size() >= capacity_) {
    return {EnqueueStatus::Full, 0};  // invariant Q1: never exceed capacity
  }
  return {EnqueueStatus::Accepted, enqueue_locked(std::move(event))};
}

EnqueueResult BoundedQueue::push_until(Event event,
                                       std::chrono::steady_clock::time_point deadline) {
  std::unique_lock<std::mutex> lock(mutex_);
  // Absolute deadline inside a predicate loop, so a spurious wakeup can never
  // shorten the timeout (invariant Q3). wait_until evaluates the predicate
  // before waiting, which makes a past deadline a plain poll (research R-11).
  const bool has_room = not_full_.wait_until(
      lock, deadline, [this] { return closed_ || items_.size() < capacity_; });

  if (closed_) {
    return {EnqueueStatus::Closed, 0};
  }
  if (!has_room) {
    return {EnqueueStatus::TimedOut, 0};
  }
  return {EnqueueStatus::Accepted, enqueue_locked(std::move(event))};
}

std::optional<Event> BoundedQueue::pop() {
  std::unique_lock<std::mutex> lock(mutex_);
  not_empty_.wait(lock, [this] { return !items_.empty() || closed_; });
  if (items_.empty()) {
    return std::nullopt;  // closed and drained
  }
  Event event = std::move(items_.front());
  items_.pop_front();
  not_full_.notify_one();
  return event;
}

void BoundedQueue::close() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    closed_ = true;
  }
  not_empty_.notify_all();
  not_full_.notify_all();
}

std::size_t BoundedQueue::close_and_discard() {
  std::size_t dropped = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    closed_ = true;
    dropped = items_.size();
    items_.clear();
  }
  not_empty_.notify_all();
  not_full_.notify_all();
  return dropped;
}

std::size_t BoundedQueue::depth() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return items_.size();
}

std::size_t BoundedQueue::high_watermark() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return high_watermark_;
}

}  // namespace tcu::eventbus::detail

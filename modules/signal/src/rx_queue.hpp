#pragma once

// The bounded RX queue between project 05's transport and the decoder worker.
//
// Holds tcu::can::CanMessage values: project 05's frame already owns its
// 8-byte payload, so there is nothing to copy out of a borrowed view and no
// pointer that can outlive its source (research R-22).
//
// Bounded, drop-newest, monotonic counters. A producer never blocks
// indefinitely and the depth never exceeds the configured capacity
// (invariants V-01..V-04, contract B-13).

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>

#include "tcu/can/can_types.hpp"

namespace tcu::signal::detail {

class RxQueue {
 public:
  explicit RxQueue(std::size_t capacity) : capacity_(capacity == 0 ? 1 : capacity) {}

  /// false when the frame was dropped because the queue was full. Returns
  /// immediately either way: the RX thread must never be blocked by a slow
  /// decoder (B-13).
  bool push(const tcu::can::CanMessage& message) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!accepting_) return false;
      if (queue_.size() >= capacity_) {
        ++dropped_;
        return false;
      }
      queue_.push_back(message);
      if (queue_.size() > highWaterMark_) highWaterMark_ = queue_.size();
    }
    notEmpty_.notify_one();
    return true;
  }

  /// Blocks until a frame is available or the queue is closed. Empty optional
  /// means "closed and drained", which is how the worker learns to stop.
  std::optional<tcu::can::CanMessage> pop() {
    std::unique_lock<std::mutex> lock(mutex_);
    notEmpty_.wait(lock, [this] { return !queue_.empty() || closed_; });
    if (queue_.empty()) return std::nullopt;
    tcu::can::CanMessage message = queue_.front();
    queue_.pop_front();
    return message;
  }

  /// Stops accepting new frames but leaves what is queued to be drained.
  void stopAccepting() {
    std::lock_guard<std::mutex> lock(mutex_);
    accepting_ = false;
  }

  /// Wakes the worker so it can finish. Call after stopAccepting(), and after
  /// deciding whether to drain or discard.
  void close() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      closed_ = true;
    }
    notEmpty_.notify_all();
  }

  void discardPending() {
    std::lock_guard<std::mutex> lock(mutex_);
    discarded_ += queue_.size();
    queue_.clear();
  }

  std::size_t depth() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
  }
  std::size_t capacity() const { return capacity_; }
  std::uint64_t dropped() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return dropped_;
  }
  std::uint64_t discarded() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return discarded_;
  }
  std::size_t highWaterMark() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return highWaterMark_;
  }

 private:
  mutable std::mutex mutex_;
  std::condition_variable notEmpty_;
  std::deque<tcu::can::CanMessage> queue_;
  const std::size_t capacity_;
  std::uint64_t dropped_{0};
  std::uint64_t discarded_{0};
  std::size_t highWaterMark_{0};
  bool accepting_{true};
  bool closed_{false};
};

}  // namespace tcu::signal::detail

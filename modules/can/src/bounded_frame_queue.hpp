#pragma once

// The RX ingress queue: a fixed-capacity ring with a DropNewest overload
// policy (ADR-04, NFR-02, research R-17).
//
// Local to this project rather than reused: project 01's event queue blocks or
// rejects per publish and project 03's ready queue is an internal callback
// FIFO. Both are private to their src/, and neither implements DropNewest.
// Promoting either would make an implementation detail of a committed project
// part of its public contract to serve a policy it does not have.
//
// The policy itself is a deliberate trade: the RX reader must never block, so
// frames are lost under overload and EVERY loss is counted. ADR-04 is explicit
// that this is "unsuitable by itself for safety-critical/control functions".

#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <optional>
#include <vector>

#include "tcu/can/can_types.hpp"

namespace tcu::can {

class BoundedFrameQueue {
 public:
  /// Allocates once and never grows (NFR-02). A capacity of 0 is a
  /// configuration error the loader rejects before this is constructed.
  explicit BoundedFrameQueue(std::size_t capacity) : buffer_(capacity == 0 ? 1 : capacity) {}

  BoundedFrameQueue(const BoundedFrameQueue&) = delete;
  BoundedFrameQueue& operator=(const BoundedFrameQueue&) = delete;

  /// Returns false when the arriving frame was dropped because the queue was
  /// full - DropNewest. Never blocks, so the single RX reader cannot be stalled
  /// by a slow decoder (spec section 13.7).
  bool push(const CanMessage& msg) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (closed_) {
        return false;
      }
      if (size_ == buffer_.size()) {
        return false;
      }
      buffer_[tail_] = msg;
      tail_ = (tail_ + 1) % buffer_.size();
      ++size_;
    }
    cv_.notify_one();
    return true;
  }

  /// Blocks while empty and open. Returns nullopt only when the queue is closed
  /// AND drained, which is how the decode worker learns to exit without a
  /// sentinel value or a timeout (spec section 13.8 steps 3-5).
  std::optional<CanMessage> pop() {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this] { return size_ > 0 || closed_; });
    if (size_ == 0) {
      return std::nullopt;
    }
    CanMessage msg = buffer_[head_];
    head_ = (head_ + 1) % buffer_.size();
    --size_;
    return msg;
  }

  /// Non-blocking variant, for the bounded shutdown drain: the worker must be
  /// able to stop at the deadline rather than wait for one more frame.
  std::optional<CanMessage> tryPop() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (size_ == 0) {
      return std::nullopt;
    }
    CanMessage msg = buffer_[head_];
    head_ = (head_ + 1) % buffer_.size();
    --size_;
    return msg;
  }

  /// Wakes every waiting consumer so it can drain and exit. Already-queued
  /// frames remain poppable: closing is not discarding.
  void close() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      closed_ = true;
    }
    cv_.notify_all();
  }

  std::size_t depth() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return size_;
  }

  std::size_t capacity() const { return buffer_.size(); }

 private:
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::vector<CanMessage> buffer_;
  std::size_t head_{0};
  std::size_t tail_{0};
  std::size_t size_{0};
  bool closed_{false};
};

}  // namespace tcu::can

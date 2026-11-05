#include "tcu/can/fake_can_transport.hpp"

#include <algorithm>

#include "can_flags.hpp"

namespace tcu::can {

void FakeCanTransport::pushFrame(CanMessage msg) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    script_.push_back(ReadResult{ReadStatus::Received, std::move(msg), 0});
  }
  cv_.notify_one();
}

void FakeCanTransport::pushRawId(std::uint32_t raw_id, std::vector<std::uint8_t> data) {
  const auto decoded = flags::decodeRawId(raw_id);

  CanMessage msg;
  msg.id = decoded.id;
  msg.format = decoded.format;
  msg.kind = decoded.kind;
  // Deliberately NOT clamped to the catalog's expected length: a short frame on
  // a known identifier is exactly what UT-08 needs.
  msg.length = static_cast<std::uint8_t>(std::min<std::size_t>(data.size(), msg.data.size()));
  std::copy_n(data.begin(), msg.length, msg.data.begin());

  pushFrame(std::move(msg));
}

void FakeCanTransport::pushReadStatus(ReadStatus status, int os_error) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    script_.push_back(ReadResult{status, std::nullopt, os_error});
  }
  cv_.notify_one();
}

std::vector<CanMessage> FakeCanTransport::sent() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return sent_;
}

void FakeCanTransport::setSendStatus(WriteStatus status) {
  std::lock_guard<std::mutex> lock(mutex_);
  send_status_ = status;
}

std::uint64_t FakeCanTransport::timeoutCount() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return timeouts_;
}

ReadResult FakeCanTransport::receive(std::chrono::milliseconds timeout) {
  std::unique_lock<std::mutex> lock(mutex_);

  // Mirrors the real transport: a stop request wins over queued input, so a
  // shutdown is not delayed by a long script (spec section 13.8 step 2).
  if (stop_requested_) {
    return ReadResult{ReadStatus::Interrupted, std::nullopt, 0};
  }

  if (script_.empty()) {
    // Wait like the real poll() does, so a test can push from another thread
    // and so the RX loop does not spin. A zero timeout means "do not wait".
    cv_.wait_for(lock, timeout, [this] { return !script_.empty() || stop_requested_; });
  }

  if (stop_requested_) {
    return ReadResult{ReadStatus::Interrupted, std::nullopt, 0};
  }
  if (script_.empty()) {
    ++timeouts_;
    return ReadResult{ReadStatus::Timeout, std::nullopt, 0};
  }

  ReadResult result = script_.front();
  script_.pop_front();
  return result;
}

WriteStatus FakeCanTransport::send(const CanMessage& msg) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (send_status_ != WriteStatus::Sent) {
    return send_status_;
  }
  sent_.push_back(msg);
  return WriteStatus::Sent;
}

void FakeCanTransport::requestStop() noexcept {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_requested_ = true;
  }
  cv_.notify_all();
}

}  // namespace tcu::can

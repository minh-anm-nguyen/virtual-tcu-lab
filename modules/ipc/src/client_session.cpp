#include "client_session.hpp"

#include <cerrno>

namespace tcu::ipc::detail {

bool ClientSession::readSome(bool& closed_by_peer) {
  closed_by_peer = false;
  char buf[8192];
  const ssize_t n = socket_.recvSome(buf, sizeof(buf));
  if (n == 0) {
    // Orderly shutdown: spec section 8.3, "read() == 0".
    closed_by_peer = true;
    return false;
  }
  if (n < 0) {
    const int err = socket_.lastErrno();
    if (err == EAGAIN || err == EWOULDBLOCK) {
      // Not ready; wait for the next poll() instead of spinning.
      return true;
    }
    return false;
  }
  rx_.append(buf, static_cast<std::size_t>(n));
  return true;
}

DecodeResult ClientSession::nextFrame(Frame& out) {
  std::size_t consumed = 0;
  const DecodeResult r = tryDecode(rx_, out, consumed);
  if (r == DecodeResult::Ok) {
    rx_.erase(0, consumed);
  }
  return r;
}

bool ClientSession::enqueue(const std::string& frame) {
  if (frame.empty()) {
    return false;
  }
  // Both caps of spec section 3.2; breaching either drops the frame rather
  // than growing without bound (invariant S11).
  if (tx_.size() >= max_tx_messages_) {
    return false;
  }
  if (tx_bytes_ + frame.size() > max_tx_bytes_) {
    return false;
  }
  // shortcut: a session that stays at its cap is counted, never closed.
  // Upgrade to a timed close if a spec revision asks for a slow-consumer
  // disconnect policy (research R-08).
  tx_.push_back(frame);
  tx_bytes_ += frame.size();
  return true;
}

bool ClientSession::flush() {
  while (!tx_.empty()) {
    const std::string& front = tx_.front();
    const std::size_t remaining = front.size() - tx_offset_;
    const ssize_t n = socket_.sendSome(front.data() + tx_offset_, remaining);
    if (n < 0) {
      const int err = socket_.lastErrno();
      if (err == EAGAIN || err == EWOULDBLOCK) {
        // Partially written: keep the rest and wait for POLLOUT.
        return true;
      }
      return false;
    }
    // Only the bytes actually sent leave the buffer (spec section 8.3).
    tx_offset_ += static_cast<std::size_t>(n);
    tx_bytes_ -= static_cast<std::size_t>(n);
    if (tx_offset_ == front.size()) {
      tx_.pop_front();
      tx_offset_ = 0;
    }
  }
  clearFullLogLatch();
  return true;
}

bool ClientSession::takeFullLogLatch() {
  if (tx_full_logged_) {
    return false;
  }
  tx_full_logged_ = true;
  return true;
}

}  // namespace tcu::ipc::detail

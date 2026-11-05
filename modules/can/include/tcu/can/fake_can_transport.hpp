#pragma once

// A scripted ICanTransport.
//
// PUBLIC on purpose, exactly like project 03's FakeClockAdapter: project 06
// replaces the decoder behind this same transport and project 07 needs two
// fake buses, so a private fake would make each of them write it again
// (Constitution Principle II, research R-22).
//
// It is also the ONLY way to reach B-10, B-13, B-14, B-15 and UT-06..UT-08:
// spec section 18.1's test note says the kernel refuses to place those
// malformed frames on a real vcan0, and forbids claiming an impossible on-wire
// test passed.

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

#include "tcu/can/can_transport.hpp"

namespace tcu::can {

class FakeCanTransport final : public ICanTransport {
 public:
  // --- scripting, called from the test thread ---

  /// Enqueue a frame to be delivered by the next receive(). Nothing is
  /// validated: a caller may set `format`, `kind` or `length` to anything,
  /// which is how a malformed frame reaches FrameValidator (UT-06..UT-08).
  void pushFrame(CanMessage msg);

  /// Enqueue a frame described the way the kernel would deliver it - a raw
  /// can_id carrying EFF, RTR or ERR bits - so the flag-splitting rule has one
  /// implementation and one test path (src/can_flags.hpp).
  void pushRawId(std::uint32_t raw_id, std::vector<std::uint8_t> data);

  /// Enqueue a non-Received outcome: LinkError and FatalError drive the
  /// degrade-and-retry path (UT-16, B-19) without needing a real interface to
  /// disappear.
  void pushReadStatus(ReadStatus status, int os_error = 0);

  /// Everything send() was given, in order.
  std::vector<CanMessage> sent() const;

  /// Force send() to return this status instead of Sent.
  void setSendStatus(WriteStatus status);

  /// How many times receive() returned Timeout because the script was empty.
  std::uint64_t timeoutCount() const;

  // --- ICanTransport ---

  ReadResult receive(std::chrono::milliseconds timeout) override;
  WriteStatus send(const CanMessage& msg) override;
  void requestStop() noexcept override;

 private:
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<ReadResult> script_;
  std::vector<CanMessage> sent_;
  WriteStatus send_status_{WriteStatus::Sent};
  std::uint64_t timeouts_{0};
  bool stop_requested_{false};
};

}  // namespace tcu::can

#pragma once

// The transport boundary. Everything above it is OS-neutral; the only Linux
// implementation is SocketCanTransport (contracts/can_api.md section 2).

#include <chrono>
#include <optional>

#include "tcu/can/can_types.hpp"

namespace tcu::can {

struct ReadResult {
  ReadStatus status{ReadStatus::Timeout};
  /// Set only when status is Received.
  std::optional<CanMessage> message;
  /// errno snapshot, captured before any other syscall could overwrite it.
  /// 0 when there is none.
  int os_error{0};
};

class ICanTransport {
 public:
  virtual ~ICanTransport() = default;

  /// Called from exactly ONE thread for the lifetime of the object - the
  /// single receiver rule (spec section 17.1 rule 1). No exception escapes; a
  /// partial read is never reported as Received.
  virtual ReadResult receive(std::chrono::milliseconds timeout) = 0;

  /// Callable from any thread; implementations serialize writes on the TX
  /// socket. Sent means the local socket accepted the frame, NOT that a peer
  /// processed it (spec section 10.2).
  virtual WriteStatus send(const CanMessage& msg) = 0;

  /// Callable from any thread, idempotent. Wakes a receive() blocked in poll()
  /// so it returns Interrupted. Closes nothing: the receiving thread owns the
  /// descriptors, and closing underneath it is the race spec section 24 lists
  /// as a crash risk.
  virtual void requestStop() noexcept = 0;
};

}  // namespace tcu::can

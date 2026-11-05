#pragma once

// Matches a synthetic 0x200 lock command to its 0x201 acknowledgment.
//
// V1 keeps exactly ONE command in flight (research R-20). That is not a
// simplification to apologise for: the sequence byte is 8 bits and spec section
// 11.3 is explicit that it "wraps; do not assume global uniqueness or
// security". With one outstanding request there is nothing to confuse, so the
// wrap the section 24 risk table warns about cannot produce an ambiguity at
// all. A keyed pending map would reintroduce exactly that risk, to serve a
// second requester V1 does not have.
//
// The sequence byte is a demo correlation mechanism and nothing more. A real
// remote-control path needs authentication, expiry and replay protection, which
// is project 16's problem (ADR-07).

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>

#include "tcu/can/can_types.hpp"

namespace tcu::can {

enum class CommandState { Idle, AwaitingAck };

/// What happened to an inbound 0x201.
enum class AckOutcome {
  /// It answered the pending request, which is now complete.
  Matched,
  /// Nothing was pending, or the sequence belonged to something else. Counted
  /// and ignored; any pending request STAYS pending (B-23).
  Unmatched,
};

struct CommandResult {
  std::uint8_t sequence{0};
  std::uint8_t result{0};
};

class CommandCorrelator {
 public:
  /// Returns false when a command is already awaiting an acknowledgment, in
  /// which case nothing was recorded and the caller must not transmit.
  bool beginCommand(std::uint8_t command, std::uint8_t sequence, SteadyTime now);

  /// Called when a transmit attempt failed after beginCommand() succeeded, so a
  /// request that never reached the bus does not block the next one.
  void abandonCommand();

  AckOutcome onAck(std::uint8_t sequence, std::uint8_t result);

  /// Clears a request whose acknowledgment never arrived. Returns true when a
  /// pending request was actually timed out, so the caller counts it once.
  bool expireIfTimedOut(SteadyTime now, std::chrono::milliseconds timeout);

  CommandState state() const;
  /// The most recent completed exchange, for logs and the health surface.
  std::optional<CommandResult> lastResult() const;

 private:
  mutable std::mutex mutex_;
  CommandState state_{CommandState::Idle};
  std::uint8_t pending_command_{0};
  std::uint8_t pending_sequence_{0};
  SteadyTime sent_at_{};
  std::optional<CommandResult> last_result_;
};

}  // namespace tcu::can

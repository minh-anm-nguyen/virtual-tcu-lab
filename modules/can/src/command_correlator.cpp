#include "command_correlator.hpp"

namespace tcu::can {

bool CommandCorrelator::beginCommand(std::uint8_t command, std::uint8_t sequence, SteadyTime now) {
  std::lock_guard<std::mutex> lock(mutex_);

  if (state_ == CommandState::AwaitingAck) {
    return false;
  }

  pending_command_ = command;
  pending_sequence_ = sequence;
  sent_at_ = now;
  state_ = CommandState::AwaitingAck;
  return true;
}

void CommandCorrelator::abandonCommand() {
  std::lock_guard<std::mutex> lock(mutex_);
  state_ = CommandState::Idle;
}

AckOutcome CommandCorrelator::onAck(std::uint8_t sequence, std::uint8_t result) {
  std::lock_guard<std::mutex> lock(mutex_);

  // Nothing pending: this is a duplicate of an exchange that already closed, or
  // traffic from somebody else on the bus. Either way it must not be mistaken
  // for an answer (spec section 15, duplicate-ACK row).
  if (state_ != CommandState::AwaitingAck) {
    return AckOutcome::Unmatched;
  }

  // A different sequence is not our answer. The crucial part is what does NOT
  // happen here: the pending request is left exactly as it was, so the real
  // acknowledgment can still complete it (B-23, UT-18).
  if (sequence != pending_sequence_) {
    return AckOutcome::Unmatched;
  }

  last_result_ = CommandResult{sequence, result};
  state_ = CommandState::Idle;
  return AckOutcome::Matched;
}

bool CommandCorrelator::expireIfTimedOut(SteadyTime now, std::chrono::milliseconds timeout) {
  std::lock_guard<std::mutex> lock(mutex_);

  if (state_ != CommandState::AwaitingAck) {
    return false;
  }
  // Strictly greater, for the same reason the freshness deadline is: the
  // boundary instant belongs to the request, not to the timeout (research R-12).
  if (!(now > sent_at_ + timeout)) {
    return false;
  }

  // Deliberately leaves last_result_ alone: a request that was never answered
  // has no result, and inventing one would be worse than reporting nothing.
  state_ = CommandState::Idle;
  return true;
}

CommandState CommandCorrelator::state() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return state_;
}

std::optional<CommandResult> CommandCorrelator::lastResult() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return last_result_;
}

}  // namespace tcu::can

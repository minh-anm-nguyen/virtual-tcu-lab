// UT-18, B-23, B-24, and the duplicate-ACK row of the spec section 15 fault
// matrix.
//
// The property these cases circle is the one that is easy to get wrong: an
// acknowledgment that does not match must leave the pending request ALONE.
// Clearing it would let a stray frame on the bus cancel a real request.

#include "command_correlator.hpp"

#include <gtest/gtest.h>

#include <cstdint>

#include "tcu/can/frame_catalog.hpp"

namespace {

using namespace tcu::can;
using namespace std::chrono_literals;

SteadyTime at(std::int64_t ms) { return SteadyTime{} + std::chrono::milliseconds{ms}; }

TEST(Correlator, StartsIdle) {
  CommandCorrelator correlator;
  EXPECT_EQ(correlator.state(), CommandState::Idle);
  EXPECT_FALSE(correlator.lastResult().has_value());
}

TEST(Correlator, CompletesOnMatchingAck) {
  CommandCorrelator correlator;
  ASSERT_TRUE(correlator.beginCommand(kCommandLock, 0x2A, at(0)));
  EXPECT_EQ(correlator.state(), CommandState::AwaitingAck);

  EXPECT_EQ(correlator.onAck(0x2A, kResultOk), AckOutcome::Matched);

  EXPECT_EQ(correlator.state(), CommandState::Idle);
  ASSERT_TRUE(correlator.lastResult().has_value());
  EXPECT_EQ(correlator.lastResult()->sequence, 0x2A);
  EXPECT_EQ(correlator.lastResult()->result, kResultOk);
}

TEST(Correlator, RecordsARejectedResult) {
  // A REJECTED answer is still an answer: the request completed, and the
  // outcome is what the caller needs (B-24).
  CommandCorrelator correlator;
  correlator.beginCommand(kCommandLock, 0x2A, at(0));

  EXPECT_EQ(correlator.onAck(0x2A, kResultRejected), AckOutcome::Matched);
  EXPECT_EQ(correlator.lastResult()->result, kResultRejected);
  EXPECT_EQ(correlator.state(), CommandState::Idle);
}

// UT-18, B-23. The decisive case.
TEST(Correlator, IgnoresUnmatchedAck) {
  CommandCorrelator correlator;
  correlator.beginCommand(kCommandLock, 0x2A, at(0));

  EXPECT_EQ(correlator.onAck(0x2B, kResultOk), AckOutcome::Unmatched);

  EXPECT_EQ(correlator.state(), CommandState::AwaitingAck)
      << "an unrelated acknowledgment cancelled a real pending request";
  EXPECT_FALSE(correlator.lastResult().has_value()) << "and it must not record a result";

  // The real answer still completes it.
  EXPECT_EQ(correlator.onAck(0x2A, kResultOk), AckOutcome::Matched);
  EXPECT_EQ(correlator.state(), CommandState::Idle);
}

// Spec section 15: "Duplicate command ACK | BCM fake sends duplicate response |
// Pending request already closed | Ignore duplicate; count anomaly."
TEST(Correlator, IgnoresDuplicateAck) {
  CommandCorrelator correlator;
  correlator.beginCommand(kCommandLock, 0x2A, at(0));
  ASSERT_EQ(correlator.onAck(0x2A, kResultOk), AckOutcome::Matched);

  EXPECT_EQ(correlator.onAck(0x2A, kResultOk), AckOutcome::Unmatched)
      << "the exchange is closed; a second copy answers nothing";
  EXPECT_EQ(correlator.state(), CommandState::Idle);
}

TEST(Correlator, IgnoresAckWhenNothingWasSent) {
  // Another requester on the bus, or the BCM talking to somebody else.
  CommandCorrelator correlator;
  EXPECT_EQ(correlator.onAck(0x2A, kResultOk), AckOutcome::Unmatched);
  EXPECT_FALSE(correlator.lastResult().has_value());
}

// Research R-20: one command in flight, which is what makes the 8-bit sequence
// wrap harmless.
TEST(Correlator, RejectsSecondInFlightCommand) {
  CommandCorrelator correlator;
  ASSERT_TRUE(correlator.beginCommand(kCommandLock, 0x2A, at(0)));

  EXPECT_FALSE(correlator.beginCommand(kCommandUnlock, 0x2B, at(10)));
  EXPECT_EQ(correlator.state(), CommandState::AwaitingAck);

  // And the first request is untouched: its acknowledgment still matches.
  EXPECT_EQ(correlator.onAck(0x2A, kResultOk), AckOutcome::Matched);
}

TEST(Correlator, AcceptsANewCommandOnceTheLastOneClosed) {
  CommandCorrelator correlator;
  correlator.beginCommand(kCommandLock, 0x2A, at(0));
  correlator.onAck(0x2A, kResultOk);

  EXPECT_TRUE(correlator.beginCommand(kCommandUnlock, 0x2B, at(10)));
}

TEST(Correlator, AbandonFreesTheSlot) {
  // A transmit that failed never reached the bus, so holding the slot would
  // block the next attempt forever.
  CommandCorrelator correlator;
  ASSERT_TRUE(correlator.beginCommand(kCommandLock, 0x2A, at(0)));

  correlator.abandonCommand();

  EXPECT_EQ(correlator.state(), CommandState::Idle);
  EXPECT_TRUE(correlator.beginCommand(kCommandLock, 0x2B, at(10)));
}

TEST(Correlator, TimesOutPendingCommand) {
  CommandCorrelator correlator;
  correlator.beginCommand(kCommandLock, 0x2A, at(0));

  EXPECT_FALSE(correlator.expireIfTimedOut(at(1000), 1000ms))
      << "the boundary instant belongs to the request, not to the timeout";
  EXPECT_EQ(correlator.state(), CommandState::AwaitingAck);

  EXPECT_TRUE(correlator.expireIfTimedOut(at(1001), 1000ms));
  EXPECT_EQ(correlator.state(), CommandState::Idle);
}

TEST(Correlator, TimeoutReportsOnlyOnce) {
  // The caller counts can_ack_timeout_total from this return value, so a tick
  // that finds nothing pending must say so.
  CommandCorrelator correlator;
  correlator.beginCommand(kCommandLock, 0x2A, at(0));

  EXPECT_TRUE(correlator.expireIfTimedOut(at(2000), 1000ms));
  EXPECT_FALSE(correlator.expireIfTimedOut(at(3000), 1000ms));
}

TEST(Correlator, TimeoutRecordsNoResult) {
  // A request nobody answered has no result, and inventing one would be worse
  // than reporting nothing.
  CommandCorrelator correlator;
  correlator.beginCommand(kCommandLock, 0x2A, at(0));
  correlator.expireIfTimedOut(at(5000), 1000ms);

  EXPECT_FALSE(correlator.lastResult().has_value());
}

TEST(Correlator, TimeoutOnIdleIsNotAnEvent) {
  CommandCorrelator correlator;
  EXPECT_FALSE(correlator.expireIfTimedOut(at(100000), 1000ms));
}

TEST(Correlator, AnAckAfterATimeoutIsUnmatched) {
  // A late answer arrives after the request was abandoned. It must not be
  // credited to a request that no longer exists.
  CommandCorrelator correlator;
  correlator.beginCommand(kCommandLock, 0x2A, at(0));
  ASSERT_TRUE(correlator.expireIfTimedOut(at(2000), 1000ms));

  EXPECT_EQ(correlator.onAck(0x2A, kResultOk), AckOutcome::Unmatched);
}

// Spec section 11.3: the sequence byte wraps and carries no uniqueness
// guarantee. With one command in flight that is harmless, and this case says so
// out loud.
TEST(Correlator, SequenceWrapIsHarmlessWithOneInFlight) {
  CommandCorrelator correlator;
  for (int round = 0; round < 300; ++round) {
    const auto sequence = static_cast<std::uint8_t>(round);  // wraps at 256
    ASSERT_TRUE(correlator.beginCommand(kCommandLock, sequence, at(round * 10)));
    ASSERT_EQ(correlator.onAck(sequence, kResultOk), AckOutcome::Matched);
    EXPECT_EQ(correlator.lastResult()->sequence, sequence);
  }
}

}  // namespace

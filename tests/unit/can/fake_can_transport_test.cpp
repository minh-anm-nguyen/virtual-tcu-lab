// The fake is public API (research R-22), so its own behaviour is under test:
// projects 06 and 07 will depend on these semantics.

#include "tcu/can/fake_can_transport.hpp"

#include <gtest/gtest.h>

#include <cerrno>
#include <future>

#include "tcu/can/frame_catalog.hpp"

namespace {

using namespace tcu::can;
using namespace std::chrono_literals;

CanMessage speedFrame() {
  CanMessage msg;
  msg.id = 0x101;
  msg.length = 8;
  msg.data = {0x70, 0x17, 0, 0, 0, 0, 0, 0};
  return msg;
}

TEST(FakeTransport, DeliversPushedFrames) {
  FakeCanTransport transport;
  transport.pushFrame(speedFrame());

  const auto result = transport.receive(0ms);
  EXPECT_EQ(result.status, ReadStatus::Received);
  ASSERT_TRUE(result.message.has_value());
  EXPECT_EQ(result.message->id, 0x101u);
  EXPECT_EQ(result.message->data[0], 0x70);
}

TEST(FakeTransport, DeliversInFifoOrder) {
  FakeCanTransport transport;
  for (std::uint32_t id : {0x100u, 0x101u, 0x102u}) {
    CanMessage msg;
    msg.id = id;
    msg.length = 8;
    transport.pushFrame(msg);
  }

  EXPECT_EQ(transport.receive(0ms).message->id, 0x100u);
  EXPECT_EQ(transport.receive(0ms).message->id, 0x101u);
  EXPECT_EQ(transport.receive(0ms).message->id, 0x102u);
}

TEST(FakeTransport, TimeoutWhenEmpty) {
  FakeCanTransport transport;
  const auto result = transport.receive(0ms);

  EXPECT_EQ(result.status, ReadStatus::Timeout) << "an empty bus is normal, not an error";
  EXPECT_FALSE(result.message.has_value());
  EXPECT_EQ(transport.timeoutCount(), 1u);
}

// UT-16, B-19: the degrade-and-retry path needs a retryable read failure
// without a real interface having to disappear.
TEST(FakeTransport, InjectsEio) {
  FakeCanTransport transport;
  transport.pushReadStatus(ReadStatus::LinkError, EIO);

  const auto result = transport.receive(0ms);
  EXPECT_EQ(result.status, ReadStatus::LinkError);
  EXPECT_EQ(result.os_error, EIO);
  EXPECT_FALSE(result.message.has_value()) << "a failure carries no frame";
}

TEST(FakeTransport, RecordsSentFrames) {
  FakeCanTransport transport;
  CanMessage command;
  command.id = 0x200;
  command.length = 8;
  command.data = {kCommandLock, 0x2A, 0, 0, 0, 0, 0, 0};

  EXPECT_EQ(transport.send(command), WriteStatus::Sent);

  const auto sent = transport.sent();
  ASSERT_EQ(sent.size(), 1u);
  EXPECT_EQ(sent[0].id, 0x200u);
  EXPECT_EQ(sent[0].data[1], 0x2A);
}

TEST(FakeTransport, ForcedSendFailureRecordsNothing) {
  FakeCanTransport transport;
  transport.setSendStatus(WriteStatus::LinkUnavailable);

  EXPECT_EQ(transport.send(speedFrame()), WriteStatus::LinkUnavailable);
  EXPECT_TRUE(transport.sent().empty()) << "a refused write must not look like a sent frame";
}

// pushRawId mirrors what the kernel hands over, so the flag-splitting rule in
// src/can_flags.hpp has one implementation and one test path.
TEST(FakeTransport, PushRawIdSplitsStandardDataFrame) {
  FakeCanTransport transport;
  transport.pushRawId(0x101, {0x70, 0x17, 0, 0, 0, 0, 0, 0});

  const auto msg = transport.receive(0ms).message;
  ASSERT_TRUE(msg.has_value());
  EXPECT_EQ(msg->id, 0x101u);
  EXPECT_EQ(msg->format, FrameFormat::Standard11);
  EXPECT_EQ(msg->kind, FrameKind::Data);
  EXPECT_EQ(msg->length, 8);
}

TEST(FakeTransport, PushRawIdPreservesExtendedFlag) {
  FakeCanTransport transport;
  // EFF set, low 11 bits equal the speed identifier: B-14 aliasing attempt.
  transport.pushRawId(0x80000000u | 0x101u, {0x70, 0x17, 0, 0, 0, 0, 0, 0});

  const auto msg = transport.receive(0ms).message;
  ASSERT_TRUE(msg.has_value());
  // The numeric identifier IS 0x101 - that is the whole danger. Nothing in the
  // number distinguishes this frame from the standard speed frame, so `format`
  // is the only thing standing between an extended frame and a decoded speed
  // signal. B-14 therefore tests the format field, not identifier arithmetic,
  // and Validator.RejectsExtendedNoAlias is where the rejection is asserted.
  EXPECT_EQ(msg->id, 0x101u);
  EXPECT_EQ(msg->format, FrameFormat::Extended29) << "the flag must survive the split";
}

TEST(FakeTransport, PushRawIdPreservesRemoteAndErrorFlags) {
  FakeCanTransport transport;
  transport.pushRawId(0x40000000u | 0x101u, {});
  transport.pushRawId(0x20000000u | 0x101u, {});

  EXPECT_EQ(transport.receive(0ms).message->kind, FrameKind::Remote);
  EXPECT_EQ(transport.receive(0ms).message->kind, FrameKind::Error);
}

// UT-08 depends on this: a short frame on a known identifier must survive the
// fake unchanged, or the length rejection has nothing to reject.
TEST(FakeTransport, PushRawIdKeepsShortLength) {
  FakeCanTransport transport;
  transport.pushRawId(0x101, {0x70, 0x17, 0x00});

  const auto msg = transport.receive(0ms).message;
  ASSERT_TRUE(msg.has_value());
  EXPECT_EQ(msg->length, 3) << "not padded to the catalog expected 8";
}

TEST(FakeTransport, RequestStopWinsOverQueuedInput) {
  // Mirrors the real transport: a shutdown must not wait for a long script to
  // drain (spec section 13.8 step 2).
  FakeCanTransport transport;
  transport.pushFrame(speedFrame());
  transport.requestStop();

  EXPECT_EQ(transport.receive(0ms).status, ReadStatus::Interrupted);
}

TEST(FakeTransport, RequestStopWakesABlockedReceive) {
  FakeCanTransport transport;
  auto blocked =
      std::async(std::launch::async, [&transport] { return transport.receive(60s).status; });
  transport.requestStop();

  EXPECT_EQ(blocked.get(), ReadStatus::Interrupted);
}

TEST(FakeTransport, RequestStopIsIdempotent) {
  FakeCanTransport transport;
  transport.requestStop();
  transport.requestStop();
  EXPECT_EQ(transport.receive(0ms).status, ReadStatus::Interrupted);
}

}  // namespace

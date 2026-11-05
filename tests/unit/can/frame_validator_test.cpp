// UT-05..UT-08, UT-20, B-10, B-13, B-14, B-15.
//
// Every malformed frame here is constructed in memory rather than sent over
// vcan0: spec section 18.1's test note says the kernel refuses to carry them,
// and forbids claiming an impossible on-wire test passed.

#include "tcu/can/frame_validator.hpp"

#include <gtest/gtest.h>

#include "tcu/can/frame_catalog.hpp"

namespace {

using namespace tcu::can;

CanMessage frame(std::uint32_t id, std::array<std::uint8_t, 8> data, std::uint8_t length = 8) {
  CanMessage msg;
  msg.id = id;
  msg.data = data;
  msg.length = length;
  return msg;
}

TEST(Validator, AcceptsGoldenVectors) {
  const FrameValidator validator;
  // contracts/frame_catalog.md section 3.
  EXPECT_TRUE(validator.validate(frame(0x100, {0x01, 0, 0, 0, 0, 0, 0, 0})).ok);
  EXPECT_TRUE(validator.validate(frame(0x101, {0x70, 0x17, 0, 0, 0, 0, 0, 0})).ok);
  EXPECT_TRUE(validator.validate(frame(0x102, {0xB0, 0x04, 0x55, 0, 0, 0, 0, 0})).ok);
  EXPECT_TRUE(validator.validate(frame(0x103, {0x10, 0, 0, 0, 0, 0, 0, 0})).ok);
  EXPECT_TRUE(validator.validate(frame(0x110, {0x01, 0, 0, 0, 0, 0, 0, 0})).ok);
  EXPECT_TRUE(validator.validate(frame(0x201, {0x2A, 0x00, 0, 0, 0, 0, 0, 0})).ok);
}

// UT-06, B-14: an extended identifier whose low 11 bits equal 0x101 must be
// rejected, never decoded as the standard speed frame.
TEST(Validator, RejectsExtendedNoAlias) {
  const FrameValidator validator;
  CanMessage msg = frame(0x101, {0x70, 0x17, 0, 0, 0, 0, 0, 0});
  msg.format = FrameFormat::Extended29;

  const auto result = validator.validate(msg);
  EXPECT_FALSE(result.ok);
  EXPECT_EQ(result.reason, RejectReason::Flags);
}

// UT-07, B-13.
TEST(Validator, RejectsRtr) {
  const FrameValidator validator;
  CanMessage msg = frame(0x101, {0x70, 0x17, 0, 0, 0, 0, 0, 0});
  msg.kind = FrameKind::Remote;

  const auto result = validator.validate(msg);
  EXPECT_FALSE(result.ok);
  EXPECT_EQ(result.reason, RejectReason::Flags);
}

// UT-07, B-15: an error frame is a transport event, not domain data.
TEST(Validator, RejectsErrorFrame) {
  const FrameValidator validator;
  CanMessage msg = frame(0x101, {0x70, 0x17, 0, 0, 0, 0, 0, 0});
  msg.kind = FrameKind::Error;

  const auto result = validator.validate(msg);
  EXPECT_FALSE(result.ok);
  EXPECT_EQ(result.reason, RejectReason::Flags);
}

// Flags are checked before the identifier, so a malformed unknown frame is
// classified by its worst problem rather than by the lookup that happens to
// fail first.
TEST(Validator, FlagsAreCheckedBeforeIdentifier) {
  const FrameValidator validator;
  CanMessage msg = frame(0x555, {});
  msg.kind = FrameKind::Error;

  EXPECT_EQ(validator.validate(msg).reason, RejectReason::Flags);
}

// UT-08, B-10. Spec section 6 fixes 8 bytes for every V1 identifier.
TEST(Validator, RejectsLength) {
  const FrameValidator validator;
  for (const std::uint8_t length : {0, 1, 7}) {
    const auto result = validator.validate(frame(0x101, {0x70, 0x17, 0, 0, 0, 0, 0, 0}, length));
    EXPECT_FALSE(result.ok) << "length " << static_cast<int>(length);
    EXPECT_EQ(result.reason, RejectReason::Length) << "length " << static_cast<int>(length);
  }
}

// UT-05, B-12: an unknown identifier is counted as unknown, NOT as invalid.
// contracts/metrics.md keeps can_unknown_id_total and can_rx_invalid_total
// apart because they mean different things to an operator.
TEST(Validator, UnknownIdIsNotInvalid) {
  const FrameValidator validator;
  const auto result = validator.validate(frame(0x555, {}));

  EXPECT_FALSE(result.ok);
  EXPECT_EQ(result.reason, RejectReason::UnknownId);
}

// UT-20, research R-15: counted, not rejected.
TEST(Validator, FlagsReservedNonzeroWithoutRejecting) {
  const FrameValidator validator;
  // 0x101 uses bytes 0-1; byte 4 is reserved.
  const auto result = validator.validate(frame(0x101, {0x70, 0x17, 0, 0, 0x99, 0, 0, 0}));

  EXPECT_TRUE(result.ok) << "V1 accepts the frame";
  EXPECT_TRUE(result.reserved_nonzero) << "but records the anomaly";
  EXPECT_EQ(result.reason, RejectReason::None);
}

TEST(Validator, ReservedBitsInsideAPartiallyUsedByteAreDetected) {
  const FrameValidator validator;
  // 0x100 uses byte 0 bit 0 only; bits 1-7 are reserved (mask 0xFE).
  const auto clean = validator.validate(frame(0x100, {0x01, 0, 0, 0, 0, 0, 0, 0}));
  EXPECT_TRUE(clean.ok);
  EXPECT_FALSE(clean.reserved_nonzero);

  const auto dirty = validator.validate(frame(0x100, {0x03, 0, 0, 0, 0, 0, 0, 0}));
  EXPECT_TRUE(dirty.ok);
  EXPECT_TRUE(dirty.reserved_nonzero);
}

TEST(Validator, CleanGoldenVectorsHaveNoReservedAnomaly) {
  const FrameValidator validator;
  EXPECT_FALSE(validator.validate(frame(0x102, {0xB0, 0x04, 0x55, 0, 0, 0, 0, 0})).reserved_nonzero);
  EXPECT_FALSE(validator.validate(frame(0x103, {0x10, 0, 0, 0, 0, 0, 0, 0})).reserved_nonzero);
}

}  // namespace

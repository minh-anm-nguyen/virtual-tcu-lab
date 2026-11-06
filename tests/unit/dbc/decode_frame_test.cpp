// T028/T034/T035: frame-level decisions, scaling, enums and the policy rules.
// Invariants P-05, P-06, P-11..P-16; contracts B-02, B-03, B-06, B-07, B-08,
// B-16; FR-08, FR-10, FR-13.

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>

#include "tcu/dbc/dbc_parser.hpp"
#include "tcu/dbc/signal_decoder.hpp"

namespace {

using tcu::dbc::CanFrameView;
using tcu::dbc::DecodeError;
using tcu::dbc::FrameFormat;
using tcu::dbc::SignalQuality;

std::string readFixture(const std::string& relative) {
  std::ifstream in(std::string(TCU_DBC_FIXTURE_DIR) + "/" + relative, std::ios::binary);
  std::ostringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

std::shared_ptr<const tcu::dbc::CompiledDbc> labDatabase() {
  auto result = tcu::dbc::loadDbcBytes(readFixture("virtual_tcu.dbc"), "virtual_tcu.dbc");
  return result.db;
}

CanFrameView frameOf(std::uint32_t id, const std::uint8_t* data, std::uint8_t size,
                     FrameFormat format = FrameFormat::Standard11) {
  CanFrameView frame;
  frame.arbitrationId = id;
  frame.format = format;
  frame.data = data;
  frame.size = size;
  frame.rxTime = std::chrono::steady_clock::time_point{std::chrono::milliseconds{1234}};
  return frame;
}

const tcu::dbc::SignalValue* find(const tcu::dbc::DecodedBatch& batch, const char* name) {
  for (const auto& value : batch.signals) {
    if (value.signalName == name) return &value;
  }
  return nullptr;
}

constexpr std::array<std::uint8_t, 8> kVehicle{0x10, 0x27, 0x01, 0x00, 0x50, 0x00, 0x00, 0x00};
constexpr std::array<std::uint8_t, 8> kTorque{0xAB, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
constexpr std::array<std::uint8_t, 8> kMuxPage1{0x01, 0x7F, 0xD2, 0x04, 0x00, 0x00, 0x00, 0x00};
constexpr std::array<std::uint8_t, 8> kMuxPage2{0x02, 0x7F, 0x9C, 0xFF, 0x00, 0x00, 0x00, 0x00};

TEST(Scaling_PhysicalFromFactorOffset, SpecSection73VehicleStatus) {
  tcu::dbc::SignalDecoder decoder(labDatabase(), tcu::dbc::DecodePolicy{});
  const auto result = decoder.decode(frameOf(256, kVehicle.data(), 8));
  ASSERT_EQ(result.error, DecodeError::None);
  ASSERT_TRUE(result.batch.has_value());

  const auto* speed = find(*result.batch, "VehicleSpeed");
  ASSERT_NE(speed, nullptr);
  EXPECT_EQ(speed->raw, 0x2710u);
  EXPECT_DOUBLE_EQ(speed->physical, 100.0);
  EXPECT_EQ(speed->unit, "km/h");
  EXPECT_EQ(speed->quality, SignalQuality::Valid);

  const auto* ignition = find(*result.batch, "Ignition");
  ASSERT_NE(ignition, nullptr);
  EXPECT_EQ(ignition->raw, 1u);
  EXPECT_DOUBLE_EQ(ignition->physical, 1.0);

  const auto* cabin = find(*result.batch, "CabinTemp");
  ASSERT_NE(cabin, nullptr);
  EXPECT_DOUBLE_EQ(cabin->physical, -40.0);
  EXPECT_EQ(cabin->unit, "degC");
}

TEST(Scaling_PhysicalFromFactorOffset, SpecSection73MotorolaSigned) {
  tcu::dbc::SignalDecoder decoder(labDatabase(), tcu::dbc::DecodePolicy{});
  const auto result = decoder.decode(frameOf(257, kTorque.data(), 8));
  ASSERT_EQ(result.error, DecodeError::None);
  const auto* torque = find(*result.batch, "Torque");
  ASSERT_NE(torque, nullptr);
  EXPECT_EQ(torque->raw, 0xABCu);
  ASSERT_TRUE(torque->signedRaw.has_value());
  EXPECT_EQ(*torque->signedRaw, -1348);
  EXPECT_DOUBLE_EQ(torque->physical, -674.0);
  EXPECT_EQ(torque->quality, SignalQuality::Valid);
}

TEST(Decode_EnumLabelResolved, LabelledAndUnlabelledValues) {
  tcu::dbc::SignalDecoder decoder(labDatabase(), tcu::dbc::DecodePolicy{});
  const auto on = decoder.decode(frameOf(256, kVehicle.data(), 8));
  ASSERT_TRUE(on.batch.has_value());
  const auto* ignition = find(*on.batch, "Ignition");
  ASSERT_NE(ignition, nullptr);
  ASSERT_TRUE(ignition->enumLabel.has_value());
  EXPECT_EQ(*ignition->enumLabel, "On");

  std::array<std::uint8_t, 8> off = kVehicle;
  off[2] = 0x00;  // clear bit 16
  const auto result = decoder.decode(frameOf(256, off.data(), 8));
  ASSERT_TRUE(result.batch.has_value());
  const auto* offIgnition = find(*result.batch, "Ignition");
  ASSERT_NE(offIgnition, nullptr);
  ASSERT_TRUE(offIgnition->enumLabel.has_value());
  EXPECT_EQ(*offIgnition->enumLabel, "Off");

  // A signal with no VAL_ has no label but still has its number (D-11).
  const auto* speed = find(*result.batch, "VehicleSpeed");
  ASSERT_NE(speed, nullptr);
  EXPECT_FALSE(speed->enumLabel.has_value());
  EXPECT_EQ(speed->raw, 0x2710u);
}

TEST(Decode_WrongLengthRejected, ShortPayloadDecodesNothing) {
  tcu::dbc::SignalDecoder decoder(labDatabase(), tcu::dbc::DecodePolicy{});
  const auto result = decoder.decode(frameOf(256, kVehicle.data(), 7));
  EXPECT_EQ(result.error, DecodeError::WrongLength);
  EXPECT_FALSE(result.batch.has_value());
}

TEST(Decode_RejectsOversizePayload, AboveClassicalMaximum) {
  tcu::dbc::SignalDecoder decoder(labDatabase(), tcu::dbc::DecodePolicy{});
  const std::array<std::uint8_t, 16> big{};
  const auto result = decoder.decode(frameOf(256, big.data(), 16));
  EXPECT_EQ(result.error, DecodeError::WrongLength);
}

TEST(Decode_UnknownIdCounted, EmptyBatchNoThrow) {
  tcu::dbc::SignalDecoder decoder(labDatabase(), tcu::dbc::DecodePolicy{});
  const auto result = decoder.decode(frameOf(0x123, kVehicle.data(), 8));
  EXPECT_EQ(result.error, DecodeError::UnknownMessage);
  EXPECT_FALSE(result.batch.has_value());
}

TEST(Decode_UnknownIdCounted, ExtendedFrameIsNotAliasedOntoStandard) {
  // B-07: the lab DBC defines standard 256 only.
  tcu::dbc::SignalDecoder decoder(labDatabase(), tcu::dbc::DecodePolicy{});
  const auto result =
      decoder.decode(frameOf(256, kVehicle.data(), 8, FrameFormat::Extended29));
  EXPECT_EQ(result.error, DecodeError::UnknownMessage);
}

TEST(Decode_ErrorFrameRejected, BeforeAnyLookup) {
  tcu::dbc::SignalDecoder decoder(labDatabase(), tcu::dbc::DecodePolicy{});
  auto frame = frameOf(256, nullptr, 8);
  frame.isError = true;
  const auto result = decoder.decode(frame);
  EXPECT_EQ(result.error, DecodeError::ErrorFrame);
  EXPECT_FALSE(result.batch.has_value());
}

TEST(Decode_RemoteFrameRejected, BeforeAnyLookup) {
  tcu::dbc::SignalDecoder decoder(labDatabase(), tcu::dbc::DecodePolicy{});
  auto frame = frameOf(256, nullptr, 8);
  frame.isRemote = true;
  const auto result = decoder.decode(frame);
  EXPECT_EQ(result.error, DecodeError::RemoteFrame);
}

TEST(Decode_BatchHasOneSchemaIdentity, VersionAndHashComeFromTheDatabase) {
  auto db = labDatabase();
  tcu::dbc::SignalDecoder decoder(db, tcu::dbc::DecodePolicy{});
  const auto result = decoder.decode(frameOf(256, kVehicle.data(), 8));
  ASSERT_TRUE(result.batch.has_value());
  EXPECT_EQ(result.batch->dbcVersion, db->version());
  EXPECT_EQ(result.batch->dbcHash, db->hash());
  EXPECT_EQ(result.batch->rxTime,
            std::chrono::steady_clock::time_point{std::chrono::milliseconds{1234}});
}

TEST(Decode_NeverThrowsForAnyPayload, EveryLengthAndIdentifier) {
  tcu::dbc::SignalDecoder decoder(labDatabase(), tcu::dbc::DecodePolicy{});
  const std::array<std::uint8_t, 8> payload{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  EXPECT_NO_THROW({
    for (std::uint32_t id = 250; id < 262; ++id) {
      for (std::uint8_t size = 0; size <= 8; ++size) {
        (void)decoder.decode(frameOf(id, payload.data(), size));
        (void)decoder.decode(frameOf(id, payload.data(), size, FrameFormat::Extended29));
      }
    }
  });
}

}  // namespace

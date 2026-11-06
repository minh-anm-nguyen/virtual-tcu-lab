// T026: the six hand-computed vectors of spec section 7.3, written by hand and
// placed FIRST, before any generated vector exists. They are the check on the
// oracle, not the other way round (research R-20).

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <limits>

#include "tcu/dbc/bit_extractor.hpp"

namespace {

using tcu::dbc::ByteOrder;
using tcu::dbc::extractRaw;

// Spec section 7.3, message 0x100.
constexpr std::array<std::uint8_t, 8> kVehicleStatus{0x10, 0x27, 0x01, 0x00, 0x50, 0x00, 0x00, 0x00};
// Spec section 7.3, message 0x101.
constexpr std::array<std::uint8_t, 8> kTorqueStatus{0xAB, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

TEST(BitExtractor_IntelGoldens, VehicleSpeedIsTenThousandRaw) {
  const auto raw = extractRaw(kVehicleStatus.data(), kVehicleStatus.size(), 0, 16, ByteOrder::Intel);
  ASSERT_TRUE(raw.has_value());
  EXPECT_EQ(*raw, 0x2710u);
  EXPECT_DOUBLE_EQ(static_cast<double>(*raw) * 0.01, 100.0);
}

TEST(BitExtractor_IntelGoldens, IgnitionIsBitSixteen) {
  const auto raw = extractRaw(kVehicleStatus.data(), kVehicleStatus.size(), 16, 1, ByteOrder::Intel);
  ASSERT_TRUE(raw.has_value());
  EXPECT_EQ(*raw, 1u);
}

TEST(BitExtractor_IntelGoldens, CabinTempIsMinusForty) {
  const auto raw = extractRaw(kVehicleStatus.data(), kVehicleStatus.size(), 24, 8, ByteOrder::Intel);
  ASSERT_TRUE(raw.has_value());
  EXPECT_EQ(*raw, 0x00u);
  EXPECT_DOUBLE_EQ(static_cast<double>(*raw) * 1.0 + -40.0, -40.0);
}

TEST(BitExtractor_IntelGoldens, CabinTempDoesNotReadByteFour) {
  // Spec section 7.3's note: 0x50 sits at index 4 and is NOT part of CabinTemp,
  // which lives at index 3. The distinction is what catches an off-by-one.
  auto payload = kVehicleStatus;
  payload[4] = 0xFF;
  const auto raw = extractRaw(payload.data(), payload.size(), 24, 8, ByteOrder::Intel);
  ASSERT_TRUE(raw.has_value());
  EXPECT_EQ(*raw, 0x00u) << "CabinTemp read a byte that does not belong to it";
}

TEST(BitExtractor_MotorolaGoldens, TorqueRawIsAbc) {
  const auto raw = extractRaw(kTorqueStatus.data(), kTorqueStatus.size(), 7, 12, ByteOrder::Motorola);
  ASSERT_TRUE(raw.has_value());
  EXPECT_EQ(*raw, 0xABCu);
}

TEST(BitExtractor_CrossByteMotorola, SpansThreeBytesWithTheSawtoothStep) {
  // 0x12 0x34 0x56: starting at bit 7 (MSB of byte 0), 24 bits big-endian must
  // read the three bytes in order.
  const std::array<std::uint8_t, 3> payload{0x12, 0x34, 0x56};
  const auto raw = extractRaw(payload.data(), payload.size(), 7, 24, ByteOrder::Motorola);
  ASSERT_TRUE(raw.has_value());
  EXPECT_EQ(*raw, 0x123456u);
}

TEST(BitExtractor_CrossByteMotorola, UnalignedStartCrossesAByteBoundary) {
  // Start at bit 3 of byte 0 (value 0b1010_0101 = 0xA5), 8 bits big-endian:
  // reads bits 3,2,1,0 of byte 0 then 7,6,5,4 of byte 1.
  const std::array<std::uint8_t, 2> payload{0xA5, 0x3C};
  const auto raw = extractRaw(payload.data(), payload.size(), 3, 8, ByteOrder::Motorola);
  ASSERT_TRUE(raw.has_value());
  EXPECT_EQ(*raw, 0x53u);
}

TEST(BitExtractor_Width1And64, Width64UnsignedIsExact) {
  const std::array<std::uint8_t, 8> all{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  const auto intel = extractRaw(all.data(), all.size(), 0, 64, ByteOrder::Intel);
  ASSERT_TRUE(intel.has_value());
  EXPECT_EQ(*intel, std::numeric_limits<std::uint64_t>::max());
  const auto motorola = extractRaw(all.data(), all.size(), 7, 64, ByteOrder::Motorola);
  ASSERT_TRUE(motorola.has_value());
  EXPECT_EQ(*motorola, std::numeric_limits<std::uint64_t>::max());
}

TEST(BitExtractor_Width1And64, EveryIntelSingleBitIsReadable) {
  const std::array<std::uint8_t, 2> payload{0b00000001, 0b10000000};
  for (unsigned bit = 0; bit < 16; ++bit) {
    const auto raw = extractRaw(payload.data(), payload.size(), bit, 1, ByteOrder::Intel);
    ASSERT_TRUE(raw.has_value()) << "bit " << bit;
    const unsigned expected = (bit == 0 || bit == 15) ? 1u : 0u;
    EXPECT_EQ(*raw, expected) << "bit " << bit;
  }
}

TEST(BitExtractor_RejectsOutOfRangePosition, NeverReadsPastTheFrame) {
  const std::array<std::uint8_t, 2> payload{0x11, 0x22};
  EXPECT_FALSE(extractRaw(payload.data(), payload.size(), 8, 16, ByteOrder::Intel).has_value());
  EXPECT_FALSE(extractRaw(payload.data(), payload.size(), 16, 1, ByteOrder::Intel).has_value());
  EXPECT_FALSE(extractRaw(payload.data(), payload.size(), 0, 65, ByteOrder::Intel).has_value());
  EXPECT_FALSE(extractRaw(payload.data(), payload.size(), 0, 0, ByteOrder::Intel).has_value());
  EXPECT_FALSE(extractRaw(nullptr, 8, 0, 8, ByteOrder::Intel).has_value());
}

TEST(BitExtractor_RejectsOutOfRangePosition, MotorolaWalkingOffTheEndIsRefused) {
  const std::array<std::uint8_t, 1> payload{0xFF};
  // Starting at the MSB of the only byte, 16 bits would need a second byte.
  EXPECT_FALSE(extractRaw(payload.data(), payload.size(), 7, 16, ByteOrder::Motorola).has_value());
}

}  // namespace

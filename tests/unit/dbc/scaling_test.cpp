// T028: the parts of scaling that are policy rather than arithmetic -
// non-finite results, 64-bit precision, invalid-raw codes and opt-in range
// enforcement. Invariants P-05, P-06; FR-06, FR-10; research R-04, R-05.

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <limits>
#include <string>

#include "tcu/dbc/dbc_parser.hpp"
#include "tcu/dbc/signal_decoder.hpp"

namespace {

using tcu::dbc::CanFrameView;
using tcu::dbc::DecodeError;
using tcu::dbc::SignalQuality;

std::shared_ptr<const tcu::dbc::CompiledDbc> databaseOf(const std::string& text) {
  auto result = tcu::dbc::loadDbcBytes(text, "scaling.dbc");
  return result.db;
}

CanFrameView frameOf(std::uint32_t id, const std::uint8_t* data, std::uint8_t size) {
  CanFrameView frame;
  frame.arbitrationId = id;
  frame.data = data;
  frame.size = size;
  return frame;
}

const tcu::dbc::SignalValue* find(const tcu::dbc::DecodedBatch& batch, const char* name) {
  for (const auto& value : batch.signals) {
    if (value.signalName == name) return &value;
  }
  return nullptr;
}

TEST(Scaling_NonFinitePhysicalIsDecodeError, OneBadSignalDoesNotSinkTheBatch) {
  // A huge factor plus a large raw value overflows a double to infinity. The
  // signal becomes DecodeError; the other signal in the frame is untouched
  // (invariant P-05, spec section 10.3 partial-batch rule).
  auto db = databaseOf(
      "VERSION \"lab\"\n"
      "BO_ 256 M: 8 ECU\n"
      " SG_ Huge : 0|32@1+ (1e300,0) [0|0] \"\" TCU\n"
      " SG_ Fine : 32|8@1+ (1,0) [0|255] \"\" TCU\n");
  ASSERT_NE(db, nullptr);
  tcu::dbc::SignalDecoder decoder(db, tcu::dbc::DecodePolicy{});

  const std::array<std::uint8_t, 8> payload{0xFF, 0xFF, 0xFF, 0xFF, 0x2A, 0, 0, 0};
  const auto result = decoder.decode(frameOf(256, payload.data(), 8));
  ASSERT_EQ(result.error, DecodeError::None);
  ASSERT_TRUE(result.batch.has_value());

  const auto* huge = find(*result.batch, "Huge");
  ASSERT_NE(huge, nullptr);
  EXPECT_EQ(huge->quality, SignalQuality::DecodeError);
  EXPECT_EQ(huge->raw, 0xFFFFFFFFu) << "the raw value is still reported exactly";

  const auto* fine = find(*result.batch, "Fine");
  ASSERT_NE(fine, nullptr);
  EXPECT_EQ(fine->quality, SignalQuality::Valid);
  EXPECT_DOUBLE_EQ(fine->physical, 42.0);
}

TEST(Scaling_RawU64PreservedAboveTwoPow53, ExactRawAndAFlaggedPhysical) {
  auto db = databaseOf(
      "VERSION \"lab\"\n"
      "BO_ 256 M: 8 ECU\n"
      " SG_ Wide : 0|64@1+ (1,0) [0|0] \"\" TCU\n");
  ASSERT_NE(db, nullptr);
  tcu::dbc::SignalDecoder decoder(db, tcu::dbc::DecodePolicy{});

  const std::array<std::uint8_t, 8> all{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  const auto result = decoder.decode(frameOf(256, all.data(), 8));
  ASSERT_TRUE(result.batch.has_value());
  const auto* wide = find(*result.batch, "Wide");
  ASSERT_NE(wide, nullptr);
  EXPECT_EQ(wide->raw, std::numeric_limits<std::uint64_t>::max())
      << "the raw value must survive exactly, whatever the double does";
  EXPECT_TRUE(wide->physicalPrecisionLoss);
  EXPECT_EQ(wide->quality, SignalQuality::Valid);
  EXPECT_FALSE(wide->signedRaw.has_value()) << "unsigned signals carry no signed raw (P-04)";
}

TEST(Scaling_RawU64PreservedAboveTwoPow53, SmallValuesAreNotFlagged) {
  auto db = databaseOf(
      "VERSION \"lab\"\n"
      "BO_ 256 M: 8 ECU\n"
      " SG_ Wide : 0|64@1+ (1,0) [0|0] \"\" TCU\n");
  tcu::dbc::SignalDecoder decoder(db, tcu::dbc::DecodePolicy{});
  const std::array<std::uint8_t, 8> small{0x2A, 0, 0, 0, 0, 0, 0, 0};
  const auto result = decoder.decode(frameOf(256, small.data(), 8));
  ASSERT_TRUE(result.batch.has_value());
  const auto* wide = find(*result.batch, "Wide");
  ASSERT_NE(wide, nullptr);
  EXPECT_FALSE(wide->physicalPrecisionLoss);
  EXPECT_DOUBLE_EQ(wide->physical, 42.0);
}

TEST(Decode_InvalidRawFromPolicy, OnlyWhenTheConfigurationSaysSo) {
  auto db = databaseOf(
      "VERSION \"lab\"\n"
      "BO_ 256 VEHICLE_STATUS: 8 ECU\n"
      " SG_ VehicleSpeed : 0|16@1+ (0.01,0) [0|250] \"km/h\" TCU\n");
  ASSERT_NE(db, nullptr);
  const std::array<std::uint8_t, 8> payload{0xFF, 0xFF, 0, 0, 0, 0, 0, 0};

  // Without a policy table the same frame is a perfectly valid 655.35.
  tcu::dbc::SignalDecoder permissive(db, tcu::dbc::DecodePolicy{});
  const auto allowed = permissive.decode(frameOf(256, payload.data(), 8));
  ASSERT_TRUE(allowed.batch.has_value());
  ASSERT_NE(find(*allowed.batch, "VehicleSpeed"), nullptr);
  EXPECT_EQ(find(*allowed.batch, "VehicleSpeed")->quality, SignalQuality::Valid);

  // With 65535 declared invalid, the signal is InvalidRaw - and the raw value
  // is still reported, so a diagnostic can show what arrived (FR-10).
  tcu::dbc::InvalidRawTable table;
  table["VEHICLE_STATUS.VehicleSpeed"] = {65535u};
  tcu::dbc::DecodePolicy policy;
  policy.invalidRaw = &table;
  tcu::dbc::SignalDecoder strict(db, policy);
  const auto refused = strict.decode(frameOf(256, payload.data(), 8));
  ASSERT_TRUE(refused.batch.has_value());
  const auto* speed = find(*refused.batch, "VehicleSpeed");
  ASSERT_NE(speed, nullptr);
  EXPECT_EQ(speed->quality, SignalQuality::InvalidRaw);
  EXPECT_EQ(speed->raw, 65535u);
}

TEST(Decode_RangeEnforcedOnlyWhenConfigured, AndNeverClamped) {
  auto db = databaseOf(
      "VERSION \"lab\"\n"
      "BO_ 256 VEHICLE_STATUS: 8 ECU\n"
      " SG_ VehicleSpeed : 0|16@1+ (0.01,0) [0|250] \"km/h\" TCU\n");
  ASSERT_NE(db, nullptr);
  // 0x7530 = 30000 -> 300.00 km/h, above the declared maximum of 250.
  const std::array<std::uint8_t, 8> payload{0x30, 0x75, 0, 0, 0, 0, 0, 0};

  // Default: bounds are metadata, so the value is Valid (research R-04).
  tcu::dbc::SignalDecoder metadata(db, tcu::dbc::DecodePolicy{});
  const auto lenient = metadata.decode(frameOf(256, payload.data(), 8));
  ASSERT_TRUE(lenient.batch.has_value());
  const auto* asMetadata = find(*lenient.batch, "VehicleSpeed");
  ASSERT_NE(asMetadata, nullptr);
  EXPECT_EQ(asMetadata->quality, SignalQuality::Valid);
  EXPECT_DOUBLE_EQ(asMetadata->physical, 300.0);

  // Opt in, and the same frame is OutOfRange - with the real value intact,
  // never clamped to 250 (spec section 7.2).
  tcu::dbc::RangeEnforcement enforcement;
  enforcement["VEHICLE_STATUS.VehicleSpeed"] = true;
  tcu::dbc::DecodePolicy policy;
  policy.rangeEnforce = &enforcement;
  tcu::dbc::SignalDecoder enforced(db, policy);
  const auto strict = enforced.decode(frameOf(256, payload.data(), 8));
  ASSERT_TRUE(strict.batch.has_value());
  const auto* checked = find(*strict.batch, "VehicleSpeed");
  ASSERT_NE(checked, nullptr);
  EXPECT_EQ(checked->quality, SignalQuality::OutOfRange);
  EXPECT_DOUBLE_EQ(checked->physical, 300.0) << "an out-of-range value must not be clamped";
}

TEST(Decode_RangeEnforcedOnlyWhenConfigured, InRangeValueStaysValid) {
  auto db = databaseOf(
      "VERSION \"lab\"\n"
      "BO_ 256 VEHICLE_STATUS: 8 ECU\n"
      " SG_ VehicleSpeed : 0|16@1+ (0.01,0) [0|250] \"km/h\" TCU\n");
  tcu::dbc::RangeEnforcement enforcement;
  enforcement["VEHICLE_STATUS.VehicleSpeed"] = true;
  tcu::dbc::DecodePolicy policy;
  policy.rangeEnforce = &enforcement;
  tcu::dbc::SignalDecoder decoder(db, policy);

  const std::array<std::uint8_t, 8> payload{0x10, 0x27, 0, 0, 0, 0, 0, 0};
  const auto result = decoder.decode(frameOf(256, payload.data(), 8));
  ASSERT_TRUE(result.batch.has_value());
  ASSERT_NE(find(*result.batch, "VehicleSpeed"), nullptr);
  EXPECT_EQ(find(*result.batch, "VehicleSpeed")->quality, SignalQuality::Valid);
}

}  // namespace

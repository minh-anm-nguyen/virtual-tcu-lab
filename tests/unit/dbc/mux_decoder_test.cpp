// T032: one-level multiplexing. The active branch and only the active branch,
// and never an invented one - invariants P-14, P-15; contracts B-04, B-05, B-20.

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
using tcu::dbc::SignalQuality;

std::string readFixture(const std::string& relative) {
  std::ifstream in(std::string(TCU_DBC_FIXTURE_DIR) + "/" + relative, std::ios::binary);
  std::ostringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

tcu::dbc::SignalDecoder labDecoder() {
  auto result = tcu::dbc::loadDbcBytes(readFixture("virtual_tcu.dbc"), "virtual_tcu.dbc");
  return tcu::dbc::SignalDecoder(result.db, tcu::dbc::DecodePolicy{});
}

CanFrameView muxFrame(const std::uint8_t* data) {
  CanFrameView frame;
  frame.arbitrationId = 258;
  frame.data = data;
  frame.size = 8;
  frame.rxTime = std::chrono::steady_clock::now();
  return frame;
}

const tcu::dbc::SignalValue* find(const tcu::dbc::DecodedBatch& batch, const char* name) {
  for (const auto& value : batch.signals) {
    if (value.signalName == name) return &value;
  }
  return nullptr;
}

constexpr std::array<std::uint8_t, 8> kPage1{0x01, 0x7F, 0xD2, 0x04, 0x00, 0x00, 0x00, 0x00};
constexpr std::array<std::uint8_t, 8> kPage2{0x02, 0x7F, 0x9C, 0xFF, 0x00, 0x00, 0x00, 0x00};
constexpr std::array<std::uint8_t, 8> kPage7{0x07, 0x7F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

TEST(Mux_Page1EmitsBatteryOnly, SpecSection73BatteryVoltage) {
  auto decoder = labDecoder();
  const auto result = decoder.decode(muxFrame(kPage1.data()));
  ASSERT_EQ(result.error, DecodeError::None);
  ASSERT_TRUE(result.batch.has_value());
  EXPECT_FALSE(result.batch->muxUnresolved);

  const auto* page = find(*result.batch, "Page");
  ASSERT_NE(page, nullptr);
  EXPECT_EQ(page->raw, 1u);

  const auto* common = find(*result.batch, "CommonStatus");
  ASSERT_NE(common, nullptr);
  EXPECT_EQ(common->raw, 0x7Fu);

  const auto* battery = find(*result.batch, "BatteryVoltage");
  ASSERT_NE(battery, nullptr);
  EXPECT_EQ(battery->raw, 0x04D2u);
  EXPECT_DOUBLE_EQ(battery->physical, 12.34);
  EXPECT_EQ(battery->quality, SignalQuality::Valid);

  // B-04: the inactive branch is ABSENT, not present-and-invalid.
  EXPECT_EQ(find(*result.batch, "MotorTemp"), nullptr);
}

TEST(Mux_Page2EmitsMotorTempOnly, SpecSection73MotorTemp) {
  auto decoder = labDecoder();
  const auto result = decoder.decode(muxFrame(kPage2.data()));
  ASSERT_EQ(result.error, DecodeError::None);
  ASSERT_TRUE(result.batch.has_value());
  EXPECT_FALSE(result.batch->muxUnresolved);

  const auto* motor = find(*result.batch, "MotorTemp");
  ASSERT_NE(motor, nullptr);
  EXPECT_EQ(motor->raw, 0xFF9Cu);
  ASSERT_TRUE(motor->signedRaw.has_value());
  EXPECT_EQ(*motor->signedRaw, -100);
  EXPECT_DOUBLE_EQ(motor->physical, -10.0);

  EXPECT_EQ(find(*result.batch, "BatteryVoltage"), nullptr);
}

TEST(Mux_UnknownSelectorKeepsUnconditional, B20NoInventedBranch) {
  auto decoder = labDecoder();
  const auto result = decoder.decode(muxFrame(kPage7.data()));
  ASSERT_EQ(result.error, DecodeError::None);
  ASSERT_TRUE(result.batch.has_value());
  EXPECT_TRUE(result.batch->muxUnresolved);

  // The selector and the unconditional signal still come out.
  ASSERT_NE(find(*result.batch, "Page"), nullptr);
  ASSERT_NE(find(*result.batch, "CommonStatus"), nullptr);
  // No branch signal was guessed.
  EXPECT_EQ(find(*result.batch, "BatteryVoltage"), nullptr);
  EXPECT_EQ(find(*result.batch, "MotorTemp"), nullptr);
}

TEST(Mux_UnconditionalSignalsAreNeverBranchDependent, SameAcrossPages) {
  auto decoder = labDecoder();
  const auto page1 = decoder.decode(muxFrame(kPage1.data()));
  const auto page2 = decoder.decode(muxFrame(kPage2.data()));
  ASSERT_TRUE(page1.batch.has_value());
  ASSERT_TRUE(page2.batch.has_value());
  const auto* a = find(*page1.batch, "CommonStatus");
  const auto* b = find(*page2.batch, "CommonStatus");
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  EXPECT_EQ(a->raw, b->raw);
}

TEST(Mux_UndecodableSelectorSkipsBranches, SelectorOutsideAShortPayload) {
  // A message whose selector lives in the last byte, decoded against a shorter
  // declared length, is caught by the length check first; this asserts the
  // branch-only rejection path with a selector that cannot be read.
  const auto parsed = tcu::dbc::loadDbcBytes(
      "VERSION \"lab\"\n"
      "BO_ 300 M: 1 ECU\n"
      " SG_ Page M : 0|8@1+ (1,0) [0|255] \"\" TCU\n",
      "mux.dbc");
  ASSERT_TRUE(parsed.ok()) << (parsed.diagnostics.empty() ? "" : parsed.diagnostics[0].message);
  tcu::dbc::SignalDecoder decoder(parsed.db, tcu::dbc::DecodePolicy{});

  const std::array<std::uint8_t, 1> payload{0x05};
  CanFrameView frame;
  frame.arbitrationId = 300;
  frame.data = payload.data();
  frame.size = 1;
  const auto result = decoder.decode(frame);
  ASSERT_EQ(result.error, DecodeError::None);
  ASSERT_TRUE(result.batch.has_value());
  // Selector decoded, but value 5 has no branch: unresolved, nothing invented.
  EXPECT_TRUE(result.batch->muxUnresolved);
  ASSERT_NE(find(*result.batch, "Page"), nullptr);
  EXPECT_EQ(result.batch->signals.size(), 1u);
}

}  // namespace

// UT-01..UT-04, UT-09, UT-10, UT-19, B-03, B-04, B-17.
//
// The expected values are the independent reference table of
// contracts/frame_catalog.md section 3, taken from spec section 6.2 - not
// recomputed from the decoder. That is the point: section 24 lists "simulator
// and TCU using same code assumptions" as a risk, and the same strings are what
// the integration tests feed through cansend.

#include "tcu/can/demo_signal_decoder.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <map>
#include <string>

#include "tcu/can/frame_catalog.hpp"

namespace {

using namespace tcu::can;

CanMessage frame(std::uint32_t id, std::array<std::uint8_t, 8> data, std::uint8_t length = 8) {
  CanMessage msg;
  msg.id = id;
  msg.data = data;
  msg.length = length;
  msg.received_at = SteadyTime{} + std::chrono::milliseconds{1234};
  return msg;
}

/// The decoded values of one frame, by signal name, so a case can assert on the
/// pair a multi-signal frame produces.
std::map<std::string, double> decodeAll(const CanMessage& msg) {
  DemoSignalDecoder decoder;
  const auto result = decoder.decode(msg);
  std::map<std::string, double> out;
  for (const auto& update : result.updates) {
    out.emplace(update.signal_name, *update.value);
  }
  return out;
}

// UT-01..UT-04, UT-19.
TEST(Decoder, GoldenVectors) {
  // 100#0100000000000000 -> ignition ON
  EXPECT_EQ(decodeAll(frame(0x100, {0x01, 0, 0, 0, 0, 0, 0, 0})).at("ignition"), 1.0);
  // 100#0000000000000000 -> ignition OFF
  EXPECT_EQ(decodeAll(frame(0x100, {0x00, 0, 0, 0, 0, 0, 0, 0})).at("ignition"), 0.0);

  // 101#7017000000000000 -> raw 0x1770 = 6000 -> 60.00 km/h  (UT-01)
  EXPECT_DOUBLE_EQ(decodeAll(frame(0x101, {0x70, 0x17, 0, 0, 0, 0, 0, 0})).at("speed_kmh"), 60.00);
  // 101#0000000000000000 -> a valid 0.00, not a missing value  (B-04)
  EXPECT_DOUBLE_EQ(decodeAll(frame(0x101, {0x00, 0x00, 0, 0, 0, 0, 0, 0})).at("speed_kmh"), 0.00);

  // 102#B004550000000000 -> 0x04B0 = 1200 -> 12.00 V, 0x55 = 85 %  (UT-02)
  const auto battery = decodeAll(frame(0x102, {0xB0, 0x04, 0x55, 0, 0, 0, 0, 0}));
  EXPECT_DOUBLE_EQ(battery.at("battery_voltage_v"), 12.00);
  EXPECT_DOUBLE_EQ(battery.at("battery_soc_pct"), 85.0);

  // 103#1000000000000000 -> all doors closed, locked  (UT-04)
  EXPECT_EQ(decodeAll(frame(0x103, {0x10, 0, 0, 0, 0, 0, 0, 0})).at("door_state"), 16.0);
  // 103#0F00000000000000 -> all four doors open, unlocked
  EXPECT_EQ(decodeAll(frame(0x103, {0x0F, 0, 0, 0, 0, 0, 0, 0})).at("door_state"), 15.0);

  // 110#0100000000000000 -> heartbeat 1, ECU OK
  const auto beat = decodeAll(frame(0x110, {0x01, 0x00, 0, 0, 0, 0, 0, 0}));
  EXPECT_EQ(beat.at("heartbeat"), 1.0);
  EXPECT_EQ(beat.at("ecu_status"), 0.0);
  // 110#0101000000000000 -> ECU DEGRADED
  EXPECT_EQ(decodeAll(frame(0x110, {0x01, 0x01, 0, 0, 0, 0, 0, 0})).at("ecu_status"), 1.0);
}

// UT-03: ignition reads only bit 0, so the reserved bits of byte 0 cannot
// change it.
TEST(Decoder, IgnitionIgnoresTheRestOfTheByte) {
  EXPECT_EQ(decodeAll(frame(0x100, {0xFF, 0, 0, 0, 0, 0, 0, 0})).at("ignition"), 1.0);
  EXPECT_EQ(decodeAll(frame(0x100, {0xFE, 0, 0, 0, 0, 0, 0, 0})).at("ignition"), 0.0);
}

// UT-19: the counter wraps, and a wrap is normal behaviour rather than an
// error. Every value 0..255 is legal.
TEST(Decoder, HeartbeatWrapsAtTwoFiftyFive) {
  EXPECT_EQ(decodeAll(frame(0x110, {0xFF, 0x00, 0, 0, 0, 0, 0, 0})).at("heartbeat"), 255.0);
  EXPECT_EQ(decodeAll(frame(0x110, {0x00, 0x00, 0, 0, 0, 0, 0, 0})).at("heartbeat"), 0.0);

  DemoSignalDecoder decoder;
  EXPECT_TRUE(decoder.decode(frame(0x110, {0xFF, 0x00, 0, 0, 0, 0, 0, 0})).accepted);
  EXPECT_TRUE(decoder.decode(frame(0x110, {0x00, 0x00, 0, 0, 0, 0, 0, 0})).accepted);
}

// UT-09, B-11. The inclusive maximum is accepted; one raw step past it is not.
TEST(Decoder, RejectsOutOfRange) {
  DemoSignalDecoder decoder;

  // 101#A861... -> raw 0x61A8 = 25000 -> exactly 250.00
  const auto at_max = decoder.decode(frame(0x101, {0xA8, 0x61, 0, 0, 0, 0, 0, 0}));
  EXPECT_TRUE(at_max.accepted) << "250.00 is an inclusive bound, not a rejection";
  EXPECT_DOUBLE_EQ(*at_max.updates.at(0).value, 250.00);

  // 101#A961... -> raw 25001 -> 250.01
  const auto past_max = decoder.decode(frame(0x101, {0xA9, 0x61, 0, 0, 0, 0, 0, 0}));
  EXPECT_FALSE(past_max.accepted);
  EXPECT_EQ(past_max.reason, RejectReason::Range);
  EXPECT_TRUE(past_max.updates.empty());
}

TEST(Decoder, RejectsVoltageBelowTheLowerBound) {
  DemoSignalDecoder decoder;
  // raw 600 -> exactly 6.00 V, the inclusive minimum.
  EXPECT_TRUE(decoder.decode(frame(0x102, {0x58, 0x02, 0x55, 0, 0, 0, 0, 0})).accepted);
  // raw 599 -> 5.99 V.
  const auto below = decoder.decode(frame(0x102, {0x57, 0x02, 0x55, 0, 0, 0, 0, 0}));
  EXPECT_FALSE(below.accepted);
  EXPECT_EQ(below.reason, RejectReason::Range);
}

// UT-10, research R-14. The decisive case for atomic frame reject.
TEST(Decoder, AtomicRejectOnMultiSignalFrame) {
  DemoSignalDecoder decoder;
  // 102#B004780000000000: voltage 12.00 is perfectly valid, SOC 0x78 = 120 is
  // not. Neither signal may be committed.
  const auto result = decoder.decode(frame(0x102, {0xB0, 0x04, 0x78, 0, 0, 0, 0, 0}));

  EXPECT_FALSE(result.accepted);
  EXPECT_EQ(result.reason, RejectReason::Range);
  EXPECT_TRUE(result.updates.empty())
      << "the valid voltage must not be committed alongside the invalid SOC";
}

TEST(Decoder, RejectsEcuStatusTwo) {
  // 0x110 carries two signals, so the same atomic rule applies: a bad status
  // byte takes the heartbeat counter down with it.
  DemoSignalDecoder decoder;
  const auto result = decoder.decode(frame(0x110, {0x01, 0x02, 0, 0, 0, 0, 0, 0}));

  EXPECT_FALSE(result.accepted);
  EXPECT_EQ(result.reason, RejectReason::Range);
  EXPECT_TRUE(result.updates.empty());
}

TEST(Decoder, MultiSignalFramesYieldBothSignals) {
  DemoSignalDecoder decoder;
  EXPECT_EQ(decoder.decode(frame(0x102, {0xB0, 0x04, 0x55, 0, 0, 0, 0, 0})).updates.size(), 2u);
  EXPECT_EQ(decoder.decode(frame(0x110, {0x01, 0x00, 0, 0, 0, 0, 0, 0})).updates.size(), 2u);
  EXPECT_EQ(decoder.decode(frame(0x101, {0x70, 0x17, 0, 0, 0, 0, 0, 0})).updates.size(), 1u);
}

// contracts/metrics.md: can_decoded_total does not equal signal_update_total,
// and this is why.
TEST(Decoder, DecodedFrameCountDiffersFromSignalCount) {
  DemoSignalDecoder decoder;
  std::size_t frames = 0;
  std::size_t signals = 0;
  for (const auto& msg : {frame(0x101, {0x70, 0x17, 0, 0, 0, 0, 0, 0}),
                          frame(0x102, {0xB0, 0x04, 0x55, 0, 0, 0, 0, 0}),
                          frame(0x110, {0x01, 0x00, 0, 0, 0, 0, 0, 0})}) {
    const auto result = decoder.decode(msg);
    ASSERT_TRUE(result.accepted);
    ++frames;
    signals += result.updates.size();
  }
  EXPECT_EQ(frames, 3u);
  EXPECT_EQ(signals, 5u);
}

// B-17: interleaved identifiers must not contaminate one another.
TEST(Decoder, InterleavedIdsNoCrossTalk) {
  DemoSignalDecoder decoder;
  const CanMessage speed = frame(0x101, {0x70, 0x17, 0, 0, 0, 0, 0, 0});
  const CanMessage doors = frame(0x103, {0x10, 0, 0, 0, 0, 0, 0, 0});
  const CanMessage battery = frame(0x102, {0xB0, 0x04, 0x55, 0, 0, 0, 0, 0});

  for (int round = 0; round < 10; ++round) {
    EXPECT_DOUBLE_EQ(*decoder.decode(speed).updates.at(0).value, 60.00);
    EXPECT_EQ(*decoder.decode(doors).updates.at(0).value, 16.0);
    const auto b = decoder.decode(battery);
    EXPECT_DOUBLE_EQ(*b.updates.at(0).value, 12.00);
    EXPECT_DOUBLE_EQ(*b.updates.at(1).value, 85.0);
  }
}

TEST(Decoder, CopiesSourceIdAndReceiptTime) {
  // The decoder reports where and when, and invents neither: FR-07 requires the
  // source identifier and the timestamp to travel with the value.
  DemoSignalDecoder decoder;
  const CanMessage msg = frame(0x102, {0xB0, 0x04, 0x55, 0, 0, 0, 0, 0});
  const auto result = decoder.decode(msg);

  ASSERT_EQ(result.updates.size(), 2u);
  for (const auto& update : result.updates) {
    EXPECT_EQ(update.source_id, 0x102u);
    EXPECT_EQ(update.last_good_at, msg.received_at);
  }
}

// UT-05. An identifier outside the catalog produces no signal at all.
TEST(Decoder, UnknownIdProducesNoSignal) {
  DemoSignalDecoder decoder;
  const auto result = decoder.decode(frame(0x555, {}));

  EXPECT_FALSE(result.accepted);
  EXPECT_EQ(result.reason, RejectReason::UnknownId);
  EXPECT_TRUE(result.updates.empty());
}

// The command and acknowledgment frames are in the catalog but carry no
// signals: 0x201 is routed to the correlator, not here.
TEST(Decoder, CommandFramesCarryNoSignals) {
  DemoSignalDecoder decoder;
  EXPECT_FALSE(decoder.decode(frame(0x200, {kCommandLock, 0x2A, 0, 0, 0, 0, 0, 0})).accepted);
  EXPECT_FALSE(decoder.decode(frame(0x201, {0x2A, kResultOk, 0, 0, 0, 0, 0, 0})).accepted);
}

// UT-08. The decoder repeats the structural checks rather than trusting that
// the validator ran.
TEST(Decoder, RejectsShortFrameWithoutTrustingTheValidator) {
  DemoSignalDecoder decoder;
  const auto result = decoder.decode(frame(0x101, {0x70, 0x17, 0, 0, 0, 0, 0, 0}, 3));

  EXPECT_FALSE(result.accepted);
  EXPECT_EQ(result.reason, RejectReason::Length);
}

TEST(Decoder, RejectsRemoteAndExtendedFramesWithoutTrustingTheValidator) {
  DemoSignalDecoder decoder;

  CanMessage remote = frame(0x101, {0x70, 0x17, 0, 0, 0, 0, 0, 0});
  remote.kind = FrameKind::Remote;
  EXPECT_EQ(decoder.decode(remote).reason, RejectReason::Flags);

  CanMessage extended = frame(0x101, {0x70, 0x17, 0, 0, 0, 0, 0, 0});
  extended.format = FrameFormat::Extended29;
  EXPECT_EQ(decoder.decode(extended).reason, RejectReason::Flags);
}

// Reserved bytes are padding, never a signal (spec section 6.1).
TEST(Decoder, ReservedBytesDoNotAffectAnyValue) {
  const auto clean = decodeAll(frame(0x101, {0x70, 0x17, 0, 0, 0, 0, 0, 0}));
  const auto noisy = decodeAll(frame(0x101, {0x70, 0x17, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}));

  EXPECT_DOUBLE_EQ(clean.at("speed_kmh"), noisy.at("speed_kmh"));
}

// Every signal in the catalog must actually be reachable, or a configured
// deadline would guard a value nothing ever produces.
TEST(Decoder, EverySignalInTheCatalogIsProducible) {
  DemoSignalDecoder decoder;
  std::map<std::string, bool> seen;
  for (const auto& signal : kSignals) {
    seen[std::string(signal.name)] = false;
  }

  // A payload of zeros is in range for every signal except battery voltage,
  // whose minimum is 6.00 V - so that frame gets the smallest legal raw value.
  for (const std::uint32_t id : {0x100u, 0x101u, 0x103u, 0x110u}) {
    for (const auto& update : decoder.decode(frame(id, {})).updates) {
      seen[update.signal_name] = true;
    }
  }
  for (const auto& update :
       decoder.decode(frame(0x102, {0x58, 0x02, 0x00, 0, 0, 0, 0, 0})).updates) {
    seen[update.signal_name] = true;
  }

  for (const auto& entry : seen) {
    EXPECT_TRUE(entry.second) << entry.first << " is in the catalog but no frame produces it";
  }
}

}  // namespace

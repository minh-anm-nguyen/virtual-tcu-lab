// T061: differential comparison against the committed cantools vectors.
//
// The oracle is not ours (research R-20), the file is committed so no Python
// runs at test time, and the comparison follows section 15.5's discipline:
// physical values with a tolerance, never == on a double.

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "tcu/dbc/dbc_parser.hpp"
#include "tcu/dbc/signal_decoder.hpp"

namespace {

struct Expectation {
  std::string name;
  double physical{0.0};
};

struct Case {
  std::uint32_t id{0};
  bool extended{false};
  std::vector<std::uint8_t> payload;
  std::vector<Expectation> expected;
  std::string rawLine;
};

bool parseHexByte(const std::string& text, std::size_t at, std::uint8_t& out) {
  const auto nibble = [](char c, std::uint8_t& value) {
    if (c >= '0' && c <= '9') { value = static_cast<std::uint8_t>(c - '0'); return true; }
    if (c >= 'a' && c <= 'f') { value = static_cast<std::uint8_t>(c - 'a' + 10); return true; }
    if (c >= 'A' && c <= 'F') { value = static_cast<std::uint8_t>(c - 'A' + 10); return true; }
    return false;
  };
  std::uint8_t high = 0;
  std::uint8_t low = 0;
  if (!nibble(text[at], high) || !nibble(text[at + 1], low)) return false;
  out = static_cast<std::uint8_t>((high << 4) | low);
  return true;
}

std::vector<Case> loadMatrix(std::string& header) {
  std::vector<Case> cases;
  std::ifstream in(std::string(TCU_DBC_FIXTURE_DIR) + "/golden/matrix.txt");
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty()) continue;
    if (line[0] == '#') {
      if (header.empty() && line.find("cantools=") != std::string::npos) header = line;
      continue;
    }
    std::istringstream stream(line);
    Case entry;
    entry.rawLine = line;
    int extended = 0;
    std::string hex;
    if (!(stream >> entry.id >> extended >> hex)) continue;
    entry.extended = extended != 0;
    if ((hex.size() % 2u) != 0u) continue;
    bool ok = true;
    for (std::size_t at = 0; at + 1 < hex.size(); at += 2) {
      std::uint8_t byte = 0;
      if (!parseHexByte(hex, at, byte)) { ok = false; break; }
      entry.payload.push_back(byte);
    }
    if (!ok) continue;

    std::string field;
    while (stream >> field) {
      const std::size_t equals = field.find('=');
      if (equals == std::string::npos) continue;
      Expectation expectation;
      expectation.name = field.substr(0, equals);
      expectation.physical = std::strtod(field.c_str() + equals + 1, nullptr);
      entry.expected.push_back(expectation);
    }
    cases.push_back(std::move(entry));
  }
  return cases;
}

std::string readFixture(const std::string& relative) {
  std::ifstream in(std::string(TCU_DBC_FIXTURE_DIR) + "/" + relative, std::ios::binary);
  std::ostringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

/// Absolute near zero, relative above it. Comparing scaled doubles with ==
/// would fail on arithmetic that is perfectly correct (section 15.5).
bool closeEnough(double ours, double theirs) {
  const double difference = std::fabs(ours - theirs);
  if (difference <= 1e-9) return true;
  const double scale = std::fmax(std::fabs(ours), std::fabs(theirs));
  return difference <= scale * 1e-9;
}

TEST(Golden_GeneratedMatrix, EveryCantoolsVectorAgrees) {
  std::string header;
  const auto cases = loadMatrix(header);
  ASSERT_FALSE(cases.empty()) << "the committed matrix is missing or empty; regenerate it with "
                                 "scripts/generate_dbc_vectors.py";
  ASSERT_FALSE(header.empty()) << "the matrix has no generator header, so it is not reproducible";
  std::cout << "oracle: " << header << std::endl;

  const auto loaded = tcu::dbc::loadDbcBytes(readFixture("virtual_tcu.dbc"), "virtual_tcu.dbc");
  ASSERT_TRUE(loaded.ok());
  const tcu::dbc::SignalDecoder decoder(loaded.db, tcu::dbc::DecodePolicy{});

  std::size_t compared = 0;
  std::size_t signalsCompared = 0;
  for (const auto& entry : cases) {
    tcu::dbc::CanFrameView frame;
    frame.arbitrationId = entry.id;
    frame.format = entry.extended ? tcu::dbc::FrameFormat::Extended29
                                  : tcu::dbc::FrameFormat::Standard11;
    frame.data = entry.payload.data();
    frame.size = static_cast<std::uint8_t>(entry.payload.size());

    const auto result = decoder.decode(frame);
    ASSERT_EQ(result.error, tcu::dbc::DecodeError::None) << entry.rawLine;
    ASSERT_TRUE(result.batch.has_value()) << entry.rawLine;
    ++compared;

    for (const auto& expectation : entry.expected) {
      const tcu::dbc::SignalValue* ours = nullptr;
      for (const auto& value : result.batch->signals) {
        if (value.signalName == expectation.name) ours = &value;
      }
      ASSERT_NE(ours, nullptr) << "we did not emit " << expectation.name << " for "
                               << entry.rawLine;
      EXPECT_TRUE(closeEnough(ours->physical, expectation.physical))
          << expectation.name << ": ours=" << ours->physical
          << " cantools=" << expectation.physical << " for " << entry.rawLine;
      ++signalsCompared;
    }

    for (const auto& value : result.batch->signals) {
      bool found = false;
      for (const auto& expectation : entry.expected) {
        if (expectation.name == value.signalName) found = true;
      }
      EXPECT_TRUE(found) << "we emitted " << value.signalName
                         << " which cantools did not, for " << entry.rawLine;
    }
  }

  std::cout << "compared " << compared << " frames and " << signalsCompared << " signals"
            << std::endl;
  EXPECT_GE(compared, 1000u);
}

TEST(Golden_HandComputedVectors, SpecSection73ByHand) {
  // The hand-computed table is the check on the oracle itself: if cantools and
  // we agreed on something wrong, only a hand calculation would catch it.
  const auto loaded = tcu::dbc::loadDbcBytes(readFixture("virtual_tcu.dbc"), "virtual_tcu.dbc");
  ASSERT_TRUE(loaded.ok());
  const tcu::dbc::SignalDecoder decoder(loaded.db, tcu::dbc::DecodePolicy{});

  struct Vector {
    std::uint32_t id;
    std::vector<std::uint8_t> payload;
    const char* signal;
    double physical;
  };
  const Vector vectors[] = {
      {256, {0x10, 0x27, 0x01, 0x00, 0x50, 0, 0, 0}, "VehicleSpeed", 100.0},
      {256, {0x10, 0x27, 0x01, 0x00, 0x50, 0, 0, 0}, "Ignition", 1.0},
      {256, {0x10, 0x27, 0x01, 0x00, 0x50, 0, 0, 0}, "CabinTemp", -40.0},
      {257, {0xAB, 0xC0, 0, 0, 0, 0, 0, 0}, "Torque", -674.0},
      {258, {0x01, 0x7F, 0xD2, 0x04, 0, 0, 0, 0}, "BatteryVoltage", 12.34},
      {258, {0x02, 0x7F, 0x9C, 0xFF, 0, 0, 0, 0}, "MotorTemp", -10.0},
  };

  for (const auto& vector : vectors) {
    tcu::dbc::CanFrameView frame;
    frame.arbitrationId = vector.id;
    frame.data = vector.payload.data();
    frame.size = static_cast<std::uint8_t>(vector.payload.size());
    const auto result = decoder.decode(frame);
    ASSERT_EQ(result.error, tcu::dbc::DecodeError::None) << vector.signal;
    const tcu::dbc::SignalValue* value = nullptr;
    for (const auto& candidate : result.batch->signals) {
      if (candidate.signalName == vector.signal) value = &candidate;
    }
    ASSERT_NE(value, nullptr) << vector.signal;
    EXPECT_NEAR(value->physical, vector.physical, 1e-9) << vector.signal;
    EXPECT_EQ(value->quality, tcu::dbc::SignalQuality::Valid) << vector.signal;
  }
}

}  // namespace

// Spec section 18.2 scenarios 1-5, plus B-03, B-07, B-09, B-11, B-14, B-16,
// B-18 and FR-01..FR-04 on a real bus.
//
// Every case here SKIPS, loudly, when vcan0 is unavailable (research R-01). The
// development host for this repository is Windows with WSL2, whose stock kernel
// ships no vcan module at all, so a suite that failed instead would be red on
// every developer machine and would teach people to ignore it. CI provisions
// vcan0, which is where these actually run.
//
// Frames are injected with cansend rather than built by the library under test:
// spec section 24 lists "simulator and TCU using same code assumptions" as a
// risk, and an independent injector is the answer to it.

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <utility>

#include "support/vcan_probe.hpp"
#include "tcu/can/can_config.hpp"
#include "tcu/can/can_service.hpp"
#include "tcu/can/frame_catalog.hpp"

namespace {

using namespace tcu::can;
using namespace std::chrono_literals;

constexpr const char* kInterface = "vcan0";

std::string configText() {
  return R"json({
  "schema_version": 1,
  "mode": "lab_only",
  "can": {
    "interface": "vcan0",
    "format": "classic_standard_11bit",
    "rx_queue_capacity": 1024,
    "health_check_ms": 20,
    "drain_deadline_ms": 200,
    "reconnect": { "initial_ms": 50, "max_ms": 400, "jitter_percent": 0 },
    "allowed_rx_ids": [256, 257, 258, 259, 272, 513]
  },
  "signals": {
    "ignition":          { "can_id": 256, "timeout_ms": 3000 },
    "speed_kmh":         { "can_id": 257, "timeout_ms": 300  },
    "battery_voltage_v": { "can_id": 258, "timeout_ms": 3000 },
    "battery_soc_pct":   { "can_id": 258, "timeout_ms": 3000 },
    "door_state":        { "can_id": 259, "timeout_ms": 3000 },
    "heartbeat":         { "can_id": 272, "timeout_ms": 3000 },
    "ecu_status":        { "can_id": 272, "timeout_ms": 3000 }
  },
  "simulator": { "speed_period_ms": 100, "heartbeat_period_ms": 1000, "fixed_seed": 42 }
})json";
}

CanLabConfig labConfig() {
  CanLabConfig config;
  const auto result = parseCanLabConfig(configText(), config);
  EXPECT_TRUE(result) << result.pointer << " " << result.publicReason;
  return config;
}

/// Injects one frame with can-utils, so the decoder is checked against an
/// independent sender rather than against itself.
bool cansend(const std::string& frame) {
  const std::string command = std::string("cansend ") + kInterface + " " + frame + " 2>/dev/null";
  return std::system(command.c_str()) == 0;
}

template <typename Predicate>
bool waitFor(Predicate predicate, std::chrono::milliseconds budget = 3000ms) {
  const auto deadline = std::chrono::steady_clock::now() + budget;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(2ms);
  }
  return predicate();
}

/// A started service on the real interface, torn down on destruction.
struct LiveService {
  std::unique_ptr<CanService> service;

  LiveService() {
    CanServiceDeps deps;  // real clock, real socket, no bus, no logger
    service = std::make_unique<CanService>(std::move(deps));
  }

  ~LiveService() {
    if (service) {
      service->stop();
    }
  }

  bool start(const CanLabConfig& config) { return service->start(config); }
};

class VcanRoundtrip : public ::testing::Test {
 protected:
  void SetUp() override { TCU_SKIP_WITHOUT_VCAN(); }
};

// FR-01, and the first half of section 18.2 scenario 1.
TEST_F(VcanRoundtrip, BindsConfiguredInterface) {
  LiveService live;
  ASSERT_TRUE(live.start(labConfig()));
  EXPECT_EQ(live.service->state(), ServiceState::Running)
      << "a present, up vcan0 must bind on the first try";
}

// FR-02, B-03, section 18.2 scenario 2. The golden vector is the spec's own.
TEST_F(VcanRoundtrip, CansendRoundTrip) {
  LiveService live;
  ASSERT_TRUE(live.start(labConfig()));

  ASSERT_TRUE(cansend("101#7017000000000000")) << "cansend failed; is can-utils installed?";

  ASSERT_TRUE(waitFor([&] {
    const auto sample = live.service->readSignal("speed_kmh");
    return sample && sample->value.has_value();
  })) << "the frame never arrived or never decoded";

  const auto sample = live.service->readSignal("speed_kmh");
  EXPECT_DOUBLE_EQ(*sample->value, 60.00);
  EXPECT_EQ(sample->quality, SignalQuality::Fresh);
  EXPECT_EQ(sample->source_id, 0x101u);
}

TEST_F(VcanRoundtrip, DecodesTheWholeCatalogFromTheWire) {
  LiveService live;
  ASSERT_TRUE(live.start(labConfig()));

  ASSERT_TRUE(cansend("100#0100000000000000"));
  ASSERT_TRUE(cansend("101#7017000000000000"));
  ASSERT_TRUE(cansend("102#B004550000000000"));
  ASSERT_TRUE(cansend("103#1000000000000000"));
  ASSERT_TRUE(cansend("110#0100000000000000"));

  ASSERT_TRUE(waitFor([&] { return live.service->metrics().can_decoded_total >= 5; }));

  EXPECT_DOUBLE_EQ(*live.service->readSignal("ignition")->value, 1.0);
  EXPECT_DOUBLE_EQ(*live.service->readSignal("speed_kmh")->value, 60.00);
  EXPECT_DOUBLE_EQ(*live.service->readSignal("battery_voltage_v")->value, 12.00);
  EXPECT_DOUBLE_EQ(*live.service->readSignal("battery_soc_pct")->value, 85.0);
  EXPECT_DOUBLE_EQ(*live.service->readSignal("door_state")->value, 16.0);
  EXPECT_DOUBLE_EQ(*live.service->readSignal("heartbeat")->value, 1.0);
  EXPECT_DOUBLE_EQ(*live.service->readSignal("ecu_status")->value, 0.0);
}

// FR-04, section 18.2 scenario 3. The kernel filter, not the validator, is what
// is under test here.
TEST_F(VcanRoundtrip, FilterExcludesUnrelatedId) {
  LiveService live;
  ASSERT_TRUE(live.start(labConfig()));

  ASSERT_TRUE(cansend("555#0011223344556677"));
  ASSERT_TRUE(cansend("101#7017000000000000"));

  // Wait for the frame we DO expect, then check the other one never arrived.
  ASSERT_TRUE(waitFor([&] { return live.service->metrics().can_decoded_total >= 1; }));

  const auto m = live.service->metrics();
  EXPECT_EQ(m.can_unknown_id_total, 0u)
      << "an unfiltered identifier reached the application; the CAN_RAW_FILTER is wrong";
  EXPECT_EQ(m.can_rx_read_total, 1u) << "only the filtered identifier should have been read";
}

// B-14 on a real bus: an extended frame whose low 11 bits are 0x101 must not be
// decoded as the standard speed frame. The kernel filter is expected to stop it
// before the validator ever sees it (research R-11).
TEST_F(VcanRoundtrip, ExtendedFrameDoesNotAliasOntoAStandardId) {
  LiveService live;
  ASSERT_TRUE(live.start(labConfig()));

  // cansend uses an 8-digit identifier for extended frames.
  ASSERT_TRUE(cansend("00000101#A861000000000000"));
  ASSERT_TRUE(cansend("101#7017000000000000"));
  ASSERT_TRUE(waitFor([&] { return live.service->metrics().can_decoded_total >= 1; }));

  EXPECT_DOUBLE_EQ(*live.service->readSignal("speed_kmh")->value, 60.00)
      << "the extended frame was decoded as speed";
  EXPECT_EQ(live.service->metrics().can_rx_read_total, 1u);
}

// FR-03: the outbound path. candump is the independent observer.
TEST_F(VcanRoundtrip, CandumpSeesOutgoingFrame) {
  LiveService live;
  ASSERT_TRUE(live.start(labConfig()));

  // candump exits after one matching frame, so the command completes on its own
  // rather than needing a kill.
  const std::string command = std::string("candump -n 1 -T 3000 ") + kInterface +
                              ",200:7FF > /tmp/tcu_candump.out 2>&1 &";
  ASSERT_EQ(std::system(command.c_str()), 0);
  std::this_thread::sleep_for(200ms);  // let candump bind before transmitting

  ASSERT_EQ(live.service->sendDemoCommand(kCommandLock, 0x2A), WriteStatus::Sent);

  EXPECT_TRUE(waitFor(
      [] { return std::system("grep -qi '012A' /tmp/tcu_candump.out 2>/dev/null") == 0; }))
      << "candump did not observe the 0x200 command on the bus";
}

// Section 18.2 scenario 4, B-07, B-18: suspend ONE signal and watch only that
// one go stale.
TEST_F(VcanRoundtrip, PausedIdGoesStaleAlone) {
  LiveService live;
  ASSERT_TRUE(live.start(labConfig()));

  ASSERT_TRUE(cansend("101#7017000000000000"));  // speed, 300 ms deadline here
  ASSERT_TRUE(cansend("110#0100000000000000"));  // heartbeat, 3000 ms
  ASSERT_TRUE(waitFor([&] { return live.service->metrics().can_decoded_total >= 2; }));

  // Send nothing more. Speed must expire; heartbeat must not.
  ASSERT_TRUE(waitFor(
      [&] { return live.service->readSignal("speed_kmh")->quality == SignalQuality::Stale; }));

  EXPECT_EQ(live.service->readSignal("heartbeat")->quality, SignalQuality::Fresh)
      << "an unrelated signal went stale with it";
  EXPECT_DOUBLE_EQ(*live.service->readSignal("speed_kmh")->value, 60.00)
      << "the last good value must survive staleness";
  EXPECT_GE(live.service->metrics().signal_stale_transitions_total, 1u);
}

// Section 18.2 scenario 5, B-09: resume and recover, without restarting the TCU.
TEST_F(VcanRoundtrip, ResumedIdRecoversOnce) {
  LiveService live;
  ASSERT_TRUE(live.start(labConfig()));

  ASSERT_TRUE(cansend("101#7017000000000000"));
  ASSERT_TRUE(waitFor(
      [&] { return live.service->readSignal("speed_kmh")->quality == SignalQuality::Stale; }));
  const auto stale_transitions = live.service->metrics().signal_stale_transitions_total;

  ASSERT_TRUE(cansend("101#A817000000000000"));  // 61.04 km/h
  ASSERT_TRUE(waitFor(
      [&] { return live.service->readSignal("speed_kmh")->quality == SignalQuality::Fresh; }));

  EXPECT_GE(live.service->metrics().signal_recovered_total, 1u);
  EXPECT_EQ(live.service->metrics().signal_stale_transitions_total, stale_transitions)
      << "recovering must not also report another stale transition";
}

// B-11 on a real bus: an out-of-range SOC takes the whole 0x102 frame with it
// and leaves both battery signals as they were (research R-14).
TEST_F(VcanRoundtrip, OutOfRangeFrameIsRejectedWhole) {
  LiveService live;
  ASSERT_TRUE(live.start(labConfig()));

  ASSERT_TRUE(cansend("102#B004550000000000"));  // 12.00 V, 85 %
  ASSERT_TRUE(waitFor([&] { return live.service->metrics().can_decoded_total >= 1; }));

  ASSERT_TRUE(cansend("102#B004780000000000"));  // 12.00 V, SOC 120
  ASSERT_TRUE(waitFor([&] { return live.service->metrics().can_reject_range_total >= 1; }));

  EXPECT_DOUBLE_EQ(*live.service->readSignal("battery_voltage_v")->value, 12.00);
  EXPECT_DOUBLE_EQ(*live.service->readSignal("battery_soc_pct")->value, 85.0);
}

// B-16, FR-10, and the section 15 burst row: cangen floods the bus and the
// queue stays bounded with every drop counted.
TEST_F(VcanRoundtrip, BurstTrafficStaysBounded) {
  CanLabConfig config = labConfig();
  config.rx_queue_capacity = 8;  // small, so the drop path is certain

  LiveService live;
  ASSERT_TRUE(live.start(config));

  // -g 0 is back-to-back; -n bounds the run so the test cannot hang.
  const std::string command =
      std::string("cangen ") + kInterface + " -I 101 -L 8 -g 0 -n 20000 > /dev/null 2>&1";
  ASSERT_EQ(std::system(command.c_str()), 0) << "cangen failed; is can-utils installed?";

  ASSERT_TRUE(waitFor([&] { return live.service->metrics().can_rx_read_total > 0; }));

  const auto m = live.service->metrics();
  EXPECT_LE(m.can_queue_depth, m.can_queue_capacity) << "the queue exceeded its bound";
  EXPECT_EQ(m.can_rx_enqueued_total + m.can_rx_queue_drop_total, m.can_rx_read_total)
      << "frames went missing without being counted";
}

}  // namespace

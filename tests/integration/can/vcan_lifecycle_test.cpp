// Spec section 18.2 scenarios 6-8, plus B-19, B-20, B-22, B-23, B-24, B-25,
// B-28 and FR-11, FR-13, FR-14 on a real bus.
//
// Probe-gated like the rest of the integration suite (research R-01).
//
// The BCM side is a small responder built here on a second SocketCanTransport
// rather than the bcm_simulator process. That keeps the test to one executable
// while still exercising the real kernel path in both directions - and the
// correlation logic under test lives in the library, not in the app.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <variant>

#include "support/vcan_probe.hpp"
#include "tcu/can/can_config.hpp"
#include "tcu/can/can_service.hpp"
#include "tcu/can/frame_catalog.hpp"
#include "tcu/can/socketcan_transport.hpp"
#include "tcu/scheduler/clock_adapter.hpp"

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
    "rx_queue_capacity": 256,
    "health_check_ms": 20,
    "drain_deadline_ms": 200,
    "reconnect": { "initial_ms": 50, "max_ms": 400, "jitter_percent": 0 },
    "allowed_rx_ids": [256, 257, 258, 259, 272, 513]
  },
  "signals": {
    "ignition":          { "can_id": 256, "timeout_ms": 3000 },
    "speed_kmh":         { "can_id": 257, "timeout_ms": 3000 },
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

bool cansend(const std::string& frame) {
  const std::string command = std::string("cansend ") + kInterface + " " + frame + " 2>/dev/null";
  return std::system(command.c_str()) == 0;
}

template <typename Predicate>
bool waitFor(Predicate predicate, std::chrono::milliseconds budget = 5000ms) {
  const auto deadline = std::chrono::steady_clock::now() + budget;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(2ms);
  }
  return predicate();
}

/// A minimal body control module on its own socket: answers 0x200 with 0x201.
/// `echo_sequence` false replies with a sequence nobody is waiting for, which
/// is how B-23 is reached on a real bus.
class BcmResponder {
 public:
  BcmResponder(bool echo_sequence, std::uint8_t result)
      : echo_sequence_(echo_sequence), result_(result) {}

  bool start() {
    auto clock = std::make_shared<tcu::scheduler::SteadyClockAdapter>();
    SocketCanTransport::Options options;
    options.interface_name = kInterface;
    options.clock = clock;
    options.rx_filter = {kBcmRxFilterId};

    auto opened = SocketCanTransport::open(options);
    if (std::holds_alternative<OpenError>(opened)) {
      return false;
    }
    transport_ = std::move(std::get<std::unique_ptr<SocketCanTransport>>(opened));

    worker_ = std::thread([this, clock] {
      while (!stop_.load()) {
        auto result = transport_->receive(50ms);
        if (result.status != ReadStatus::Received || result.message->id != 0x200) {
          continue;
        }
        const std::uint8_t sequence = result.message->data[1];

        CanMessage ack;
        ack.id = 0x201;
        ack.length = 8;
        ack.data = {echo_sequence_ ? sequence : static_cast<std::uint8_t>(sequence ^ 0xFF),
                    result_, 0, 0, 0, 0, 0, 0};
        ack.received_at = clock->now();
        transport_->send(ack);
        ++answered_;
      }
    });
    return true;
  }

  ~BcmResponder() {
    stop_.store(true);
    if (transport_) {
      transport_->requestStop();
    }
    if (worker_.joinable()) {
      worker_.join();
    }
  }

  std::uint32_t answered() const { return answered_.load(); }

 private:
  bool echo_sequence_;
  std::uint8_t result_;
  std::unique_ptr<SocketCanTransport> transport_;
  std::thread worker_;
  std::atomic<bool> stop_{false};
  std::atomic<std::uint32_t> answered_{0};
};

struct LiveService {
  std::unique_ptr<CanService> service;

  LiveService() {
    CanServiceDeps deps;
    deps.command_timeout = 500ms;
    service = std::make_unique<CanService>(std::move(deps));
  }

  ~LiveService() {
    if (service) {
      service->stop();
    }
  }

  bool start(const CanLabConfig& config) { return service->start(config); }
};

class VcanLifecycle : public ::testing::Test {
 protected:
  void SetUp() override { TCU_SKIP_WITHOUT_VCAN(); }
};

// Section 18.2 scenario 6, B-22, FR-14.
TEST_F(VcanLifecycle, BcmLockAck) {
  BcmResponder bcm(/*echo_sequence=*/true, kResultOk);
  ASSERT_TRUE(bcm.start()) << "the responder could not open its own socket";

  LiveService live;
  ASSERT_TRUE(live.start(labConfig()));

  ASSERT_EQ(live.service->sendDemoCommand(kCommandLock, 0x2A), WriteStatus::Sent);

  ASSERT_TRUE(waitFor([&] { return live.service->metrics().can_ack_matched_total >= 1; }))
      << "the acknowledgment never came back, or never matched";

  EXPECT_EQ(live.service->metrics().can_ack_unmatched_total, 0u);
  EXPECT_GE(bcm.answered(), 1u);
  // The slot is free again, which is the observable consequence of completing.
  EXPECT_EQ(live.service->sendDemoCommand(kCommandUnlock, 0x2B), WriteStatus::Sent);
}

// B-23 on a real bus: an acknowledgment with the wrong sequence must not
// complete the pending request.
TEST_F(VcanLifecycle, UnmatchedAckLeavesTheRequestPending) {
  BcmResponder bcm(/*echo_sequence=*/false, kResultOk);
  ASSERT_TRUE(bcm.start());

  LiveService live;
  ASSERT_TRUE(live.start(labConfig()));

  ASSERT_EQ(live.service->sendDemoCommand(kCommandLock, 0x2A), WriteStatus::Sent);

  ASSERT_TRUE(waitFor([&] { return live.service->metrics().can_ack_unmatched_total >= 1; }));
  EXPECT_EQ(live.service->metrics().can_ack_matched_total, 0u);

  // The request is still outstanding, so a second one is refused - until the
  // command timeout clears it.
  EXPECT_EQ(live.service->sendDemoCommand(kCommandUnlock, 0x2B), WriteStatus::InvalidFrame);
  EXPECT_TRUE(waitFor([&] { return live.service->metrics().can_ack_timeout_total >= 1; }))
      << "an unanswered request must eventually be abandoned, not held forever";
}

// B-24: an unknown command byte never leaves the service, so the BCM is never
// asked to decide what it means.
TEST_F(VcanLifecycle, UnknownCommandByteIsRefusedLocally) {
  LiveService live;
  ASSERT_TRUE(live.start(labConfig()));

  EXPECT_EQ(live.service->sendDemoCommand(0x09, 0x2A), WriteStatus::InvalidFrame);
  EXPECT_EQ(live.service->metrics().can_tx_attempt_total, 0u) << "nothing reached the bus";
}

// Section 18.2 scenario 7, B-19, B-20, FR-13. The assertion is on the final
// observable state, never on a particular errno sequence: spec section 21.6
// says that order "can vary across Linux versions".
//
// Needs CAP_NET_ADMIN to bring the link down, so it skips without it rather
// than failing.
TEST_F(VcanLifecycle, InterfaceDownUp) {
  if (std::system("ip link set dev vcan0 down 2>/dev/null") != 0) {
    GTEST_SKIP() << "cannot change vcan0 state: this process lacks CAP_NET_ADMIN";
  }
  // Put it back whatever happens below.
  struct Restore {
    ~Restore() { std::system("ip link set dev vcan0 up 2>/dev/null"); }
  } restore;

  std::system("ip link set dev vcan0 up 2>/dev/null");

  LiveService live;
  ASSERT_TRUE(live.start(labConfig()));
  ASSERT_TRUE(cansend("101#7017000000000000"));
  ASSERT_TRUE(waitFor([&] { return live.service->metrics().can_decoded_total >= 1; }));

  ASSERT_EQ(std::system("ip link set dev vcan0 down 2>/dev/null"), 0);
  ASSERT_TRUE(waitFor([&] {
    return live.service->state() == ServiceState::Degraded ||
           live.service->metrics().can_link_error_total >= 1;
  })) << "the service did not notice the interface going down";

  ASSERT_EQ(std::system("ip link set dev vcan0 up 2>/dev/null"), 0);
  ASSERT_TRUE(waitFor([&] { return live.service->state() == ServiceState::Running; }))
      << "the service did not recover after the interface came back";

  // B-20: recovery alone must not restore freshness.
  const auto sample = live.service->readSignal("speed_kmh");
  ASSERT_TRUE(sample->value.has_value()) << "the last good value must be retained";
  EXPECT_NE(sample->quality, SignalQuality::Fresh);

  ASSERT_TRUE(cansend("101#A817000000000000"));
  EXPECT_TRUE(waitFor(
      [&] { return live.service->readSignal("speed_kmh")->quality == SignalQuality::Fresh; }))
      << "a new frame after recovery must restore freshness";
}

// Section 18.2 scenario 8, B-25, FR-11: stop while idle, and nothing hangs. The
// TIMEOUT property on the suite is what turns a stuck join into a failure.
TEST_F(VcanLifecycle, StopWakesPollWhileIdle) {
  LiveService live;
  ASSERT_TRUE(live.start(labConfig()));
  ASSERT_EQ(live.service->state(), ServiceState::Running);

  const auto before = std::chrono::steady_clock::now();
  live.service->stop();
  const auto elapsed = std::chrono::steady_clock::now() - before;

  EXPECT_EQ(live.service->state(), ServiceState::Stopped);
  // The reader polls with a 100 ms timeout, so a stop that had to wait for the
  // poll to expire would be visible here. The eventfd is what makes it prompt
  // (research R-09).
  EXPECT_LT(elapsed, 1s) << "stop() waited for the poll timeout instead of being woken";
}

TEST_F(VcanLifecycle, RepeatedStartStopOnTheRealSocket) {
  // UT-17 against real descriptors, where a leak actually costs something.
  LiveService live;
  for (int cycle = 0; cycle < 5; ++cycle) {
    ASSERT_TRUE(live.start(labConfig())) << "cycle " << cycle;
    ASSERT_EQ(live.service->state(), ServiceState::Running) << "cycle " << cycle;
    live.service->stop();
  }
}

// B-28: frames are not persisted and not replayed, so a restarted service
// starts blind and recovers from the next frame.
TEST_F(VcanLifecycle, RestartIsNeverReceived) {
  LiveService live;
  ASSERT_TRUE(live.start(labConfig()));
  ASSERT_TRUE(cansend("101#7017000000000000"));
  ASSERT_TRUE(waitFor([&] { return live.service->metrics().can_decoded_total >= 1; }));
  live.service->stop();

  ASSERT_TRUE(live.start(labConfig()));
  EXPECT_EQ(live.service->readSignal("speed_kmh")->quality, SignalQuality::NeverReceived);
  EXPECT_FALSE(live.service->readSignal("speed_kmh")->value.has_value())
      << "a restarted service must not resurrect a value from before";

  ASSERT_TRUE(cansend("101#7017000000000000"));
  EXPECT_TRUE(waitFor(
      [&] { return live.service->readSignal("speed_kmh")->quality == SignalQuality::Fresh; }));
}

// Starting on an interface that does not exist is a transient condition, not a
// failure: ADR-06 chooses link recovery over process termination.
TEST_F(VcanLifecycle, MissingInterfaceStartsDegraded) {
  CanLabConfig config = labConfig();
  config.interface_name = "vcan99";

  LiveService live;
  ASSERT_TRUE(live.start(config));
  EXPECT_EQ(live.service->state(), ServiceState::Degraded);
}

}  // namespace

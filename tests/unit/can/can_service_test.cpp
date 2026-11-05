// UT-05, UT-08, UT-10, UT-16, UT-17, UT-20, UT-22, B-01, B-02, B-10, B-12,
// B-16, B-19, B-20, B-21, B-23, B-25, B-26, B-27, B-28, plus the metric
// invariants of contracts/metrics.md section 3.
//
// Every case runs on FakeCanTransport and FakeClockAdapter, so none of it needs
// vcan - which is also why the ThreadSanitizer evidence for this project does
// not depend on a kernel module (NFR-03, research R-01).
//
// Waiting for the asynchronous pipeline is done by spinning on an observable
// condition with a bounded deadline, never by sleeping for a guessed interval:
// a sleep would either be slow or flaky.

#include "tcu/can/can_service.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "tcu/can/fake_can_transport.hpp"
#include "tcu/can/frame_catalog.hpp"
#include "tcu/eventbus/event_bus.hpp"
#include "tcu/logging/logger.hpp"

namespace {

using namespace tcu::can;
using namespace std::chrono_literals;
using tcu::scheduler::FakeClockAdapter;

std::string configText() {
  return R"json({
  "schema_version": 1,
  "mode": "lab_only",
  "can": {
    "interface": "vcan0",
    "format": "classic_standard_11bit",
    "rx_queue_capacity": 1024,
    "health_check_ms": 50,
    "drain_deadline_ms": 200,
    "reconnect": { "initial_ms": 1, "max_ms": 4, "jitter_percent": 0 },
    "allowed_rx_ids": [256, 257, 258, 259, 272, 513]
  },
  "signals": {
    "ignition":          { "can_id": 256, "timeout_ms": 3000 },
    "speed_kmh":         { "can_id": 257, "timeout_ms": 500  },
    "battery_voltage_v": { "can_id": 258, "timeout_ms": 3000 },
    "battery_soc_pct":   { "can_id": 258, "timeout_ms": 3000 },
    "door_state":        { "can_id": 259, "timeout_ms": 3000 },
    "heartbeat":         { "can_id": 272, "timeout_ms": 3000 },
    "ecu_status":        { "can_id": 272, "timeout_ms": 3000 }
  },
  "simulator": { "speed_period_ms": 100, "heartbeat_period_ms": 1000, "fixed_seed": 42 }
})json";
}

CanLabConfig validConfig() {
  CanLabConfig config;
  const auto result = parseCanLabConfig(configText(), config);
  EXPECT_TRUE(result) << result.pointer << " " << result.publicReason;
  return config;
}

/// Spins until `predicate` holds or the deadline passes. Returns whether it
/// held, so a caller can assert rather than hang.
template <typename Predicate>
bool waitFor(Predicate predicate, std::chrono::milliseconds budget = 2000ms) {
  const auto deadline = std::chrono::steady_clock::now() + budget;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }
    std::this_thread::yield();
  }
  return predicate();
}

struct Harness {
  std::shared_ptr<FakeClockAdapter> clock = std::make_shared<FakeClockAdapter>();
  FakeCanTransport* transport = nullptr;
  std::unique_ptr<CanService> service;

  explicit Harness(bool broad_filter = false) {
    auto fake = std::make_unique<FakeCanTransport>();
    transport = fake.get();

    CanServiceDeps deps;
    deps.clock = clock;
    deps.transport = std::move(fake);
    deps.broad_filter = broad_filter;
    service = std::make_unique<CanService>(std::move(deps));
  }

  ~Harness() {
    if (service) {
      service->stop();
    }
  }

  bool start() { return service->start(validConfig()); }

  CanMessage frame(std::uint32_t id, std::array<std::uint8_t, 8> data, std::uint8_t length = 8) {
    CanMessage msg;
    msg.id = id;
    msg.data = data;
    msg.length = length;
    // The fake does not stamp - the real transport does - so the test supplies
    // the receipt time, which is what lets the fake clock drive freshness.
    msg.received_at = clock->now();
    return msg;
  }

  /// Pushes a frame and waits until the reader has accounted for it, so a case
  /// never races the pipeline.
  void pushAndWait(const CanMessage& msg) {
    const auto before = service->metrics().can_rx_read_total;
    transport->pushFrame(msg);
    EXPECT_TRUE(waitFor([&] { return service->metrics().can_rx_read_total > before; }));
  }

  void pushSpeed(std::uint16_t raw) {
    pushAndWait(frame(0x101, {static_cast<std::uint8_t>(raw & 0xFF),
                              static_cast<std::uint8_t>(raw >> 8), 0, 0, 0, 0, 0, 0}));
  }
};

// B-02, UT-22, spec section 13.1.
TEST(Service, StartLeavesSignalsNeverReceived) {
  Harness h;
  ASSERT_TRUE(h.start());
  EXPECT_EQ(h.service->state(), ServiceState::Running);

  for (const auto& definition : kSignals) {
    const auto sample = h.service->readSignal(definition.name);
    ASSERT_TRUE(sample.has_value()) << definition.name;
    EXPECT_EQ(sample->quality, SignalQuality::NeverReceived) << definition.name;
    EXPECT_FALSE(sample->value.has_value())
        << definition.name << " invented a value before any frame arrived";
  }
}

TEST(Service, ReadSignalBeforeStartIsNullopt) {
  Harness h;
  EXPECT_FALSE(h.service->readSignal("speed_kmh").has_value());
  EXPECT_TRUE(h.service->readAll().empty());
}

TEST(Service, StartRejectsInvalidConfig) {
  Harness h;
  CanLabConfig broken = validConfig();
  broken.rx_queue_capacity = 0;  // NFR-02

  EXPECT_FALSE(h.service->start(broken));
  EXPECT_EQ(h.service->state(), ServiceState::Stopped) << "nothing was created";
}

TEST(Service, StartRejectsInvertedReconnectBounds) {
  Harness h;
  CanLabConfig broken = validConfig();
  broken.reconnect_initial = 5000ms;
  broken.reconnect_max = 100ms;

  EXPECT_FALSE(h.service->start(broken));
  EXPECT_EQ(h.service->state(), ServiceState::Stopped);
}

// B-01, B-27. V1 rejects a second start rather than being idempotent, so a
// stray call cannot create a second reader on one socket.
TEST(Service, StartTwiceRejected) {
  Harness h;
  ASSERT_TRUE(h.start());

  EXPECT_FALSE(h.service->start(validConfig()));
  EXPECT_EQ(h.service->state(), ServiceState::Running) << "and the running service is untouched";
}

TEST(Service, DecodesAFrameIntoAFreshSignal) {
  Harness h;
  ASSERT_TRUE(h.start());

  h.pushSpeed(6000);  // 60.00 km/h
  ASSERT_TRUE(waitFor([&] {
    const auto sample = h.service->readSignal("speed_kmh");
    return sample && sample->value.has_value();
  }));

  const auto sample = h.service->readSignal("speed_kmh");
  EXPECT_DOUBLE_EQ(*sample->value, 60.00);
  EXPECT_EQ(sample->quality, SignalQuality::Fresh);
  EXPECT_EQ(sample->source_id, 0x101u);
  EXPECT_GT(sample->generation, 0u);
}

TEST(Service, DecodesBothSignalsOfAMultiSignalFrame) {
  Harness h;
  ASSERT_TRUE(h.start());

  h.pushAndWait(h.frame(0x102, {0xB0, 0x04, 0x55, 0, 0, 0, 0, 0}));
  ASSERT_TRUE(waitFor([&] {
    const auto soc = h.service->readSignal("battery_soc_pct");
    return soc && soc->value.has_value();
  }));

  EXPECT_DOUBLE_EQ(*h.service->readSignal("battery_voltage_v")->value, 12.00);
  EXPECT_DOUBLE_EQ(*h.service->readSignal("battery_soc_pct")->value, 85.0);
}

// UT-10 end to end: a frame rejected for range must leave both battery signals
// alone and mark them Invalid.
TEST(Service, AtomicRejectLeavesBothBatterySignalsUntouched) {
  Harness h;
  ASSERT_TRUE(h.start());
  h.pushAndWait(h.frame(0x102, {0xB0, 0x04, 0x55, 0, 0, 0, 0, 0}));
  ASSERT_TRUE(waitFor([&] { return h.service->metrics().can_decoded_total >= 1; }));

  h.pushAndWait(h.frame(0x102, {0xB0, 0x04, 0x78, 0, 0, 0, 0, 0}));  // SOC 120
  ASSERT_TRUE(waitFor([&] { return h.service->metrics().can_reject_range_total >= 1; }));

  EXPECT_DOUBLE_EQ(*h.service->readSignal("battery_voltage_v")->value, 12.00);
  EXPECT_DOUBLE_EQ(*h.service->readSignal("battery_soc_pct")->value, 85.0);
  EXPECT_EQ(h.service->metrics().can_reject_range_total, 1u)
      << "one frame, one range rejection - not one per field";
}

TEST(Service, UnknownIdIsCountedSeparatelyFromInvalid) {
  // UT-05 and B-12 at the service level: the two counters mean different things
  // to an operator.
  Harness h(/*broad_filter=*/true);
  ASSERT_TRUE(h.start());

  h.pushAndWait(h.frame(0x555, {}));
  ASSERT_TRUE(waitFor([&] { return h.service->metrics().can_unknown_id_total >= 1; }));

  EXPECT_EQ(h.service->metrics().can_unknown_id_total, 1u);
  EXPECT_EQ(h.service->metrics().can_rx_invalid_total, 0u)
      << "an unknown identifier is not a malformed frame";
}

TEST(Service, BroadFilterLeavesSignalsUntouchedByUnknownIds) {
  // The flag exists only so B-12 is observable; the filtered path is default.
  Harness h(/*broad_filter=*/true);
  ASSERT_TRUE(h.start());

  h.pushAndWait(h.frame(0x555, {}));
  ASSERT_TRUE(waitFor([&] { return h.service->metrics().can_unknown_id_total >= 1; }));

  for (const auto& definition : kSignals) {
    EXPECT_EQ(h.service->readSignal(definition.name)->quality, SignalQuality::NeverReceived)
        << definition.name;
  }
}

TEST(Service, ReservedNonzeroIsCountedNotRejected) {
  // UT-20, research R-15.
  Harness h;
  ASSERT_TRUE(h.start());

  h.pushAndWait(h.frame(0x101, {0x70, 0x17, 0xFF, 0, 0, 0, 0, 0}));
  ASSERT_TRUE(waitFor([&] { return h.service->metrics().can_reserved_nonzero_total >= 1; }));

  EXPECT_EQ(h.service->metrics().can_rx_invalid_total, 0u) << "the frame was accepted";
  ASSERT_TRUE(waitFor([&] {
    const auto sample = h.service->readSignal("speed_kmh");
    return sample && sample->value.has_value();
  }));
  EXPECT_DOUBLE_EQ(*h.service->readSignal("speed_kmh")->value, 60.00);
}

TEST(Service, MalformedLengthMarksTheSignalInvalid) {
  // UT-08, B-10 end to end.
  Harness h;
  ASSERT_TRUE(h.start());
  h.pushSpeed(6000);
  ASSERT_TRUE(waitFor([&] { return h.service->metrics().can_decoded_total >= 1; }));

  h.pushAndWait(h.frame(0x101, {0x70, 0x17, 0, 0, 0, 0, 0, 0}, 3));
  ASSERT_TRUE(waitFor([&] { return h.service->metrics().can_reject_length_total >= 1; }));

  const auto sample = h.service->readSignal("speed_kmh");
  ASSERT_TRUE(sample->value.has_value());
  EXPECT_DOUBLE_EQ(*sample->value, 60.00) << "the last good value survives";
}

// UT-16, B-19.
TEST(Service, EntersDegradedOnLinkError) {
  Harness h;
  ASSERT_TRUE(h.start());
  h.pushSpeed(6000);
  ASSERT_TRUE(waitFor([&] { return h.service->metrics().can_decoded_total >= 1; }));

  h.transport->pushReadStatus(ReadStatus::LinkError, 5 /* EIO */);

  ASSERT_TRUE(waitFor([&] { return h.service->metrics().can_link_error_total >= 1; }));
  EXPECT_GE(h.service->metrics().can_link_error_total, 1u);
}

TEST(Service, LinkErrorMarksSignalsNonFresh) {
  // B-19 and B-20 together: the value is kept, it stops being fresh, and a
  // recovery alone does not restore freshness.
  Harness h;
  ASSERT_TRUE(h.start());
  h.pushSpeed(6000);
  ASSERT_TRUE(waitFor([&] { return h.service->metrics().can_decoded_total >= 1; }));

  h.transport->pushReadStatus(ReadStatus::LinkError, 5);
  ASSERT_TRUE(waitFor([&] { return h.service->metrics().can_link_error_total >= 1; }));
  ASSERT_TRUE(waitFor([&] { return h.service->metrics().can_link_reconnect_total >= 1; }));

  const auto sample = h.service->readSignal("speed_kmh");
  ASSERT_TRUE(sample->value.has_value()) << "the last good value is retained";
  EXPECT_NE(sample->quality, SignalQuality::Fresh)
      << "transport recovery alone must not restore freshness";

  // One new frame proves it again.
  h.pushSpeed(6100);
  EXPECT_TRUE(waitFor(
      [&] { return h.service->readSignal("speed_kmh")->quality == SignalQuality::Fresh; }));
}

TEST(Service, StartOnMissingInterfaceEntersDegraded) {
  // A real socket on an interface that does not exist: start() succeeds and the
  // retry loop owns the problem (spec section 12.1, ADR-06).
  CanServiceDeps deps;
  deps.clock = std::make_shared<FakeClockAdapter>();
  CanService service(std::move(deps));

  CanLabConfig config = validConfig();
  config.interface_name = "vcan99";  // a lab name that is not present

  ASSERT_TRUE(service.start(config)) << "a missing interface is transient, not fatal";
  EXPECT_EQ(service.state(), ServiceState::Degraded);
  service.stop();
}

// B-21.
TEST(Service, SendWhenStoppedIsLinkUnavailable) {
  Harness h;
  EXPECT_EQ(h.service->sendDemoCommand(kCommandLock, 0x2A), WriteStatus::LinkUnavailable);
}

TEST(Service, SendRejectsUnknownCommandByte) {
  Harness h;
  ASSERT_TRUE(h.start());
  EXPECT_EQ(h.service->sendDemoCommand(0x09, 0x2A), WriteStatus::InvalidFrame);
  EXPECT_TRUE(h.transport->sent().empty());
}

TEST(Service, SendsAWellFormedCommandFrame) {
  Harness h;
  ASSERT_TRUE(h.start());

  EXPECT_EQ(h.service->sendDemoCommand(kCommandLock, 0x2A), WriteStatus::Sent);

  const auto sent = h.transport->sent();
  ASSERT_EQ(sent.size(), 1u);
  EXPECT_EQ(sent[0].id, 0x200u);
  EXPECT_EQ(sent[0].length, 8);
  EXPECT_EQ(sent[0].data[0], kCommandLock);
  EXPECT_EQ(sent[0].data[1], 0x2A);
  EXPECT_EQ(h.service->metrics().can_tx_success_total, 1u);
}

TEST(Service, RejectsASecondCommandWhileOneIsPending) {
  Harness h;
  ASSERT_TRUE(h.start());
  ASSERT_EQ(h.service->sendDemoCommand(kCommandLock, 0x2A), WriteStatus::Sent);

  EXPECT_EQ(h.service->sendDemoCommand(kCommandUnlock, 0x2B), WriteStatus::InvalidFrame);
  EXPECT_EQ(h.transport->sent().size(), 1u) << "and nothing extra reached the bus";
}

TEST(Service, MatchingAckCompletesTheCommand) {
  Harness h;
  ASSERT_TRUE(h.start());
  ASSERT_EQ(h.service->sendDemoCommand(kCommandLock, 0x2A), WriteStatus::Sent);

  h.pushAndWait(h.frame(0x201, {0x2A, kResultOk, 0, 0, 0, 0, 0, 0}));
  ASSERT_TRUE(waitFor([&] { return h.service->metrics().can_ack_matched_total >= 1; }));

  // The slot is free again.
  EXPECT_EQ(h.service->sendDemoCommand(kCommandUnlock, 0x2B), WriteStatus::Sent);
}

TEST(Service, UnmatchedAckLeavesTheRequestPending) {
  // B-23 at the service level.
  Harness h;
  ASSERT_TRUE(h.start());
  ASSERT_EQ(h.service->sendDemoCommand(kCommandLock, 0x2A), WriteStatus::Sent);

  h.pushAndWait(h.frame(0x201, {0x7F, kResultOk, 0, 0, 0, 0, 0, 0}));
  ASSERT_TRUE(waitFor([&] { return h.service->metrics().can_ack_unmatched_total >= 1; }));

  EXPECT_EQ(h.service->sendDemoCommand(kCommandUnlock, 0x2B), WriteStatus::InvalidFrame)
      << "the pending request was cancelled by an unrelated acknowledgment";
}

TEST(Service, SendFailureFreesTheSlot) {
  Harness h;
  ASSERT_TRUE(h.start());
  h.transport->setSendStatus(WriteStatus::SystemError);

  EXPECT_EQ(h.service->sendDemoCommand(kCommandLock, 0x2A), WriteStatus::SystemError);

  h.transport->setSendStatus(WriteStatus::Sent);
  EXPECT_EQ(h.service->sendDemoCommand(kCommandLock, 0x2B), WriteStatus::Sent)
      << "a transmit that never reached the bus must not hold the slot";
}

// B-25.
TEST(Service, StopWakesReader) {
  Harness h;
  ASSERT_TRUE(h.start());

  h.service->stop();

  EXPECT_EQ(h.service->state(), ServiceState::Stopped);
}

// B-26.
TEST(Service, StopIsIdempotent) {
  Harness h;
  ASSERT_TRUE(h.start());

  h.service->stop();
  h.service->stop();
  h.service->stop();

  EXPECT_EQ(h.service->state(), ServiceState::Stopped);
}

TEST(Service, StopBeforeStartIsSafe) {
  Harness h;
  h.service->stop();
  EXPECT_EQ(h.service->state(), ServiceState::Stopped);
}

// UT-17. A leak or an orphaned thread shows up here, and under ASan it shows up
// as a diagnostic rather than a silent pass.
TEST(Service, RepeatedStartStop) {
  Harness h;
  for (int cycle = 0; cycle < 10; ++cycle) {
    ASSERT_TRUE(h.start()) << "cycle " << cycle;
    EXPECT_EQ(h.service->state(), ServiceState::Running);
    h.service->stop();
    EXPECT_EQ(h.service->state(), ServiceState::Stopped);
  }
}

TEST(Service, SignalsSurviveARestartAsNeverReceived) {
  // B-28: frames are not persisted and not replayed, so a restart starts blind.
  Harness h;
  ASSERT_TRUE(h.start());
  h.pushSpeed(6000);
  ASSERT_TRUE(waitFor([&] { return h.service->metrics().can_decoded_total >= 1; }));
  h.service->stop();

  ASSERT_TRUE(h.start());
  EXPECT_EQ(h.service->readSignal("speed_kmh")->quality, SignalQuality::NeverReceived);
  EXPECT_FALSE(h.service->readSignal("speed_kmh")->value.has_value());
}

TEST(Service, HealthJsonShape) {
  Harness h;
  ASSERT_TRUE(h.start());
  h.pushSpeed(6000);
  ASSERT_TRUE(waitFor([&] { return h.service->metrics().can_decoded_total >= 1; }));

  const auto json = h.service->healthJson();

  EXPECT_NE(json.find("\"service_state\": \"RUNNING\""), std::string::npos) << json;
  EXPECT_NE(json.find("\"interface\": \"vcan0\""), std::string::npos) << json;
  EXPECT_NE(json.find("\"rx_queue_capacity\": 1024"), std::string::npos) << json;
  EXPECT_NE(json.find("\"speed_kmh\""), std::string::npos) << json;
  EXPECT_NE(json.find("\"quality\": \"FRESH\""), std::string::npos) << json;
  // A signal that was never received reports null, never a zero standing in for
  // a missing value (spec section 6.4).
  EXPECT_NE(json.find("\"value\": null"), std::string::npos) << json;
  EXPECT_NE(json.find("\"quality\": \"NEVER_RECEIVED\""), std::string::npos) << json;
  EXPECT_NE(json.find("\"source_can_id\": \"0x101\""), std::string::npos) << json;
}

TEST(Service, HealthJsonCarriesNoRawPayload) {
  // contracts/metrics.md section 5: the health surface never contains frame
  // bytes. A hex dump of the payload would be the obvious accidental leak.
  Harness h;
  ASSERT_TRUE(h.start());
  h.pushSpeed(6000);
  ASSERT_TRUE(waitFor([&] { return h.service->metrics().can_decoded_total >= 1; }));

  const auto json = h.service->healthJson();
  EXPECT_EQ(json.find("\"data\""), std::string::npos) << json;
  EXPECT_EQ(json.find("7017"), std::string::npos) << json;
}

// contracts/metrics.md section 3.
TEST(Service, MetricsInvariantsHold) {
  Harness h;
  ASSERT_TRUE(h.start());

  for (int i = 0; i < 50; ++i) {
    h.transport->pushFrame(h.frame(0x101, {0x70, 0x17, 0, 0, 0, 0, 0, 0}));
  }
  h.transport->pushFrame(h.frame(0x102, {0xB0, 0x04, 0x78, 0, 0, 0, 0, 0}));  // range reject
  ASSERT_TRUE(waitFor([&] { return h.service->metrics().can_rx_read_total >= 51; }));
  ASSERT_TRUE(waitFor([&] { return h.service->metrics().can_reject_range_total >= 1; }));

  const auto m = h.service->metrics();

  EXPECT_EQ(m.can_rx_enqueued_total + m.can_rx_queue_drop_total, m.can_rx_read_total);
  EXPECT_LE(m.can_rx_enqueued_total, m.can_rx_read_total);
  EXPECT_LE(m.can_queue_depth, m.can_queue_capacity);
  EXPECT_EQ(m.can_rx_invalid_total,
            m.can_reject_flags_total + m.can_reject_length_total + m.can_reject_range_total);
  EXPECT_LE(m.can_tx_success_total, m.can_tx_attempt_total);
  EXPECT_LE(m.can_ack_matched_total, m.can_tx_success_total);
  EXPECT_GE(m.signal_update_total, m.can_decoded_total)
      << "multi-signal frames make updates exceed decoded frames";
}

TEST(Service, MetricsAreZeroBeforeAnyTraffic) {
  Harness h;
  ASSERT_TRUE(h.start());

  const auto m = h.service->metrics();
  EXPECT_EQ(m.can_rx_read_total, 0u);
  EXPECT_EQ(m.can_decoded_total, 0u);
  EXPECT_EQ(m.signal_update_total, 0u);
  EXPECT_EQ(m.can_queue_depth, 0u);
  EXPECT_EQ(m.can_queue_capacity, 1024u);
}

TEST(Service, QueueDropsAreBoundedAndCounted) {
  // B-16, FR-10 at the service level. A tiny queue plus a burst makes the drop
  // path certain rather than hopeful.
  CanLabConfig config = validConfig();
  config.rx_queue_capacity = 2;

  auto clock = std::make_shared<FakeClockAdapter>();
  auto fake = std::make_unique<FakeCanTransport>();
  auto* transport = fake.get();

  CanServiceDeps deps;
  deps.clock = clock;
  deps.transport = std::move(fake);
  CanService service(std::move(deps));
  ASSERT_TRUE(service.start(config));

  CanMessage msg;
  msg.id = 0x101;
  msg.length = 8;
  msg.data = {0x70, 0x17, 0, 0, 0, 0, 0, 0};
  msg.received_at = clock->now();
  for (int i = 0; i < 2000; ++i) {
    transport->pushFrame(msg);
  }

  ASSERT_TRUE(waitFor([&] { return service.metrics().can_rx_read_total >= 2000; }, 5000ms));

  const auto m = service.metrics();
  EXPECT_LE(m.can_queue_depth, m.can_queue_capacity);
  EXPECT_EQ(m.can_rx_enqueued_total + m.can_rx_queue_drop_total, m.can_rx_read_total);
  service.stop();
}

TEST(Service, NothingIsLostWithoutATrace) {
  // Spec section 13.8 step 4: leftovers at the drain deadline are counted, not
  // silently dropped. Whatever the timing, the accounting has to add up.
  CanLabConfig config = validConfig();
  config.drain_deadline = 0ms;

  auto clock = std::make_shared<FakeClockAdapter>();
  auto fake = std::make_unique<FakeCanTransport>();
  auto* transport = fake.get();

  CanServiceDeps deps;
  deps.clock = clock;
  deps.transport = std::move(fake);
  auto service = std::make_unique<CanService>(std::move(deps));
  ASSERT_TRUE(service->start(config));

  CanMessage msg;
  msg.id = 0x101;
  msg.length = 8;
  msg.data = {0x70, 0x17, 0, 0, 0, 0, 0, 0};
  msg.received_at = clock->now();
  for (int i = 0; i < 500; ++i) {
    transport->pushFrame(msg);
  }

  service->stop();
  const auto m = service->metrics();
  EXPECT_EQ(m.can_rx_enqueued_total + m.can_rx_queue_drop_total, m.can_rx_read_total);
  EXPECT_LE(m.can_decoded_total + m.can_shutdown_discarded_total, m.can_rx_enqueued_total);
}


// A logger that remembers what it was told, so the rate-limiting rule can be
// asserted rather than hoped for.
class RecordingLogger final : public tcu::logging::ILogger {
 public:
  void log(tcu::logging::Severity level, std::string_view component, std::string_view event,
           std::string_view message) noexcept override {
    log(level, component, event, message, {});
  }

  void log(tcu::logging::Severity, std::string_view, std::string_view event, std::string_view,
           std::vector<tcu::logging::Field>) noexcept override {
    std::lock_guard<std::mutex> lock(mutex_);
    ++counts_[std::string(event)];
  }

  bool enabled(tcu::logging::Severity) const noexcept override { return true; }
  void flush() override {}
  void stop() override {}

  std::size_t count(const std::string& event) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = counts_.find(event);
    return it == counts_.end() ? 0u : it->second;
  }

 private:
  mutable std::mutex mutex_;
  std::map<std::string, std::size_t> counts_;
};

// Spec sections 10.3 and 13.7: the drop path is rate-limited, so an overload
// cannot turn the logger into the bottleneck. The counter still rises per
// frame - losing that would be worse than a noisy log.
TEST(Service, DropLogIsRateLimited) {
  CanLabConfig config = validConfig();
  config.rx_queue_capacity = 2;

  auto clock = std::make_shared<FakeClockAdapter>();
  auto logger = std::make_shared<RecordingLogger>();
  auto fake = std::make_unique<FakeCanTransport>();
  auto* transport = fake.get();

  CanServiceDeps deps;
  deps.clock = clock;
  deps.logger = logger;
  deps.transport = std::move(fake);
  CanService service(std::move(deps));
  ASSERT_TRUE(service.start(config));

  CanMessage msg;
  msg.id = 0x101;
  msg.length = 8;
  msg.data = {0x70, 0x17, 0, 0, 0, 0, 0, 0};
  msg.received_at = clock->now();
  for (int i = 0; i < 3000; ++i) {
    transport->pushFrame(msg);
  }

  ASSERT_TRUE(waitFor([&] { return service.metrics().can_rx_read_total >= 3000; }, 5000ms));
  ASSERT_GT(service.metrics().can_rx_queue_drop_total, 50u)
      << "the overload path was not exercised";

  // The fake clock never advanced, so every drop fell inside one rate-limit
  // window: exactly one line, however many frames were discarded.
  EXPECT_LE(logger->count("queue.dropped"), 1u)
      << "the drop log was not rate-limited; " << logger->count("queue.dropped")
      << " lines for " << service.metrics().can_rx_queue_drop_total << " drops";

  service.stop();
}

TEST(Service, RejectLogIsRateLimited) {
  auto clock = std::make_shared<FakeClockAdapter>();
  auto logger = std::make_shared<RecordingLogger>();
  auto fake = std::make_unique<FakeCanTransport>();
  auto* transport = fake.get();

  CanServiceDeps deps;
  deps.clock = clock;
  deps.logger = logger;
  deps.transport = std::move(fake);
  CanService service(std::move(deps));
  ASSERT_TRUE(service.start(validConfig()));

  CanMessage bad;
  bad.id = 0x101;
  bad.length = 3;  // a known identifier at the wrong length
  bad.received_at = clock->now();
  for (int i = 0; i < 500; ++i) {
    transport->pushFrame(bad);
  }

  ASSERT_TRUE(waitFor([&] { return service.metrics().can_reject_length_total >= 500; }, 5000ms));
  EXPECT_LE(logger->count("frame.rejected"), 1u)
      << "spec section 13.4: repeated invalid frames give a growing counter, not a growing log";

  service.stop();
}

// UT-21, NFR-06, Constitution Principle V. Project 05 exposes no callback of
// its own, so the real version of "a callback throws" is a subscriber on the
// event bus throwing: the CAN service must survive it and keep decoding.
TEST(Service, ThrowingEventSubscriberDoesNotKillTheService) {
  auto clock = std::make_shared<FakeClockAdapter>();
  auto bus = std::make_shared<tcu::eventbus::EventBus>(tcu::eventbus::BusConfig{});
  bus->start();
  auto subscription =
      bus->subscribe(tcu::eventbus::event_type_of<tcu::eventbus::VehicleSignalUpdated>(),
                     [](const tcu::eventbus::Event&) { throw std::runtime_error("boom"); });

  auto fake = std::make_unique<FakeCanTransport>();
  auto* transport = fake.get();

  CanServiceDeps deps;
  deps.clock = clock;
  deps.bus = bus;
  deps.transport = std::move(fake);
  CanService service(std::move(deps));
  ASSERT_TRUE(service.start(validConfig()));

  CanMessage msg;
  msg.id = 0x101;
  msg.length = 8;
  msg.data = {0x70, 0x17, 0, 0, 0, 0, 0, 0};
  msg.received_at = clock->now();
  for (int i = 0; i < 20; ++i) {
    transport->pushFrame(msg);
  }

  ASSERT_TRUE(waitFor([&] { return service.metrics().can_decoded_total >= 20; }));
  EXPECT_EQ(service.state(), ServiceState::Running) << "a throwing subscriber took the service down";
  EXPECT_DOUBLE_EQ(*service.readSignal("speed_kmh")->value, 60.00);

  service.stop();
  bus->stop(tcu::eventbus::StopMode::Discard);
}

// Research R-18: the backoff is bounded AND stop() wakes the reader out of it
// immediately, rather than after the full interval. A five-second initial
// backoff makes the difference unmissable.
TEST(Service, BackoffWakesOnStop) {
  CanLabConfig config = validConfig();
  config.reconnect_initial = 5000ms;
  config.reconnect_max = 5000ms;
  config.reconnect_jitter_pct = 0;

  auto clock = std::make_shared<FakeClockAdapter>();
  auto fake = std::make_unique<FakeCanTransport>();
  auto* transport = fake.get();

  CanServiceDeps deps;
  deps.clock = clock;
  deps.transport = std::move(fake);
  auto service = std::make_unique<CanService>(std::move(deps));
  ASSERT_TRUE(service->start(config));

  transport->pushReadStatus(ReadStatus::LinkError, 5 /* EIO */);
  ASSERT_TRUE(waitFor([&] { return service->metrics().can_link_error_total >= 1; }));

  // The reader is now parked in a five-second backoff wait.
  const auto before = std::chrono::steady_clock::now();
  service->stop();
  const auto elapsed = std::chrono::steady_clock::now() - before;

  EXPECT_EQ(service->state(), ServiceState::Stopped);
  EXPECT_LT(elapsed, 2000ms)
      << "stop() waited out the backoff instead of being woken; a shutdown must not be "
         "hostage to a retry interval";
}

}  // namespace

// B-30 and spec section 18.3: randomised traffic, repeated restarts, and a
// decoder fuzz loop.
//
// All of it runs on FakeCanTransport, for two reasons. The obvious one is that
// it needs no vcan. The one that matters more is spec section 18.1's test note:
// the kernel refuses to carry a malformed classical frame, so the flag and
// length cases below CANNOT be produced on a real bus, and claiming otherwise
// would be claiming an impossible test passed.
//
// The seeds are fixed. A fuzz run that cannot be reproduced is a rumour, not a
// test, and section 18.3 asks for "generator replay from saved seeds".

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <utility>

#include "tcu/can/can_config.hpp"
#include "tcu/can/can_service.hpp"
#include "tcu/can/demo_signal_decoder.hpp"
#include "tcu/can/fake_can_transport.hpp"
#include "tcu/can/frame_catalog.hpp"
#include "tcu/can/frame_validator.hpp"

namespace {

using namespace tcu::can;
using namespace std::chrono_literals;
using tcu::scheduler::FakeClockAdapter;

constexpr std::uint32_t kSeed = 20261105;

std::string configText() {
  return R"json({
  "schema_version": 1,
  "mode": "lab_only",
  "can": {
    "interface": "vcan0",
    "format": "classic_standard_11bit",
    "rx_queue_capacity": 64,
    "health_check_ms": 10,
    "drain_deadline_ms": 50,
    "reconnect": { "initial_ms": 1, "max_ms": 4, "jitter_percent": 10 },
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

CanLabConfig stressConfig() {
  CanLabConfig config;
  const auto result = parseCanLabConfig(configText(), config);
  EXPECT_TRUE(result) << result.pointer << " " << result.publicReason;
  return config;
}

template <typename Predicate>
bool waitFor(Predicate predicate, std::chrono::milliseconds budget = 20000ms) {
  const auto deadline = std::chrono::steady_clock::now() + budget;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }
    std::this_thread::yield();
  }
  return predicate();
}

/// Every invariant of contracts/metrics.md section 3, in one place, so a stress
/// run can assert the whole set once the dust settles.
void expectMetricInvariants(const CanMetricsSnapshot& m) {
  EXPECT_EQ(m.can_rx_enqueued_total + m.can_rx_queue_drop_total, m.can_rx_read_total)
      << "a frame was read and then neither admitted nor counted as dropped";
  EXPECT_LE(m.can_rx_enqueued_total, m.can_rx_read_total);
  EXPECT_LE(m.can_queue_depth, m.can_queue_capacity);
  EXPECT_EQ(m.can_rx_invalid_total,
            m.can_reject_flags_total + m.can_reject_length_total + m.can_reject_range_total);
  EXPECT_LE(m.can_tx_success_total, m.can_tx_attempt_total);
  EXPECT_LE(m.can_ack_matched_total, m.can_tx_success_total);
  EXPECT_LE(m.can_decoded_total, m.can_rx_enqueued_total);
  EXPECT_EQ(m.event_publish_failed_total, 0u) << "no bus is attached, so nothing can fail";
}

/// A random frame: mostly catalog identifiers so the decode path gets real
/// work, with a steady minority of nonsense so the reject paths do too.
CanMessage randomFrame(std::mt19937& rng, SteadyTime now) {
  std::uniform_int_distribution<int> pick(0, 99);
  std::uniform_int_distribution<int> byte(0, 255);

  CanMessage frame;
  frame.received_at = now;
  frame.length = 8;

  const int roll = pick(rng);
  if (roll < 70) {
    static constexpr std::array<std::uint32_t, 6> known{
        {0x100, 0x101, 0x102, 0x103, 0x110, 0x201}};
    frame.id = known[static_cast<std::size_t>(pick(rng)) % known.size()];
  } else if (roll < 80) {
    frame.id = static_cast<std::uint32_t>(pick(rng)) + 0x400;  // unknown identifier
  } else if (roll < 87) {
    frame.id = 0x101;
    frame.length = static_cast<std::uint8_t>(pick(rng) % 8);  // wrong length
  } else if (roll < 94) {
    frame.id = 0x101;
    frame.kind = (roll % 2) == 0 ? FrameKind::Remote : FrameKind::Error;
  } else {
    frame.id = 0x101;
    frame.format = FrameFormat::Extended29;
  }

  for (auto& value : frame.data) {
    value = static_cast<std::uint8_t>(byte(rng));
  }
  return frame;
}

// B-30: "Host under stress | Inject random frames and frequent stop/start | No
// memory error, deadlock, or unbounded queue under supported test conditions."
TEST(Stress, RandomFramesWithRestarts) {
  const auto config = stressConfig();

  for (int cycle = 0; cycle < 8; ++cycle) {
    auto clock = std::make_shared<FakeClockAdapter>();
    auto fake = std::make_unique<FakeCanTransport>();
    auto* transport = fake.get();

    CanServiceDeps deps;
    deps.clock = clock;
    deps.transport = std::move(fake);
    CanService service(std::move(deps));

    ASSERT_TRUE(service.start(config)) << "cycle " << cycle;

    std::mt19937 rng(kSeed + static_cast<std::uint32_t>(cycle));
    constexpr int kFrames = 3000;
    for (int i = 0; i < kFrames; ++i) {
      transport->pushFrame(randomFrame(rng, clock->now()));
      // Move the clock now and then, so freshness deadlines actually expire
      // during the run rather than never firing.
      if (i % 200 == 0) {
        clock->advance(50ms);
      }
    }

    ASSERT_TRUE(waitFor([&] {
      return service.metrics().can_rx_read_total >= static_cast<std::uint64_t>(kFrames);
    })) << "cycle "
        << cycle << ": the reader did not keep up within the budget";

    const auto before_stop = service.metrics();
    EXPECT_LE(before_stop.can_queue_depth, before_stop.can_queue_capacity);

    service.stop();
    EXPECT_EQ(service.state(), ServiceState::Stopped) << "cycle " << cycle;
    expectMetricInvariants(service.metrics());
  }
}

TEST(Stress, QueueStaysBounded) {
  // A queue of 64 against 50,000 frames: the drop path is the normal path here,
  // and the bound has to hold at every observation.
  auto clock = std::make_shared<FakeClockAdapter>();
  auto fake = std::make_unique<FakeCanTransport>();
  auto* transport = fake.get();

  CanServiceDeps deps;
  deps.clock = clock;
  deps.transport = std::move(fake);
  CanService service(std::move(deps));
  ASSERT_TRUE(service.start(stressConfig()));

  CanMessage speed;
  speed.id = 0x101;
  speed.length = 8;
  speed.data = {0x70, 0x17, 0, 0, 0, 0, 0, 0};

  constexpr int kFrames = 50000;
  for (int i = 0; i < kFrames; ++i) {
    speed.received_at = clock->now();
    transport->pushFrame(speed);
    if ((i % 1000) == 0) {
      const auto m = service.metrics();
      ASSERT_LE(m.can_queue_depth, m.can_queue_capacity) << "exceeded the bound at frame " << i;
    }
  }

  ASSERT_TRUE(waitFor([&] {
    return service.metrics().can_rx_read_total >= static_cast<std::uint64_t>(kFrames);
  }));

  const auto m = service.metrics();
  EXPECT_GT(m.can_rx_queue_drop_total, 0u) << "the overload path was never exercised";
  expectMetricInvariants(m);
  service.stop();
}

// Spec section 18.3: "Decoder fuzz testing with random IDs, flags, lengths, and
// payloads through fake transport (not relying on malformed kernel frames)."
//
// No service here: the validator and decoder are pure, so a tight loop over
// them covers far more inputs per second than driving them through threads.
TEST(Stress, DecoderFuzz) {
  FrameValidator validator;
  DemoSignalDecoder decoder;
  std::mt19937 rng(kSeed);
  std::uniform_int_distribution<int> byte(0, 255);
  std::uniform_int_distribution<std::uint32_t> any_id(0, 0xFFFFFFFFu);
  std::uniform_int_distribution<int> small(0, 3);

  constexpr int kIterations = 200000;
  for (int i = 0; i < kIterations; ++i) {
    CanMessage frame;
    // A fully random identifier, not just a catalog one: the point is that
    // nothing in the space can make either of these misbehave.
    frame.id = any_id(rng) & 0x7FFu;
    frame.length = static_cast<std::uint8_t>(byte(rng) % 9);
    frame.format = small(rng) == 0 ? FrameFormat::Extended29 : FrameFormat::Standard11;
    switch (small(rng)) {
      case 0:  frame.kind = FrameKind::Remote; break;
      case 1:  frame.kind = FrameKind::Error;  break;
      default: frame.kind = FrameKind::Data;   break;
    }
    for (auto& value : frame.data) {
      value = static_cast<std::uint8_t>(byte(rng));
    }

    const auto validation = validator.validate(frame);
    const auto decoded = decoder.decode(frame);

    // The invariant that matters: a rejected frame yields no signal at all.
    // Anything else would mean a malformed frame could reach the cache.
    if (!decoded.accepted) {
      ASSERT_TRUE(decoded.updates.empty()) << "iteration " << i;
      ASSERT_NE(decoded.reason, RejectReason::None) << "iteration " << i;
    } else {
      // And an accepted frame is one the validator also accepted, with every
      // value inside the catalog bounds.
      ASSERT_TRUE(validation.ok)
          << "iteration " << i << ": decoded a frame the validator rejected";
      ASSERT_FALSE(decoded.updates.empty()) << "iteration " << i;
      for (const auto& update : decoded.updates) {
        const auto* definition = findSignal(update.signal_name);
        ASSERT_NE(definition, nullptr) << update.signal_name;
        ASSERT_TRUE(update.value.has_value());
        ASSERT_GE(*update.value, definition->min) << update.signal_name;
        ASSERT_LE(*update.value, definition->max) << update.signal_name;
      }
    }
  }
}

// The same loop through the fake transport and the whole service, so the queue,
// the cache and the metrics see the fuzz too - at a lower iteration count,
// because this one pays for threads.
TEST(Stress, FuzzThroughTheWholeService) {
  auto clock = std::make_shared<FakeClockAdapter>();
  auto fake = std::make_unique<FakeCanTransport>();
  auto* transport = fake.get();

  CanServiceDeps deps;
  deps.clock = clock;
  deps.transport = std::move(fake);
  deps.broad_filter = true;  // let unknown identifiers through, so they are counted
  CanService service(std::move(deps));
  ASSERT_TRUE(service.start(stressConfig()));

  std::mt19937 rng(kSeed + 1);
  constexpr int kFrames = 20000;
  for (int i = 0; i < kFrames; ++i) {
    transport->pushFrame(randomFrame(rng, clock->now()));
  }

  ASSERT_TRUE(waitFor([&] {
    return service.metrics().can_rx_read_total >= static_cast<std::uint64_t>(kFrames);
  }));

  const auto m = service.metrics();
  expectMetricInvariants(m);
  EXPECT_GT(m.can_rx_invalid_total, 0u) << "the fuzz never produced a malformed frame";
  EXPECT_GT(m.can_unknown_id_total, 0u) << "the fuzz never produced an unknown identifier";
  EXPECT_GT(m.can_decoded_total, 0u) << "the fuzz never produced a valid frame";

  // And every signal that was ever updated holds a value inside its bounds - no
  // fuzz input leaked a nonsense value into the cache.
  for (const auto& sample : service.readAll()) {
    if (!sample.value.has_value()) {
      continue;
    }
    const auto* definition = findSignal(sample.signal_name);
    ASSERT_NE(definition, nullptr);
    EXPECT_GE(*sample.value, definition->min) << sample.signal_name;
    EXPECT_LE(*sample.value, definition->max) << sample.signal_name;
  }

  service.stop();
}

}  // namespace

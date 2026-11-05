// FR-07, and Constitution Principle V on the failure path.
//
// Run against the real project 01 bus rather than a mock: what is worth testing
// is that project 05 publishes payloads project 01 actually accepts, and the
// appended variant alternatives are exactly where that could break.

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include "can_metrics_store.hpp"
#include "event_bus_adapter.hpp"
#include "tcu/can/frame_catalog.hpp"

namespace {

using namespace tcu::can;
using namespace std::chrono_literals;
using tcu::scheduler::FakeClockAdapter;

/// Collects the events the bus dispatches. The dispatcher runs on its own
/// thread, so everything here is behind a mutex.
struct Recorder {
  mutable std::mutex mutex;
  std::vector<tcu::eventbus::VehicleSignalUpdated> values;
  std::vector<tcu::eventbus::SignalQualityChanged> qualities;
  std::vector<tcu::eventbus::CanLinkStatusChanged> links;

  std::size_t count() const {
    std::lock_guard<std::mutex> lock(mutex);
    return values.size() + qualities.size() + links.size();
  }
};

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

struct Fixture {
  std::shared_ptr<FakeClockAdapter> clock = std::make_shared<FakeClockAdapter>();
  std::shared_ptr<tcu::eventbus::EventBus> bus =
      std::make_shared<tcu::eventbus::EventBus>(tcu::eventbus::BusConfig{});
  CanMetricsStore metrics;
  Recorder recorder;
  std::vector<tcu::eventbus::Subscription> subscriptions;
  EventBusAdapter adapter{bus, clock, metrics};

  Fixture() {
    bus->start();
    subscriptions.push_back(
        bus->subscribe(tcu::eventbus::event_type_of<tcu::eventbus::VehicleSignalUpdated>(),
                       [this](const tcu::eventbus::Event& event) {
                         std::lock_guard<std::mutex> lock(recorder.mutex);
                         recorder.values.push_back(
                             std::get<tcu::eventbus::VehicleSignalUpdated>(event.payload));
                       }));
    subscriptions.push_back(
        bus->subscribe(tcu::eventbus::event_type_of<tcu::eventbus::SignalQualityChanged>(),
                       [this](const tcu::eventbus::Event& event) {
                         std::lock_guard<std::mutex> lock(recorder.mutex);
                         recorder.qualities.push_back(
                             std::get<tcu::eventbus::SignalQualityChanged>(event.payload));
                       }));
    subscriptions.push_back(
        bus->subscribe(tcu::eventbus::event_type_of<tcu::eventbus::CanLinkStatusChanged>(),
                       [this](const tcu::eventbus::Event& event) {
                         std::lock_guard<std::mutex> lock(recorder.mutex);
                         recorder.links.push_back(
                             std::get<tcu::eventbus::CanLinkStatusChanged>(event.payload));
                       }));
  }

  SignalSample speedSample() {
    SignalSample sample;
    sample.signal_name = "speed_kmh";
    sample.value = 60.0;
    sample.quality = SignalQuality::Fresh;
    sample.source_id = 0x101;
    sample.generation = 7;
    sample.last_good_at = SteadyTime{};
    return sample;
  }
};

// Appending to a closed variant is only safe if the existing alternatives keep
// their indices. If this ever fails, every subscriber in projects 01 and 04 is
// listening to the wrong event type.
TEST(EventAdapter, AppendedAlternativesDidNotRenumberTheExistingOnes) {
  EXPECT_EQ(tcu::eventbus::event_type_of<tcu::eventbus::IgnitionChanged>(), 0u);
  EXPECT_EQ(tcu::eventbus::event_type_of<tcu::eventbus::VehicleSpeedUpdated>(), 1u);
  EXPECT_EQ(tcu::eventbus::event_type_of<tcu::eventbus::NetworkStatusChanged>(), 2u);
  EXPECT_EQ(tcu::eventbus::event_type_of<tcu::eventbus::ConfigChanged>(), 3u);
  // And project 05's three sit after them.
  EXPECT_EQ(tcu::eventbus::event_type_of<tcu::eventbus::VehicleSignalUpdated>(), 4u);
  EXPECT_EQ(tcu::eventbus::event_type_of<tcu::eventbus::SignalQualityChanged>(), 5u);
  EXPECT_EQ(tcu::eventbus::event_type_of<tcu::eventbus::CanLinkStatusChanged>(), 6u);
}

// FR-07: the value, where it came from, when, and how much to trust it.
TEST(EventAdapter, PublishesSignalUpdated) {
  Fixture f;
  f.clock->advance(72ms);

  f.adapter.publishValue(f.speedSample());

  ASSERT_TRUE(waitFor([&] { return f.recorder.count() >= 1; }));
  std::lock_guard<std::mutex> lock(f.recorder.mutex);
  ASSERT_EQ(f.recorder.values.size(), 1u);
  const auto& payload = f.recorder.values[0];
  EXPECT_EQ(payload.signal_name, "speed_kmh");
  EXPECT_DOUBLE_EQ(payload.value, 60.0);
  EXPECT_EQ(payload.source_can_id, 0x101u);
  EXPECT_EQ(payload.generation, 7u);
  EXPECT_EQ(payload.monotonic_age_ms, 72) << "an age, not a time point (spec section 25)";
  EXPECT_EQ(f.metrics.event_publish_total.load(), 1u);
}

TEST(EventAdapter, SkipsASampleWithNoValue) {
  // A NeverReceived signal has nothing to publish, and publishing a zero would
  // be the exact misrepresentation NFR-07 forbids.
  Fixture f;
  SignalSample empty;
  empty.signal_name = "speed_kmh";
  empty.quality = SignalQuality::NeverReceived;

  f.adapter.publishValue(empty);

  EXPECT_EQ(f.metrics.event_publish_total.load(), 0u);
  EXPECT_EQ(f.recorder.count(), 0u);
}

TEST(EventAdapter, PublishesOneEventPerTransition) {
  // B-08: the adapter publishes exactly the list the cache handed it.
  Fixture f;
  const std::vector<QualityTransition> transitions{
      {"speed_kmh", SignalQuality::Fresh, SignalQuality::Stale, 7},
      {"heartbeat", SignalQuality::Fresh, SignalQuality::Stale, 3},
  };

  f.adapter.publishTransitions(transitions);

  ASSERT_TRUE(waitFor([&] { return f.recorder.count() >= 2; }));
  std::lock_guard<std::mutex> lock(f.recorder.mutex);
  ASSERT_EQ(f.recorder.qualities.size(), 2u);
  EXPECT_EQ(f.recorder.qualities[0].signal_name, "speed_kmh");
  EXPECT_EQ(f.recorder.qualities[0].quality, static_cast<std::uint8_t>(SignalQuality::Stale));
  EXPECT_EQ(f.recorder.qualities[0].generation, 7u);
  EXPECT_EQ(f.recorder.qualities[1].signal_name, "heartbeat");
}

TEST(EventAdapter, AnEmptyTransitionListPublishesNothing) {
  // The common case on a quiet health tick, and the reason B-08 holds.
  Fixture f;
  f.adapter.publishTransitions({});

  EXPECT_EQ(f.metrics.event_publish_total.load(), 0u);
  EXPECT_EQ(f.recorder.count(), 0u);
}

TEST(EventAdapter, PublishesLinkStatus) {
  Fixture f;
  f.adapter.publishLinkStatus(false, ServiceState::Degraded);

  ASSERT_TRUE(waitFor([&] { return f.recorder.count() >= 1; }));
  std::lock_guard<std::mutex> lock(f.recorder.mutex);
  ASSERT_EQ(f.recorder.links.size(), 1u);
  EXPECT_FALSE(f.recorder.links[0].available);
  EXPECT_EQ(f.recorder.links[0].service_state,
            static_cast<std::uint8_t>(ServiceState::Degraded));
}

// Constitution Principle V: a publish failure is counted, never swallowed and
// never allowed to propagate.
TEST(EventAdapter, PublishFailureIsCountedNotThrown) {
  auto clock = std::make_shared<FakeClockAdapter>();
  auto bus = std::make_shared<tcu::eventbus::EventBus>(tcu::eventbus::BusConfig{});
  bus->start();
  bus->stop(tcu::eventbus::StopMode::Discard);  // every later publish is refused

  CanMetricsStore metrics;
  EventBusAdapter adapter(bus, clock, metrics);

  SignalSample sample;
  sample.signal_name = "speed_kmh";
  sample.value = 60.0;
  sample.source_id = 0x101;
  sample.generation = 1;

  EXPECT_NO_THROW(adapter.publishValue(sample));

  EXPECT_EQ(metrics.event_publish_total.load(), 0u);
  EXPECT_EQ(metrics.event_publish_failed_total.load(), 1u)
      << "a refused publish must be visible, not invisible";
}

// A null bus is the default for unit tests and for a standalone service run.
// It must be inert, not a crash.
TEST(EventAdapter, NullBusIsInert) {
  auto clock = std::make_shared<FakeClockAdapter>();
  CanMetricsStore metrics;
  EventBusAdapter adapter(nullptr, clock, metrics);

  EXPECT_FALSE(adapter.enabled());

  SignalSample sample;
  sample.signal_name = "speed_kmh";
  sample.value = 60.0;
  EXPECT_NO_THROW(adapter.publishValue(sample));
  EXPECT_NO_THROW(adapter.publishTransitions(
      {{"speed_kmh", SignalQuality::Fresh, SignalQuality::Stale, 1}}));
  EXPECT_NO_THROW(adapter.publishLinkStatus(true, ServiceState::Running));

  EXPECT_EQ(metrics.event_publish_total.load(), 0u);
  EXPECT_EQ(metrics.event_publish_failed_total.load(), 0u)
      << "no bus is not a failure, it is no publishing";
}

// contracts/metrics.md: every publish attempt is accounted for by exactly one
// of the two counters.
TEST(EventAdapter, EveryAttemptIsAccountedFor) {
  Fixture f;
  for (int i = 0; i < 25; ++i) {
    f.adapter.publishValue(f.speedSample());
  }
  f.adapter.publishLinkStatus(true, ServiceState::Running);

  const auto total =
      f.metrics.event_publish_total.load() + f.metrics.event_publish_failed_total.load();
  EXPECT_EQ(total, 26u);
}

}  // namespace

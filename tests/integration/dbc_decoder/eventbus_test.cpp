// T056: FR-18. Decoded signals reach project 01's Event Bus through an adapter
// that lives outside both libraries, so neither tcu_dbc nor tcu_signal depends
// on a consumer (research R-18).

#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

#include "support/service_fixture.hpp"
#include "tcu/eventbus/event_bus.hpp"

namespace {

using namespace tcu::signal::test;
using tcu::signal::DecoderService;
using tcu::signal::SignalKey;

std::shared_ptr<tcu::scheduler::TimerScheduler> startedScheduler() {
  tcu::scheduler::Config config;
  config.workerThreads = 2;
  auto scheduler = std::make_shared<tcu::scheduler::TimerScheduler>(config);
  scheduler->start();
  return scheduler;
}

/// The same adapter apps/virtual_dbc_demo installs, so this test covers the
/// code path the demo demonstrates rather than a parallel one.
class BusAdapter : public tcu::signal::ISignalObserver {
 public:
  explicit BusAdapter(std::shared_ptr<tcu::eventbus::EventBus> bus) : bus_(std::move(bus)) {}

  void onBatch(const tcu::dbc::DecodedBatch& batch) override {
    for (const auto& value : batch.signals) {
      if (value.quality != tcu::dbc::SignalQuality::Valid) continue;
      tcu::eventbus::Event event;
      event.type = tcu::eventbus::event_type_of<tcu::eventbus::VehicleSignalUpdated>();
      event.created_at = std::chrono::steady_clock::now();
      event.payload = tcu::eventbus::VehicleSignalUpdated{
          value.signalName, value.physical, batch.arbitrationId, batch.dbcVersion, 0};
      if (bus_->publish(event) != tcu::eventbus::PublishStatus::Accepted) ++publishFailures;
    }
  }

  void onQualityChanged(const SignalKey& key, tcu::dbc::SignalQuality quality) override {
    tcu::eventbus::Event event;
    event.type = tcu::eventbus::event_type_of<tcu::eventbus::SignalQualityChanged>();
    event.created_at = std::chrono::steady_clock::now();
    event.payload = tcu::eventbus::SignalQualityChanged{key.signalName,
                                                        static_cast<std::uint8_t>(quality), 0};
    if (bus_->publish(event) != tcu::eventbus::PublishStatus::Accepted) ++publishFailures;
  }

  std::atomic<std::uint64_t> publishFailures{0};

 private:
  std::shared_ptr<tcu::eventbus::EventBus> bus_;
};

struct BusHarness {
  TempDir dir;
  std::shared_ptr<tcu::eventbus::EventBus> bus =
      std::make_shared<tcu::eventbus::EventBus>(tcu::eventbus::BusConfig{});
  std::shared_ptr<tcu::scheduler::TimerScheduler> scheduler = startedScheduler();
  tcu::can::FakeCanTransport* transport{nullptr};
  std::unique_ptr<DecoderService> service;
  std::shared_ptr<BusAdapter> adapter;

  explicit BusHarness(std::size_t queueCapacity = 1024) {
    bus = std::make_shared<tcu::eventbus::EventBus>(
        tcu::eventbus::BusConfig{queueCapacity});
    bus->start();
    const std::string path = dir.write("lab.dbc", labDbc());
    auto fake = std::make_unique<tcu::can::FakeCanTransport>();
    transport = fake.get();
    tcu::signal::DecoderServiceDeps deps;
    deps.transport = std::move(fake);
    deps.scheduler = scheduler;
    service = std::make_unique<DecoderService>(configFor(path), std::move(deps));
    adapter = std::make_shared<BusAdapter>(bus);
    service->addObserver(adapter);
  }

  ~BusHarness() {
    if (service) service->stop();
    scheduler->stop(tcu::scheduler::StopMode::Discard);
    bus->stop();
  }
};

TEST(EventBus_PublishesSignalsDecoded, SubscriberSeesTheDecodedValues) {
  BusHarness harness;
  std::mutex mutex;
  std::vector<tcu::eventbus::VehicleSignalUpdated> seen;
  auto subscription =
      harness.bus->subscribe(tcu::eventbus::event_type_of<tcu::eventbus::VehicleSignalUpdated>(),
                             [&](const tcu::eventbus::Event& event) {
                               std::lock_guard<std::mutex> lock(mutex);
                               seen.push_back(
                                   std::get<tcu::eventbus::VehicleSignalUpdated>(event.payload));
                             });

  ASSERT_TRUE(harness.service->start().ready);
  harness.transport->pushFrame(frameOf(256, {0x10, 0x27, 0x01, 0, 0, 0, 0, 0}, 1000));

  ASSERT_TRUE(waitFor([&] {
    std::lock_guard<std::mutex> lock(mutex);
    return seen.size() >= 2;  // VehicleSpeed and Ignition
  }));

  // Copy under the lock, then RELEASE it before unsubscribing.
  //
  // Holding this mutex across unsubscribe() is a lock-order inversion, and
  // ThreadSanitizer caught it: the dispatcher holds the subscription's
  // in_flight mutex and then takes this one inside the handler, while
  // unsubscribe() waits for in_flight - so doing it the other way round closes
  // the cycle. The bug was mine, not the bus's; project 01 documents that
  // unsubscribe waits out an in-flight invocation, which is exactly why a
  // caller must not hold a mutex the handler also takes.
  std::vector<tcu::eventbus::VehicleSignalUpdated> collected;
  {
    std::lock_guard<std::mutex> lock(mutex);
    collected = seen;
  }
  subscription.unsubscribe();

  bool sawSpeed = false;
  for (const auto& payload : collected) {
    if (payload.signal_name != "VehicleSpeed") continue;
    sawSpeed = true;
    EXPECT_DOUBLE_EQ(payload.value, 100.0);
    EXPECT_EQ(payload.source_can_id, 256u);
    EXPECT_EQ(payload.generation, 1u) << "the DBC version travels with the value";
  }
  EXPECT_TRUE(sawSpeed);
}

TEST(EventBus_PublishesSignalsDecoded, QualityChangesReachTheBus) {
  BusHarness harness;
  std::atomic<int> qualityEvents{0};
  auto subscription =
      harness.bus->subscribe(tcu::eventbus::event_type_of<tcu::eventbus::SignalQualityChanged>(),
                             [&](const tcu::eventbus::Event&) { ++qualityEvents; });

  ASSERT_TRUE(harness.service->start().ready);
  harness.transport->pushFrame(frameOf(256, {0x10, 0x27, 0x01, 0, 0, 0, 0, 0}, 1000));
  ASSERT_TRUE(waitFor([&] { return qualityEvents.load() >= 1; }))
      << "NotReceived -> Valid is a quality change and must be published";
  subscription.unsubscribe();
}

TEST(EventBus_PublishFailureCounted, AFullBusDoesNotStallTheWorker) {
  // A one-slot bus with no subscriber draining it: publishes will be rejected.
  BusHarness harness(1);
  ASSERT_TRUE(harness.service->start().ready);
  for (int i = 0; i < 300; ++i) {
    harness.transport->pushFrame(frameOf(256, {0x10, 0x27, 0x01, 0, 0, 0, 0, 0}, 1000 + i));
  }
  // Wait on the DECODE, not on the receive: framesReceived is incremented by
  // the reader thread before it enqueues, so it can race ahead of the worker.
  // Waiting on the wrong counter is what made this test flaky the first time.
  ASSERT_TRUE(waitFor([&] {
    const auto snapshot = harness.service->query(SignalKey{0, false, 256, "VehicleSpeed"});
    return snapshot.has_value() && snapshot->lastGood.has_value();
  }));

  // The decoder kept running and the cache is still correct, whatever the bus did.
  EXPECT_EQ(harness.service->state(), tcu::signal::ServiceState::Ready);
  const auto snapshot = harness.service->query(SignalKey{0, false, 256, "VehicleSpeed"});
  ASSERT_TRUE(snapshot.has_value());
  ASSERT_TRUE(snapshot->lastGood.has_value());
  EXPECT_DOUBLE_EQ(snapshot->lastGood->physical, 100.0);
}

TEST(EventBus_NoPublishUnderCacheLock, AdapterMayQueryTheServiceFromItsCallback) {
  // If a publish happened while the cache lock was held, an adapter that reads
  // the service from inside its own callback would deadlock (invariant S-07).
  BusHarness harness;
  ASSERT_TRUE(harness.service->start().ready);

  class ReenteringObserver : public tcu::signal::ISignalObserver {
   public:
    explicit ReenteringObserver(DecoderService& service) : service_(service) {}
    void onBatch(const tcu::dbc::DecodedBatch&) override {
      (void)service_.query(SignalKey{0, false, 256, "VehicleSpeed"});
      (void)service_.queryAll();
      ++completed;
    }
    void onQualityChanged(const SignalKey& key, tcu::dbc::SignalQuality) override {
      (void)service_.query(key);
    }
    std::atomic<int> completed{0};

   private:
    DecoderService& service_;
  };

  auto reentering = std::make_shared<ReenteringObserver>(*harness.service);
  harness.service->addObserver(reentering);
  harness.transport->pushFrame(frameOf(256, {0x10, 0x27, 0x01, 0, 0, 0, 0, 0}, 1000));
  ASSERT_TRUE(waitFor([&] { return reentering->completed.load() >= 1; }))
      << "the observer never returned - something is holding a lock across the callback";
}

}  // namespace

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "tcu/eventbus/event.hpp"
#include "tcu/eventbus/event_bus.hpp"
#include "tcu/eventbus/metrics.hpp"

namespace {

using namespace tcu::eventbus;
using namespace std::chrono_literals;

// Test-local helper. Deliberately not public API: contracts/event_bus_api.md
// fixes the public surface, and publish() validates invariant E2 anyway.
template <typename T>
Event make_event(T payload) {
  Event event{};
  event.type = event_type_of<T>();
  event.created_at = std::chrono::steady_clock::now();
  event.payload = std::move(payload);
  return event;
}

// T011 / invariant T1: event_type_of<T>() is the alternative index, so
// Event::type and Event::payload cannot disagree (research R-01).
static_assert(event_type_of<IgnitionChanged>() == 0);
static_assert(event_type_of<VehicleSpeedUpdated>() == 1);
static_assert(event_type_of<NetworkStatusChanged>() == 2);
static_assert(std::variant_size_v<EventPayload> == 3);

TEST(EventTypeMapping, MatchesVariantIndexAtRuntime) {
  const EventPayload payload = VehicleSpeedUpdated{42.5};
  EXPECT_EQ(static_cast<EventType>(payload.index()), event_type_of<VehicleSpeedUpdated>());
}

// T013 / data-model.md section 4, spec section 17.2: capacity 0 is rejected.
TEST(EventBusConstruction, RejectsZeroCapacity) {
  EXPECT_THROW({ EventBus bus(BusConfig{0}); (void)bus.metrics(); }, std::invalid_argument);
}

TEST(EventBusConstruction, AcceptsPositiveCapacity) {
  EXPECT_NO_THROW({ EventBus bus(BusConfig{1}); (void)bus.metrics(); });
}

TEST(EventBusConstruction, StartsWithZeroedMetrics) {
  EventBus bus(BusConfig{8});
  const MetricsSnapshot metrics = bus.metrics();
  EXPECT_EQ(metrics.accepted_total, 0u);
  EXPECT_EQ(metrics.dispatched_total, 0u);
  EXPECT_EQ(metrics.queue_depth, 0u);
  EXPECT_EQ(metrics.queue_high_watermark, 0u);
}

// ---------------------------------------------------------------------------
// T015 / UT-01: delivery happens off the publisher thread (FR-03, G4).
// ---------------------------------------------------------------------------
TEST(EventBusDelivery, UT01_SingleSubscriberReceivesEventOffPublisherThread) {
  EventBus bus(BusConfig{16});

  std::promise<std::pair<Event, std::thread::id>> received;
  auto future = received.get_future();
  auto subscription =
      bus.subscribe(event_type_of<IgnitionChanged>(), [&received](const Event& event) {
        received.set_value({event, std::this_thread::get_id()});
      });

  bus.start();
  ASSERT_EQ(bus.publish(make_event(IgnitionChanged{true})), PublishStatus::Accepted);
  ASSERT_EQ(future.wait_for(5s), std::future_status::ready);

  const std::pair<Event, std::thread::id> result = future.get();
  EXPECT_EQ(result.first.type, event_type_of<IgnitionChanged>());
  EXPECT_TRUE(std::get<IgnitionChanged>(result.first.payload).on);
  EXPECT_GT(result.first.id, 0u);
  EXPECT_NE(result.second, std::this_thread::get_id());
  bus.stop();
}

// ---------------------------------------------------------------------------
// T016 / UT-02: every subscriber once, in registration order (G9, U4, R-04).
// ---------------------------------------------------------------------------
TEST(EventBusDelivery, UT02_ManySubscribersEachOnceInRegistrationOrder) {
  EventBus bus(BusConfig{16});

  std::mutex mutex;
  std::condition_variable cv;
  std::vector<int> order;

  auto subscribe_tagged = [&](int tag) {
    return bus.subscribe(event_type_of<IgnitionChanged>(), [&, tag](const Event&) {
      {
        std::lock_guard<std::mutex> lock(mutex);
        order.push_back(tag);
      }
      cv.notify_all();
    });
  };

  auto first = subscribe_tagged(1);
  auto second = subscribe_tagged(2);
  auto third = subscribe_tagged(3);

  bus.start();
  ASSERT_EQ(bus.publish(make_event(IgnitionChanged{true})), PublishStatus::Accepted);

  {
    std::unique_lock<std::mutex> lock(mutex);
    ASSERT_TRUE(cv.wait_for(lock, 5s, [&] { return order.size() == 3; }));
  }
  bus.stop();
  EXPECT_EQ(order, std::vector<int>({1, 2, 3}));
}

// ---------------------------------------------------------------------------
// T017 / UT-03: concurrent publishers, accepted == dispatched after Drain (FR-01).
// ---------------------------------------------------------------------------
TEST(EventBusDelivery, UT03_ConcurrentPublishersAcceptedEqualsDispatched) {
  constexpr int kThreads = 4;
  constexpr int kPerThread = 500;

  EventBus bus(BusConfig{4096});
  std::atomic<int> invocations{0};
  auto subscription = bus.subscribe(event_type_of<VehicleSpeedUpdated>(),
                                    [&invocations](const Event&) { invocations.fetch_add(1); });
  bus.start();

  std::atomic<int> accepted{0};
  std::vector<std::thread> publishers;
  publishers.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    publishers.emplace_back([&] {
      for (int i = 0; i < kPerThread; ++i) {
        if (bus.publish(make_event(VehicleSpeedUpdated{static_cast<double>(i)})) ==
            PublishStatus::Accepted) {
          accepted.fetch_add(1);
        }
      }
    });
  }
  for (std::thread& publisher : publishers) {
    publisher.join();
  }
  bus.stop(StopMode::Drain);

  const MetricsSnapshot metrics = bus.metrics();
  EXPECT_EQ(accepted.load(), kThreads * kPerThread);
  EXPECT_EQ(metrics.accepted_total, static_cast<std::uint64_t>(accepted.load()));
  EXPECT_EQ(metrics.dispatched_total, metrics.accepted_total);
  EXPECT_EQ(invocations.load(), kThreads * kPerThread);
  EXPECT_EQ(metrics.queue_depth, 0u);
}

// ---------------------------------------------------------------------------
// T018 / UT-13: the payload outlives its producer (P1, NFR-04).
// ---------------------------------------------------------------------------
TEST(EventBusDelivery, UT13_PayloadOutlivesItsProducer) {
  EventBus bus(BusConfig{8});

  std::promise<double> received;
  auto future = received.get_future();
  auto subscription =
      bus.subscribe(event_type_of<VehicleSpeedUpdated>(), [&received](const Event& event) {
        received.set_value(std::get<VehicleSpeedUpdated>(event.payload).kmh);
      });
  bus.start();

  {
    const VehicleSpeedUpdated local{88.5};
    ASSERT_EQ(bus.publish(make_event(local)), PublishStatus::Accepted);
  }  // local is gone before the handler runs

  ASSERT_EQ(future.wait_for(5s), std::future_status::ready);
  EXPECT_DOUBLE_EQ(future.get(), 88.5);
  bus.stop();
}

// ---------------------------------------------------------------------------
// T019 / UT-14: Stopped before start and after stop (research R-02).
// ---------------------------------------------------------------------------
TEST(EventBusLifecycle, UT14_PublishBeforeStartAndAfterStopReturnsStopped) {
  EventBus bus(BusConfig{4});
  EXPECT_EQ(bus.publish(make_event(IgnitionChanged{true})), PublishStatus::Stopped);
  bus.start();
  EXPECT_EQ(bus.publish(make_event(IgnitionChanged{true})), PublishStatus::Accepted);
  bus.stop();
  EXPECT_EQ(bus.publish(make_event(IgnitionChanged{false})), PublishStatus::Stopped);
}

// Invariant E2: a type that disagrees with its payload is InvalidArgument.
TEST(EventBusLifecycle, MismatchedTypeAndPayloadIsInvalidArgument) {
  EventBus bus(BusConfig{4});
  bus.start();
  Event event = make_event(IgnitionChanged{true});
  event.type = event_type_of<NetworkStatusChanged>();  // lie about the type
  EXPECT_EQ(bus.publish(event), PublishStatus::InvalidArgument);
  bus.stop();
  EXPECT_EQ(bus.metrics().accepted_total, 0u);
}

// start() is valid only from Created (research R-08, invariant S2).
TEST(EventBusLifecycle, SecondStartThrowsAndCreatesNoSecondWorker) {
  EventBus bus(BusConfig{4});
  bus.start();
  EXPECT_THROW(bus.start(), std::logic_error);
  bus.stop();
}

// ---------------------------------------------------------------------------
// T020 / B01: normal delivery of VehicleSpeedUpdated 42.5.
// ---------------------------------------------------------------------------
TEST(Behaviour, B01_NormalDelivery) {
  EventBus bus(BusConfig{8});

  std::atomic<int> invocations{0};
  std::promise<double> received;
  auto future = received.get_future();
  auto subscription =
      bus.subscribe(event_type_of<VehicleSpeedUpdated>(), [&](const Event& event) {
        invocations.fetch_add(1);
        received.set_value(std::get<VehicleSpeedUpdated>(event.payload).kmh);
      });
  bus.start();
  ASSERT_EQ(bus.publish(make_event(VehicleSpeedUpdated{42.5})), PublishStatus::Accepted);
  ASSERT_EQ(future.wait_for(5s), std::future_status::ready);
  EXPECT_DOUBLE_EQ(future.get(), 42.5);

  bus.stop(StopMode::Drain);
  EXPECT_EQ(invocations.load(), 1);
  EXPECT_EQ(bus.metrics().dispatched_total, 1u);
}

// ---------------------------------------------------------------------------
// T021 / B02: no subscriber, but the event is still consumed and counted.
// ---------------------------------------------------------------------------
TEST(Behaviour, B02_NoSubscriberStillDispatches) {
  EventBus bus(BusConfig{4});
  bus.start();
  ASSERT_EQ(bus.publish(make_event(NetworkStatusChanged{true})), PublishStatus::Accepted);
  bus.stop(StopMode::Drain);

  const MetricsSnapshot metrics = bus.metrics();
  EXPECT_EQ(metrics.accepted_total, 1u);
  EXPECT_EQ(metrics.dispatched_total, 1u);
  EXPECT_EQ(metrics.callback_invocations_total, 0u);
}

// A destroyed Subscription stops delivery (invariant U1).
TEST(Behaviour, DestroyedSubscriptionStopsDelivery) {
  EventBus bus(BusConfig{8});
  std::atomic<int> invocations{0};
  {
    auto subscription = bus.subscribe(event_type_of<IgnitionChanged>(),
                                      [&invocations](const Event&) { invocations.fetch_add(1); });
  }
  bus.start();
  ASSERT_EQ(bus.publish(make_event(IgnitionChanged{true})), PublishStatus::Accepted);
  bus.stop(StopMode::Drain);
  EXPECT_EQ(invocations.load(), 0);
  EXPECT_EQ(bus.metrics().dispatched_total, 1u);
}

}  // namespace

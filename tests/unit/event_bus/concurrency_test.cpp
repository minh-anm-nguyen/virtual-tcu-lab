#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <utility>
#include <vector>

#include "tcu/eventbus/event.hpp"
#include "tcu/eventbus/event_bus.hpp"
#include "tcu/eventbus/metrics.hpp"

namespace {

using namespace tcu::eventbus;
using namespace std::chrono_literals;

constexpr int kPublisherThreads = 8;
constexpr int kEventsPerThread = 1000;

template <typename T>
Event make_event(T payload) {
  Event event{};
  event.type = event_type_of<T>();
  event.created_at = std::chrono::steady_clock::now();
  event.payload = std::move(payload);
  return event;
}

PublishOptions block_options(std::chrono::milliseconds timeout) {
  PublishOptions options;
  options.on_full = FullQueuePolicy::BlockUntil;
  options.timeout = timeout;
  return options;
}

struct StressResult {
  MetricsSnapshot metrics;
  std::vector<std::uint64_t> observed_ids;
  int accepted = 0;
};

// Runs the stress load and stops with the given mode.
StressResult run_stress(StopMode mode) {
  EventBus bus(BusConfig{1024});

  // Written only by the single dispatcher thread and read after the worker has
  // been joined, so the vector itself needs no synchronization.
  std::vector<std::uint64_t> observed_ids;
  observed_ids.reserve(kPublisherThreads * kEventsPerThread);

  auto subscription = bus.subscribe(
      event_type_of<VehicleSpeedUpdated>(),
      [&observed_ids](const Event& event) { observed_ids.push_back(event.id); });
  bus.start();

  std::atomic<int> accepted{0};
  std::vector<std::thread> publishers;
  publishers.reserve(kPublisherThreads);
  for (int thread_index = 0; thread_index < kPublisherThreads; ++thread_index) {
    publishers.emplace_back([&bus, &accepted, thread_index] {
      for (int i = 0; i < kEventsPerThread; ++i) {
        // A per-thread sequence id in the payload, and a generous deadline so
        // backpressure delays a publisher rather than dropping its event.
        const double sequence = static_cast<double>(thread_index * kEventsPerThread + i);
        if (bus.publish(make_event(VehicleSpeedUpdated{sequence}), block_options(30s)) ==
            PublishStatus::Accepted) {
          accepted.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
  }
  for (std::thread& publisher : publishers) {
    publisher.join();
  }

  bus.stop(mode);

  StressResult result;
  result.metrics = bus.metrics();
  result.observed_ids = std::move(observed_ids);
  result.accepted = accepted.load();
  return result;
}

// ---------------------------------------------------------------------------
// T066 / B09: 8 publishers, 1000 events each. accepted == dispatched after
// Drain, and FIFO holds by enqueue order - asserted on the monotonic
// Event::id (invariant E3), never on wall-clock order between publishers
// (invariant Q2).
// ---------------------------------------------------------------------------
TEST(Concurrency, B09_ConcurrentPublishersPreserveFifoByEnqueueOrder) {
  const StressResult result = run_stress(StopMode::Drain);

  EXPECT_EQ(result.accepted, kPublisherThreads * kEventsPerThread);
  EXPECT_EQ(result.metrics.accepted_total, static_cast<std::uint64_t>(result.accepted));
  EXPECT_EQ(result.metrics.dispatched_total, result.metrics.accepted_total);
  EXPECT_EQ(result.metrics.discarded_on_stop_total, 0u);
  EXPECT_EQ(result.metrics.queue_depth, 0u);
  EXPECT_LE(result.metrics.queue_high_watermark, 1024u);  // invariant M1

  ASSERT_EQ(result.observed_ids.size(), static_cast<std::size_t>(result.accepted));
  for (std::size_t i = 1; i < result.observed_ids.size(); ++i) {
    ASSERT_LT(result.observed_ids[i - 1], result.observed_ids[i]) << "FIFO violated at index " << i;
  }
}

// ---------------------------------------------------------------------------
// T067: the conservation equations of contracts/metrics.md, under both stop
// modes, plus the relation that must NOT be assumed.
// ---------------------------------------------------------------------------
TEST(Concurrency, DrainConservationEquationHolds) {
  const StressResult result = run_stress(StopMode::Drain);

  EXPECT_EQ(result.metrics.accepted_total, result.metrics.dispatched_total);
  EXPECT_EQ(result.metrics.discarded_on_stop_total, 0u);
  EXPECT_LE(result.metrics.callback_failures_total, result.metrics.callback_invocations_total);
}

TEST(Concurrency, DiscardConservationEquationHolds) {
  const StressResult result = run_stress(StopMode::Discard);

  EXPECT_EQ(result.metrics.accepted_total,
            result.metrics.dispatched_total + result.metrics.discarded_on_stop_total);
  EXPECT_EQ(result.metrics.queue_depth, 0u);
  EXPECT_LE(result.metrics.callback_failures_total, result.metrics.callback_invocations_total);
}

// callback_invocations_total has no fixed relation to dispatched_total: larger
// with several subscribers, smaller with none (contracts/metrics.md).
TEST(Concurrency, CallbackInvocationsAreNotTiedToDispatchedCount) {
  EventBus bus(BusConfig{64});
  auto first = bus.subscribe(event_type_of<IgnitionChanged>(), [](const Event&) {});
  auto second = bus.subscribe(event_type_of<IgnitionChanged>(), [](const Event&) {});
  bus.start();

  for (int i = 0; i < 10; ++i) {
    ASSERT_EQ(bus.publish(make_event(IgnitionChanged{true})), PublishStatus::Accepted);
  }
  for (int i = 0; i < 10; ++i) {
    ASSERT_EQ(bus.publish(make_event(NetworkStatusChanged{true})), PublishStatus::Accepted);
  }
  bus.stop(StopMode::Drain);

  const MetricsSnapshot metrics = bus.metrics();
  EXPECT_EQ(metrics.dispatched_total, 20u);
  EXPECT_EQ(metrics.callback_invocations_total, 20u);  // 10 events x 2 subscribers
  EXPECT_EQ(metrics.accepted_total, metrics.dispatched_total);
}

// Subscribe and unsubscribe churn racing against dispatch: the case the
// per-subscription admission control of research R-13 exists for.
TEST(Concurrency, SubscribeUnsubscribeChurnDuringDispatch) {
  EventBus bus(BusConfig{256});
  std::atomic<bool> stop_churn{false};
  std::atomic<int> churn_calls{0};

  auto stable = bus.subscribe(event_type_of<IgnitionChanged>(), [](const Event&) {});
  bus.start();

  std::thread churner([&] {
    while (!stop_churn.load(std::memory_order_relaxed)) {
      Subscription temporary =
          bus.subscribe(event_type_of<IgnitionChanged>(), [&churn_calls](const Event&) {
            churn_calls.fetch_add(1, std::memory_order_relaxed);
          });
      temporary.unsubscribe();
    }
  });

  std::thread publisher([&] {
    for (int i = 0; i < 2000; ++i) {
      bus.publish(make_event(IgnitionChanged{true}), block_options(30s));
    }
  });

  publisher.join();
  stop_churn.store(true, std::memory_order_relaxed);
  churner.join();
  bus.stop(StopMode::Drain);

  const MetricsSnapshot metrics = bus.metrics();
  EXPECT_EQ(metrics.accepted_total, metrics.dispatched_total);
  EXPECT_EQ(metrics.callback_failures_total, 0u);
}

}  // namespace

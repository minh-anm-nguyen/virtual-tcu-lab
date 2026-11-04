#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <utility>
#include <vector>

#include "support/test_gate.hpp"
#include "tcu/eventbus/event.hpp"
#include "tcu/eventbus/event_bus.hpp"
#include "tcu/eventbus/metrics.hpp"

namespace {

using namespace tcu::eventbus;
using namespace std::chrono_literals;

template <typename T>
Event make_event(T payload) {
  Event event{};
  event.type = event_type_of<T>();
  event.created_at = std::chrono::steady_clock::now();
  event.payload = std::move(payload);
  return event;
}

// Spins on a condition with a bounded deadline. Not a sleep for ordering: it
// asserts a state change the code under test must make (spec section 9.1).
template <typename Predicate>
bool wait_until_true(Predicate predicate,
                     std::chrono::milliseconds limit = std::chrono::milliseconds(10000)) {
  const auto deadline = std::chrono::steady_clock::now() + limit;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }
    std::this_thread::yield();
  }
  return predicate();
}

// Parks the worker inside the first callback, then queues two more events, so
// the backlog at shutdown is exactly E2 and E3.
class BacklogFixture {
 public:
  BacklogFixture() : bus_(BusConfig{8}) {
    subscription_ = bus_.subscribe(event_type_of<IgnitionChanged>(), [this](const Event&) {
      delivered_.fetch_add(1);
      if (calls_.fetch_add(1) == 0) {
        gate_.wait_until_opened();
      }
    });
    bus_.start();

    EXPECT_EQ(bus_.publish(make_event(IgnitionChanged{true})), PublishStatus::Accepted);  // E1
    gate_.wait_until_entered();
    EXPECT_EQ(bus_.publish(make_event(IgnitionChanged{false})), PublishStatus::Accepted);  // E2
    EXPECT_EQ(bus_.publish(make_event(IgnitionChanged{true})), PublishStatus::Accepted);   // E3
    EXPECT_EQ(bus_.metrics().queue_depth, 2u);
  }

  EventBus& bus() { return bus_; }
  void release_worker() { gate_.open(); }
  int delivered() const { return delivered_.load(); }

 private:
  EventBus bus_;
  Subscription subscription_;
  tcu::test::TestGate gate_;
  std::atomic<int> calls_{0};
  std::atomic<int> delivered_{0};
};

// ---------------------------------------------------------------------------
// T047 / UT-10: Drain dispatches the whole backlog before stop() returns.
// ---------------------------------------------------------------------------
TEST(Shutdown, UT10_DrainDispatchesWholeBacklogBeforeReturning) {
  BacklogFixture fixture;

  std::thread stopper([&] { fixture.bus().stop(StopMode::Drain); });
  fixture.release_worker();
  stopper.join();

  const MetricsSnapshot metrics = fixture.bus().metrics();
  EXPECT_EQ(metrics.accepted_total, 3u);
  EXPECT_EQ(metrics.dispatched_total, metrics.accepted_total);  // Drain equation
  EXPECT_EQ(metrics.discarded_on_stop_total, 0u);
  EXPECT_EQ(metrics.queue_depth, 0u);
  EXPECT_EQ(fixture.delivered(), 3);
}

// T051 / B07: including an event with no subscriber, handled as a no-op.
TEST(Behaviour, B07_StopDrainIncludingAnEventWithNoSubscriber) {
  EventBus bus(BusConfig{8});
  tcu::test::TestGate gate;
  std::atomic<int> calls{0};

  auto subscription = bus.subscribe(event_type_of<IgnitionChanged>(), [&](const Event&) {
    if (calls.fetch_add(1) == 0) {
      gate.wait_until_opened();
    }
  });
  bus.start();

  ASSERT_EQ(bus.publish(make_event(IgnitionChanged{true})), PublishStatus::Accepted);
  gate.wait_until_entered();
  ASSERT_EQ(bus.publish(make_event(IgnitionChanged{false})), PublishStatus::Accepted);
  ASSERT_EQ(bus.publish(make_event(NetworkStatusChanged{true})), PublishStatus::Accepted);

  std::thread stopper([&] { bus.stop(StopMode::Drain); });
  gate.open();
  stopper.join();

  const MetricsSnapshot metrics = bus.metrics();
  EXPECT_EQ(metrics.accepted_total, 3u);
  EXPECT_EQ(metrics.dispatched_total, 3u);
  EXPECT_EQ(metrics.callback_invocations_total, 2u);  // the third event had none
  EXPECT_EQ(calls.load(), 2);
}

// ---------------------------------------------------------------------------
// T048 / UT-11: Discard drops the queued events and counts them.
// ---------------------------------------------------------------------------
TEST(Shutdown, UT11_DiscardDropsBacklogAndSatisfiesItsEquation) {
  BacklogFixture fixture;

  std::thread stopper([&] { fixture.bus().stop(StopMode::Discard); });

  // Wait until the queue has actually been cleared before releasing the
  // worker, otherwise it could pop E2 first and the drop count would be racy.
  // EXPECT, not ASSERT: a failure here must not skip release_worker() and the
  // join below, or the still-joinable stopper thread would abort the binary.
  EXPECT_TRUE(
      wait_until_true([&] { return fixture.bus().metrics().discarded_on_stop_total == 2u; }));

  fixture.release_worker();
  stopper.join();

  const MetricsSnapshot metrics = fixture.bus().metrics();
  EXPECT_EQ(metrics.accepted_total, 3u);
  EXPECT_EQ(metrics.discarded_on_stop_total, 2u);
  // Discard equation from contracts/metrics.md.
  EXPECT_EQ(metrics.accepted_total, metrics.dispatched_total + metrics.discarded_on_stop_total);
  EXPECT_EQ(metrics.queue_depth, 0u);
  EXPECT_EQ(fixture.delivered(), 1);  // only E1, which was already running
}

// T052 / B08: exactly two discarded, and E1 was allowed to finish.
TEST(Behaviour, B08_StopDiscardCountsExactlyTwo) {
  BacklogFixture fixture;

  std::thread stopper([&] { fixture.bus().stop(StopMode::Discard); });
  // EXPECT, not ASSERT: a failure here must not skip release_worker() and the
  // join below, or the still-joinable stopper thread would abort the binary.
  EXPECT_TRUE(
      wait_until_true([&] { return fixture.bus().metrics().discarded_on_stop_total == 2u; }));
  fixture.release_worker();
  stopper.join();

  EXPECT_EQ(fixture.bus().metrics().discarded_on_stop_total, 2u);
  EXPECT_EQ(fixture.delivered(), 1);
}

// ---------------------------------------------------------------------------
// T049 / UT-12: stop() twice, and concurrent callers, with no double join.
// ---------------------------------------------------------------------------
TEST(Shutdown, UT12_StopTwiceIsIdempotent) {
  EventBus bus(BusConfig{4});
  bus.start();
  ASSERT_EQ(bus.publish(make_event(IgnitionChanged{true})), PublishStatus::Accepted);

  EXPECT_NO_THROW(bus.stop(StopMode::Drain));
  EXPECT_NO_THROW(bus.stop(StopMode::Drain));
  EXPECT_NO_THROW(bus.stop(StopMode::Discard));
  EXPECT_EQ(bus.metrics().dispatched_total, 1u);
}

TEST(Shutdown, UT12_StopOnANeverStartedBusIsSafe) {
  EventBus bus(BusConfig{4});
  EXPECT_NO_THROW(bus.stop(StopMode::Drain));
  EXPECT_EQ(bus.publish(make_event(IgnitionChanged{true})), PublishStatus::Stopped);
}

// Research R-09: a concurrent caller must not return before the state is
// Stopped, so the second caller observes the completed Drain.
TEST(Shutdown, UT12_ConcurrentStopCallersAllWaitForStopped) {
  BacklogFixture fixture;

  std::thread first([&] { fixture.bus().stop(StopMode::Drain); });

  // Once publish is refused, the first caller has passed the Running ->
  // Stopping transition (invariant S1), so the second caller takes the
  // wait-for-Stopped path rather than starting its own shutdown.
  // The only side-effect-free-enough probe for the private state transition is
  // publish itself, and a probe that lands while the bus is still Running is a
  // real enqueue. Probing with a type nobody subscribed to keeps the extra
  // events no-ops, and the assertions below compare against accepted_total
  // instead of a hard-coded 3, so the probe cannot distort the verdict.
  ASSERT_TRUE(wait_until_true([&] {
    return fixture.bus().publish(make_event(NetworkStatusChanged{true})) ==
           PublishStatus::Stopped;
  }));

  std::atomic<std::uint64_t> observed_by_second{0};
  std::thread second([&] {
    fixture.bus().stop(StopMode::Drain);
    observed_by_second.store(fixture.bus().metrics().dispatched_total);
  });

  fixture.release_worker();
  first.join();
  second.join();

  const MetricsSnapshot metrics = fixture.bus().metrics();
  EXPECT_EQ(metrics.dispatched_total, metrics.accepted_total);  // Drain equation
  EXPECT_EQ(metrics.queue_depth, 0u);
  EXPECT_EQ(fixture.delivered(), 3);  // the three IgnitionChanged events
  // Had the second caller returned before the drain finished, it would have
  // observed fewer than every accepted event.
  EXPECT_EQ(observed_by_second.load(), metrics.accepted_total);
}

TEST(Shutdown, ManyConcurrentStopCallersDoNotCrash) {
  EventBus bus(BusConfig{64});
  auto subscription = bus.subscribe(event_type_of<IgnitionChanged>(), [](const Event&) {});
  bus.start();
  for (int i = 0; i < 32; ++i) {
    ASSERT_EQ(bus.publish(make_event(IgnitionChanged{true})), PublishStatus::Accepted);
  }

  std::vector<std::thread> stoppers;
  stoppers.reserve(8);
  for (int i = 0; i < 8; ++i) {
    stoppers.emplace_back([&] { bus.stop(StopMode::Drain); });
  }
  for (std::thread& stopper : stoppers) {
    stopper.join();
  }

  const MetricsSnapshot metrics = bus.metrics();
  EXPECT_EQ(metrics.accepted_total, 32u);
  EXPECT_EQ(metrics.dispatched_total, 32u);
  EXPECT_EQ(metrics.queue_depth, 0u);
}

}  // namespace

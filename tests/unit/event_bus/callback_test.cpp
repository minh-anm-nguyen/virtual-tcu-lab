#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <stdexcept>
#include <utility>

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

PublishOptions block_options(std::chrono::milliseconds timeout) {
  PublishOptions options;
  options.on_full = FullQueuePolicy::BlockUntil;
  options.timeout = timeout;
  return options;
}

// ---------------------------------------------------------------------------
// T063 / UT-07: a throwing handler is caught, counted, and the worker lives on.
// ---------------------------------------------------------------------------
TEST(Callback, UT07_ThrowingHandlerIsCaughtAndCounted) {
  EventBus bus(BusConfig{16});
  std::atomic<int> calls{0};
  std::promise<void> second_seen;
  auto second_seen_future = second_seen.get_future();

  auto subscription = bus.subscribe(event_type_of<IgnitionChanged>(), [&](const Event&) {
    const int call = calls.fetch_add(1);
    if (call == 0) {
      throw std::runtime_error("deliberate handler failure");
    }
    if (call == 1) {
      second_seen.set_value();
    }
  });

  bus.start();
  ASSERT_EQ(bus.publish(make_event(IgnitionChanged{true})), PublishStatus::Accepted);
  ASSERT_EQ(bus.publish(make_event(IgnitionChanged{false})), PublishStatus::Accepted);

  // The worker survived the exception and kept dispatching.
  ASSERT_EQ(second_seen_future.wait_for(10s), std::future_status::ready);
  bus.stop(StopMode::Drain);

  const MetricsSnapshot metrics = bus.metrics();
  EXPECT_EQ(metrics.callback_failures_total, 1u);
  EXPECT_EQ(metrics.callback_invocations_total, 2u);
  EXPECT_EQ(metrics.dispatched_total, 2u);  // a thrown handler still completes the round
  EXPECT_EQ(metrics.accepted_total, metrics.dispatched_total);
}

// ---------------------------------------------------------------------------
// T064 / UT-15: re-entrant publish from inside a handler.
// ---------------------------------------------------------------------------
TEST(Callback, UT15_ReentrantRejectPublishFromHandlerIsAllowed) {
  EventBus bus(BusConfig{16});
  std::atomic<int> calls{0};
  std::atomic<int> reentrant_accepted{0};
  std::promise<void> done;
  auto done_future = done.get_future();

  auto subscription = bus.subscribe(event_type_of<IgnitionChanged>(), [&](const Event&) {
    if (calls.fetch_add(1) == 0) {
      // Reject from inside the handler is supported (guarantee G5).
      const PublishStatus status = bus.publish(make_event(IgnitionChanged{false}));
      EXPECT_TRUE(status == PublishStatus::Accepted || status == PublishStatus::QueueFull);
      if (status == PublishStatus::Accepted) {
        reentrant_accepted.fetch_add(1);
      }
    } else {
      done.set_value();
    }
  });

  bus.start();
  ASSERT_EQ(bus.publish(make_event(IgnitionChanged{true})), PublishStatus::Accepted);
  ASSERT_EQ(done_future.wait_for(10s), std::future_status::ready);
  bus.stop(StopMode::Drain);

  EXPECT_EQ(reentrant_accepted.load(), 1);
  EXPECT_EQ(calls.load(), 2);  // the re-entrant event was processed after the current one
}

// Research R-10: BlockUntil from the dispatcher thread must be refused at once,
// because the only thread that could free a slot is the one that would wait.
TEST(Callback, UT15_ReentrantBlockUntilFromDispatcherIsInvalidArgument) {
  EventBus bus(BusConfig{1});
  std::promise<PublishStatus> reentrant;
  auto reentrant_future = reentrant.get_future();
  std::atomic<int> calls{0};

  auto subscription = bus.subscribe(event_type_of<IgnitionChanged>(), [&](const Event&) {
    if (calls.fetch_add(1) == 0) {
      reentrant.set_value(bus.publish(make_event(IgnitionChanged{false}), block_options(30s)));
    }
  });

  bus.start();
  ASSERT_EQ(bus.publish(make_event(IgnitionChanged{true})), PublishStatus::Accepted);

  // A 30s deadline means a hang would be visible; InvalidArgument must come
  // back immediately instead.
  ASSERT_EQ(reentrant_future.wait_for(10s), std::future_status::ready);
  EXPECT_EQ(reentrant_future.get(), PublishStatus::InvalidArgument);

  bus.stop(StopMode::Drain);
  EXPECT_EQ(bus.metrics().publish_timeout_total, 0u);  // not a timeout
}

// ---------------------------------------------------------------------------
// T065 / B06: handler A throws, handler B still receives the event, and
// callback_failures_total increments by exactly 1.
// ---------------------------------------------------------------------------
TEST(Behaviour, B06_CallbackThrowDoesNotStarveTheOtherSubscriber) {
  EventBus bus(BusConfig{8});
  std::atomic<int> b_calls{0};
  std::promise<void> b_called;
  auto b_called_future = b_called.get_future();

  auto a = bus.subscribe(event_type_of<IgnitionChanged>(),
                         [](const Event&) { throw std::runtime_error("A always fails"); });
  auto b = bus.subscribe(event_type_of<IgnitionChanged>(), [&](const Event&) {
    b_calls.fetch_add(1);
    b_called.set_value();
  });

  bus.start();
  ASSERT_EQ(bus.publish(make_event(IgnitionChanged{true})), PublishStatus::Accepted);
  ASSERT_EQ(b_called_future.wait_for(10s), std::future_status::ready);
  bus.stop(StopMode::Drain);

  const MetricsSnapshot metrics = bus.metrics();
  EXPECT_EQ(b_calls.load(), 1);
  EXPECT_EQ(metrics.callback_failures_total, 1u);
  EXPECT_EQ(metrics.callback_invocations_total, 2u);
  EXPECT_EQ(metrics.dispatched_total, 1u);
}

// A non-std exception must also be isolated: the dispatcher catches everything.
TEST(Callback, NonStandardExceptionIsAlsoIsolated) {
  EventBus bus(BusConfig{8});
  std::atomic<int> calls{0};
  std::promise<void> second_seen;
  auto second_seen_future = second_seen.get_future();

  auto subscription = bus.subscribe(event_type_of<IgnitionChanged>(), [&](const Event&) {
    if (calls.fetch_add(1) == 0) {
      throw 42;  // not derived from std::exception
    }
    second_seen.set_value();
  });

  bus.start();
  ASSERT_EQ(bus.publish(make_event(IgnitionChanged{true})), PublishStatus::Accepted);
  ASSERT_EQ(bus.publish(make_event(IgnitionChanged{false})), PublishStatus::Accepted);
  ASSERT_EQ(second_seen_future.wait_for(10s), std::future_status::ready);
  bus.stop(StopMode::Drain);

  EXPECT_EQ(bus.metrics().callback_failures_total, 1u);
}

}  // namespace

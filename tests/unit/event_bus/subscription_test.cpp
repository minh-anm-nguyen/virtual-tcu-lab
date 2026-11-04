#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <stdexcept>
#include <thread>
#include <utility>

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

// ---------------------------------------------------------------------------
// T045 / UT-08: unsubscribe before publishing means the handler never runs.
// ---------------------------------------------------------------------------
TEST(Subscription, UT08_UnsubscribeBeforePublishNeverInvokesHandler) {
  EventBus bus(BusConfig{8});
  std::atomic<int> calls{0};

  auto subscription = bus.subscribe(event_type_of<IgnitionChanged>(),
                                    [&calls](const Event&) { calls.fetch_add(1); });
  subscription.unsubscribe();

  bus.start();
  ASSERT_EQ(bus.publish(make_event(IgnitionChanged{true})), PublishStatus::Accepted);
  bus.stop(StopMode::Drain);

  EXPECT_EQ(calls.load(), 0);
  EXPECT_EQ(bus.metrics().dispatched_total, 1u);  // consumed as a no-op
}

// ---------------------------------------------------------------------------
// T046 / UT-09: unsubscribe() must wait out an in-flight invocation, start no
// new one, and never deadlock (research R-13).
// ---------------------------------------------------------------------------
TEST(Subscription, UT09_UnsubscribeWaitsOutInFlightCallback) {
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
  gate.wait_until_entered();  // the handler is now running and parked

  std::promise<void> returned;
  auto returned_future = returned.get_future();
  std::thread unsubscriber([&] {
    subscription.unsubscribe();
    returned.set_value();
  });

  // unsubscribe() must still be waiting: the callback has not finished. If it
  // returned early the future would already be ready, so this cannot pass by
  // accident on a slow machine.
  EXPECT_EQ(returned_future.wait_for(200ms), std::future_status::timeout);

  gate.open();
  ASSERT_EQ(returned_future.wait_for(10s), std::future_status::ready);
  unsubscriber.join();

  // No new invocation may start after unsubscribe() returned.
  ASSERT_EQ(bus.publish(make_event(IgnitionChanged{false})), PublishStatus::Accepted);
  bus.stop(StopMode::Drain);
  EXPECT_EQ(calls.load(), 1);
}

// R-13 caveat: unsubscribing from inside the subscription's own callback must
// not self-deadlock on the per-subscription mutex.
TEST(Subscription, UT09_SelfUnsubscribeFromInsideHandlerDoesNotDeadlock) {
  EventBus bus(BusConfig{8});
  std::atomic<Subscription*> self{nullptr};
  std::atomic<int> calls{0};
  std::promise<void> first_call;
  auto first_call_future = first_call.get_future();

  Subscription subscription = bus.subscribe(event_type_of<IgnitionChanged>(), [&](const Event&) {
    calls.fetch_add(1);
    if (Subscription* handle = self.load()) {
      handle->unsubscribe();  // must return, not hang
    }
    first_call.set_value();
  });
  self.store(&subscription);

  bus.start();
  ASSERT_EQ(bus.publish(make_event(IgnitionChanged{true})), PublishStatus::Accepted);
  ASSERT_EQ(first_call_future.wait_for(10s), std::future_status::ready);

  ASSERT_EQ(bus.publish(make_event(IgnitionChanged{false})), PublishStatus::Accepted);
  bus.stop(StopMode::Drain);
  EXPECT_EQ(calls.load(), 1);  // the second event found no active subscription
}

// ---------------------------------------------------------------------------
// T050 / B05: after unsubscribe returns, no new invocation of A begins; one
// already started returns normally; no crash, no deadlock.
// ---------------------------------------------------------------------------
TEST(Behaviour, B05_UnsubscribeConcurrentWithDispatch) {
  EventBus bus(BusConfig{16});
  tcu::test::TestGate gate;
  std::atomic<int> a_calls{0};
  std::atomic<int> b_calls{0};
  std::atomic<bool> a_completed{false};

  auto a = bus.subscribe(event_type_of<IgnitionChanged>(), [&](const Event&) {
    if (a_calls.fetch_add(1) == 0) {
      gate.wait_until_opened();
      a_completed.store(true);  // the in-flight invocation ran to completion
    }
  });
  auto b =
      bus.subscribe(event_type_of<IgnitionChanged>(), [&](const Event&) { b_calls.fetch_add(1); });

  bus.start();
  ASSERT_EQ(bus.publish(make_event(IgnitionChanged{true})), PublishStatus::Accepted);
  gate.wait_until_entered();

  std::thread unsubscriber([&] { a.unsubscribe(); });
  gate.open();
  unsubscriber.join();

  ASSERT_EQ(bus.publish(make_event(IgnitionChanged{false})), PublishStatus::Accepted);
  bus.stop(StopMode::Drain);

  EXPECT_TRUE(a_completed.load());
  EXPECT_EQ(a_calls.load(), 1);  // no new A invocation
  EXPECT_EQ(b_calls.load(), 2);  // B is untouched by A's unsubscribe
}

// ---------------------------------------------------------------------------
// T053: a Subscription handle outliving its EventBus is inert, not dangling
// (invariant U3, guarantee G15). Must be clean under ASan.
// ---------------------------------------------------------------------------
TEST(Subscription, OutlivingTheBusIsInertNotDangling) {
  Subscription subscription;
  {
    EventBus bus(BusConfig{4});
    subscription = bus.subscribe(event_type_of<IgnitionChanged>(), [](const Event&) {});
    bus.start();
    bus.stop(StopMode::Drain);
  }  // the bus, and the registry that owned the state, are gone

  EXPECT_NO_THROW(subscription.unsubscribe());
  EXPECT_NO_THROW(subscription.unsubscribe());  // idempotent (invariant U2)
}

// ---------------------------------------------------------------------------
// T054 / spec section 17.2: no new subscription once shutdown has begun.
// ---------------------------------------------------------------------------
TEST(Subscription, SubscribeAfterStopThrowsLogicError) {
  EventBus bus(BusConfig{4});
  bus.start();
  bus.stop(StopMode::Drain);
  EXPECT_THROW(bus.subscribe(event_type_of<IgnitionChanged>(), [](const Event&) {}),
               std::logic_error);
}

TEST(Subscription, SubscribeWithEmptyHandlerThrowsInvalidArgument) {
  EventBus bus(BusConfig{4});
  EXPECT_THROW(bus.subscribe(event_type_of<IgnitionChanged>(), Handler{}), std::invalid_argument);
}

// Invariant U1: move-assignment drops the previous registration.
TEST(Subscription, MoveAssignmentUnsubscribesThePreviousRegistration) {
  EventBus bus(BusConfig{8});
  std::atomic<int> first_calls{0};
  std::atomic<int> second_calls{0};

  Subscription handle = bus.subscribe(event_type_of<IgnitionChanged>(),
                                      [&first_calls](const Event&) { first_calls.fetch_add(1); });
  handle = bus.subscribe(event_type_of<IgnitionChanged>(),
                         [&second_calls](const Event&) { second_calls.fetch_add(1); });

  bus.start();
  ASSERT_EQ(bus.publish(make_event(IgnitionChanged{true})), PublishStatus::Accepted);
  bus.stop(StopMode::Drain);

  EXPECT_EQ(first_calls.load(), 0);
  EXPECT_EQ(second_calls.load(), 1);
}

}  // namespace

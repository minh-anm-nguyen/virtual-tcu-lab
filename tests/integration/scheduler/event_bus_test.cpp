// IT-05 and FR-12: a scheduler timer drives project 01 Event Bus traffic.
//
// The integration is a callback and nothing more: no scheduler code knows about
// events, and no event-bus code knows about timers (Principle II, spec
// section 16).

#include <gtest/gtest.h>

#include <atomic>
#include <thread>

#include "support/fixture.hpp"
#include "support/recorder.hpp"
#include "support/test_gate.hpp"
#include "tcu/eventbus/event.hpp"
#include "tcu/eventbus/event_bus.hpp"
#include "tcu/scheduler/timer_scheduler.hpp"

using namespace std::chrono_literals;
using tcu::eventbus::BusConfig;
using tcu::eventbus::Event;
using tcu::eventbus::EventBus;
using tcu::eventbus::event_type_of;
using tcu::eventbus::IgnitionChanged;
using tcu::eventbus::PublishStatus;
using tcu::scheduler::Config;
using tcu::scheduler::Error;
using tcu::scheduler::PeriodicMode;
using tcu::scheduler::StopMode;
using tcu::scheduler::TimerScheduler;
using tcu::test::Recorder;

namespace {

Event heartbeat(bool on) {
  Event e;
  e.type = event_type_of<IgnitionChanged>();
  e.created_at = std::chrono::steady_clock::now();
  e.payload = IgnitionChanged{on};
  return e;
}

}  // namespace

TEST(EventBusHeartbeat, SubscriberReceivesTicks) {  // IT-05, FR-12
  EventBus bus(BusConfig{});
  bus.start();
  Recorder received;
  auto sub = bus.subscribe(event_type_of<IgnitionChanged>(),
                           [&received](const Event&) { received.record(); });

  TimerScheduler s(Config{1, 16, 8});
  ASSERT_EQ(s.start(), Error::None);
  std::atomic<int> rejected{0};
  ASSERT_TRUE(s.scheduleEvery(1ms, PeriodicMode::FixedRateSkipMissed, [&] {
    // The callback checks the publish result: backpressure on the bus is
    // handled, never ignored (spec section 16).
    if (bus.publish(heartbeat(true)) != PublishStatus::Accepted) ++rejected;
  }));

  received.waitFor(5);
  s.stop(StopMode::Discard);
  bus.stop(tcu::eventbus::StopMode::Drain);

  EXPECT_GE(received.count(), 5u);
  EXPECT_EQ(rejected.load(), 0) << "a 1024-deep bus should not reject a 1ms heartbeat";
}

TEST(EventBusHeartbeat, PublishDoesNotRunOnTheCoordinatorThread) {
  // Spec section 16: events are not dispatched on the coordinator thread. The
  // publishing thread must be a scheduler worker, and the delivering thread the
  // bus dispatcher - three different threads.
  EventBus bus(BusConfig{});
  bus.start();

  std::atomic<bool> published_on_distinct_thread{false};
  std::thread::id publisher{};
  Recorder received;
  auto sub = bus.subscribe(event_type_of<IgnitionChanged>(),
                           [&](const Event&) {
                             published_on_distinct_thread =
                                 std::this_thread::get_id() != publisher;
                             received.record();
                           });

  TimerScheduler s(Config{1, 16, 8});
  ASSERT_EQ(s.start(), Error::None);
  ASSERT_TRUE(s.scheduleAfter(0ms, [&] {
    publisher = std::this_thread::get_id();
    ASSERT_EQ(bus.publish(heartbeat(false)), PublishStatus::Accepted);
  }));

  received.waitFor(1);
  s.stop(StopMode::Drain);
  bus.stop(tcu::eventbus::StopMode::Drain);

  EXPECT_NE(publisher, std::this_thread::get_id())
      << "the callback ran on a worker, not on the caller";
  EXPECT_TRUE(published_on_distinct_thread.load())
      << "the bus delivered on its own thread, not on the scheduler worker";
}

TEST(EventBusHeartbeat, BusBackpressureIsVisibleToTheCallback) {
  // A one-slot bus whose subscriber is held fills up, and the timer callback
  // sees the rejection instead of losing the event silently.
  BusConfig cfg;
  cfg.queue_capacity = 1;
  EventBus bus(cfg);
  bus.start();
  tcu::test::TestGate held;
  auto sub = bus.subscribe(event_type_of<IgnitionChanged>(),
                           [&held](const Event&) { held.wait_until_opened(); });

  TimerScheduler s(Config{1, 64, 16});
  ASSERT_EQ(s.start(), Error::None);
  std::atomic<int> rejected{0};
  Recorder attempted;
  ASSERT_TRUE(s.scheduleEvery(1ms, PeriodicMode::FixedDelay, [&] {
    if (bus.publish(heartbeat(true)) != PublishStatus::Accepted) ++rejected;
    attempted.record();
  }));

  ASSERT_TRUE(tcu::test::FakeClockTest::waitForMetric(
      [&rejected] { return static_cast<std::uint64_t>(rejected.load()); }, 1))
      << "the callback must observe the bus refusing an event";

  s.stop(StopMode::Discard);
  held.open();
  bus.stop(tcu::eventbus::StopMode::Discard);
}

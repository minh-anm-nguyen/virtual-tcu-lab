#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
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

// Fills a capacity-1 queue deterministically: the worker is parked inside its
// first callback by the gate, so the single queue slot can be occupied and then
// contended. Spec section 9.1 forbids sleep-based synchronization.
class FullQueueFixture {
 public:
  explicit FullQueueFixture(std::size_t capacity = 1) : bus_(BusConfig{capacity}) {
    subscription_ = bus_.subscribe(event_type_of<IgnitionChanged>(), [this](const Event&) {
      if (calls_.fetch_add(1) == 0) {
        gate_.wait_until_opened();  // park the dispatcher on the first event only
      }
    });
    bus_.start();

    // Event 1 is popped at once and parks the worker inside the handler.
    EXPECT_EQ(bus_.publish(make_event(IgnitionChanged{true})), PublishStatus::Accepted);
    gate_.wait_until_entered();

    // Event 2 now occupies the only queue slot: the queue is full.
    EXPECT_EQ(bus_.publish(make_event(IgnitionChanged{false})), PublishStatus::Accepted);
  }

  ~FullQueueFixture() {
    gate_.open();
    bus_.stop(StopMode::Drain);
  }

  EventBus& bus() { return bus_; }
  void release_worker() { gate_.open(); }

 private:
  EventBus bus_;
  Subscription subscription_;
  tcu::test::TestGate gate_;
  std::atomic<int> calls_{0};
};

PublishOptions reject_options() {
  PublishOptions options;
  options.on_full = FullQueuePolicy::Reject;
  return options;
}

PublishOptions block_options(std::chrono::milliseconds timeout) {
  PublishOptions options;
  options.on_full = FullQueuePolicy::BlockUntil;
  options.timeout = timeout;
  return options;
}

// ---------------------------------------------------------------------------
// T031 / UT-04: Reject on a full queue yields QueueFull, and only that counter
// moves (contracts/metrics.md).
// ---------------------------------------------------------------------------
TEST(Queue, UT04_RejectOnFullQueueReturnsQueueFull) {
  FullQueueFixture fixture;
  const MetricsSnapshot before = fixture.bus().metrics();

  EXPECT_EQ(fixture.bus().publish(make_event(IgnitionChanged{true}), reject_options()),
            PublishStatus::QueueFull);

  const MetricsSnapshot after = fixture.bus().metrics();
  EXPECT_EQ(after.rejected_queue_full_total, before.rejected_queue_full_total + 1);
  EXPECT_EQ(after.publish_timeout_total, before.publish_timeout_total);
  EXPECT_EQ(after.accepted_total, before.accepted_total);
  EXPECT_LE(after.queue_depth, 1u);  // invariant Q1: never above capacity
}

// ---------------------------------------------------------------------------
// T032 / UT-05: BlockUntil with a deadline and no freed slot yields TimedOut.
// ---------------------------------------------------------------------------
TEST(Queue, UT05_BlockUntilTimesOutOnFullQueue) {
  FullQueueFixture fixture;
  const MetricsSnapshot before = fixture.bus().metrics();

  const auto started = std::chrono::steady_clock::now();
  EXPECT_EQ(fixture.bus().publish(make_event(IgnitionChanged{true}), block_options(100ms)),
            PublishStatus::TimedOut);
  const auto elapsed = std::chrono::steady_clock::now() - started;

  EXPECT_GE(elapsed, 100ms);  // the deadline was actually honoured
  const MetricsSnapshot after = fixture.bus().metrics();
  EXPECT_EQ(after.publish_timeout_total, before.publish_timeout_total + 1);
  EXPECT_EQ(after.rejected_queue_full_total, before.rejected_queue_full_total);
  EXPECT_EQ(after.accepted_total, before.accepted_total);
}

// ---------------------------------------------------------------------------
// T033 / UT-06: a publisher waiting in BlockUntil is woken by stop() and
// returns Stopped, not TimedOut and not Accepted (guarantee G16).
// ---------------------------------------------------------------------------
TEST(Queue, UT06_BlockedPublisherWokenByStopReturnsStopped) {
  FullQueueFixture fixture;

  std::promise<PublishStatus> blocked;
  auto blocked_future = blocked.get_future();
  std::thread publisher([&] {
    // A deadline far longer than the test, so TimedOut cannot explain a pass.
    blocked.set_value(fixture.bus().publish(make_event(IgnitionChanged{true}), block_options(30s)));
  });

  std::thread stopper([&] { fixture.bus().stop(StopMode::Drain); });

  ASSERT_EQ(blocked_future.wait_for(10s), std::future_status::ready);
  EXPECT_EQ(blocked_future.get(), PublishStatus::Stopped);

  fixture.release_worker();  // let the parked handler finish so stop() can join
  publisher.join();
  stopper.join();
}

// ---------------------------------------------------------------------------
// T034 / B03: gate-blocked worker, full queue, Reject refuses and delivers
// nothing; the high watermark stays within capacity (invariant M1).
// ---------------------------------------------------------------------------
TEST(Behaviour, B03_QueueFullAndReject) {
  FullQueueFixture fixture;
  EXPECT_EQ(fixture.bus().publish(make_event(IgnitionChanged{true}), reject_options()),
            PublishStatus::QueueFull);

  const MetricsSnapshot metrics = fixture.bus().metrics();
  EXPECT_LE(metrics.queue_depth, metrics.queue_high_watermark);
  EXPECT_LE(metrics.queue_high_watermark, 1u);
  EXPECT_EQ(metrics.rejected_queue_full_total, 1u);
}

// ---------------------------------------------------------------------------
// T035 / B04: busy worker, full queue, BlockUntil 100ms, event not enqueued.
// ---------------------------------------------------------------------------
TEST(Behaviour, B04_QueueFullAndTimeout) {
  FullQueueFixture fixture;
  const MetricsSnapshot before = fixture.bus().metrics();

  EXPECT_EQ(fixture.bus().publish(make_event(IgnitionChanged{true}), block_options(100ms)),
            PublishStatus::TimedOut);

  // Not enqueued: accepted_total did not move, so no id was consumed (E3).
  EXPECT_EQ(fixture.bus().metrics().accepted_total, before.accepted_total);
}

// ---------------------------------------------------------------------------
// T036 / B10: a blocked publisher must wake on shutdown, not hang.
// ---------------------------------------------------------------------------
TEST(Behaviour, B10_ShutdownWakesBlockedPublisher) {
  FullQueueFixture fixture;

  std::promise<PublishStatus> blocked;
  auto blocked_future = blocked.get_future();
  std::thread publisher([&] {
    blocked.set_value(fixture.bus().publish(make_event(IgnitionChanged{true}), block_options(30s)));
  });

  std::thread stopper([&] { fixture.bus().stop(StopMode::Discard); });

  // The test's own guard is shorter than the CTest timeout, so a hang fails
  // here loudly instead of being killed as a timeout.
  ASSERT_EQ(blocked_future.wait_for(10s), std::future_status::ready);
  EXPECT_EQ(blocked_future.get(), PublishStatus::Stopped);

  fixture.release_worker();
  publisher.join();
  stopper.join();
}

// ---------------------------------------------------------------------------
// T037 / research R-11: BlockUntil with a zero timeout is a poll. A full queue
// gives TimedOut, never QueueFull.
// ---------------------------------------------------------------------------
TEST(Queue, ZeroTimeoutPollOnFullQueueIsTimedOut) {
  FullQueueFixture fixture;
  const MetricsSnapshot before = fixture.bus().metrics();

  EXPECT_EQ(fixture.bus().publish(make_event(IgnitionChanged{true}), block_options(0ms)),
            PublishStatus::TimedOut);

  const MetricsSnapshot after = fixture.bus().metrics();
  EXPECT_EQ(after.publish_timeout_total, before.publish_timeout_total + 1);
  EXPECT_EQ(after.rejected_queue_full_total, before.rejected_queue_full_total);
}

TEST(Queue, ZeroTimeoutPollWithRoomIsAccepted) {
  EventBus bus(BusConfig{4});
  bus.start();
  EXPECT_EQ(bus.publish(make_event(IgnitionChanged{true}), block_options(0ms)),
            PublishStatus::Accepted);
  bus.stop(StopMode::Drain);
}

// ---------------------------------------------------------------------------
// T038 / data-model.md section 4: a negative timeout is InvalidArgument.
// ---------------------------------------------------------------------------
TEST(Queue, NegativeTimeoutIsInvalidArgument) {
  EventBus bus(BusConfig{4});
  bus.start();
  EXPECT_EQ(bus.publish(make_event(IgnitionChanged{true}), block_options(-1ms)),
            PublishStatus::InvalidArgument);
  bus.stop(StopMode::Drain);
  EXPECT_EQ(bus.metrics().accepted_total, 0u);
}

}  // namespace

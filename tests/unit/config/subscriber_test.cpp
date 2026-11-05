// Notification: BC-11, BC-12, BC-25 and invariants N1 to N6, S10.

#include <gtest/gtest.h>

#include <atomic>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "support/service_fixture.hpp"
#include "support/test_gate.hpp"

namespace tcu::config {
namespace {

using test::ServiceFixture;

// Collects the generations an observer was told about, in order.
class Recorder {
 public:
  void operator()(ConfigPtr current, ConfigPtr previous) {
    std::lock_guard<std::mutex> lock(mutex_);
    seen_.push_back(current == nullptr ? 0 : current->generation);
    previous_null_.push_back(previous == nullptr);
  }

  std::vector<std::uint64_t> seen() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return seen_;
  }

  std::vector<bool> previousNull() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return previous_null_;
  }

 private:
  mutable std::mutex mutex_;
  std::vector<std::uint64_t> seen_;
  std::vector<bool> previous_null_;
};

// The notifier runs on the scheduler, so a test waits on the counter the
// service publishes rather than on a sleep.
void waitForNotifications(ConfigurationService& service, std::uint64_t count) {
  for (int i = 0; i < 100000; ++i) {
    if (service.metrics().notifyDeliveredTotal >= count) {
      return;
    }
    std::this_thread::yield();
  }
}

// S10: the commit happened before the callback was enqueued, so an observer
// always sees at least its own generation.
TEST(Notify, RunsAfterCommit) {
  ServiceFixture fx;
  ASSERT_EQ(fx.service().start(), LoadError::None);

  std::atomic<std::uint64_t> observed{0};
  fx.service().subscribe([&](ConfigPtr current, ConfigPtr) {
    observed.store(current->generation);
  });

  fx.writeWith("\"period_ms\": 5000", "\"period_ms\": 2000");
  const ReloadResult result = fx.service().reload();
  ASSERT_TRUE(result.changed);
  waitForNotifications(fx.service(), 1);

  EXPECT_EQ(observed.load(), result.generation);
}

// N1: one worker draining a FIFO queue gives publication order for free.
TEST(Notify, OrderedAcrossGenerations) {
  ServiceFixture fx;
  ASSERT_EQ(fx.service().start(), LoadError::None);

  Recorder recorder;
  fx.service().subscribe([&recorder](ConfigPtr c, ConfigPtr p) { recorder(c, p); });

  for (int i = 0; i < 5; ++i) {
    fx.writeWith("\"period_ms\": 5000",
                 "\"period_ms\": " + std::to_string(1000 + i * 100));
    ASSERT_TRUE(fx.service().reload().changed);
  }
  waitForNotifications(fx.service(), 5);

  const std::vector<std::uint64_t> seen = recorder.seen();
  ASSERT_EQ(seen.size(), 5u);
  for (std::size_t i = 0; i < seen.size(); ++i) {
    EXPECT_EQ(seen[i], i + 2) << "notification " << i << " out of order";
  }
}

// N5: previous is null only for the first notification and for a resync.
TEST(Notify, PreviousIsNullOnFirstAndResync) {
  ServiceFixture fx;
  ASSERT_EQ(fx.service().start(), LoadError::None);

  Recorder recorder;
  fx.service().subscribe([&recorder](ConfigPtr c, ConfigPtr p) { recorder(c, p); });

  fx.writeWith("\"period_ms\": 5000", "\"period_ms\": 2000");
  ASSERT_TRUE(fx.service().reload().changed);
  fx.writeWith("\"period_ms\": 5000", "\"period_ms\": 3000");
  ASSERT_TRUE(fx.service().reload().changed);
  waitForNotifications(fx.service(), 2);

  const std::vector<bool> nulls = recorder.previousNull();
  ASSERT_EQ(nulls.size(), 2u);
  EXPECT_FALSE(nulls[0]) << "a committed change always has a predecessor";
  EXPECT_FALSE(nulls[1]);
}

// BC-11, N2: the middle observer throws; the third still runs, and the failure
// is counted rather than swallowed.
TEST(Notify, ObserverThrowIsIsolatedAndCounted) {
  ServiceFixture fx;
  ASSERT_EQ(fx.service().start(), LoadError::None);

  std::atomic<int> first{0};
  std::atomic<int> third{0};
  fx.service().subscribe([&](ConfigPtr, ConfigPtr) { first.fetch_add(1); });
  fx.service().subscribe([](ConfigPtr, ConfigPtr) {
    throw std::runtime_error("observer is having a bad day");
  });
  fx.service().subscribe([&](ConfigPtr, ConfigPtr) { third.fetch_add(1); });

  fx.writeWith("\"period_ms\": 5000", "\"period_ms\": 2000");
  ASSERT_TRUE(fx.service().reload().changed);
  waitForNotifications(fx.service(), 1);

  EXPECT_EQ(first.load(), 1);
  EXPECT_EQ(third.load(), 1) << "a throwing observer must not stop the others";
  EXPECT_GE(fx.service().metrics().notifyFailuresTotal, 1u);
}

// BC-12, N3: after unsubscribe returns, no further callback starts.
TEST(Notify, UnsubscribeStopsFutureCallbacks) {
  ServiceFixture fx;
  ASSERT_EQ(fx.service().start(), LoadError::None);

  std::atomic<int> calls{0};
  const SubscriptionId id =
      fx.service().subscribe([&](ConfigPtr, ConfigPtr) { calls.fetch_add(1); });

  fx.writeWith("\"period_ms\": 5000", "\"period_ms\": 2000");
  ASSERT_TRUE(fx.service().reload().changed);
  waitForNotifications(fx.service(), 1);
  ASSERT_EQ(calls.load(), 1);

  EXPECT_TRUE(fx.service().unsubscribe(id));
  EXPECT_FALSE(fx.service().unsubscribe(id)) << "a second unsubscribe is false";

  fx.writeWith("\"period_ms\": 5000", "\"period_ms\": 3000");
  ASSERT_TRUE(fx.service().reload().changed);
  waitForNotifications(fx.service(), 2);

  EXPECT_EQ(calls.load(), 1) << "no callback may start after unsubscribe";
}

TEST(Notify, UnsubscribeFromInsideCallbackIsAllowed) {
  ServiceFixture fx;
  ASSERT_EQ(fx.service().start(), LoadError::None);

  std::atomic<int> calls{0};
  std::atomic<SubscriptionId> id{0};
  id.store(fx.service().subscribe([&](ConfigPtr, ConfigPtr) {
    calls.fetch_add(1);
    fx.service().unsubscribe(id.load());
  }));

  for (int i = 0; i < 3; ++i) {
    fx.writeWith("\"period_ms\": 5000",
                 "\"period_ms\": " + std::to_string(1000 + i * 100));
    fx.service().reload();
  }
  waitForNotifications(fx.service(), 1);

  EXPECT_EQ(calls.load(), 1);
}

// A new subscriber is not told about the generation already in force: it reads
// snapshot() itself, which is the resync pattern section 8 describes.
TEST(Notify, NewSubscriberIsNotCalledForTheCurrentGeneration) {
  ServiceFixture fx;
  ASSERT_EQ(fx.service().start(), LoadError::None);

  std::atomic<int> calls{0};
  fx.service().subscribe([&](ConfigPtr, ConfigPtr) { calls.fetch_add(1); });

  EXPECT_EQ(calls.load(), 0);
  EXPECT_EQ(fx.service().snapshot()->generation, 1u);
}

// BC-25, N4: the notification queue is bounded by the scheduler ready queue. An
// observer that blocks fills it, and the service degrades to a resync rather
// than losing the fact that something changed - the commits still happened.
TEST(Notify, OverflowMarksDegradedAndResyncs) {
  // The gate is declared FIRST so it outlives the service: an observer holds a
  // reference to it, and a notification can still be inside wait_until_opened
  // while the fixture is being torn down. ThreadSanitizer caught the other
  // order as a race on the condition variable.
  tcu::test::TestGate gate;

  // Two timer slots: one notification runs (blocked on the gate), one waits,
  // and every further one is refused - which is the overflow path. One slot
  // would also overflow, but then the resync could not be queued either while
  // the blocked delivery still holds its slot.
  ServiceFixture fx(2);
  ASSERT_EQ(fx.service().start(), LoadError::None);
  fx.service().subscribe([&](ConfigPtr, ConfigPtr) { gate.wait_until_opened(); });

  for (int i = 0; i < 12; ++i) {
    fx.writeWith("\"period_ms\": 5000",
                 "\"period_ms\": " + std::to_string(1000 + i * 100));
    const ReloadResult result = fx.service().reload();
    EXPECT_EQ(result.error, LoadError::None) << result.publicReason;
    EXPECT_TRUE(result.changed) << "a commit is a commit even if nobody is told";
  }

  const ConfigMetrics m = fx.service().metrics();
  EXPECT_TRUE(m.notifyDegraded) << "overflow must latch degraded health";
  EXPECT_GE(m.notifyFailuresTotal, 1u);
  EXPECT_EQ(m.generation, 13u) << "every commit still happened";

  gate.open();

  // The resync is delivered once the notifier can breathe again. It is the
  // thing being waited for, so the wait is on the resync counter rather than on
  // a delivery count that could be satisfied by the already queued delivery.
  bool resynced = false;
  for (int i = 0; i < 200000 && !resynced; ++i) {
    resynced = fx.service().metrics().resyncNotificationsTotal >= 1;
    std::this_thread::yield();
  }
  EXPECT_TRUE(resynced)
      << "after an overflow the next accepted notification must be a resync";
}

}  // namespace
}  // namespace tcu::config

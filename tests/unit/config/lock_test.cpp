// Spec section 14 and invariant S4: no callback runs with a service mutex held,
// and the lock order is one way. Both cases here deadlock if that is wrong,
// which is why the suite carries a CTest TIMEOUT.

#include <gtest/gtest.h>

#include <atomic>
#include <thread>

#include "support/service_fixture.hpp"

namespace tcu::config {
namespace {

using test::ServiceFixture;

// An observer that does everything a real module does: reads the snapshot,
// touches the registry, and logs. A nested lock anywhere on that path hangs.
TEST(Lock, ObserverCanReadSnapshotAndLog) {
  ServiceFixture fx;
  ASSERT_EQ(fx.service().start(), LoadError::None);

  std::atomic<bool> done{false};
  fx.service().subscribe([&](ConfigPtr current, ConfigPtr) {
    const ConfigPtr again = fx.service().snapshot();
    const SubscriptionId extra =
        fx.service().subscribe([](ConfigPtr, ConfigPtr) {});
    fx.service().unsubscribe(extra);
    fx.logger().log(tcu::logging::Severity::Info, "observer", "applied", "",
                    {tcu::logging::f("generation", static_cast<std::int64_t>(
                                                       current->generation))});
    done.store(again != nullptr);
  });

  fx.writeWith("\"period_ms\": 5000", "\"period_ms\": 2000");
  ASSERT_TRUE(fx.service().reload().changed);

  for (int i = 0; i < 100000 && !done.load(); ++i) {
    std::this_thread::yield();
  }
  EXPECT_TRUE(done.load()) << "the observer never completed - a lock is nested";
}

// A reload requested from another thread while a notification is running must
// not deadlock either: the reload mutex is released before the notifier runs.
TEST(Lock, ReloadDuringNotificationDoesNotDeadlock) {
  ServiceFixture fx;
  ASSERT_EQ(fx.service().start(), LoadError::None);

  std::atomic<bool> inside{false};
  std::atomic<bool> released{false};
  fx.service().subscribe([&](ConfigPtr, ConfigPtr) {
    inside.store(true);
    while (!released.load()) {
      std::this_thread::yield();
    }
  });

  fx.writeWith("\"period_ms\": 5000", "\"period_ms\": 2000");
  ASSERT_TRUE(fx.service().reload().changed);
  for (int i = 0; i < 100000 && !inside.load(); ++i) {
    std::this_thread::yield();
  }
  ASSERT_TRUE(inside.load());

  // The notifier is busy. A reload on this thread must still complete.
  fx.writeWith("\"period_ms\": 5000", "\"period_ms\": 3000");
  const ReloadResult result = fx.service().reload();
  EXPECT_EQ(result.error, LoadError::None) << result.publicReason;

  released.store(true);
}

}  // namespace
}  // namespace tcu::config

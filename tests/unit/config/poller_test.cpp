// Change detection: BC-13 and research R-03. The poller is a fixed-delay timer
// on the scheduler that compares mtime and size, so debounce is free: a burst
// of writes inside one interval collapses into a single probe.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include "support/service_fixture.hpp"

namespace tcu::config {
namespace {

using namespace std::chrono_literals;
using test::ServiceFixture;

bool waitForGeneration(ConfigurationService& service, std::uint64_t target) {
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (std::chrono::steady_clock::now() < deadline) {
    const ConfigPtr snapshot = service.snapshot();
    if (snapshot != nullptr && snapshot->generation >= target) {
      return true;
    }
    std::this_thread::yield();
  }
  return false;
}

// The temp-file plus rename pattern spec section 25.4 recommends.
TEST(Poller, DetectsAtomicRename) {
  ServiceFixture fx(64, 20ms);
  ASSERT_EQ(fx.service().start(), LoadError::None);

  fx.writeWith("\"period_ms\": 5000", "\"period_ms\": 2000");

  ASSERT_TRUE(waitForGeneration(fx.service(), 2))
      << "the poller never noticed the rename";
  EXPECT_EQ(fx.service().snapshot()->telemetry.periodMs, 2000);
}

// BC-13: five writes in quick succession must not produce five generations.
// Each one is a different value, so the content really does change every time;
// what collapses them is that the poller looks once per interval.
TEST(Poller, DebouncesRepeatedEvents) {
  ServiceFixture fx(64, 200ms);
  ASSERT_EQ(fx.service().start(), LoadError::None);

  for (int i = 0; i < 5; ++i) {
    fx.writeWith("\"period_ms\": 5000",
                 "\"period_ms\": " + std::to_string(2000 + i));
  }

  ASSERT_TRUE(waitForGeneration(fx.service(), 2));
  // The final state is what the last write said, and it took one generation to
  // get there rather than five.
  EXPECT_EQ(fx.service().snapshot()->telemetry.periodMs, 2004);
  EXPECT_LE(fx.service().metrics().reloadSuccessChangedTotal, 2u);
}

// pollInterval == 0 means explicit reload only, which is what every other unit
// test relies on.
TEST(Poller, DisabledWhenIntervalIsZero) {
  ServiceFixture fx(64, 0ms);
  ASSERT_EQ(fx.service().start(), LoadError::None);

  fx.writeWith("\"period_ms\": 5000", "\"period_ms\": 2000");
  std::this_thread::yield();

  EXPECT_EQ(fx.service().snapshot()->generation, 1u);
  EXPECT_EQ(fx.service().metrics().reloadAttemptsTotal, 0u);
}

// A rejected candidate must update the recorded stat too, or the poller would
// re-read the same bad file every interval for the life of the process.
TEST(Poller, RejectedReloadDoesNotRetryForever) {
  ServiceFixture fx(64, 20ms);
  ASSERT_EQ(fx.service().start(), LoadError::None);

  fx.writeFixture("invalid_retry.json");

  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (std::chrono::steady_clock::now() < deadline &&
         fx.service().metrics().reloadAttemptsTotal == 0) {
    std::this_thread::yield();
  }
  const std::uint64_t after_first = fx.service().metrics().reloadAttemptsTotal;
  ASSERT_GE(after_first, 1u) << "the poller never read the bad file";

  // Give it several more intervals: the count must not keep climbing.
  const auto settle = std::chrono::steady_clock::now() + 300ms;
  while (std::chrono::steady_clock::now() < settle) {
    std::this_thread::yield();
  }
  EXPECT_LE(fx.service().metrics().reloadAttemptsTotal, after_first + 1)
      << "a rejected file must not be re-read on every interval";
  EXPECT_EQ(fx.service().snapshot()->generation, 1u);
}

// A file that disappears is not a reason to spin either.
TEST(Poller, MissingFileDoesNotSpin) {
  ServiceFixture fx(64, 20ms);
  ASSERT_EQ(fx.service().start(), LoadError::None);

  std::filesystem::remove(fx.dir().path());
  const auto settle = std::chrono::steady_clock::now() + 200ms;
  while (std::chrono::steady_clock::now() < settle) {
    std::this_thread::yield();
  }

  EXPECT_EQ(fx.service().snapshot()->generation, 1u)
      << "the last known good snapshot stays in force";
}

}  // namespace
}  // namespace tcu::config

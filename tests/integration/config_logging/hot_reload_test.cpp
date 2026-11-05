// Scenario B of spec section 12, end to end: an operator edits a hot key with
// temp-file plus rename, the poller notices, the candidate commits, and the
// observer applies it - rescheduling the telemetry timer exactly once and
// moving the logger level without a restart (research R-15).

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include "support/stack.hpp"
#include "support/test_gate.hpp"

namespace tcu::itest {
namespace {

using namespace std::chrono_literals;

// The same observer the demo installs (tasks.md T067), reduced to what can be
// asserted: reschedule telemetry, apply the two hot logger settings, and record
// the generation it applied.
struct TelemetryModule {
  scheduler::TimerScheduler& timers;
  logging::AsyncLogger& logger;
  std::atomic<int> reschedules{0};
  std::atomic<std::int64_t> period_ms{0};
  std::atomic<std::uint64_t> generation_applied{0};
  scheduler::TimerId timer{0};
  tcu::test::TestGate applied;

  TelemetryModule(scheduler::TimerScheduler& t, logging::AsyncLogger& l)
      : timers(t), logger(l) {}

  void apply(const config::ConfigPtr& snapshot) {
    if (snapshot == nullptr) {
      return;
    }
    if (snapshot->telemetry.periodMs != period_ms.load()) {
      if (timer != 0) {
        timers.cancel(timer);
      }
      const auto scheduled = timers.scheduleEvery(
          std::chrono::milliseconds(snapshot->telemetry.periodMs),
          scheduler::PeriodicMode::FixedDelay, [] {});
      timer = scheduled ? scheduled.id : 0;
      period_ms.store(snapshot->telemetry.periodMs);
      reschedules.fetch_add(1);
    }
    logger.setLevel(snapshot->logging.level);
    logger.setConsoleEnabled(snapshot->logging.consoleEnabled);
    generation_applied.store(snapshot->generation);
  }
};

TEST(HotReload, PollerCommitsAndObserverApplies) {
  Stack stack(20ms);
  TelemetryModule module(*stack.timers, *stack.logger);

  ASSERT_EQ(stack.service->start(), config::LoadError::None);
  module.apply(stack.service->snapshot());  // startup application
  ASSERT_EQ(module.reschedules.load(), 1);
  ASSERT_EQ(module.period_ms.load(), 5000);

  stack.service->subscribe([&module](config::ConfigPtr current, config::ConfigPtr) {
    module.apply(current);
    module.applied.open();
  });

  // The operator edit: a temp file and a rename, as section 25.4 recommends.
  stack.writeWith("\"period_ms\": 5000", "\"period_ms\": 2000");

  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (std::chrono::steady_clock::now() < deadline &&
         module.generation_applied.load() < 2) {
    std::this_thread::yield();
  }
  stack.logger->flush();

  EXPECT_EQ(stack.service->snapshot()->generation, 2u);
  EXPECT_EQ(module.generation_applied.load(), 2u);
  EXPECT_EQ(module.period_ms.load(), 2000);
  EXPECT_EQ(module.reschedules.load(), 2)
      << "exactly one reschedule for exactly one effective change";
  EXPECT_TRUE(stack.sink->containsSubstring("reload_committed"));
  EXPECT_TRUE(stack.sink->containsSubstring("\"generation\":2"));

  if (module.timer != 0) {
    stack.timers->cancel(module.timer);
  }
}

// A hot level change takes effect on the running logger: a DEBUG record is
// dropped before the change and written after it.
TEST(HotReload, LoggerLevelFollowsTheSnapshot) {
  Stack stack;
  TelemetryModule module(*stack.timers, *stack.logger);
  ASSERT_EQ(stack.service->start(), config::LoadError::None);

  stack.logger->setLevel(logging::Severity::Warn);
  stack.logger->log(logging::Severity::Debug, "demo", "before_change", "");
  stack.logger->flush();
  ASSERT_FALSE(stack.sink->containsSubstring("before_change"));

  stack.service->subscribe([&module](config::ConfigPtr current, config::ConfigPtr) {
    module.apply(current);
    module.applied.open();
  });
  stack.writeWith("\"level\": \"INFO\"", "\"level\": \"DEBUG\"");
  ASSERT_TRUE(stack.service->reload().changed);
  module.applied.wait_until_opened();

  stack.logger->log(logging::Severity::Debug, "demo", "after_change", "");
  stack.logger->flush();
  EXPECT_TRUE(stack.sink->containsSubstring("after_change"));

  if (module.timer != 0) {
    stack.timers->cancel(module.timer);
  }
}

// A no-op reload must not make a module do work: the observer is not called,
// so nothing is rescheduled (BC-06).
TEST(HotReload, NoOpReloadDoesNotRescheduleAnything) {
  Stack stack;
  TelemetryModule module(*stack.timers, *stack.logger);
  ASSERT_EQ(stack.service->start(), config::LoadError::None);
  module.apply(stack.service->snapshot());
  const int baseline = module.reschedules.load();

  stack.service->subscribe([&module](config::ConfigPtr current, config::ConfigPtr) {
    module.apply(current);
  });
  stack.writeFixture("tcu.json");
  const config::ReloadResult result = stack.service->reload();

  EXPECT_EQ(result.error, config::LoadError::None);
  EXPECT_FALSE(result.changed);
  EXPECT_EQ(module.reschedules.load(), baseline);

  if (module.timer != 0) {
    stack.timers->cancel(module.timer);
  }
}

}  // namespace
}  // namespace tcu::itest

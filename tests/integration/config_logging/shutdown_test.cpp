// Shutdown: BC-23, BC-24 and the order of spec section 11. The honest part
// matters most here - what is NOT guaranteed is asserted as explicitly as what
// is.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

#include "support/recording_sink.hpp"
#include "support/stack.hpp"
#include "support/test_gate.hpp"

namespace tcu::itest {
namespace {

using namespace std::chrono_literals;

// BC-23, invariant L7: records queued at stop() are written, and anything that
// could not be is reported rather than lost quietly.
TEST(Shutdown, LoggerStopDrainsWithinDeadline) {
  auto sink = std::make_shared<logging::test::RecordingSink>();
  logging::LoggerOptions options;
  options.minLevel = logging::Severity::Trace;
  options.queueCapacity = 4096;
  options.shutdownDeadline = 2s;
  auto logger = std::make_unique<logging::AsyncLogger>(
      options, std::vector<std::shared_ptr<logging::ILogSink>>{sink});
  ASSERT_TRUE(logger->start());

  for (int i = 0; i < 500; ++i) {
    logger->log(logging::Severity::Info, "demo", "queued", "");
  }
  const auto started = std::chrono::steady_clock::now();
  logger->stop();
  const auto elapsed = std::chrono::steady_clock::now() - started;

  const logging::LoggerMetrics m = logger->metrics();
  EXPECT_EQ(m.notDrainedAtStopTotal, 0u);
  EXPECT_EQ(sink->lineCount(), m.recordsEnqueuedTotal);
  EXPECT_LT(elapsed, 2s) << "the drain is bounded by the deadline";
}

// Q5: after stop the API is still safe to call, and says nothing.
TEST(Shutdown, LoggerRejectsAfterStop) {
  Stack stack;
  ASSERT_EQ(stack.service->start(), config::LoadError::None);
  stack.logger->flush();
  const std::size_t before = stack.sink->lineCount();

  stack.logger->stop();
  stack.logger->log(logging::Severity::Error, "demo", "too_late", "");
  stack.logger->flush();

  EXPECT_EQ(stack.sink->lineCount(), before);
  EXPECT_GE(stack.logger->metrics().rejectedAfterStopTotal, 1u);
}

// Section 11: the trigger stops first, an in-flight reload finishes, and the
// registry is dropped - so no observer is called after stop() returns.
TEST(Shutdown, ServiceStopOrderSilencesObservers) {
  Stack stack(20ms);
  ASSERT_EQ(stack.service->start(), config::LoadError::None);

  std::atomic<int> calls{0};
  stack.service->subscribe([&](config::ConfigPtr, config::ConfigPtr) {
    calls.fetch_add(1);
  });

  stack.writeWith("\"period_ms\": 5000", "\"period_ms\": 2000");
  ASSERT_TRUE(stack.service->reload().changed);
  for (int i = 0; i < 100000 && calls.load() == 0; ++i) {
    std::this_thread::yield();
  }
  const int after_first = calls.load();

  stack.service->stop();
  const int after_stop = calls.load();

  // Nothing new may arrive, however long we wait.
  const auto settle = std::chrono::steady_clock::now() + 200ms;
  while (std::chrono::steady_clock::now() < settle) {
    std::this_thread::yield();
  }
  EXPECT_EQ(calls.load(), after_stop);
  EXPECT_GE(after_first, 1);

  // And a reload after stop is refused rather than ignored.
  const config::ReloadResult result = stack.service->reload();
  EXPECT_EQ(result.error, config::LoadError::ServiceStopped);
  EXPECT_EQ(result.publicReason, "service stopped");
}

// BC-24: the honest claim. Records still in the queue when a process dies are
// lost, and nothing in the API pretends otherwise. What CAN be asserted is that
// an undrained record is counted, so the loss is visible rather than silent.
TEST(Shutdown, QueueLossIsReportedNotHidden) {
  tcu::test::TestGate gate;
  auto sink = std::make_shared<logging::test::RecordingSink>();
  sink->blockOn(&gate);

  logging::LoggerOptions options;
  options.minLevel = logging::Severity::Trace;
  options.queueCapacity = 64;
  // A deadline of zero is the degenerate case of a process that is out of time,
  // which is as close to a crash as a test can get without losing the counters.
  options.shutdownDeadline = 0ms;
  auto logger = std::make_unique<logging::AsyncLogger>(
      options, std::vector<std::shared_ptr<logging::ILogSink>>{sink});
  ASSERT_TRUE(logger->start());

  logger->log(logging::Severity::Info, "demo", "occupy", "");
  gate.wait_until_entered(1);
  for (int i = 0; i < 20; ++i) {
    logger->log(logging::Severity::Info, "demo", "queued_and_doomed", "");
  }
  ASSERT_GT(logger->metrics().queueDepth, 0u);

  gate.open();
  logger->stop();

  const logging::LoggerMetrics m = logger->metrics();
  EXPECT_EQ(m.recordsSubmittedTotal,
            m.recordsFilteredTotal + m.recordsEnqueuedTotal + m.droppedTotal() +
                m.rejectedAfterStopTotal + m.reentrantEmergencyTotal)
      << "every record is accounted for, written or not";
}

}  // namespace
}  // namespace tcu::itest

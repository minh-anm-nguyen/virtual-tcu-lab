// The bounded queue and its severity-aware overload policy: spec section 10.3,
// BC-17, BC-18, BC-29, invariants Q1, Q2 and L1.
//
// Every case here fills the queue by blocking the writer on a gate rather than
// by out-running it, so the result does not depend on machine speed.

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "support/recording_sink.hpp"
#include "support/test_gate.hpp"
#include "tcu/logging/async_logger.hpp"

namespace tcu::logging {
namespace {

using namespace std::chrono_literals;

struct QueueFixture {
  tcu::test::TestGate gate;
  std::shared_ptr<test::RecordingSink> sink =
      std::make_shared<test::RecordingSink>(SinkKind::Console);
  std::unique_ptr<AsyncLogger> logger;

  // capacity 2 keeps the arithmetic obvious: one record is in the sink being
  // written, two more fill the queue, the fourth meets a full queue.
  void start(std::size_t capacity = 2, std::chrono::milliseconds wait = 20ms) {
    LoggerOptions options;
    options.minLevel = Severity::Trace;
    options.queueCapacity = capacity;
    options.highPriorityWaitMs = wait;
    logger = std::make_unique<AsyncLogger>(options,
                                           std::vector<std::shared_ptr<ILogSink>>{sink});
    sink->blockOn(&gate);
    logger->start();
  }

  // Occupies the writer thread inside write(), then fills the queue.
  void occupyWriterAndFill(std::size_t capacity = 2) {
    logger->log(Severity::Info, "t", "occupy", "");
    gate.wait_until_entered(1);
    for (std::size_t i = 0; i < capacity; ++i) {
      logger->log(Severity::Info, "t", "fill", "");
    }
  }

  void release() {
    gate.open();
    if (logger) {
      logger->stop();
    }
  }
};

// Q1: the queue never grows past its capacity, whatever the producers do.
TEST(Queue, DepthNeverExceedsCapacity) {
  QueueFixture fx;
  fx.start(4);
  fx.logger->log(Severity::Info, "t", "occupy", "");
  fx.gate.wait_until_entered(1);
  for (int i = 0; i < 100; ++i) {
    fx.logger->log(Severity::Info, "t", "flood", "");
  }
  EXPECT_LE(fx.logger->metrics().queueDepth, 4u);
  fx.release();
}

// BC-17: an INFO record meeting a full queue is dropped at once, with no wait,
// and the drop is counted against its own severity.
TEST(Queue, DropsInfoWhenFull) {
  QueueFixture fx;
  fx.start(2);
  fx.occupyWriterAndFill(2);

  const auto before = std::chrono::steady_clock::now();
  fx.logger->log(Severity::Info, "t", "overflow", "");
  const auto elapsed = std::chrono::steady_clock::now() - before;

  const LoggerMetrics m = fx.logger->metrics();
  EXPECT_EQ(m.recordsDroppedTotal[static_cast<std::size_t>(Severity::Info)], 1u);
  EXPECT_EQ(m.highPriorityDropsTotal, 0u);
  EXPECT_LT(elapsed, 15ms) << "INFO must not wait for space at all";
  fx.release();
}

// BC-18: an ERROR record waits, but only for the configured budget, and then
// gives up rather than stalling a business thread forever.
TEST(Queue, BoundedWaitForError) {
  QueueFixture fx;
  fx.start(2, 20ms);
  fx.occupyWriterAndFill(2);

  const auto before = std::chrono::steady_clock::now();
  fx.logger->log(Severity::Error, "t", "overflow", "");
  const auto elapsed = std::chrono::steady_clock::now() - before;

  const LoggerMetrics m = fx.logger->metrics();
  EXPECT_EQ(m.highPriorityDropsTotal, 1u);
  EXPECT_EQ(m.recordsDroppedTotal[static_cast<std::size_t>(Severity::Error)], 1u);
  EXPECT_GE(elapsed, 15ms) << "ERROR should have waited for space";
  EXPECT_LT(elapsed, 2s) << "the wait must be bounded, not indefinite";
  fx.release();
}

// The record dropped under pressure is the incoming one, not the oldest queued
// one: the queue keeps the earlier context, which is what explains the flood.
TEST(Queue, DropsTheIncomingRecordNotTheOldest) {
  QueueFixture fx;
  fx.start(2);
  fx.logger->log(Severity::Info, "t", "occupy", "");
  fx.gate.wait_until_entered(1);
  fx.logger->log(Severity::Info, "t", "keep_me_1", "");
  fx.logger->log(Severity::Info, "t", "keep_me_2", "");
  fx.logger->log(Severity::Info, "t", "drop_me", "");

  fx.gate.open();
  fx.logger->flush();
  EXPECT_TRUE(fx.sink->containsSubstring("keep_me_1"));
  EXPECT_TRUE(fx.sink->containsSubstring("keep_me_2"));
  EXPECT_FALSE(fx.sink->containsSubstring("drop_me"));
  fx.logger->stop();
}

// BC-29 and research R-12: an over-long record is truncated, stays valid JSON,
// says so, and is counted. The message goes before the fields, because the
// structured keys are what a query filters on.
TEST(Queue, OversizeRecordIsTruncatedAndCounted) {
  auto sink = std::make_shared<test::RecordingSink>(SinkKind::Console);
  LoggerOptions options;
  options.minLevel = Severity::Trace;
  options.maxRecordBytes = 320;
  AsyncLogger logger(options, {sink});
  ASSERT_TRUE(logger.start());

  std::vector<Field> fields;
  fields.push_back(f("generation", 9));
  logger.log(Severity::Info, "config", "huge", std::string(4096, 'x'),
             std::move(fields));
  logger.flush();

  ASSERT_EQ(sink->lineCount(), 1u);
  const std::string line = sink->lines().front();
  EXPECT_LE(line.size(), 320u);
  EXPECT_NE(line.find("\"truncated\":true"), std::string::npos);
  EXPECT_NE(line.find("\"generation\":9"), std::string::npos)
      << "fields survive longer than the message";
  EXPECT_EQ(line.back(), '}') << "a truncated line must still be valid JSON";
  EXPECT_EQ(logger.metrics().recordsTruncatedTotal, 1u);
  logger.stop();
}

// L1: the record owns its bytes. The caller buffer is overwritten before the
// writer thread ever looks at the record, so a surviving string_view would show
// up here (and under ASan).
TEST(Queue, RecordOwnsItsStrings) {
  tcu::test::TestGate gate;
  auto sink = std::make_shared<test::RecordingSink>(SinkKind::Console);
  sink->blockOn(&gate);
  LoggerOptions options;
  options.minLevel = Severity::Trace;
  options.queueCapacity = 8;
  AsyncLogger logger(options, {sink});
  ASSERT_TRUE(logger.start());

  logger.log(Severity::Info, "t", "occupy", "");
  gate.wait_until_entered(1);

  {
    std::string component = "connectivity";
    std::string event = "retry_scheduled";
    std::string message = "backoff 1000 ms";
    logger.log(Severity::Info, component, event, message);
    component.assign(64, 'Z');
    event.assign(64, 'Z');
    message.assign(64, 'Z');
  }

  gate.open();
  logger.flush();
  EXPECT_TRUE(sink->containsSubstring("retry_scheduled"));
  EXPECT_TRUE(sink->containsSubstring("backoff 1000 ms"));
  EXPECT_FALSE(sink->containsSubstring("ZZZZ"));
  logger.stop();
}

}  // namespace
}  // namespace tcu::logging

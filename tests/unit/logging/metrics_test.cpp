// Conservation: contracts/metrics.md section 4, invariants M3, M4, M7, M8, M9.
// These turn "no record was lost" into arithmetic rather than a claim.

#include <gtest/gtest.h>

#include <memory>
#include <vector>

#include "support/recording_sink.hpp"
#include "support/test_gate.hpp"
#include "tcu/logging/async_logger.hpp"

namespace tcu::logging {
namespace {

LoggerOptions options(std::size_t capacity = 8192) {
  LoggerOptions opts;
  opts.minLevel = Severity::Debug;  // Trace is filtered, so both paths are used
  opts.queueCapacity = capacity;
  return opts;
}

// M3: every submitted record is accounted for exactly once.
TEST(Metrics, ConservationAfterFlood) {
  tcu::test::TestGate gate;
  auto sink = std::make_shared<test::RecordingSink>(SinkKind::Console);
  AsyncLogger logger(options(8), {sink});
  ASSERT_TRUE(logger.start());

  // Block the writer so the queue fills and the drop path is exercised too.
  sink->blockOn(&gate);
  logger.log(Severity::Info, "t", "occupy", "");
  gate.wait_until_entered(1);

  for (int i = 0; i < 500; ++i) {
    logger.log(Severity::Trace, "t", "filtered", "");   // below the level
    logger.log(Severity::Info, "t", "maybe_dropped", "");
  }
  gate.open();
  logger.flush();

  const LoggerMetrics m = logger.metrics();
  EXPECT_EQ(m.recordsSubmittedTotal,
            m.recordsFilteredTotal + m.recordsEnqueuedTotal + m.droppedTotal() +
                m.rejectedAfterStopTotal + m.reentrantEmergencyTotal);
  EXPECT_EQ(m.recordsFilteredTotal, 500u);
  EXPECT_GT(m.droppedTotal(), 0u) << "the queue was deliberately too small";
  logger.stop();
}

// M7, with one sink so the arithmetic is unambiguous: everything that entered
// the queue either reached the sink, failed at the sink, failed to format, or
// is reported as not drained.
TEST(Metrics, EnqueuedAccountingBalances) {
  auto sink = std::make_shared<test::RecordingSink>(SinkKind::File);
  AsyncLogger logger(options(), {sink});
  ASSERT_TRUE(logger.start());

  for (int i = 0; i < 200; ++i) {
    logger.log(Severity::Info, "t", "e", "payload");
  }
  logger.flush();
  logger.stop();

  const LoggerMetrics m = logger.metrics();
  const std::size_t file = sinkIndex(SinkKind::File);
  EXPECT_EQ(m.recordsEnqueuedTotal,
            m.recordsWrittenTotal[file] + m.sinkErrorsTotal[file] +
                m.formatFailuresTotal + m.notDrainedAtStopTotal);
  EXPECT_EQ(m.recordsWrittenTotal[file], 200u);
}

// M8: a failed write is an error, never a write.
TEST(Metrics, FailedWriteDoesNotCountAsWritten) {
  auto sink = std::make_shared<test::RecordingSink>(SinkKind::File);
  sink->failFromWrite(3);  // the first two succeed
  AsyncLogger logger(options(), {sink});
  ASSERT_TRUE(logger.start());

  for (int i = 0; i < 5; ++i) {
    logger.log(Severity::Info, "t", "e", "");
  }
  logger.flush();

  const LoggerMetrics m = logger.metrics();
  const std::size_t file = sinkIndex(SinkKind::File);
  EXPECT_EQ(m.recordsWrittenTotal[file], 2u);
  EXPECT_EQ(m.sinkErrorsTotal[file], 3u);
  EXPECT_EQ(m.sinkHealth[file], SinkHealth::Unhealthy);
  EXPECT_FALSE(m.allSinksHealthy());
  logger.stop();
}

// M9: flush and stop both leave the queue empty.
TEST(Metrics, QueueDepthZeroAfterFlushAndStop) {
  auto sink = std::make_shared<test::RecordingSink>();
  AsyncLogger logger(options(), {sink});
  ASSERT_TRUE(logger.start());

  for (int i = 0; i < 50; ++i) {
    logger.log(Severity::Info, "t", "e", "");
  }
  logger.flush();
  EXPECT_EQ(logger.metrics().queueDepth, 0u);

  logger.stop();
  EXPECT_EQ(logger.metrics().queueDepth, 0u);
  EXPECT_EQ(logger.metrics().notDrainedAtStopTotal, 0u);
}

// M4: a label cannot be invented at runtime, because each array is exactly as
// long as the enum it is indexed by. A new enumerator without a new slot fails
// here rather than at a monitoring system.
TEST(Metrics, LabelsAreClosedSets) {
  LoggerMetrics m;
  EXPECT_EQ(m.recordsDroppedTotal.size(), kSeverityCount);
  EXPECT_EQ(m.recordsWrittenTotal.size(), kSinkCount);
  EXPECT_EQ(m.sinkErrorsTotal.size(), kSinkCount);
  EXPECT_EQ(m.sinkHealth.size(), kSinkCount);
  EXPECT_EQ(static_cast<std::size_t>(Severity::Fatal) + 1, kSeverityCount);
  EXPECT_EQ(sinkIndex(SinkKind::File) + 1, kSinkCount);
}

// The histogram is reported, never asserted on as a budget (research R-21), so
// the only invariant is that it is internally consistent.
TEST(Metrics, WriteLatencyHistogramIsConsistent) {
  auto sink = std::make_shared<test::RecordingSink>();
  AsyncLogger logger(options(), {sink});
  ASSERT_TRUE(logger.start());
  for (int i = 0; i < 20; ++i) {
    logger.log(Severity::Info, "t", "e", "");
  }
  logger.flush();

  const Histogram h = logger.metrics().writeLatencyMs;
  std::uint64_t bucketed = 0;
  for (const std::uint64_t n : h.buckets) {
    bucketed += n;
  }
  EXPECT_EQ(bucketed, h.count);
  EXPECT_EQ(h.count, 20u) << "one observation per sink write";
  logger.stop();
}

}  // namespace
}  // namespace tcu::logging

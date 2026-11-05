// BC-16 and research R-15: the level check comes first, so a filtered call
// costs one atomic load and nothing else - no formatting, no copy, no enqueue.

#include <gtest/gtest.h>

#include <memory>
#include <vector>

#include "support/recording_sink.hpp"
#include "tcu/logging/async_logger.hpp"

namespace tcu::logging {
namespace {

// A sink that fails the test if it is ever written to. The strongest available
// statement of "nothing was formatted": if a filtered record had been formatted
// and handed on, this fires.
class ForbiddenSink final : public ILogSink {
 public:
  std::string_view name() const noexcept override { return "console"; }
  SinkKind kind() const noexcept override { return SinkKind::Console; }
  bool write(std::string_view line) override {
    ADD_FAILURE() << "a filtered record reached a sink: " << line;
    return true;
  }
  void flush() override {}
  SinkHealth health() const noexcept override { return SinkHealth::Healthy; }
};

TEST(Logger, FilterBelowLevelNeverFormats) {
  LoggerOptions options;
  options.minLevel = Severity::Warn;
  AsyncLogger logger(options, {std::make_shared<ForbiddenSink>()});
  ASSERT_TRUE(logger.start());

  logger.log(Severity::Trace, "t", "e", "");
  logger.log(Severity::Debug, "t", "e", "");
  logger.log(Severity::Info, "t", "e", "");
  logger.flush();

  const LoggerMetrics m = logger.metrics();
  EXPECT_EQ(m.recordsSubmittedTotal, 3u);
  EXPECT_EQ(m.recordsFilteredTotal, 3u);
  EXPECT_EQ(m.recordsEnqueuedTotal, 0u);
  EXPECT_EQ(m.droppedTotal(), 0u) << "filtered is not dropped; different facts";
  logger.stop();
}

TEST(Logger, EnabledMatchesLevel) {
  LoggerOptions options;
  options.minLevel = Severity::Info;
  AsyncLogger logger(options, {std::make_shared<test::RecordingSink>()});
  ASSERT_TRUE(logger.start());

  EXPECT_FALSE(logger.enabled(Severity::Trace));
  EXPECT_FALSE(logger.enabled(Severity::Debug));
  EXPECT_TRUE(logger.enabled(Severity::Info));
  EXPECT_TRUE(logger.enabled(Severity::Warn));
  EXPECT_TRUE(logger.enabled(Severity::Error));
  EXPECT_TRUE(logger.enabled(Severity::Fatal));
  logger.stop();
}

// R-15: /logging/level is a hot key, so the setter has to take effect on a
// running logger without a restart.
TEST(Logger, SetLevelAppliesImmediately) {
  auto sink = std::make_shared<test::RecordingSink>();
  LoggerOptions options;
  options.minLevel = Severity::Warn;
  AsyncLogger logger(options, {sink});
  ASSERT_TRUE(logger.start());

  logger.log(Severity::Debug, "t", "before", "");
  logger.setLevel(Severity::Debug);
  EXPECT_TRUE(logger.enabled(Severity::Debug));
  logger.log(Severity::Debug, "t", "after", "");
  logger.flush();

  EXPECT_FALSE(sink->containsSubstring("before"));
  EXPECT_TRUE(sink->containsSubstring("after"));
  logger.stop();
}

// FATAL is never filtered out: a process that is dying must be able to say so.
TEST(Logger, FatalAlwaysPasses) {
  auto sink = std::make_shared<test::RecordingSink>();
  LoggerOptions options;
  options.minLevel = Severity::Fatal;
  AsyncLogger logger(options, {sink});
  ASSERT_TRUE(logger.start());

  logger.log(Severity::Fatal, "t", "dying", "out of memory");
  logger.flush();

  EXPECT_TRUE(sink->containsSubstring("dying"));
  EXPECT_GE(logger.metrics().fatalEmergencyTotal, 1u)
      << "FATAL also takes the synchronous emergency path (section 10.3)";
  logger.stop();
}

// A record exactly at the configured level passes; the test is >=, not >.
TEST(Logger, RecordAtTheLevelPasses) {
  auto sink = std::make_shared<test::RecordingSink>();
  LoggerOptions options;
  options.minLevel = Severity::Info;
  AsyncLogger logger(options, {sink});
  ASSERT_TRUE(logger.start());
  logger.log(Severity::Info, "t", "at_level", "");
  logger.flush();
  EXPECT_TRUE(sink->containsSubstring("at_level"));
  logger.stop();
}

}  // namespace
}  // namespace tcu::logging

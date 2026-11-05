// Lifecycle contracts: BC-23 (partly), BC-26, BC-30, invariants L6, L9 and Q4.
// Each case here is a hang or a crash if the implementation gets it wrong,
// which is why the suite carries a CTest TIMEOUT.

#include <gtest/gtest.h>

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "support/recording_sink.hpp"
#include "tcu/logging/async_logger.hpp"

namespace tcu::logging {
namespace {

LoggerOptions traceOptions() {
  LoggerOptions options;
  options.minLevel = Severity::Trace;
  return options;
}

// BC-30, L6: the second stop() must not join an already joined thread.
TEST(Logger, StopIsIdempotent) {
  auto sink = std::make_shared<test::RecordingSink>();
  AsyncLogger logger(traceOptions(), {sink});
  ASSERT_TRUE(logger.start());
  logger.log(Severity::Info, "t", "e", "");
  logger.stop();
  logger.stop();
  logger.stop();
  SUCCEED();
}

// Q5: after stop() the API stays callable and stays silent - a counted no-op,
// never an exception into a shutting-down caller.
TEST(Logger, RejectsAfterStop) {
  auto sink = std::make_shared<test::RecordingSink>();
  AsyncLogger logger(traceOptions(), {sink});
  ASSERT_TRUE(logger.start());
  logger.stop();

  const std::size_t before = sink->lineCount();
  logger.log(Severity::Error, "t", "after_stop", "");
  logger.flush();

  EXPECT_EQ(sink->lineCount(), before);
  EXPECT_EQ(logger.metrics().rejectedAfterStopTotal, 1u);
}

// BC-26, Q4: a sink that logs. Without the reentrancy guard this deadlocks on
// the queue mutex or amplifies into an unbounded loop.
class SelfLoggingSink final : public ILogSink {
 public:
  void attach(ILogger* logger) { logger_ = logger; }
  std::string_view name() const noexcept override { return "console"; }
  SinkKind kind() const noexcept override { return SinkKind::Console; }
  bool write(std::string_view) override {
    ++writes_;
    if (logger_ != nullptr && writes_ < 4) {
      logger_->log(Severity::Error, "sink", "reentrant", "writing while writing");
    }
    return true;
  }
  void flush() override {}
  SinkHealth health() const noexcept override { return SinkHealth::Healthy; }
  std::size_t writes() const { return writes_; }

 private:
  ILogger* logger_ = nullptr;
  std::size_t writes_ = 0;
};

TEST(Logger, ReentrantLogFromSinkDoesNotDeadlock) {
  auto sink = std::make_shared<SelfLoggingSink>();
  AsyncLogger logger(traceOptions(), {sink});
  sink->attach(&logger);
  ASSERT_TRUE(logger.start());

  logger.log(Severity::Info, "t", "first", "");
  logger.flush();
  logger.stop();

  EXPECT_GE(logger.metrics().reentrantEmergencyTotal, 1u)
      << "the reentrant call must take the emergency path, not the queue";
  EXPECT_LE(sink->writes(), 2u) << "no queue amplification";
}

// L9: no exception escapes log(). A sink that throws is a sink error, not a
// crash in the business thread.
class ThrowingSink final : public ILogSink {
 public:
  std::string_view name() const noexcept override { return "file"; }
  SinkKind kind() const noexcept override { return SinkKind::File; }
  bool write(std::string_view) override { throw std::runtime_error("disk on fire"); }
  void flush() override {}
  SinkHealth health() const noexcept override { return SinkHealth::Unhealthy; }
};

TEST(Logger, ThrowingSinkIsCountedNotPropagated) {
  auto good = std::make_shared<test::RecordingSink>(SinkKind::Console);
  AsyncLogger logger(traceOptions(), {std::make_shared<ThrowingSink>(), good});
  ASSERT_TRUE(logger.start());

  EXPECT_NO_THROW(logger.log(Severity::Error, "t", "e", "still fine"));
  logger.flush();

  EXPECT_EQ(logger.metrics().sinkErrorsTotal[sinkIndex(SinkKind::File)], 1u);
  EXPECT_TRUE(good->containsSubstring("still fine"))
      << "one failing sink must not stop the others";
  logger.stop();
}

// A logger that was never started still must not throw or hang on use.
TEST(Logger, UseBeforeStartIsSafe) {
  auto sink = std::make_shared<test::RecordingSink>();
  AsyncLogger logger(traceOptions(), {sink});
  EXPECT_NO_THROW(logger.log(Severity::Info, "t", "e", ""));
  EXPECT_NO_THROW(logger.flush());
  EXPECT_NO_THROW(logger.stop());
}

// A logger with no sink at all is legal: records are accounted for and dropped
// at the sink boundary, not lost silently.
TEST(Logger, NoSinksIsLegalAndAccounted) {
  AsyncLogger logger(traceOptions(), {});
  ASSERT_TRUE(logger.start());
  logger.log(Severity::Info, "t", "e", "");
  logger.flush();
  EXPECT_EQ(logger.metrics().recordsEnqueuedTotal, 1u);
  EXPECT_EQ(logger.metrics().writtenTotal(), 0u);
  logger.stop();
}


// flush() from the writer thread would wait for itself. It returns instead.
class FlushingSink final : public ILogSink {
 public:
  void attach(ILogger* logger) { logger_ = logger; }
  std::string_view name() const noexcept override { return "console"; }
  SinkKind kind() const noexcept override { return SinkKind::Console; }
  bool write(std::string_view) override {
    if (logger_ != nullptr) { logger_->flush(); }
    return true;
  }
  void flush() override {}
  SinkHealth health() const noexcept override { return SinkHealth::Healthy; }

 private:
  ILogger* logger_ = nullptr;
};

TEST(Logger, FlushIsNoOpFromWriterThread) {
  auto sink = std::make_shared<FlushingSink>();
  AsyncLogger logger(traceOptions(), {sink});
  sink->attach(&logger);
  ASSERT_TRUE(logger.start());
  logger.log(Severity::Info, "t", "e", "");
  logger.flush();
  logger.stop();
  SUCCEED();
}

// stop() from inside a sink callback must not self-join either (L6).
class StoppingSink final : public ILogSink {
 public:
  void attach(ILogger* logger) { logger_ = logger; }
  std::string_view name() const noexcept override { return "console"; }
  SinkKind kind() const noexcept override { return SinkKind::Console; }
  bool write(std::string_view) override {
    if (logger_ != nullptr) { logger_->stop(); }
    return true;
  }
  void flush() override {}
  SinkHealth health() const noexcept override { return SinkHealth::Healthy; }

 private:
  ILogger* logger_ = nullptr;
};

TEST(Logger, StopFromSinkCallbackDoesNotSelfJoin) {
  auto sink = std::make_shared<StoppingSink>();
  AsyncLogger logger(traceOptions(), {sink});
  sink->attach(&logger);
  ASSERT_TRUE(logger.start());
  logger.log(Severity::Info, "t", "e", "");
  logger.flush();
  logger.stop();
  SUCCEED();
}
}  // namespace
}  // namespace tcu::logging

// The console switch is a hot-reloadable setting (research R-15), and it lives
// in AsyncLogger rather than in ConsoleSink because the logger is the only
// caller: one source of truth instead of two that can disagree.

#include <gtest/gtest.h>

#include <memory>
#include <vector>

#include "support/recording_sink.hpp"
#include "tcu/logging/async_logger.hpp"

namespace tcu::logging {
namespace {

TEST(ConsoleSink, DisabledEmitsNothing) {
  auto console = std::make_shared<test::RecordingSink>(SinkKind::Console);
  auto file = std::make_shared<test::RecordingSink>(SinkKind::File);
  LoggerOptions options;
  options.minLevel = Severity::Trace;
  options.consoleEnabled = false;
  AsyncLogger logger(options, {console, file});
  ASSERT_TRUE(logger.start());

  logger.log(Severity::Info, "t", "quiet", "");
  logger.flush();

  EXPECT_EQ(console->lineCount(), 0u);
  EXPECT_EQ(file->lineCount(), 1u) << "disabling the console must not mute the file";
  logger.stop();
}

TEST(ConsoleSink, HotToggleTakesEffectWithoutRestart) {
  auto console = std::make_shared<test::RecordingSink>(SinkKind::Console);
  LoggerOptions options;
  options.minLevel = Severity::Trace;
  options.consoleEnabled = true;
  AsyncLogger logger(options, {console});
  ASSERT_TRUE(logger.start());

  logger.log(Severity::Info, "t", "visible", "");
  logger.flush();
  ASSERT_EQ(console->lineCount(), 1u);

  logger.setConsoleEnabled(false);
  logger.log(Severity::Info, "t", "hidden", "");
  logger.flush();
  EXPECT_EQ(console->lineCount(), 1u);
  EXPECT_FALSE(console->containsSubstring("hidden"));

  logger.setConsoleEnabled(true);
  logger.log(Severity::Info, "t", "visible_again", "");
  logger.flush();
  EXPECT_TRUE(console->containsSubstring("visible_again"));
  logger.stop();
}

}  // namespace
}  // namespace tcu::logging

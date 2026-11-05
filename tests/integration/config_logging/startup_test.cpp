// Scenario A of spec section 12: normal startup. The whole stack together -
// scheduler, logger with a real sink, configuration service - and the lines and
// health flags the scenario promises.

#include <gtest/gtest.h>

#include "support/stack.hpp"

namespace tcu::itest {
namespace {

TEST(Startup, PublishesGenerationOneAndSaysSo) {
  Stack stack;
  ASSERT_EQ(stack.service->start(), config::LoadError::None);
  stack.logger->flush();

  EXPECT_EQ(stack.service->snapshot()->generation, 1u);
  EXPECT_TRUE(stack.sink->containsSubstring("startup_loaded"));
  EXPECT_TRUE(stack.sink->containsSubstring("\"generation\":1"));
  EXPECT_TRUE(stack.sink->containsSubstring("\"component\":\"config\""));
}

// The health line of contracts/metrics.md section 5, assembled from both metric
// snapshots: config_valid=true, logger_healthy=true, generation=1.
TEST(Startup, HealthReportsSinkAndConfigState) {
  Stack stack;
  ASSERT_EQ(stack.service->start(), config::LoadError::None);
  stack.logger->flush();

  const config::ConfigMetrics config_metrics = stack.service->metrics();
  const logging::LoggerMetrics logger_metrics = stack.logger->metrics();

  EXPECT_TRUE(config_metrics.configValid);
  EXPECT_EQ(config_metrics.generation, 1u);
  EXPECT_FALSE(config_metrics.notifyDegraded);
  EXPECT_TRUE(logger_metrics.allSinksHealthy());
  EXPECT_EQ(logger_metrics.queueDepth, 0u);
  EXPECT_EQ(logger_metrics.droppedTotal(), 0u);
}

// No configuration VALUE may appear in a log line: a generation and a pointer
// are all the configuration content the vocabulary allows.
TEST(Startup, LogLinesCarryNoConfigurationValues) {
  Stack stack;
  ASSERT_EQ(stack.service->start(), config::LoadError::None);
  stack.logger->flush();

  EXPECT_FALSE(stack.sink->containsSubstring("mock-cloud.local"))
      << "the endpoint is a configuration value, not a log field";
  EXPECT_FALSE(stack.sink->containsSubstring("\"payload\""));
}

// BC-02 at the stack level: an invalid file stops the service from starting and
// says why, naming the pointer but not the value.
TEST(Startup, InvalidConfigFailsAndReportsThePointer) {
  Stack stack(std::chrono::milliseconds{0}, 64, "invalid_retry.json");

  EXPECT_EQ(stack.service->start(), config::LoadError::InvalidValue);
  stack.logger->flush();

  EXPECT_EQ(stack.service->snapshot(), nullptr);
  EXPECT_TRUE(stack.sink->containsSubstring("startup_failed"));
  EXPECT_TRUE(stack.sink->containsSubstring("/connectivity/retry_max_ms"));
  EXPECT_FALSE(stack.service->metrics().configValid);
}

}  // namespace
}  // namespace tcu::itest

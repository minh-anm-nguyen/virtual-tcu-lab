// Scenarios C and D of spec section 12: an invalid reload and a restart-only
// change. Both keep the running generation, and D additionally must not apply
// the hot key that moved in the same file (BC-08).

#include <gtest/gtest.h>

#include "support/stack.hpp"

namespace tcu::itest {
namespace {

// Scenario C.
TEST(Rejection, InvalidReloadKeepsGenerationAndReportsTheField) {
  Stack stack;
  ASSERT_EQ(stack.service->start(), config::LoadError::None);

  stack.writeFixture("invalid_retry.json");
  const config::ReloadResult result = stack.service->reload();
  stack.logger->flush();

  EXPECT_EQ(result.error, config::LoadError::InvalidValue);
  EXPECT_FALSE(result.changed);
  EXPECT_EQ(result.generation, 1u);
  EXPECT_EQ(stack.service->snapshot()->generation, 1u);
  EXPECT_EQ(stack.service->snapshot()->connectivity.retryMaxMs, 30000);

  EXPECT_TRUE(stack.sink->containsSubstring("reload_rejected"));
  EXPECT_TRUE(stack.sink->containsSubstring("\"reason\":\"InvalidValue\""));
  EXPECT_TRUE(stack.sink->containsSubstring("/connectivity/retry_max_ms"));
  EXPECT_TRUE(stack.sink->containsSubstring("\"severity\":\"WARN\""));
}

// Scenario D: queue_capacity is restart-only and level is hot, both moved in
// one file. The whole candidate is refused and NEITHER is applied.
TEST(Rejection, RestartOnlyChangeAppliesNothing) {
  Stack stack;
  ASSERT_EQ(stack.service->start(), config::LoadError::None);
  const auto before = stack.service->snapshot();

  stack.writeFixture("restart_only.json");
  const config::ReloadResult result = stack.service->reload();
  stack.logger->flush();

  EXPECT_EQ(result.error, config::LoadError::RestartRequired);
  EXPECT_EQ(result.generation, 1u);
  EXPECT_EQ(stack.service->snapshot(), before) << "the same snapshot object";
  EXPECT_EQ(stack.service->snapshot()->logging.queueCapacity, 8192);
  EXPECT_EQ(stack.service->snapshot()->logging.level, logging::Severity::Info)
      << "the co-changed hot key must not slip through";

  EXPECT_TRUE(stack.sink->containsSubstring("\"reason\":\"RestartRequired\""));
  EXPECT_TRUE(stack.sink->containsSubstring("/logging/queue_capacity"));
}

// An unparseable file is the same story, and the reason carries an offset
// rather than any of the file contents.
TEST(Rejection, UnparseableReloadKeepsGeneration) {
  Stack stack;
  ASSERT_EQ(stack.service->start(), config::LoadError::None);

  stack.dir.writeRaw("{ this is not json");
  const config::ReloadResult result = stack.service->reload();
  stack.logger->flush();

  EXPECT_EQ(result.error, config::LoadError::ParseError);
  EXPECT_EQ(stack.service->snapshot()->generation, 1u);
  EXPECT_TRUE(stack.sink->containsSubstring("parse error at byte offset"));
  EXPECT_FALSE(stack.sink->containsSubstring("this is not json"));
}

// After a rejection the service is still fully usable: a later valid file
// commits normally. Last-known-good is not a terminal state.
TEST(Rejection, ServiceRecoversAfterARejectedCandidate) {
  Stack stack;
  ASSERT_EQ(stack.service->start(), config::LoadError::None);

  stack.writeFixture("invalid_retry.json");
  ASSERT_NE(stack.service->reload().error, config::LoadError::None);

  stack.writeWith("\"period_ms\": 5000", "\"period_ms\": 2500");
  const config::ReloadResult good = stack.service->reload();

  EXPECT_EQ(good.error, config::LoadError::None) << good.publicReason;
  EXPECT_TRUE(good.changed);
  EXPECT_EQ(good.generation, 2u) << "a rejected candidate consumed no generation";
  EXPECT_EQ(stack.service->snapshot()->telemetry.periodMs, 2500);
}

}  // namespace
}  // namespace tcu::itest
